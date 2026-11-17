/*
 * t_reverb -- reference/reverb.c's command line and stream handling driven
 * through src/reverb_dsp.h, the code the PipeWire filter actually runs.
 * Diffing this against bin/ref/reverb proves the DSP survived the port
 * unchanged, down to the bit.
 *
 * One addition the reference has no use for: -X FRAME redesigns at that
 * frame with -Y size and -Z predelay (each defaulting to its first value),
 * the way a live change does, so tests/reverb.py can check the glide.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/reverb_dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

int main(int argc, char **argv)
{
    double mix = 25, decay = 2, hf = 50, damp = 4000, size = 70,
           predelay = 10, mod = 30, lc = 0, rate = 48000;
    double size2 = -1, predelay2 = -1;
    long   at = -1, done = 0;
    int channels = 1, block = 128, i, ch;
    static reverb_state st;
    static reverb_coeffs c;
    static float buf[MAX_BLOCK * MAX_CHANNELS];
    static float ib[MAX_CHANNELS][MAX_BLOCK], ob[MAX_CHANNELS][MAX_BLOCK];
    float *in[MAX_CHANNELS], *out[MAX_CHANNELS];
    size_t got;

    for (i = 1; i < argc; i++) {
        if (i + 1 >= argc) return 2;
        if      (!strcmp(argv[i], "-m")) mix       = atof(argv[++i]);
        else if (!strcmp(argv[i], "-d")) decay     = atof(argv[++i]);
        else if (!strcmp(argv[i], "-H")) hf        = atof(argv[++i]);
        else if (!strcmp(argv[i], "-F")) damp      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-s")) size      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-p")) predelay  = atof(argv[++i]);
        else if (!strcmp(argv[i], "-M")) mod       = atof(argv[++i]);
        else if (!strcmp(argv[i], "-L")) lc        = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r")) rate      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c")) channels  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b")) block     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-X")) at        = atol(argv[++i]);
        else if (!strcmp(argv[i], "-Y")) size2     = atof(argv[++i]);
        else if (!strcmp(argv[i], "-Z")) predelay2 = atof(argv[++i]);
        else return 2;
    }
    if (size2 < 0) size2 = size;
    if (predelay2 < 0) predelay2 = predelay;

    if (reverb_init(&st, (uint32_t)channels, rate) < 0) return 1;
    reverb_design(&c, rate, size, predelay, decay, hf, damp, mod, mix, lc,
                  st.dcap, st.tcap);
    for (ch = 0; ch < channels; ch++) { in[ch] = ib[ch]; out[ch] = ob[ch]; }

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels, n;
        dsp_scrub(buf, (unsigned)got);
        for (n = 0; n < frames; n++)
            for (ch = 0; ch < channels; ch++) ib[ch][n] = buf[n * channels + ch];
        if (at >= 0 && done <= at && at < done + (long)frames) {
            /* split the block where the change lands, as the graph would
             * deliver it at a block boundary near there */
            uint32_t head = (uint32_t)(at - done);
            float *in2[MAX_CHANNELS], *out2[MAX_CHANNELS];
            reverb_run(&st, &c, in, out, (uint32_t)channels, head);
            reverb_design(&c, rate, size2, predelay2, decay, hf, damp, mod, mix,
                          lc, st.dcap, st.tcap);
            for (ch = 0; ch < channels; ch++) {
                in2[ch] = ib[ch] + head;
                out2[ch] = ob[ch] + head;
            }
            reverb_run(&st, &c, in2, out2, (uint32_t)channels,
                       (uint32_t)frames - head);
        } else {
            reverb_run(&st, &c, in, out, (uint32_t)channels, (uint32_t)frames);
        }
        done += (long)frames;
        for (n = 0; n < frames; n++)
            for (ch = 0; ch < channels; ch++) buf[n * channels + ch] = ob[ch][n];
        fwrite(buf, sizeof(float), frames * channels, stdout);
        /* a torn frame at EOF passes through, as in the reference */
        if (got % (size_t)channels) {
            fwrite(&buf[frames * channels], sizeof(float),
                   got % (size_t)channels, stdout);
            break;
        }
        fflush(stdout);
    }
    reverb_fini(&st);
    return 0;
}
