/*
 * t_notch -- reference/notch.c's command line and stream handling driven
 * through src/notch_dsp.h, the code the PipeWire filter actually runs.
 * Diffing this against bin/ref/notch proves the DSP survived the port
 * unchanged, down to the bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/notch_dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

int main(int argc, char **argv)
{
    double freq = 50.0, q = 10.0, rate = 48000.0;
    int order = 2, harmonics = 1, channels = 1, block = 128, i, ch;
    static notch_t d;
    static bqstate_t bq[MAX_CHANNELS][NOTCH_MAX_SECTIONS];
    static float buf[MAX_BLOCK * MAX_CHANNELS];
    size_t got;

    for (i = 1; i < argc; i++) {
        if (i + 1 >= argc) return 2;
        if      (!strcmp(argv[i], "-f")) freq      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-Q")) q         = atof(argv[++i]);
        else if (!strcmp(argv[i], "-n")) order     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-H")) harmonics = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r")) rate      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c")) channels  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b")) block     = atoi(argv[++i]);
        else return 2;
    }

    notch_design(&d, order, harmonics, freq, q, rate);
    memset(bq, 0, sizeof bq);

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels, n;
        dsp_scrub(buf, (unsigned)got);
        for (n = 0; n < frames; n++)
            for (ch = 0; ch < channels; ch++)
                buf[n * channels + ch] = (float)notch_run(&d, bq[ch],
                        (double)buf[n * channels + ch]);
        fwrite(buf, sizeof(float), frames * channels, stdout);
        fflush(stdout);
        if (got % (size_t)channels) break;
    }
    return 0;
}
