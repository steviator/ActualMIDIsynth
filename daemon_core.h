/*
 * daemon_core.h -- the interface between daemon_core.c (platform-
 * agnostic: BASSMIDI + MIDI translation) and whichever platform input
 * file is compiled in (alsa_input.c on Linux, coremidi_input.c on
 * macOS). Direct function calls now, not a wire protocol -- there is no
 * IPC boundary here, just two .c files linked into one binary.
 */

#ifndef ACTUALMIDISYNTH_DAEMON_CORE_H
#define ACTUALMIDISYNTH_DAEMON_CORE_H

#define EVENT_NOTEON      1
#define EVENT_NOTEOFF     2
#define EVENT_KEYPRESS    3  /* polyphonic aftertouch */
#define EVENT_CONTROLLER  4
#define EVENT_PGMCHANGE   5
#define EVENT_CHANPRESS   6  /* channel aftertouch */
/* 7 is deliberately unused here -- sysex doesn't fit this fixed
 * (ev_type, channel, data1, data2) shape (it's variable-length), so it
 * gets its own function, on_midi_sysex(), below, instead of a tag in
 * this enum. The gap is kept anyway so these numbers stay stable and
 * match the values used in the project's earlier subprocess-based
 * prototype, where 7 *was* the sysex tag in a byte-stream wire format --
 * not load-bearing here, just avoids renumbering history for no reason. */
#define EVENT_PITCHBEND   8

/* Set by main.c from --verbose. Read by daemon_core.c to decide whether
 * to print each translated event; not touched by the platform input
 * layers directly. */
extern int g_verbose;

/*
 * Called by the platform input layer once per decoded MIDI event.
 * Implemented in daemon_core.c: translates and forwards to BASSMIDI.
 *
 * data1/data2 meaning depends on ev_type:
 *   NOTEON/NOTEOFF/KEYPRESS : (note, velocity)
 *   CONTROLLER               : (cc_number, value)
 *   PGMCHANGE                : (program, unused)
 *   CHANPRESS                : (pressure, unused)
 *   PITCHBEND                : (value, unused) -- value is 0..16383,
 *                               centered at 8192, matching BASSMIDI's
 *                               own MIDI_EVENT_PITCH convention
 *                               directly. Platforms whose native pitch
 *                               bend range differs (ALSA's signed
 *                               -8192..8191 does; CoreMIDI's raw
 *                               0..16383 doesn't) must convert to this
 *                               convention before calling -- see
 *                               alsa_input.c for where that conversion
 *                               lives.
 */
void on_midi_event(int ev_type, int channel, int data1, int data2);

/* Called for one complete sysex message, including the F0...F7 framing. */
void on_midi_sysex(const unsigned char *data, unsigned int len);

/* Implemented in daemon_core.c. gain is a post-mix multiplier on the
 * whole stream's output, applied via BASS_ATTRIB_VOL: 0.0 = silent,
 * 1.0 = normal/unity (the default), values above 1.0 amplify. This is
 * independent of whatever the MIDI content itself sets via CC7/etc --
 * it's a fixed adjustment for the daemon's overall output level,
 * useful as headroom/attenuation for dense material (e.g. black MIDI)
 * that would otherwise clip. */
void daemon_init(const char *soundfont_path, int device, float gain);
void daemon_shutdown(void);
void daemon_list_devices(void);

/*
 * Implemented by whichever platform input file is linked in. Blocks
 * until shutdown (Ctrl+C, source disconnect, etc.) -- this call IS the
 * program's main loop once daemon_init() has completed.
 */
void run_input_loop(void);

#endif /* ACTUALMIDISYNTH_DAEMON_CORE_H */
