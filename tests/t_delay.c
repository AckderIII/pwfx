/*
 * t_delay -- reference/delay.c's command line and stream handling driven
 * through src/delay_dsp.h, the code the PipeWire filter actually runs.
 *
 * Unlike the lookahead filters there is nothing to reinstate: the delay is
 * the output, in the reference as in the filter, so the two streams line up
 * as they come. The core's ring is sized for the longest delay rather than
 * this one, and it reads in blocks the graph chooses -- which is exactly
 * what the block sizes in cases.sh are there to catch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/delay_dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

static float *inb[MAX_CHANNELS], *outb[MAX_CHANNELS];
static float  buf[MAX_BLOCK * MAX_CHANNELS];

int main(int argc, char **argv)
{
    double rate = 48000.0, time_ms = 0.0, feedback = 0.0, mix = 100.0;
    int channels = 1, block = 128, i, ch;
    delay_state s;
    delay_coeffs c;
    size_t got, n;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (i + 1 >= argc) return 2;
        if      (!strcmp(a, "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(a, "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(a, "-b")) block    = atoi(argv[++i]);
        else if (!strcmp(a, "-t")) time_ms  = atof(argv[++i]);
        else if (!strcmp(a, "-f")) feedback = atof(argv[++i]);
        else if (!strcmp(a, "-m")) mix      = atof(argv[++i]);
        else return 2;
    }
    if (block < 1 || block > MAX_BLOCK) return 2;

    if (delay_init(&s, (uint32_t)channels, rate) < 0) return 1;
    delay_design(&c, rate, time_ms, feedback, mix);

    for (ch = 0; ch < channels; ch++) {
        inb[ch]  = calloc(MAX_BLOCK, sizeof **inb);
        outb[ch] = calloc(MAX_BLOCK, sizeof **outb);
        if (!inb[ch] || !outb[ch]) return 1;
    }

    while ((got = fread(buf, sizeof(float), (size_t)block * channels,
                        stdin)) > 0) {
        size_t frames = got / (size_t)channels;
        if (frames == 0) break;
        for (ch = 0; ch < channels; ch++)
            for (n = 0; n < frames; n++)
                inb[ch][n] = buf[n * channels + ch];
        delay_run(&s, &c, inb, outb, (uint32_t)channels, (uint32_t)frames);
        for (n = 0; n < frames; n++)
            for (ch = 0; ch < channels; ch++)
                buf[n * channels + ch] = outb[ch][n];
        fwrite(buf, sizeof(float), frames * (size_t)channels, stdout);
        fflush(stdout);
        if (got % (size_t)channels) break;     /* torn frame: malformed input */
    }

    delay_fini(&s);
    return 0;
}
