/*
 * daemon_core.c -- ActualMIDISynth's platform-agnostic core.
 *
 * Knows nothing about ALSA or CoreMIDI. on_midi_event()/on_midi_sysex()
 * are called directly, as plain function calls, by whichever platform
 * input file is linked into this binary (alsa_input.c or
 * coremidi_input.c) -- no subprocess, no pipe, no byte serialization.
 *
 * The RPN/NRPN state machine and translate_event() logic are unchanged
 * from the proven daemon.c (and, before that, the proven Python
 * prototype) -- same branches, same reasoning, just called differently.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bass.h"
#include "bassmidi.h"
#include "daemon_core.h"

int g_verbose = 0;

static HSTREAM g_stream = 0;

/* =============================================================================
 * RPN/NRPN state -- see daemon.c's identical block for the full
 * rationale: Data Entry (CC6/38) only means something in light of what
 * was selected last via CC98/99/100/101, and selecting an NRPN cancels
 * an active RPN selection (and vice versa) per the MIDI spec.
 * ============================================================================= */

#define RPN_NONE 0
#define RPN_RPN  1
#define RPN_NRPN 2

/* One of these per MIDI channel, and it genuinely needs to persist
 * between calls to translate_event() -- this is the one place this
 * file isn't a pure "event in, BASSMIDI call out" function, and that's
 * intentional, not an accident. The reason: MIDI's RPN/NRPN mechanism
 * is inherently a two-step protocol -- one CC message *selects* a
 * parameter (which one is only known from a *previous* message), and a
 * later CC message supplies its value. There's no way to interpret
 * "Data Entry MSB = 12" correctly without remembering what was
 * selected before it arrived. Real hardware synths keep exactly this
 * same per-channel state internally; this isn't a workaround, it's
 * following the spec. */
typedef struct {
    int selected_kind;  /* RPN_NONE, RPN_RPN, or RPN_NRPN -- which of
                            CC98/99 (NRPN) or CC100/101 (RPN) was used
                            most recently. Selecting one implicitly
                            deselects the other, per spec. */
    int msb, lsb;        /* The two bytes that were sent along with
                            whichever selector was used above --
                            together they identify *which* RPN/NRPN
                            parameter (e.g. RPN (0,0) = pitch bend
                            range; see rpn_data_entry() below). */
} RpnState;

static RpnState rpn_states[16];  /* MIDI has 16 channels (0-15), fixed
                                     by the spec, not a tunable. */

/* Standard MIDI CC number (0-127) -> the specific BASSMIDI event that
 * handles it, as a direct lookup table rather than a scanned array.
 * (The Python prototype used a dict here -- O(1). An early pass at this
 * C port used a linear-scan array of {cc, event} pairs, which silently
 * downgraded that to O(n) on every single CC event. Fixed: this is a
 * genuine hot path -- sustain, expression, and modulation CCs show up
 * constantly in busy streams.) 0 means "unmapped, fall through to
 * MIDI_EVENT_CONTROL". */
static DWORD cc_to_event_table[128];

static void init_cc_to_event_table(void) {
    memset(cc_to_event_table, 0, sizeof(cc_to_event_table));
    cc_to_event_table[0] = MIDI_EVENT_BANK;
    cc_to_event_table[1] = MIDI_EVENT_MODULATION;
    cc_to_event_table[5] = MIDI_EVENT_PORTATIME;
    cc_to_event_table[7] = MIDI_EVENT_VOLUME;
    cc_to_event_table[10] = MIDI_EVENT_PAN;
    cc_to_event_table[11] = MIDI_EVENT_EXPRESSION;
    cc_to_event_table[32] = MIDI_EVENT_BANK_LSB;
    cc_to_event_table[64] = MIDI_EVENT_SUSTAIN;
    cc_to_event_table[65] = MIDI_EVENT_PORTAMENTO;
    cc_to_event_table[66] = MIDI_EVENT_SOSTENUTO;
    cc_to_event_table[67] = MIDI_EVENT_SOFT;
    cc_to_event_table[71] = MIDI_EVENT_RESONANCE;
    cc_to_event_table[72] = MIDI_EVENT_RELEASE;
    cc_to_event_table[73] = MIDI_EVENT_ATTACK;
    cc_to_event_table[74] = MIDI_EVENT_CUTOFF;
    cc_to_event_table[75] = MIDI_EVENT_DECAY;
    cc_to_event_table[84] = MIDI_EVENT_PORTANOTE;
    cc_to_event_table[91] = MIDI_EVENT_REVERB;
    cc_to_event_table[93] = MIDI_EVENT_CHORUS;
    cc_to_event_table[120] = MIDI_EVENT_SOUNDOFF;
    cc_to_event_table[121] = MIDI_EVENT_RESET;
    cc_to_event_table[123] = MIDI_EVENT_NOTESOFF;
}

