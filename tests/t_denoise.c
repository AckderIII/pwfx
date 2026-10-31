/*
 * t_denoise -- reference/denoise.c's command line and stream handling
 * driven through src/denoise_dsp.h, the code the PipeWire filter runs.
 *
 * The reference reads the stream a frame at a time, so its only delay is
 * RNNoise's own, which it absorbs. The core takes blocks of any size and
 * plays out from the frame before, so it is DN_LATENCY late -- a frame of
 * framing on top of RNNoise's frame. Absorbing that here (dropping the
 * first DN_LATENCY outputs, pushing that much silence at EOF) lines the
 * two streams up, and if the framing is right at every block size they
 * agree to the bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/denoise_dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

static float *inb[MAX_CHANNELS], *outb[MAX_CHANNELS];
static float  buf[MAX_BLOCK * MAX_CHANNELS];
static float  obuf[MAX_BLOCK * MAX_CHANNELS];

static void chunk(denoise_state *s, const denoise_coeffs *c, const float *src,
                  size_t frames, int channels, long long *skip)
{
    size_t n, nout = 0;
    int ch;

    for (ch = 0; ch < channels; ch++)
        for (n = 0; n < frames; n++)
            inb[ch][n] = src ? src[n * channels + ch] : 0.0f;

    denoise_run(s, c, inb, outb, (uint32_t)channels, (uint32_t)frames);

    for (n = 0; n < frames; n++) {
        if (*skip > 0) { (*skip)--; continue; }
        for (ch = 0; ch < channels; ch++)
            obuf[nout * channels + ch] = outb[ch][n];
        nout++;
    }
    if (nout) {
        fwrite(obuf, sizeof(float), nout * (size_t)channels, stdout);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    double rate = 48000.0, reduction = 30.0, vad = 0.0;
    int channels = 1, block = 128, i, ch;
    denoise_state s;
    denoise_coeffs c;
    long long skip, left;
    size_t got;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (i + 1 >= argc) return 2;
        if      (!strcmp(a, "-r"))    rate      = atof(argv[++i]);
        else if (!strcmp(a, "-c"))    channels  = atoi(argv[++i]);
        else if (!strcmp(a, "-b"))    block     = atoi(argv[++i]);
        else if (!strcmp(a, "-n"))    reduction = atof(argv[++i]);
        else if (!strcmp(a, "--vad")) vad       = atof(argv[++i]);
        else return 2;
    }
    if (block < 1 || block > MAX_BLOCK) return 2;

    if (denoise_init(&s, (uint32_t)channels) < 0) return 1;
    denoise_design(&c, rate, reduction, vad);
    skip = c.pass ? 0 : DN_LATENCY;

    for (ch = 0; ch < channels; ch++) {
        inb[ch]  = calloc(MAX_BLOCK, sizeof **inb);
        outb[ch] = calloc(MAX_BLOCK, sizeof **outb);
        if (!inb[ch] || !outb[ch]) return 1;
    }

    while ((got = fread(buf, sizeof(float), (size_t)block * channels,
                        stdin)) > 0) {
        size_t frames = got / (size_t)channels;
        if (frames == 0) break;
        chunk(&s, &c, buf, frames, channels, &skip);
        if (got % (size_t)channels) break;     /* torn frame: malformed input */
    }

    for (left = c.pass ? 0 : DN_LATENCY; left > 0; ) {
        size_t f = (size_t)(left > MAX_BLOCK ? MAX_BLOCK : left);
        chunk(&s, &c, NULL, f, channels, &skip);
        left -= (long long)f;
    }

    denoise_fini(&s);
    return 0;
}
