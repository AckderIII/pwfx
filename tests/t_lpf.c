/*
 * t_lpf -- reference/lpf.c's command line and stream handling driven through
 * src/dsp.h, the code the PipeWire filter actually runs. Diffing this against
 * bin/ref/lpf proves the DSP survived the port unchanged, down to the bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

int main(int argc, char **argv)
{
    double cutoff = 12000.0, rate = 48000.0;
    int channels = 1, order = 4, block = 128, i, ch;
    butter_t d;
    static bqstate_t bq[MAX_CHANNELS][DSP_MAX_SECTIONS];
    static opstate_t op[MAX_CHANNELS];
    static float buf[MAX_BLOCK * MAX_CHANNELS];
    size_t got;

    for (i = 1; i < argc; i++) {
        if (i + 1 >= argc) return 2;
        if      (!strcmp(argv[i], "-f")) cutoff   = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n")) order    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b")) block    = atoi(argv[++i]);
        else return 2;
    }

    butter_design(&d, order, cutoff, rate, 0);
    memset(bq, 0, sizeof bq);
    memset(op, 0, sizeof op);

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels, n;
        dsp_scrub(buf, (unsigned)got);
        for (n = 0; n < frames; n++)
            for (ch = 0; ch < channels; ch++)
                buf[n * channels + ch] = (float)butter_run(&d, bq[ch], &op[ch],
                        (double)buf[n * channels + ch]);
        fwrite(buf, sizeof(float), frames * channels, stdout);
        fflush(stdout);
        if (got % (size_t)channels) break;
    }
    return 0;
}
