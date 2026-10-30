/*
 * t_deess -- reference/deess.c's command line and stream handling driven
 * through src/deess_dsp.h, the code the PipeWire filter actually runs.
 *
 * deess_run() delays its output by the lookahead, as a live filter has to.
 * The reference absorbs it instead -- drops the first L outputs, pushes L
 * frames of silence at EOF -- so that is reinstated here around the same
 * code, and the two streams line up to be compared with cmp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/deess_dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

static float *inb[MAX_CHANNELS], *outb[MAX_CHANNELS];
static float  buf[MAX_BLOCK * MAX_CHANNELS];
static float  obuf[MAX_BLOCK * MAX_CHANNELS];

/* Run one chunk and write whatever survives the absorbed delay. src ==
 * NULL feeds silence, which is how the tail is drained. */
static void chunk(deess_state *s, const deess_coeffs *c, const float *src,
                  size_t frames, int channels, long long *skip)
{
    size_t n, nout = 0;
    int ch;

    for (ch = 0; ch < channels; ch++)
        for (n = 0; n < frames; n++)
            inb[ch][n] = src ? src[n * channels + ch] : 0.0f;

    deess_run(s, c, inb, outb, (uint32_t)channels, (uint32_t)frames);

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
    double rate = 48000.0, freq = 6500.0, q = 1.5, thr = -12.0, ratio = 4.0;
    double range = 12.0, attack = 1.0, release = 60.0, look = 5.0;
    int channels = 1, block = 128, detect = DEESS_DETECT_REL;
    int mode = DEESS_MODE_SPLIT, listen = 0, i, ch;
    deess_state s;
    deess_coeffs c;
    long long skip, left;
    size_t got;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-L")) { listen = 1; continue; }
        if (i + 1 >= argc) return 2;
        if      (!strcmp(a, "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(a, "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(a, "-b")) block    = atoi(argv[++i]);
        else if (!strcmp(a, "-f")) freq     = atof(argv[++i]);
        else if (!strcmp(a, "-Q")) q        = atof(argv[++i]);
        else if (!strcmp(a, "-t")) thr      = atof(argv[++i]);
        else if (!strcmp(a, "-R")) ratio    = atof(argv[++i]);
        else if (!strcmp(a, "-g")) range    = atof(argv[++i]);
        else if (!strcmp(a, "-a")) attack   = atof(argv[++i]);
        else if (!strcmp(a, "-e")) release  = atof(argv[++i]);
        else if (!strcmp(a, "-k")) look     = atof(argv[++i]);
        else if (!strcmp(a, "-d"))
            detect = !strcmp(argv[++i], "abs") ? DEESS_DETECT_ABS : DEESS_DETECT_REL;
        else if (!strcmp(a, "-m"))
            mode = !strcmp(argv[++i], "wide") ? DEESS_MODE_WIDE : DEESS_MODE_SPLIT;
        else return 2;
    }
    if (block < 1 || block > MAX_BLOCK) return 2;

    if (deess_init(&s, (uint32_t)channels, rate) < 0) return 1;
    deess_design(&c, rate, freq, q, thr, ratio, range, attack, release, look,
                 detect, mode, listen);
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

    deess_fini(&s);
    return 0;
}