static int lookup_cc_event(int cc, DWORD *out_event) {
    /* MIDI CC numbers are always 0-127 by spec, but data1 arrives here
     * as a plain int with no type-level guarantee of that -- a
     * malformed or unusual source could in principle hand us something
     * outside that range, so bounds-check before indexing rather than
     * trust it. */
    if (cc < 0 || cc > 127) return 0;
    DWORD mapped = cc_to_event_table[cc];
    if (mapped == 0) return 0;  /* 0 is the table's "unmapped" sentinel --
                                    safe because no real MIDI_EVENT_*
                                    constant is ever 0. */
    *out_event = mapped;
    return 1;
}

/*
 * Called when a Data Entry MSB (CC6) arrives while an RPN (not NRPN) is
 * the currently-selected target. The (msb, lsb) pairs checked here are
 * MIDI's three *Registered* (i.e. spec-defined, universal, not
 * vendor-specific) Parameter Numbers -- there are only these three:
 *   RPN (0, 0) = Pitch Bend Sensitivity  (range of the pitch wheel, in
 *                semitones -- this is the one that mattered for the
 *                "Lightning Speed" bug: without this function, a fast
 *                pitch-bend sweep authored for a 12-semitone range got
 *                rendered using BASSMIDI's default range instead,
 *                producing an audibly wrong bend depth)
 *   RPN (0, 1) = Master Fine Tuning      (small pitch offset, cents)
 *   RPN (0, 2) = Master Coarse Tuning    (pitch offset, semitones)
 * Any other (msb, lsb) is a real RPN we simply don't have a BASSMIDI
 * event for, so it's dropped rather than guessed at -- same policy as
 * an NRPN-selected Data Entry (handled by the caller, see
 * EVENT_CONTROLLER below).
 */
static int rpn_data_entry(RpnState *state, int value, DWORD *out_event, DWORD *out_param) {
    if (state->selected_kind != RPN_RPN) return 0;
    if (state->msb == 0 && state->lsb == 0) {
        *out_event = MIDI_EVENT_PITCHRANGE; *out_param = (DWORD)value; return 1;
    }
    if (state->msb == 0 && state->lsb == 1) {
        *out_event = MIDI_EVENT_FINETUNE; *out_param = (DWORD)value; return 1;
    }
    if (state->msb == 0 && state->lsb == 2) {
        *out_event = MIDI_EVENT_COARSETUNE; *out_param = (DWORD)value; return 1;
    }
    return 0;
}

/* Pure translation: event -> BASSMIDI call. Returns 1 if it produced
 * something to send, 0 if the event should be dropped. */
