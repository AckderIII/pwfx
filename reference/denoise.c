/*
 * denoise.c -- noise suppression for raw 32-bit float audio, with RNNoise.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin at 48 kHz, writes the denoised stream to stdout in the same format.
 *
 * Design notes
 * ------------
 *  - The suppression is RNNoise (Jean-Marc Valin, Xiph.Org; BSD-3), vendored
 *    in third_party/rnnoise: a recurrent network that turns each 10 ms
 *    frame's band energies, pitch and its own memory into a gain per band.
 *    It tells a voice from a fan, a keyboard or traffic, steady or not,
 *    where a gate or a spectral subtractor only knows loud from quiet. It
 *    works at 48 kHz only, on frames of 480 samples, on a 16-bit scale --
 *    the samples are scaled to it and back.
 *  - The stream is read in frames, so nothing here is buffered beyond what
 *    RNNoise itself does: its output is two frames late -- it synthesises
 *    the previous call's spectrum, and its overlapping 960-sample windows
 *    add another frame. (One, by a reading of its code; two, by correlating
 *    its output with real speech, which is how tests/denoise.py knows.)
 *    That delay is absorbed like comp.c's lookahead -- the first two frames
 *    of output are dropped and two of silence pushed at EOF -- so the output
 *    is sample-aligned with the input and exactly as long.
 *  - The reduction is a limit, not a switch:
 *
 *        y = wet + f * (dry - wet),    f = 10^(-reduction / 20)
 *
 *    with dry the input aligned with wet: two frames back, or the mix is a
 *    comb. Where RNNoise takes the noise out
 *    entirely, what is left is the noise at -reduction dB: a room that still
 *    sounds like a room, rather than the dead air between words a full
 *    suppression leaves. Where it leaves the voice alone, wet is dry and so
 *    is y. At reduction 0 it is the input.
 *  - Optionally a gate on RNNoise's voice-activity estimate (--vad): frames
 *    it is less sure than that are speech come out at f * dry, held open
 *    200 ms after the last frame that was, and ramped linearly across a
 *    frame either way so the switch cannot click.
 *  - One RNNoise state per channel; the gate goes by the most voice-like
 *    channel, so it opens and closes on all of them together.
 *
 * Build:  cc -O2 -I../third_party/rnnoise/include -o denoise denoise.c \
 *            ../third_party/rnnoise/src/<the ten sources>.c -lm
 * Usage:  denoise [options]   (see --help)
 *
 * Example (48 kHz mono voice):
 *   ... | ./denoise -c 1 -n 30 | ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rnnoise.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#define MAX_CHANNELS 64
#define FRAME        480
#define HOLD_FRAMES  20
#define RNN_DELAY    2          /* frames RNNoise's output is late */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-n reduction_db] [--vad P] [-c channels] [-r 48000]\n"
        "\n"
        "  -n  how far noise is pulled down, 0..100 dB    (default 30)\n"
        "      100 is as far as RNNoise goes; 0 changes nothing\n"
        "      --vad P   gate frames less than P sure to be speech,\n"
        "                0..1, 0 = no gate                  (default 0)\n"
        "  -c  interleaved channel count, 1..%d            (default 1)\n"
        "  -r  sample rate; RNNoise is 48 kHz only          (default 48000)\n"
        "  -b  frames read per block, ignored: the stream is read a\n"
        "      10 ms frame at a time\n"
        "\n"
        "Reads raw 32-bit float samples from stdin, writes them to stdout,\n"
        "sample-aligned and as long: RNNoise's two-frame delay is absorbed.\n",
        argv0, MAX_CHANNELS);
}

