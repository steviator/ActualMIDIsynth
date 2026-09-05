/*
 * coremidi_input.c -- the CoreMIDI half of ActualMIDISynth on macOS.
 *
 * Creates a virtual CoreMIDI destination and calls straight into
 * on_midi_event()/on_midi_sysex() (daemon_core.h) for each decoded
 * event -- no pipe, no byte serialization. CoreMIDI's callback model
 * actually fits this better than the old subprocess/wire-protocol
 * design did: the MIDIReadProc callback just calls the shared dispatch
 * function directly instead of formatting bytes to write to stdout.
 *
 * Packet-list parsing here is unchanged from the original standalone
 * coremidi_shim.c: real C compiled against Apple's actual headers, so
 * the compiler gets MIDIPacketList's variable-length packing right by
 * construction, rather than us reimplementing MIDIPacketNext's pointer
 * arithmetic over FFI (which is a documented danger zone -- see this
 * project's notes on why that approach was rejected for the Python
 * prototype).
 *
 * KNOWN SIMPLIFICATION (unchanged from the original shim, still
 * intentional, not silent): assumes each CoreMIDI packet contains
 * complete, non-running-status messages, and does not reassemble sysex
 * fragmented across multiple packets. Covers most virtual-destination
 * senders (DAWs, emulators sending programmatically). If real hardware
 * controllers using running status or fragmented sysex come up in
 * testing, that's the next thing to harden.
 *
 * No conversion needed for pitch bend here: CoreMIDI's raw 14-bit wire
 * format already matches on_midi_event()'s convention (0..16383,
 * centered 8192) exactly.
 */

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "daemon_core.h"

#define MAX_SOURCES 16
/*
 * MIDI running status: to save bytes on the wire, consecutive messages
 * of the same type don't have to repeat their status byte -- once a
 * status byte has been sent, subsequent messages can just send data
 * bytes and the receiver is expected to remember and reuse the last
 * status byte seen. This array is that memory, one slot per possible
 * source connection (only srcIndex 0 is ever actually used right now --
 * see parse_packet's call site below -- but it's sized for the general
 * case since CoreMIDI destinations can technically have multiple
 * simultaneous source connections).
 */
static unsigned char running_status[MAX_SOURCES];

/*
 * Parses raw MIDI bytes (exactly as they'd appear on a physical MIDI
 * cable) into individual messages. This is genuinely low-level: unlike
 * ALSA, which hands us pre-decoded event structs, CoreMIDI hands us raw
 * bytes and expects the receiver to do this parsing itself.
 *
 * Byte layout being decoded here, per the MIDI spec:
 *   - A "status byte" has its top bit set (byte & 0x80) and identifies
 *     both the message type (top nibble, e.g. 0x90 = note-on) and the
 *     channel it applies to (bottom nibble, 0-15).
 *   - "Data bytes" never have the top bit set (always 0-127), and
 *     follow the status byte -- how many depends on the message type
 *     (two for note-on/off, one for program change, etc; see the
 *     needs_two_data_bytes/needs_one_data_byte classification below).
 *   - Sysex (0xF0) is the odd one out: not a fixed-size message at all,
 *     just "read bytes until you see the terminator (0xF7)."
 */
