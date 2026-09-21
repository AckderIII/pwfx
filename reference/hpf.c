/*
 * hpf.c -- high-quality Butterworth high-pass filter for raw 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the filtered stream to stdout in the same format.
 *
 * Design notes
 * ------------
 *  - Order-N Butterworth implemented as a cascade of second-order sections
 *    (plus one first-order section when N is odd). Each section is designed
 *    analog-prototype -> bilinear transform with frequency prewarping, so the
 *    -3 dB point lands exactly on the requested cutoff.
 *  - Coefficients and state are double precision even though the stream is
 *    float. This matters: at fc/fs = 80/48000 the poles sit within 0.01 of
 *    z = 1, and single-precision state accumulates audible noise and DC drift.
 *  - Sections run in transposed direct form II, which has the best rounding
 *    behaviour of the direct forms for fixed cascades.
 *  - No output clamping. The stream stays float, so transient overshoot near
 *    the cutoff (inherent to any minimum-phase HPF) is preserved rather than
 *    clipped. Put a limiter after this if you need a hard ceiling.
 *
 * Build:  cc -O2 -o hpf hpf.c -lm
 * Usage:  hpf [-f cutoff_hz] [-r rate_hz] [-c channels] [-n order]
 *
 * Example (OBS/ffmpeg pipeline, 48 kHz stereo, 80 Hz rumble cut, 4th order):
 *   ffmpeg -i in.wav -f f32le -ac 2 -ar 48000 - \
 *     | ./hpf -f 80 -r 48000 -c 2 -n 4 \
 *     | ffmpeg -f f32le -ac 2 -ar 48000 -i - out.wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_CHANNELS 64
#define MAX_ORDER    16
#define MAX_SECTIONS (MAX_ORDER / 2)
#define MAX_BLOCK    4096

/* Anything below this is inaudible at any sane gain, and zeroing it keeps the
 * filter state out of the denormal range where some CPUs stall badly. */
#define DENORM_FLOOR 1e-100

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { double s1, s2; }             biquad_state_t;
typedef struct { double b0, b1, a1; }         onepole_t;
typedef struct { double x1, y1; }             onepole_state_t;

typedef struct {
    int       order;
    int       nsections;   /* number of biquads */
    int       has_first;   /* 1 if a first-order section is present */
    biquad_t  sec[MAX_SECTIONS];
    onepole_t first;
} design_t;

static double flush_denorm(double v)
{
    return (v > -DENORM_FLOOR && v < DENORM_FLOOR) ? 0.0 : v;
}

/* Second-order high-pass section, RBJ-style bilinear design.
 * q selects the pole pair's damping; the Butterworth cascade supplies one q
 * per section. */
static void design_biquad_hp(biquad_t *s, double fc, double fs, double q)
{
    double w0    = 2.0 * M_PI * fc / fs;
    double cosw  = cos(w0);
    double alpha = sin(w0) / (2.0 * q);
    double a0    = 1.0 + alpha;
    double g     = (1.0 + cosw) / 2.0;

    s->b0 =  g  / a0;
    s->b1 = -2.0 * g / a0;
    s->b2 =  g  / a0;
    s->a1 = (-2.0 * cosw)  / a0;
    s->a2 = ( 1.0 - alpha) / a0;
}

/* First-order high-pass: H(s) = s/(s+1), bilinear with prewarped cutoff. */
static void design_onepole_hp(onepole_t *s, double fc, double fs)
{
    double k = tan(M_PI * fc / fs);
    double n = 1.0 / (1.0 + k);

    s->b0 =  n;
    s->b1 = -n;
    s->a1 = (k - 1.0) * n;
}

/* The order-N Butterworth poles sit evenly spaced on the unit circle at
 * angles phi_m = pi*(2m+N+1)/(2N). Each conjugate pair becomes one biquad
 * with Q = -1/(2*cos(phi_m)); for odd N the pole at phi = pi is real and
 * becomes the first-order section. Taking m = 0 .. N/2-1 picks exactly one
 * pole per pair and skips the real pole, for both even and odd N. */
