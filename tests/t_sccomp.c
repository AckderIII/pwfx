/*
 * t_sccomp -- t_comp with a key: reference/comp.c's command line and stream
 * handling driven through src/comp_dsp.h, the code pw_sccomp runs, with the
 * sidechain as one more interleaved channel. -c is the audio's channel
 * count; each input frame is those channels and then the key, and each
 * output frame is the audio channels alone. There is no -l: a key means one
 * detector.
 *
 * There is no reference/sccomp.c. tests/sccomp.py compares this against
 * reference/comp.c itself, on streams built so that the original's own
 * detector hears exactly the key -- see there.
 *
 * comp_run() delays its output by L samples, which is what a live filter has
 * to do. The reference absorbs that delay instead: it drops the first L
 * output samples and pushes L frames of silence at EOF, so its output is
 * sample-aligned with its input and exactly as long. Reinstating that here
 * is what makes the two streams comparable byte for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/comp_dsp.h"

#define MAX_BLOCK 4096

static float *inb[COMP_MAX_CHANNELS], *outb[COMP_MAX_CHANNELS];
static float  buf[MAX_BLOCK * (COMP_MAX_CHANNELS + 1)];
static float  key[MAX_BLOCK];
static float  obuf[MAX_BLOCK * COMP_MAX_CHANNELS];

static void chunk(comp_state *s, const comp_coeffs *c, const float *src,
                  size_t frames, int channels, long long *skip)
{
    size_t n, nout = 0, w = (size_t)channels + 1;
    int ch;

    for (ch = 0; ch < channels; ch++)
        for (n = 0; n < frames; n++)
            inb[ch][n] = src ? src[n * w + ch] : 0.0f;
    for (n = 0; n < frames; n++)
        key[n] = src ? src[n * w + channels] : 0.0f;

    comp_run(s, c, inb, key, outb, (uint32_t)channels, (uint32_t)frames);

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
    double rate = 48000.0, thr = -18.0, ratio = 3.0, knee = 6.0;
    double attack = 10.0, release = 120.0, look = 5.0, rms = 0.0;
    double makeup = 0.0, hpf = 0.0, lpf = 0.0;
    int channels = 1, block = 128, automk = 0, i, ch;
    comp_state s;
    comp_coeffs c;
    long long skip, left;
    size_t got;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-M")) { automk = 1; continue; }
        if (i + 1 >= argc) return 2;
        if      (!strcmp(a, "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(a, "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(a, "-b")) block    = atoi(argv[++i]);
        else if (!strcmp(a, "-t")) thr      = atof(argv[++i]);
        else if (!strcmp(a, "-R")) ratio    = atof(argv[++i]);
        else if (!strcmp(a, "-w")) knee     = atof(argv[++i]);
        else if (!strcmp(a, "-a")) attack   = atof(argv[++i]);
        else if (!strcmp(a, "-e")) release  = atof(argv[++i]);
        else if (!strcmp(a, "-k")) look     = atof(argv[++i]);
        else if (!strcmp(a, "-q")) rms      = atof(argv[++i]);
        else if (!strcmp(a, "-m")) makeup   = atof(argv[++i]);
        else if (!strcmp(a, "-F")) hpf      = atof(argv[++i]);
        else if (!strcmp(a, "-G")) lpf      = atof(argv[++i]);
        else return 2;
    }
    if (block < 1 || block > MAX_BLOCK) return 2;
    if (channels < 1 || channels > COMP_MAX_CHANNELS) return 2;

    if (comp_init(&s, (uint32_t)channels, rate) < 0) return 1;
    comp_design(&c, rate, (uint32_t)channels, thr, ratio, knee, attack,
                release, look, rms, makeup, automk, COMP_LINK_MAX, hpf, lpf);
    skip = c.L;

    for (ch = 0; ch < channels; ch++) {
        inb[ch]  = calloc(MAX_BLOCK, sizeof **inb);
        outb[ch] = calloc(MAX_BLOCK, sizeof **outb);
        if (!inb[ch] || !outb[ch]) return 1;
    }

    while ((got = fread(buf, sizeof(float), (size_t)block * (channels + 1),
                        stdin)) > 0) {
        size_t frames = got / (size_t)(channels + 1);
        dsp_scrub(buf, (unsigned)got);
        if (frames == 0) break;
        chunk(&s, &c, buf, frames, channels, &skip);
        if (got % (size_t)(channels + 1)) break;
    }

    for (left = c.L; left > 0; ) {
        size_t f = (size_t)(left > MAX_BLOCK ? MAX_BLOCK : left);
        chunk(&s, &c, NULL, f, channels, &skip);
        left -= (long long)f;
    }

    comp_fini(&s);
    return 0;
}