static void parse_packet(const Byte *data, UInt16 length, int srcIndex) {
    UInt16 i = 0;
    while (i < length) {
        unsigned char byte = data[i];

        if (byte == 0xF0) {
            /* Sysex: scan forward to the terminator and forward the
             * whole span (including the F0/F7 framing) as one message.
             * KNOWN SIMPLIFICATION: this assumes the terminator is
             * actually present within this same packet -- see this
             * file's header comment on why fragmented sysex isn't
             * handled (yet). */
            UInt16 start = i;
            while (i < length && data[i] != 0xF7) i++;
            if (i < length) i++;  /* include the F7 itself in the span */
            on_midi_sysex(data + start, i - start);
            continue;
        }

        unsigned char status;
        int consumed_status_byte;
        if (byte & 0x80) {
            /* A real status byte arrived -- use it, and remember it for
             * any running-status messages that might follow. */
            status = byte;
            i++;
            consumed_status_byte = 1;
        } else {
            /* No status byte here (top bit clear) -- this must be
             * running status: reuse whatever status byte we saw last,
             * and treat the CURRENT byte as already being the first
             * data byte (not something to skip past). */
            status = running_status[srcIndex];
            consumed_status_byte = 0;
            if (!status) return;  /* running status with nothing to run
                                      from yet -- malformed/unexpected
                                      input, bail out of this packet
                                      rather than guess */
        }

        unsigned char message_type = status & 0xF0;  /* top nibble: what
                                                          kind of message */
        unsigned char channel = status & 0x0F;        /* bottom nibble:
                                                          which of the 16
                                                          MIDI channels */

        if (message_type == 0xF0) return;  /* system messages (clock,
                                                start/stop, etc) -- not
                                                meaningful to a synth
                                                receiving note data, and
                                                not safe to assume a
                                                fixed data-byte count
                                                for, so just stop rather
                                                than guess how many
                                                bytes to skip */

        running_status[srcIndex] = status;  /* remember this for any
                                                 running-status messages
                                                 that follow */

        /* How many data bytes follow depends on the message type --
         * this is fixed by the MIDI spec, not something we're choosing:
         *   note off/on, poly aftertouch, CC, pitch bend -> 2 bytes
         *   program change, channel aftertouch               -> 1 byte
         *   (anything else, e.g. system messages)             -> 0,
         *     already handled/excluded above */
        int needs_two_data_bytes =
            (message_type == 0x80 || message_type == 0x90 ||
             message_type == 0xA0 || message_type == 0xB0 ||
             message_type == 0xE0);
        int needs_one_data_byte = (message_type == 0xC0 || message_type == 0xD0);

        unsigned char data1 = 0, data2 = 0;
        if (needs_two_data_bytes) {
            /* If this message used running status, "byte" (the one we
             * already read above, without advancing past it via a
             * status byte) IS the first data byte -- don't re-read the
             * next position as if it were separate. */
            data1 = consumed_status_byte ? data[i] : byte;
            if (consumed_status_byte) i++;
            if (i >= length) return;  /* packet ends mid-message --
                                          truncated/malformed, bail
                                          cleanly rather than read past
                                          the buffer */
            data2 = data[i];
            i++;
        } else if (needs_one_data_byte) {
            data1 = consumed_status_byte ? data[i] : byte;
            if (consumed_status_byte) i++;
        }

        switch (message_type) {
            case 0x80:
                on_midi_event(EVENT_NOTEOFF, channel, data1, data2);
                break;
            case 0x90:
                /* Real MIDI convention, not a CoreMIDI quirk: a
                 * note-on with velocity 0 means "release this note" --
                 * some sources send note-off this way (especially when
                 * using running status, since it saves a status-byte
                 * change) instead of a dedicated 0x80 message. */
                if (data2 == 0)
                    on_midi_event(EVENT_NOTEOFF, channel, data1, 0);
                else
                    on_midi_event(EVENT_NOTEON, channel, data1, data2);
                break;
            case 0xA0:
                on_midi_event(EVENT_KEYPRESS, channel, data1, data2);
                break;
            case 0xB0:
                on_midi_event(EVENT_CONTROLLER, channel, data1, data2);
                break;
            case 0xC0:
                on_midi_event(EVENT_PGMCHANGE, channel, data1, 0);
                break;
            case 0xD0:
                on_midi_event(EVENT_CHANPRESS, channel, data1, 0);
                break;
            case 0xE0: {
                /* Pitch bend is MIDI's one 14-bit value split across
                 * two 7-bit data bytes: data1 is the low 7 bits (LSB),
                 * data2 is the high 7 bits, sent as the SECOND byte
                 * despite being the more-significant half. Combining
                 * them with data1 | (data2<<7) reconstructs the full
                 * 0..16383 range, which already matches
                 * on_midi_event()'s expected convention directly (see
                 * this file's header comment) -- no further conversion
                 * needed, unlike alsa_input.c's ALSA-range case. */
                int raw = data1 | (data2 << 7);
                on_midi_event(EVENT_PITCHBEND, channel, raw, 0);
                break;
            }
            default:
                break;  /* message type we don't forward */
        }
    }
}

/*
 * CoreMIDI's callback for "data arrived on this destination." A single
 * call can bundle multiple packets (pktlist->numPackets), each of which
 * can itself contain multiple MIDI messages (handled by parse_packet's
 * own internal loop above) -- two separate levels of batching.
 *
 * The srcIndex hardcoded to 0 here (not, say, derived from
 * srcConnRefCon) reflects that this project only creates ONE virtual
 * destination with no per-source tracking -- fine for the common case
 * of one DAW/emulator connected at a time, but means running status is
 * shared across whatever sources happen to be connected simultaneously,
 * which could misbehave if multiple sources interleave messages. Not
 * something this project has hit in practice, but worth knowing if it
 * ever does.
 */
