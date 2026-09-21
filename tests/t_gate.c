/*
 * t_gate -- reference/gate.c's command line and stream handling driven
 * through src/gate_dsp.h, the code the PipeWire filter actually runs.
 *
 * gate_run() delays its output by L samples, which is what a live filter has
 * to do. The reference absorbs that delay instead: it drops the first L
 * output samples and pushes L frames of silence at EOF, so its output ends
 * up sample-aligned with its input and exactly as long. Reinstating that
 * here is what makes the two streams comparable -- and if the cores agree,
 * they agree to the bit, so cmp is the whole test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gate_dsp.h"

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

static float *inb[MAX_CHANNELS], *outb[MAX_CHANNELS];
static float  buf[MAX_BLOCK * MAX_CHANNELS];
static float  obuf[MAX_BLOCK * MAX_CHANNELS];

/* Run one chunk through the gate and write whatever survives the absorbed
 * delay. src == NULL feeds silence, which is how the tail is drained. */
static void chunk(gate_state *s, const gate_coeffs *c, const float *src,
                  size_t frames, int channels, long long *skip)
{
    size_t n, nout = 0;
    int ch;

    for (ch = 0; ch < channels; ch++)
        for (n = 0; n < frames; n++)
            inb[ch][n] = src ? src[n * channels + ch] : 0.0f;

    gate_run(s, c, inb, outb, (uint32_t)channels, (uint32_t)frames);

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
    double rate = 48000.0, thresh = -35.0, close_db = 1.0;
    double attack = 2.0, hold = 150.0, release = 150.0;
    double look = 5.0, range = 120.0, hpf = 120.0, lpf = 0.0;
    int channels = 1, block = 128, i, ch;
    gate_state s;
    gate_coeffs c;
    long long skip, left;
    size_t got;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (i + 1 >= argc) return 2;
        if      (!strcmp(a, "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(a, "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(a, "-b")) block    = atoi(argv[++i]);
        else if (!strcmp(a, "-t")) thresh   = atof(argv[++i]);
        else if (!strcmp(a, "-T")) close_db = atof(argv[++i]);
        else if (!strcmp(a, "-a")) attack   = atof(argv[++i]);
        else if (!strcmp(a, "-o")) hold     = atof(argv[++i]);
        else if (!strcmp(a, "-e")) release  = atof(argv[++i]);
        else if (!strcmp(a, "-k")) look     = atof(argv[++i]);
        else if (!strcmp(a, "-g")) range    = atof(argv[++i]);
        else if (!strcmp(a, "-F")) hpf      = atof(argv[++i]);
        else if (!strcmp(a, "-G")) lpf      = atof(argv[++i]);
        else return 2;
    }
    if (close_db > 0.0) close_db = thresh - 6.0;   /* the reference default */
    if (block < 1 || block > MAX_BLOCK) return 2;

    if (gate_init(&s, (uint32_t)channels, rate) < 0) return 1;
    gate_design(&c, rate, thresh, close_db, attack, hold, release, look,
                range, hpf, lpf);
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

    gate_fini(&s);
    return 0;
}
