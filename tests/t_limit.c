/*
 * t_limit -- reference/limit.c's command line and stream handling driven
 * through src/limit_dsp.h, the code the PipeWire filter actually runs.
 *
 * limit_run() delays its output by DT samples, which is what a live filter
 * has to do. The reference absorbs that delay instead, so reinstating the
 * drop-DT-then-flush-DT here is what makes the two streams comparable byte
 * for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/limit_dsp.h"

#define MAX_BLOCK 4096

static float *inb[LIMIT_MAX_CHANNELS], *outb[LIMIT_MAX_CHANNELS];
static float  buf[MAX_BLOCK * LIMIT_MAX_CHANNELS];
static float  obuf[MAX_BLOCK * LIMIT_MAX_CHANNELS];

static void chunk(limit_state *s, const limit_coeffs *c, const float *src,
                  size_t frames, int channels, long long *skip)
{
    size_t n, nout = 0;
    int ch;

    for (ch = 0; ch < channels; ch++)
        for (n = 0; n < frames; n++)
            inb[ch][n] = src ? src[n * channels + ch] : 0.0f;

    limit_run(s, c, inb, outb, (uint32_t)channels, (uint32_t)frames);

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
    double rate = 48000.0, ceil_db = -1.0, in_db = 0.0;
    double release = 60.0, look = 5.0;
    int channels = 1, block = 128, os = 8, verbose = 0, i, ch;
    limit_state s;
    limit_coeffs c;
    long long skip, left;
    size_t got;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v")) { verbose = 1; continue; }
        if (i + 1 >= argc) return 2;
        if      (!strcmp(a, "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(a, "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(a, "-b")) block    = atoi(argv[++i]);
        else if (!strcmp(a, "-C")) ceil_db  = atof(argv[++i]);
        else if (!strcmp(a, "-i")) in_db    = atof(argv[++i]);
        else if (!strcmp(a, "-e")) release  = atof(argv[++i]);
        else if (!strcmp(a, "-k")) look     = atof(argv[++i]);
        else if (!strcmp(a, "-O")) os       = atoi(argv[++i]);
        else return 2;
    }
    if (block < 1 || block > MAX_BLOCK) return 2;
    if (os < 1 || os > LIMIT_MAX_OS || (os & (os - 1))) return 2;

    if (limit_init(&s, (uint32_t)channels, rate) < 0) return 1;
    limit_design(&c, rate, ceil_db, in_db, release, look, os);
    skip = c.DT;

    for (ch = 0; ch < channels; ch++) {
        inb[ch]  = calloc(MAX_BLOCK, sizeof **inb);
        outb[ch] = calloc(MAX_BLOCK, sizeof **outb);
        if (!inb[ch] || !outb[ch]) return 1;
    }

    while ((got = fread(buf, sizeof(float), (size_t)block * channels,
                        stdin)) > 0) {
        size_t frames = got / (size_t)channels;
        dsp_scrub(buf, (unsigned)got);
        if (frames == 0) break;
        chunk(&s, &c, buf, frames, channels, &skip);
        if (got % (size_t)channels) break;
    }

    for (left = c.DT; left > 0; ) {
        size_t f = (size_t)(left > MAX_BLOCK ? MAX_BLOCK : left);
        chunk(&s, &c, NULL, f, channels, &skip);
        left -= (long long)f;
    }

    if (verbose)
        fprintf(stderr, "t_limit: peak gr %.2f dB, breaches %lld, "
                "largest excess %.2e\n", s.peak_gr, s.clamps, s.max_excess);
    limit_fini(&s);
    return 0;
}