static void design_butterworth_hp(design_t *d, int order, double fc, double fs)
{
    int m;

    d->order     = order;
    d->nsections = order / 2;
    d->has_first = order & 1;

    for (m = 0; m < d->nsections; m++) {
        double phi = M_PI * (2.0 * m + order + 1.0) / (2.0 * order);
        double q   = -1.0 / (2.0 * cos(phi));
        design_biquad_hp(&d->sec[m], fc, fs, q);
    }

    if (d->has_first)
        design_onepole_hp(&d->first, fc, fs);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-f cutoff_hz] [-r rate_hz] [-c channels] [-n order]\n"
        "\n"
        "  -f  cutoff frequency in Hz, -3 dB point   (default 80)\n"
        "  -r  sample rate in Hz                     (default 48000)\n"
        "  -c  interleaved channel count, 1..%d      (default 1)\n"
        "  -n  filter order, 1..%d                   (default 4)\n"
        "  -b  frames read per block                 (default 128)\n"
        "      smaller = lower live latency, larger = faster offline\n"
        "\n"
        "Reads raw 32-bit float samples from stdin, writes them to stdout.\n"
        "Rolloff is 6 dB/octave per order: -n 2 gives 12 dB/oct, -n 4 gives\n"
        "24 dB/oct. For voice rumble removal, -n 4 at 75-100 Hz is a good\n"
        "starting point.\n",
        argv0, MAX_CHANNELS, MAX_ORDER);
}

int main(int argc, char **argv)
{
    double cutoff   = 80.0;
    double rate     = 48000.0;
    int    channels = 1;
    int    order    = 4;
    int    block    = 128;
    int    i, ch, k;

    design_t       design;
    biquad_state_t bq_state[MAX_CHANNELS][MAX_SECTIONS];
    onepole_state_t op_state[MAX_CHANNELS];

    static float buf[MAX_BLOCK * MAX_CHANNELS];
    size_t got;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], argv[i]);
            return 2;
        }
        if      (!strcmp(argv[i], "-f")) cutoff   = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n")) order    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b")) block    = atoi(argv[++i]);
        else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (rate <= 0.0) {
        fprintf(stderr, "%s: sample rate must be positive\n", argv[0]);
        return 2;
    }
    if (cutoff <= 0.0 || cutoff >= rate / 2.0) {
        fprintf(stderr, "%s: cutoff must be between 0 and %g Hz\n",
                argv[0], rate / 2.0);
        return 2;
    }
    if (channels < 1 || channels > MAX_CHANNELS) {
        fprintf(stderr, "%s: channels must be 1..%d\n", argv[0], MAX_CHANNELS);
        return 2;
    }
    if (order < 1 || order > MAX_ORDER) {
        fprintf(stderr, "%s: order must be 1..%d\n", argv[0], MAX_ORDER);
        return 2;
    }
    if (block < 1 || block > MAX_BLOCK) {
        fprintf(stderr, "%s: block must be 1..%d frames\n", argv[0], MAX_BLOCK);
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    design_butterworth_hp(&design, order, cutoff, rate);
    memset(bq_state, 0, sizeof bq_state);
    memset(op_state, 0, sizeof op_state);

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels;
        size_t tail   = got % (size_t)channels;   /* torn frame at EOF */
        size_t n;

        /* Scrub non-finite input. A recursive filter has no way back from a
         * NaN or an Inf: one bad sample from a glitching capture device or an
         * upstream plugin enters the feedback state and every subsequent
         * output is NaN for the rest of the stream. One pass here keeps the
         * section state finite by construction. */
        for (n = 0; n < got; n++)
            if (!isfinite(buf[n])) buf[n] = 0.0f;

        for (n = 0; n < frames; n++) {
            for (ch = 0; ch < channels; ch++) {
                double x = (double)buf[n * channels + ch];

                if (design.has_first) {
                    onepole_state_t *st = &op_state[ch];
                    double y = design.first.b0 * x
                             + design.first.b1 * st->x1
                             - design.first.a1 * st->y1;
                    st->x1 = x;
                    st->y1 = flush_denorm(y);
                    x = y;
                }

                for (k = 0; k < design.nsections; k++) {
                    const biquad_t *c  = &design.sec[k];
                    biquad_state_t *st = &bq_state[ch][k];
                    double y = c->b0 * x + st->s1;

                    st->s1 = flush_denorm(c->b1 * x - c->a1 * y + st->s2);
                    st->s2 = flush_denorm(c->b2 * x - c->a2 * y);
                    x = y;
                }

                buf[n * channels + ch] = (float)x;
            }
        }

        if (fwrite(buf, sizeof(float), frames * channels, stdout)
                != frames * channels) {
            fprintf(stderr, "%s: write failed\n", argv[0]);
            return 1;
        }
        /* Flush every block. Without this the last partial stdio buffer sits
         * here instead of reaching the next stage, which shows up as latency
         * in a live pipeline rather than as a correctness bug. */
        fflush(stdout);

        /* A partial frame can only happen at EOF on a malformed stream; pass
         * the stray samples through untouched rather than dropping them. */
        if (tail) {
            fwrite(&buf[frames * channels], sizeof(float), tail, stdout);
            break;
        }
    }

    if (ferror(stdin)) {
        fprintf(stderr, "%s: read failed\n", argv[0]);
        return 1;
    }

    fflush(stdout);
    return 0;
}
