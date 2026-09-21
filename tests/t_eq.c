/*
 * t_eq -- reference/eq.c's command line and stream handling driven through
 * src/dsp.h, the code the PipeWire filter actually runs. Diffing this against
 * bin/ref/eq proves the DSP survived the port unchanged, down to the bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096
#define NBANDS       5

typedef struct { double f, g, q; int active; } band_t;

static int parse_band(const char *s, band_t *b)
{
    double f = 0.0, g = 0.0, q = 0.0;
    int n = sscanf(s, "%lf:%lf:%lf", &f, &g, &q);
    if (n < 1 || f <= 0.0) return 0;
    b->f = f;
    b->g = (n >= 2) ? g : 0.0;
    if (n >= 3) { if (q <= 0.0) return 0; b->q = q; }
    b->active = 1;
    return 1;
}

int main(int argc, char **argv)
{
    double rate = 48000.0;
    int channels = 1, block = 128, use_rbj = 0, i, ch, k;
    band_t band[NBANDS] = {
        { 100.0, 0.0, 0.7, 0 }, { 300.0, 0.0, 1.0, 0 }, { 1000.0, 0.0, 1.0, 0 },
        { 3000.0, 0.0, 1.0, 0 }, { 8000.0, 0.0, 0.7, 0 },
    };
    biquad_t sec[NBANDS];
    int on[NBANDS];
    static bqstate_t st[MAX_CHANNELS][NBANDS];
    static float buf[MAX_BLOCK * MAX_CHANNELS];
    size_t got;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--rbj")) { use_rbj = 1; continue; }
        if (i + 1 >= argc) return 2;
        if      (!strcmp(a, "-r"))   rate     = atof(argv[++i]);
        else if (!strcmp(a, "-c"))   channels = atoi(argv[++i]);
        else if (!strcmp(a, "-b"))   block    = atoi(argv[++i]);
        else if (!strcmp(a, "--ls")) { if (!parse_band(argv[++i], &band[0])) return 2; }
        else if (!strcmp(a, "--b2")) { if (!parse_band(argv[++i], &band[1])) return 2; }
        else if (!strcmp(a, "--b3")) { if (!parse_band(argv[++i], &band[2])) return 2; }
        else if (!strcmp(a, "--b4")) { if (!parse_band(argv[++i], &band[3])) return 2; }
        else if (!strcmp(a, "--hs")) { if (!parse_band(argv[++i], &band[4])) return 2; }
        else return 2;
    }

    for (k = 0; k < NBANDS; k++) {
        if (!band[k].active || band[k].g == 0.0 || band[k].f >= rate / 2.0) {
            on[k] = 0;
            biquad_identity(&sec[k]);
            continue;
        }
        on[k] = 1;
        if (k == 0)
            biquad_lowshelf(&sec[k], band[k].f, rate, band[k].g, band[k].q);
        else if (k == NBANDS - 1)
            biquad_highshelf(&sec[k], band[k].f, rate, band[k].g, band[k].q);
        else if (use_rbj)
            biquad_peak_rbj(&sec[k], band[k].f, rate, band[k].g, band[k].q);
        else
            biquad_peak_orfanidis(&sec[k], band[k].f, rate, band[k].g, band[k].q);
    }
    memset(st, 0, sizeof st);

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels, n;
        dsp_scrub(buf, (unsigned)got);
        for (n = 0; n < frames; n++) {
            for (ch = 0; ch < channels; ch++) {
                double x = (double)buf[n * channels + ch];
                for (k = 0; k < NBANDS; k++)
                    if (on[k]) x = biquad_run(&sec[k], &st[ch][k], x);
                buf[n * channels + ch] = (float)x;
            }
        }
        fwrite(buf, sizeof(float), frames * channels, stdout);
        fflush(stdout);
        if (got % (size_t)channels) break;
    }
    return 0;
}