static int translate_event(int ev_type, int channel, int data1, int data2,
                            DWORD *out_event, DWORD *out_param) {
    switch (ev_type) {
        case EVENT_NOTEON:
            /* BASSMIDI's MIDI_EVENT_NOTE packs both values into one
             * DWORD param: the note number in the low byte, velocity in
             * the next byte up (bits 8-15). This is BASSMIDI's own
             * documented format, not something we invented -- see
             * BASS_MIDI_StreamEvent's docs for MIDI_EVENT_NOTE. */
            *out_event = MIDI_EVENT_NOTE;
            *out_param = (DWORD)data1 | ((DWORD)data2 << 8);
            return 1;

        case EVENT_NOTEOFF:
            /* Same MIDI_EVENT_NOTE event as note-on, but BASSMIDI treats
             * velocity 0 (i.e. an empty high byte, since we don't set
             * bits 8+ here at all) as "release this note" rather than
             * "play it at velocity 0" -- so note-off doesn't need to
             * carry a velocity byte through at all. */
            *out_event = MIDI_EVENT_NOTE;
            *out_param = (DWORD)data1;
            return 1;

        case EVENT_KEYPRESS:
            /* Polyphonic aftertouch: same low-byte/high-byte packing as
             * note-on, but "velocity" here means per-note pressure. */
            *out_event = MIDI_EVENT_KEYPRES;
            *out_param = (DWORD)data1 | ((DWORD)data2 << 8);
            return 1;

        case EVENT_CONTROLLER: {
            int cc = data1, value = data2;
            RpnState *state = &rpn_states[channel];

            /* These six CC numbers are fixed by the MIDI spec, not
             * something specific to this project -- they're the
             * standard RPN/NRPN two-step "select a parameter, then set
             * its value" mechanism:
             *   CC101/100 = RPN  select, MSB/LSB
             *   CC99/98   = NRPN select, MSB/LSB
             *   CC6/38    = Data Entry,  MSB/LSB (sets whichever
             *               parameter was selected most recently by
             *               whichever pair above was used last)
             * None of these six are audible events by themselves --
             * they only update rpn_states, hence "return 0" (nothing to
             * send to BASSMIDI) for the select messages. */
            if (cc == 101) { state->selected_kind = RPN_RPN;  state->msb = value; return 0; }
            if (cc == 100) { state->selected_kind = RPN_RPN;  state->lsb = value; return 0; }
            if (cc == 99)  { state->selected_kind = RPN_NRPN; state->msb = value; return 0; }
            if (cc == 98)  { state->selected_kind = RPN_NRPN; state->lsb = value; return 0; }
            if (cc == 6)   return rpn_data_entry(state, value, out_event, out_param);
            /* Data Entry LSB (CC38): real RPNs occasionally use this for
             * sub-integer precision (e.g. cents on top of semitones),
             * but we only act on the MSB (CC6) above -- dropping LSB
             * refinement is a deliberate simplification, not a bug, and
             * matches the original prototype's behavior. */
            if (cc == 38)  return 0;

            DWORD mapped;
            if (lookup_cc_event(cc, &mapped)) {
                *out_event = mapped;
                *out_param = (DWORD)value;
                return 1;
            }
            /* No dedicated BASSMIDI event for this CC number -- fall
             * back to the generic MIDI_EVENT_CONTROL, which (per
             * BASSMIDI's docs) expects the same low-byte/high-byte
             * packing as note-on: CC number in the low byte, its value
             * in the next byte up. */
            *out_event = MIDI_EVENT_CONTROL;
            *out_param = (DWORD)cc | ((DWORD)value << 8);
            return 1;
        }

        case EVENT_PGMCHANGE:
            *out_event = MIDI_EVENT_PROGRAM;
            *out_param = (DWORD)data1;
            return 1;

        case EVENT_CHANPRESS:
            *out_event = MIDI_EVENT_CHANPRES;
            *out_param = (DWORD)data1;
            return 1;

        case EVENT_PITCHBEND:
            /* Deliberately no bit-shifting or offset math here: the
             * shared convention (see daemon_core.h's docs on
             * on_midi_event) already has the caller hand us a value in
             * BASSMIDI's own native range (0..16383, centered at 8192),
             * so this is a straight pass-through. Any conversion from a
             * platform's different native range (ALSA's signed
             * -8192..8191, for instance) happens in that platform's
             * input file, not here -- keeping this function ignorant of
             * which platform called it. */
            *out_event = MIDI_EVENT_PITCH;
            *out_param = (DWORD)data1;
            return 1;

        default:
            return 0;  /* an event type we don't recognize/forward */
    }
}

/* =============================================================================
 * Public interface -- called directly by the platform input layer.
 * ============================================================================= */

void on_midi_event(int ev_type, int channel, int data1, int data2) {
    DWORD bassmidi_event, param;
    if (translate_event(ev_type, channel, data1, data2, &bassmidi_event, &param)) {
        if (g_verbose) {
            printf("ch=%2d bassmidi_event=%3u param=%u\n", channel, bassmidi_event, param);
        }
        /* BASS_MIDI_StreamEvent's own BOOL return isn't checked here on
         * purpose: this runs once per MIDI event, potentially very
         * often, and a single dropped/failed event isn't fatal to the
         * running synth the way a failure during startup is -- there's
         * nothing useful to do in response except keep going, so
         * checking it would just be noise (or, if we did exit() on
         * failure here, a crash risk turning a minor glitch into the
         * whole daemon going down). Contrast with check_bass() below,
         * used only during one-time setup where failing loudly is
         * exactly the right behavior. */
        BASS_MIDI_StreamEvent(g_stream, channel, bassmidi_event, param);
    }
}

void on_midi_sysex(const unsigned char *data, unsigned int len) {
    if (g_verbose) {
        printf("SYSEX (%u bytes)\n", len);
    }
    BASS_MIDI_StreamEvents(g_stream, BASS_MIDI_EVENTS_RAW, data, len);
}

