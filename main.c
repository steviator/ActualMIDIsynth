/*
 * main.c -- entry point, identical on every platform. Doesn't know or
 * care whether run_input_loop() is backed by alsa_input.c or
 * coremidi_input.c -- that's decided entirely at build time by which
 * one you link in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "daemon_core.h"

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--list-devices") == 0) {
        daemon_list_devices();
        return 0;
    }

    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <soundfont.sf2> [--device N] [--gain FLOAT] [--verbose]\n"
            "       %s --list-devices\n"
            "  --gain: output multiplier, 0.0=silent, 1.0=normal (default),\n"
            "          above 1.0 amplifies. Useful as attenuation headroom\n"
            "          for dense material.\n",
            argv[0], argv[0]);
        return 1;
    }

    const char *soundfont_path = argv[1];
    int device = -1;  /* -1 is BASS's own sentinel for "the system
                          default output device" -- not a real device
                          index, don't confuse with daemon_list_devices'
                          0-based indices. */
    float gain = 1.0f;  /* unity: no change to output level */

    /* Simple linear scan rather than getopt(): only three optional
     * flags, none of them worth the portability/verbosity trade-off of
     * a real option-parsing library for a project this size. Flags
     * that take a value (--device, --gain) check i+1<argc before
     * consuming the next argv slot so a trailing flag with no value
     * doesn't read past the end of argv. */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
            gain = (float)atof(argv[++i]);
        }
    }

    daemon_init(soundfont_path, device, gain);
    run_input_loop();
    daemon_shutdown();
    return 0;
}