static void midi_read_proc(const MIDIPacketList *pktlist, void *readProcRefCon,
                            void *srcConnRefCon) {
    (void)readProcRefCon;  /* refCon we passed to MIDIDestinationCreate --
                               unused, we don't need per-destination
                               context since there's only ever one */
    (void)srcConnRefCon;   /* identifies which source sent this --
                               unused for the reason noted above */

    const MIDIPacket *packet = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        parse_packet(packet->data, packet->length, 0);
        /* MIDIPacketNext is a macro (not a real exported function),
         * which is exactly why this file exists as compiled C rather
         * than a ctypes/FFI binding -- see this file's header comment.
         * It exists specifically because MIDIPacket is variable-length
         * in memory (the "data[256]" in Apple's struct definition is
         * misleading; only `length` bytes are actually valid/present,
         * and the next packet starts right after them, not at a fixed
         * 256-byte stride), so naive pointer arithmetic or array
         * indexing would read garbage. Only the real compiled macro,
         * expanded against Apple's real headers, gets this right. */
        packet = MIDIPacketNext(packet);
    }
}

static void handle_signal(int sig) {
    (void)sig;
    /* Calling into CoreFoundation from a signal handler isn't strictly
     * guaranteed async-signal-safe by POSIX, but this is a very common,
     * widely-used pattern for CLI tools built on CoreMIDI/CFRunLoop,
     * and works reliably in practice -- there isn't a clean
     * fully-POSIX-safe alternative available here (no equivalent to
     * alsa_input.c's poll()-based approach, since CFRunLoopRun owns the
     * blocking rather than us). Worth knowing this is a pragmatic
     * choice, not an ironclad guarantee, if a hang-on-exit is ever
     * reported specifically on macOS. */
    CFRunLoopStop(CFRunLoopGetCurrent());
}

void run_input_loop(void) {
    memset(running_status, 0, sizeof(running_status));

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* MIDIClientCreate registers this process with the system's
     * CoreMIDI service -- required before creating any endpoints.
     * NULL, NULL: no notification callback/refcon -- we don't need to
     * be told about system-wide MIDI setup changes (new devices
     * appearing, etc), only about data arriving on our own destination
     * (handled separately by midi_read_proc via MIDIDestinationCreate
     * below). */
    MIDIClientRef client;
    OSStatus status = MIDIClientCreate(CFSTR("ActualMIDISynth"), NULL, NULL, &client);
    if (status != noErr) {
        fprintf(stderr, "MIDIClientCreate failed: %d\n", (int)status);
        return;
    }

    /* This is the call that actually makes "MIDI in" show up as a
     * selectable destination in other apps (DAWs, emulators, Audio MIDI
     * Setup) -- MIDIDestinationCreate, not MIDIInputPortCreate. The
     * distinction matters: MIDIInputPortCreate is for when THIS process
     * wants to subscribe to an existing external source (e.g. a
     * physical keyboard); MIDIDestinationCreate is for when THIS
     * process wants to BE a destination that other things send to --
     * which is our actual role here, mirroring the "WRITE" capability
     * alsa_input.c requests on Linux. */
    MIDIEndpointRef dest;
    status = MIDIDestinationCreate(client, CFSTR("MIDI in"), midi_read_proc, NULL, &dest);
    if (status != noErr) {
        fprintf(stderr, "MIDIDestinationCreate failed: %d\n", (int)status);
        return;
    }

    printf("Ready. Virtual destination \"MIDI in\" created.\n");
    printf("Connect a source to it in Audio MIDI Setup or your DAW/emulator.\n");
    printf("Press Ctrl+C to quit.\n\n");

    /* Unlike alsa_input.c's explicit poll()/read() loop, CoreMIDI
     * delivers events via callback (midi_read_proc above), invoked from
     * within this run loop's processing -- CFRunLoopRun() itself IS the
     * blocking wait; it returns once handle_signal calls
     * CFRunLoopStop(). */
    CFRunLoopRun();

    MIDIClientDispose(client);
}
