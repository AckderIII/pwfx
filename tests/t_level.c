/*
 * t_level -- reference/level.c's command line and stream handling driven
 * through src/level_dsp.h, the code the PipeWire filter actually runs.
 *
 * level_run() delays its output by the lookahead, as a live filter has to.
 * The reference absorbs it instead -- drops the first L outputs, pushes L
 * frames of silence at EOF -- so that is reinstated here around the same
 * code, and the two streams line up to be compared with cmp.
 *
 * -v prints the gain the core is applying after every block to stderr, one
 * "gain DB" line per block, for tests/level.py to follow it over time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/level_dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

static float *inb[MAX_CHANNELS], *outb[MAX_CHANNELS];
static float  buf[MAX_BLOCK * MAX_CHANNELS];
static float  obuf[MAX_BLOCK * MAX_CHANNELS];
static int    verbose;

/* Run one chunk and write whatever survives the absorbed delay. src ==
 * NULL feeds silence, which is how the tail is drained. */
static void chunk(level_state *s, const level_coeffs *c, const float *src,
                  size_t frames, int channels, long long *skip)
{
    size_t n, nout = 0;
    int ch;

    for (ch = 0; ch < channels; ch++)
        for (n = 0; n < frames; n++)
            inb[ch][n] = src ? src[n * channels + ch] : 0.0f;

    level_run(s, c, inb, outb, (uint32_t)channels, (uint32_t)frames);
    if (verbose)
        fprintf(stderr, "gain %.9f\n", level_gain_db(s));

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
    double rate = 48000.0, loud = -18.0, strength = 100.0, boost = 12.0;
    double cut = 12.0, gate = -50.0, window = 400.0, attack = 1000.0;
    double release = 3000.0, look = 0.0;
    int channels = 1, block = 128, i, ch;
    level_state s;
    level_coeffs c;
    long long skip, left;
    size_t got;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v")) { verbose = 1; continue; }
        if (i + 1 >= argc) return 2;
        if      (!strcmp(a, "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(a, "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(a, "-b")) block    = atoi(argv[++i]);
        else if (!strcmp(a, "-l")) loud     = atof(argv[++i]);
        else if (!strcmp(a, "-s")) strength = atof(argv[++i]);
        else if (!strcmp(a, "-B")) boost    = atof(argv[++i]);
        else if (!strcmp(a, "-C")) cut      = atof(argv[++i]);
        else if (!strcmp(a, "-g")) gate     = atof(argv[++i]);
        else if (!strcmp(a, "-w")) window   = atof(argv[++i]);
        else if (!strcmp(a, "-a")) attack   = atof(argv[++i]);
        else if (!strcmp(a, "-e")) release  = atof(argv[++i]);
        else if (!strcmp(a, "-k")) look     = atof(argv[++i]);
        else return 2;
    }
    if (block < 1 || block > MAX_BLOCK) return 2;
    if (channels < 1 || channels > MAX_CHANNELS) return 2;

    if (level_init(&s, (uint32_t)channels, rate) < 0) return 1;
    level_design(&c, rate, loud, strength, boost, cut, gate, window, attack,
                 release, look);
    skip = c.L;

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
        if (got % (size_t)channels) break;     /* torn frame: malformed input */
    }

    for (left = c.L; left > 0; ) {
        size_t f = (size_t)(left > MAX_BLOCK ? MAX_BLOCK : left);
        chunk(&s, &c, NULL, f, channels, &skip);
        left -= (long long)f;
    }

    level_fini(&s);
    return 0;
}
