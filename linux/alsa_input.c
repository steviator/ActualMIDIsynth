/*
 * alsa_input.c -- the ALSA half of ActualMIDISynth on Linux.
 *
 * Opens an ALSA sequencer port and calls straight into on_midi_event()/
 * on_midi_sysex() (daemon_core.h) for each decoded event -- plain
 * function calls in the same process, no pipe, no byte serialization.
 *
 * Compiled against the real <alsa/asoundlib.h>, so snd_seq_event_t's
 * fields and the port capability flags are exactly what the compiler
 * says they are -- no struct-layout guessing.
 *
 * The one genuine translation this file carries: ALSA's native pitch
 * bend range is signed (-8192..8191, centered at 0), while
 * on_midi_event()'s PITCHBEND convention is raw 0..16383 (centered at
 * 8192, matching BASSMIDI directly). That +8192 conversion happens here
 * and only here -- coremidi_input.c doesn't need it, since CoreMIDI's
 * native range already matches.
 */

#include <alsa/asoundlib.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "daemon_core.h"

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int sig) { (void)sig; g_running = 0; }

static void dispatch_event(snd_seq_event_t *ev) {
    switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            /* Real MIDI convention, not an ALSA quirk: a note-on message
             * with velocity 0 means "release this note," equivalent to
             * a genuine note-off. Some sources (especially ones using
             * MIDI running status to save bytes) send note-off this way
             * instead of a dedicated note-off message, so this check is
             * required, not optional. */
            if (ev->data.note.velocity == 0)
                on_midi_event(EVENT_NOTEOFF, ev->data.note.channel, ev->data.note.note, 0);
            else
                on_midi_event(EVENT_NOTEON, ev->data.note.channel,
                              ev->data.note.note, ev->data.note.velocity);
            break;

        case SND_SEQ_EVENT_NOTEOFF:
            on_midi_event(EVENT_NOTEOFF, ev->data.note.channel, ev->data.note.note, 0);
            break;

        case SND_SEQ_EVENT_KEYPRESS:
            on_midi_event(EVENT_KEYPRESS, ev->data.note.channel,
                          ev->data.note.note, ev->data.note.velocity);
            break;

        case SND_SEQ_EVENT_CONTROLLER:
            on_midi_event(EVENT_CONTROLLER, ev->data.control.channel,
                          ev->data.control.param, ev->data.control.value);
            break;

        case SND_SEQ_EVENT_PGMCHANGE:
            on_midi_event(EVENT_PGMCHANGE, ev->data.control.channel,
                          ev->data.control.value, 0);
            break;

        case SND_SEQ_EVENT_CHANPRESS:
            on_midi_event(EVENT_CHANPRESS, ev->data.control.channel,
                          ev->data.control.value, 0);
            break;

        case SND_SEQ_EVENT_PITCHBEND: {
            /* ALSA: signed -8192..8191, centered 0. on_midi_event wants
             * raw 0..16383, centered 8192 -- see this file's header
             * comment. */
            int raw = ev->data.control.value + 8192;
            on_midi_event(EVENT_PITCHBEND, ev->data.control.channel, raw, 0);
            break;
        }

        case SND_SEQ_EVENT_SYSEX:
            if (ev->data.ext.ptr && ev->data.ext.len) {
                on_midi_sysex((const unsigned char *)ev->data.ext.ptr, ev->data.ext.len);
            }
            break;

        default:
            break;
    }
}

void run_input_loop(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    /* Note: unlike the earlier blocking-read version of this loop,
     * nothing extra is needed here to make Ctrl+C responsive. poll()
     * (used below) is documented as never auto-restarted by SA_RESTART,
     * unlike read() -- it reliably returns EINTR on signal delivery, so
     * the g_running check in the loop below runs promptly. */

    snd_seq_t *handle;
    int err = snd_seq_open(&handle, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0) {
        fprintf(stderr, "snd_seq_open failed: %s\n", snd_strerror(err));
        return;
    }

    err = snd_seq_set_client_name(handle, "ActualMIDISynth");
    if (err < 0) {
        fprintf(stderr, "snd_seq_set_client_name failed: %s\n", snd_strerror(err));
        return;
    }

    int port = snd_seq_create_simple_port(
        handle, "MIDI in",
        /* WRITE = other clients are allowed to send data INTO this
         * port (we're a receiver, not a source -- the naming is from
         * the perspective of what capability the port itself has, not
         * what we do with it). SUBS_WRITE = those clients can connect
         * via an open/automatic subscription rather than needing
         * special permission -- without this bit, tools like aplaymidi
         * get EPERM trying to connect even though the port otherwise
         * looks right. (These two flags' bit VALUES were transposed by
         * hand-transcription error earlier in this project's life, in
         * the original ctypes prototype -- worth remembering that
         * mistake exists in the history, even though it can't recur
         * here since these are the real <alsa/seq.h> macros, not
         * hand-copied numbers.) */
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        /* MIDI_GENERIC + SYNTH together describe what kind of MIDI
         * client this is, purely informational metadata visible in
         * tools like `aconnect -l` -- doesn't affect functionality. */
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTH);
    if (port < 0) {
        fprintf(stderr, "snd_seq_create_simple_port failed: %s\n", snd_strerror(port));
        return;
    }

    printf("Ready. ALSA seq port %d (see it with: aconnect -l)\n", port);
    printf("Press Ctrl+C to quit.\n\n");

    int npfd = snd_seq_poll_descriptors_count(handle, POLLIN);
    struct pollfd *pfds = malloc(sizeof(struct pollfd) * (size_t)npfd);
    if (!pfds) {
        fprintf(stderr, "out of memory allocating poll descriptors\n");
        snd_seq_close(handle);
        return;
    }
    snd_seq_poll_descriptors(handle, pfds, (unsigned int)npfd, POLLIN);

    while (g_running) {
        int pr = poll(pfds, (nfds_t)npfd, -1);  /* block until data or a signal */
        if (pr < 0) {
            if (errno == EINTR) continue;  /* signal arrived -- g_running check below handles it */
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            break;
        }
        if (!g_running) break;

        /* Drain everything currently queued in one wake-up rather than
         * going back through poll() per event -- meaningfully fewer
         * syscalls under bursty/dense MIDI traffic. The "1" argument to
         * snd_seq_event_input_pending asks it to actually check with
         * the kernel for newly-arrived events (rather than just
         * returning a possibly-stale cached count), which is what we
         * want right after poll() told us data is available. */
        while (snd_seq_event_input_pending(handle, 1) > 0) {
            snd_seq_event_t *ev;
            int rc = snd_seq_event_input(handle, &ev);
            if (rc < 0) break;
            dispatch_event(ev);
        }
    }

    free(pfds);
    snd_seq_close(handle);
}