int main(int argc, char **argv)
{
    double reduction = 30.0, vad_thr = 0.0, rate = 48000.0, f;
    int    channels = 1, i, ch, j, k, hold = 0, primed = 0, eof = 0, pad = 0;
    double w = 1.0;
    DenoiseState *st[MAX_CHANNELS];
    static float in[FRAME * MAX_CHANNELS];       /* interleaved, as read */
    static float prev[RNN_DELAY][MAX_CHANNELS][FRAME];  /* oldest first; the
                                                 * oldest is dry */
    static float out[FRAME * MAX_CHANNELS];
    float x[FRAME], y[FRAME];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], argv[i]);
            return 2;
        }
        if      (!strcmp(argv[i], "-n"))    reduction = atof(argv[++i]);
        else if (!strcmp(argv[i], "--vad")) vad_thr   = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c"))    channels  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r"))    rate      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-b"))    ++i;
        else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (rate != 48000.0) {
        fprintf(stderr, "%s: RNNoise runs at 48000 Hz only\n", argv[0]);
        return 2;
    }
    if (reduction < 0.0 || reduction > 100.0) {
        fprintf(stderr, "%s: reduction must be 0..100 dB\n", argv[0]);
        return 2;
    }
    if (vad_thr < 0.0 || vad_thr > 1.0) {
        fprintf(stderr, "%s: --vad must be 0..1\n", argv[0]);
        return 2;
    }
    if (channels < 1 || channels > MAX_CHANNELS) {
        fprintf(stderr, "%s: channels must be 1..%d\n", argv[0], MAX_CHANNELS);
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    f = pow(10.0, -reduction / 20.0);
    for (ch = 0; ch < channels; ch++) {
        if (!(st[ch] = rnnoise_create(NULL))) {
            fprintf(stderr, "%s: cannot create the denoiser\n", argv[0]);
            return 1;
        }
    }
    memset(prev, 0, sizeof prev);

    /* One frame per pass. At EOF the last partial frame is padded with
     * silence, and RNN_DELAY more frames of silence follow to drain RNNoise,
     * so every input sample has an output. */
    for (;;) {
        size_t got = 0, frames, want = (size_t)FRAME * channels;
        double vad = 0.0, target;
        int    nout;

        if (!eof) {
            got = fread(in, sizeof(float), want, stdin);
            if (got < want) eof = 1;
            for (i = 0; (size_t)i < got; i++)
                if (!isfinite(in[i])) in[i] = 0.0f;
            frames = got / (size_t)channels;
            pad += FRAME - (int)frames;     /* silence this pass adds */
            memset(in + frames * channels, 0, (want - frames * channels) * sizeof(float));
        } else {
            memset(in, 0, want * sizeof(float));
            pad += FRAME;
        }

        for (ch = 0; ch < channels; ch++) {
            double v;
            for (j = 0; j < FRAME; j++) x[j] = in[j * channels + ch] * 32768.0f;
            v = rnnoise_process_frame(st[ch], y, x);
            if (v > vad) vad = v;
            for (j = 0; j < FRAME; j++) out[j * channels + ch] = y[j];
        }

        if (vad_thr > 0.0) {
            if (vad >= vad_thr)  { hold = HOLD_FRAMES; target = 1.0; }
            else if (hold > 0)   { hold--;             target = 1.0; }
            else                 {                     target = 0.0; }
        } else {
            target = 1.0;
        }

        for (ch = 0; ch < channels; ch++) {
            for (j = 0; j < FRAME; j++) {
                double wj  = w + (target - w) * (double)(j + 1) / FRAME;
                double wet = (double)out[j * channels + ch] / 32768.0;
                double dry = (double)prev[0][ch][j];
                out[j * channels + ch] =
                    (float)(wj * (wet + f * (dry - wet)) + (1.0 - wj) * f * dry);
            }
        }
        w = target;
        for (k = 0; k < RNN_DELAY - 1; k++)
            memcpy(prev[k], prev[k + 1], sizeof prev[k]);
        for (ch = 0; ch < channels; ch++)
            for (j = 0; j < FRAME; j++)
                prev[RNN_DELAY - 1][ch][j] = in[j * channels + ch];

        /* The first RNN_DELAY frames out are RNNoise's delay, not audio.
         * After that, the frame written covers input that ends RNN_DELAY - 1
         * frames before what has been read, padding included, so it holds
         * (RNN_DELAY + 1) * FRAME - pad real samples once padding starts. */
        if (primed < RNN_DELAY) { primed++; continue; }
        nout = FRAME;
        if (pad > RNN_DELAY * FRAME - FRAME)
            nout = (RNN_DELAY + 1) * FRAME - pad;
        if (nout > 0 &&
            fwrite(out, sizeof(float), (size_t)nout * channels, stdout)
                != (size_t)nout * channels) {
            fprintf(stderr, "%s: write failed\n", argv[0]);
            return 1;
        }
        fflush(stdout);
        if (pad >= RNN_DELAY * FRAME) break;
    }

    for (ch = 0; ch < channels; ch++) rnnoise_destroy(st[ch]);
    fflush(stdout);
    return 0;
}