/* Fatal-on-failure check, deliberately used only for one-time startup
 * calls (BASS_Init, stream/font creation, etc.) where "the daemon can't
 * possibly do anything useful from here" is actually true, and where
 * failing immediately and loudly is more helpful than pretending things
 * are fine. NOT used on the per-event hot path -- see on_midi_event's
 * comment on why that's a deliberate difference, not an oversight. */
static void check_bass(BOOL ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "%s failed (BASS error %d)\n", what, BASS_ErrorGetCode());
        exit(1);
    }
}

void daemon_list_devices(void) {
    BASS_DEVICEINFO info;
    printf("Available output devices:\n");
    int device = 0;
    int found_any = 0;
    /* BASS_GetDeviceInfo returns FALSE once you've walked past the last
     * real device index -- there's no separate "count of devices" call,
     * you just probe upward from 0 until it says no. */
    while (BASS_GetDeviceInfo(device, &info)) {
        found_any = 1;
        printf("  %d: %s%s%s\n", device, info.name,
               (info.flags & BASS_DEVICE_ENABLED) ? " (enabled)" : "",
               (info.flags & BASS_DEVICE_DEFAULT) ? " (default)" : "");
        device++;
    }
    if (!found_any) printf("  (none found)\n");
}

void daemon_init(const char *soundfont_path, int device, float gain) {
    memset(rpn_states, 0, sizeof(rpn_states));  /* all channels start with
                                                    no RPN/NRPN selected */
    init_cc_to_event_table();

    printf("Initializing BASS...\n");
    /* Default buffer is 500ms, fine for static playback, wrong for a
     * live soft-synth -- see the project's timing writeup for the full
     * story of why this matters. Both of these MUST be set before
     * BASS_Init; BASS doesn't allow changing them on an already-opened
     * device. */
    BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 10);  /* ms, mixer's internal tick */
    BASS_SetConfig(BASS_CONFIG_BUFFER, 40);        /* ms, must exceed the tick above */
    check_bass(BASS_Init(device, 44100, 0, NULL, NULL), "BASS_Init");

    printf("Loading soundfont: %s\n", soundfont_path);
    HSOUNDFONT font = BASS_MIDI_FontInit(soundfont_path, 0);
    if (!font) {
        fprintf(stderr, "BASS_MIDI_FontInit failed (BASS error %d)\n", BASS_ErrorGetCode());
        exit(1);
    }

    /* "16" here is standard MIDI's fixed channel count (0-15), not a
     * tunable -- matches the size of rpn_states[] above. BASS_MIDI_ASYNC
     * is what fixes event timing to track real elapsed time instead of
     * being quantized to the mixer's update tick; see the project's
     * timing writeup for why that mattered on fast/dense material. Freq
     * 0 means "use whatever rate BASS_Init was given above" (44100)
     * rather than specifying it twice. */
    g_stream = BASS_MIDI_StreamCreate(16, BASS_MIDI_ASYNC, 0);
    check_bass(g_stream != 0, "BASS_MIDI_StreamCreate");

    BASS_MIDI_FONT fonts[1];
    fonts[0].font = font;
    fonts[0].preset = -1;  /* -1 = load every preset/instrument in this
                               soundfont, not just one -- we want the
                               whole bank available, not a single
                               patch. */
    fonts[0].bank = 0;
    check_bass(BASS_MIDI_StreamSetFonts(g_stream, fonts, 1) != (DWORD)-1, "BASS_MIDI_StreamSetFonts");
    check_bass(BASS_ChannelPlay(g_stream, FALSE), "BASS_ChannelPlay");

    /* Exact float comparison against 1.0 is safe here specifically
     * because this isn't a precision-sensitive calculation being
     * checked for equality -- 1.0f is main.c's literal default when
     * --gain isn't passed at all, so this is really "was --gain
     * explicitly given a value," used purely to skip an unnecessary
     * BASS call (and the "Setting gain" printout) in the common case.
     * Passing --gain 1.0 explicitly still works correctly either way;
     * it would just also skip the printout, which is harmless. */
    if (gain != 1.0f) {
        printf("Setting gain to %.3f\n", gain);
        check_bass(BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, gain), "BASS_ChannelSetAttribute(VOL)");
    }
}

void daemon_shutdown(void) {
    printf("\nShutting down...\n");
    BASS_Free();
}
