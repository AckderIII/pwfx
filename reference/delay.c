/*
 * delay.c -- delay and echo for raw 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the delayed stream to stdout in the same format.
 *
 * Design notes
 * ------------
 *  - One ring buffer per channel holds w = x + feedback * d, where d is the
 *    ring read D samples back. The output is dry * x + wet * d.
 *  - With no feedback and all wet, the output is the input D samples late
 *    and nothing else: the tool for lining one source up with another --
 *    a close mic with a room mic, a camera's audio with a USB mic.
 *  - With feedback it is an echo. The k-th repeat lands at k * D and has
 *    amplitude wet * feedback^(k-1); feedback is capped at 95 % so the
 *    repeats always die away.
 *  - The delay IS the output, so it is not latency to be absorbed: unlike
 *    gate.c or comp.c, nothing here swallows the first D samples, and the
 *    stream comes out as long as it went in.
 *  - A delay of 0 is a straight passthrough, whatever the mix.
 *  - The ring and the stream are float; the feedback, the only quantity
 *    accumulated, is computed in double and stored as float.
 *
 * Build:  cc -O2 -o delay delay.c -lm
 * Usage:  delay [options]   (see --help)
 *
 * Example (48 kHz stereo, a slap echo):
 *   ... | ./delay -r 48000 -c 2 -t 120 -f 30 -m 35 | ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096
#define MAX_MS       2000.0

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-t ms] [-f feedback%%] [-m mix%%] [-r rate_hz] [-c channels]\n"
        "\n"
        "  -t  delay time in ms, 0..%g                (default 0)\n"
        "  -f  feedback in percent, 0..95             (default 0)\n"
        "  -m  wet share of the output in percent     (default 100)\n"
        "  -r  sample rate in Hz                      (default 48000)\n"
        "  -c  interleaved channel count, 1..%d       (default 1)\n"
        "  -b  frames read per block                  (default 128)\n"
        "\n"
        "Reads raw 32-bit float samples from stdin, writes them to stdout.\n"
        "The defaults with a time set are a plain delay; add feedback and\n"
        "pull the mix down for an echo.\n",
        argv0, MAX_MS, MAX_CHANNELS);
}

int main(int argc, char **argv)
{
    double time_ms  = 0.0;
    double feedback = 0.0;
    double mix      = 100.0;
    double rate     = 48000.0;
    int    channels = 1;
    int    block    = 128;
    int    i, ch, D, frames_in_ring, w = 0;
    double fb, wet, dry;
    float *ring;

    static float buf[MAX_BLOCK * MAX_CHANNELS];
    size_t got;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], argv[i]);
            return 2;
        }
        if      (!strcmp(argv[i], "-t")) time_ms  = atof(argv[++i]);
        else if (!strcmp(argv[i], "-f")) feedback = atof(argv[++i]);
        else if (!strcmp(argv[i], "-m")) mix      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b")) block    = atoi(argv[++i]);
        else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (rate <= 0.0) {
        fprintf(stderr, "%s: sample rate must be positive\n", argv[0]);
        return 2;
    }
    if (time_ms < 0.0 || time_ms > MAX_MS) {
        fprintf(stderr, "%s: time must be 0..%g ms\n", argv[0], MAX_MS);
        return 2;
    }
    if (feedback < 0.0 || feedback > 95.0) {
        fprintf(stderr, "%s: feedback must be 0..95 %%\n", argv[0]);
        return 2;
    }
    if (mix < 0.0 || mix > 100.0) {
        fprintf(stderr, "%s: mix must be 0..100 %%\n", argv[0]);
        return 2;
    }
    if (channels < 1 || channels > MAX_CHANNELS) {
        fprintf(stderr, "%s: channels must be 1..%d\n", argv[0], MAX_CHANNELS);
        return 2;
    }
    if (block < 1 || block > MAX_BLOCK) {
        fprintf(stderr, "%s: block must be 1..%d frames\n", argv[0], MAX_BLOCK);
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    D   = (int)(time_ms * rate / 1000.0 + 0.5);
    fb  = D > 0 ? feedback / 100.0 : 0.0;     /* no delay, no loop */
    wet = mix / 100.0;
    dry = 1.0 - wet;

    frames_in_ring = D + 1;
    ring = calloc((size_t)frames_in_ring * (size_t)channels, sizeof *ring);
    if (!ring) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels;
        size_t tail   = got % (size_t)channels;   /* torn frame at EOF */
        size_t n;

        /* Scrub non-finite input. The feedback loop is recursive: one NaN
         * in it would repeat for as long as the echo does. */
        for (n = 0; n < got; n++)
            if (!isfinite(buf[n])) buf[n] = 0.0f;

        for (n = 0; n < frames; n++) {
            int rd = (w + frames_in_ring - D) % frames_in_ring;

            for (ch = 0; ch < channels; ch++) {
                float x = buf[n * channels + ch];
                float d = D == 0 ? x : ring[(size_t)rd * channels + ch];

                ring[(size_t)w * channels + ch] = (float)((double)x + fb * (double)d);
                buf[n * channels + ch] = (float)(dry * (double)x + wet * (double)d);
            }
            w = (w + 1) % frames_in_ring;
        }

        if (fwrite(buf, sizeof(float), frames * channels, stdout)
                != frames * channels) {
            fprintf(stderr, "%s: write failed\n", argv[0]);
            return 1;
        }
        fflush(stdout);

        if (tail) {
            fwrite(&buf[frames * channels], sizeof(float), tail, stdout);
            fflush(stdout);
            break;
        }
    }

    if (ferror(stdin)) {
        fprintf(stderr, "%s: read failed\n", argv[0]);
        return 1;
    }

    free(ring);
    fflush(stdout);
    return 0;
}
