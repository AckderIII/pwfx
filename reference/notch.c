/*
 * notch.c -- high-quality notch filter for raw 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the filtered stream to stdout in the same format.
 *
 * Design notes
 * ------------
 *  - Each notch is a Butterworth band-stop: the order-N low-pass prototype
 *    through s -> B s/(s^2 + W0^2), 2N poles, built as N second-order
 *    sections. Order 1 is the classic notch (s^2 + W0^2)/(s^2 + B s + W0^2);
 *    higher orders keep the -3 dB width and flatten the shoulders.
 *  - The geometry is solved for in the digital domain. Under the bilinear
 *    transform W = tan(pi f/fs), and a band-stop's edges satisfy
 *    W1 W2 = W0^2, so edges exactly bw Hz apart around a null exactly at f0
 *    need tan(a) tan(a+d) = K0^2 (a = pi f1/fs, d = pi bw/fs,
 *    K0 = tan(pi f0/fs)): a quadratic in tan(a), solved in closed form. The
 *    RBJ notch gets the centre right and approximates the width, and the
 *    approximation drifts as the centre climbs.
 *  - Every section's zeros sit at +/- j W0, which the bilinear transform
 *    puts on the unit circle at f0: the null is infinitely deep by
 *    construction.
 *  - Each prototype pole p gives two band-stop poles, the roots of
 *    p s^2 - B s + p W0^2 = 0. Their product is W0^2, so the larger is
 *    computed and the smaller divided out of W0^2, avoiding the
 *    cancellation the quadratic formula suffers on a wide band.
 *  - Harmonics: -H n notches f0, 2 f0, ... n f0, each with the same Q. A
 *    harmonic at or above 0.49 fs is left out.
 *  - Coefficients and state are double precision, sections in transposed
 *    direct form II, denormals flushed. A notch at 50 Hz / 48 kHz with
 *    Q 30 has its poles within 1e-4 of the unit circle.
 *
 * Build:  cc -O2 -o notch notch.c -lm
 * Usage:  notch [-f freq_hz] [-Q q] [-n order] [-H harmonics]
 *               [-r rate_hz] [-c channels] [-b block]
 *
 * Example (mains hum, 50 Hz and four harmonics, 48 kHz stereo):
 *   ffmpeg -i in.wav -f f32le -ac 2 -ar 48000 - \
 *     | ./notch -f 50 -Q 20 -n 2 -H 5 -r 48000 -c 2 \
 *     | ffmpeg -f f32le -ac 2 -ar 48000 -i - out.wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_CHANNELS  64
#define MAX_ORDER     8
#define MAX_HARMONICS 16
#define MAX_SECTIONS  (MAX_ORDER * MAX_HARMONICS)
#define MAX_BLOCK     4096
#define MAX_FREQ      0.49     /* of the rate: the highest centre */
#define MAX_WIDTH     0.49     /* of the rate: the widest band */

#define DENORM_FLOOR 1e-100

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { double s1, s2; }             biquad_state_t;

typedef struct {
    int      nsections;
    biquad_t sec[MAX_SECTIONS];
} design_t;

static double flush_denorm(double v)
{
    return (v > -DENORM_FLOOR && v < DENORM_FLOOR) ? 0.0 : v;
}

/* H(s) = (s^2 + w2)/(s^2 + a1 s + a0), bilinear, s = (1-z^-1)/(1+z^-1). */
static void design_section(biquad_t *c, double w2, double a1, double a0)
{
    double d = 1.0 + a1 + a0;

    c->b0 = (1.0 + w2) / d;
    c->b1 = 2.0 * (w2 - 1.0) / d;
    c->b2 = (1.0 + w2) / d;
    c->a1 = 2.0 * (a0 - 1.0) / d;
    c->a2 = (1.0 - a1 + a0) / d;
}

/* One band-stop of the given order around f0, -3 dB edges bw Hz apart. */
static void design_band(biquad_t *sec, int order, double f0, double bw,
                        double fs)
{
    double K0 = tan(M_PI * f0 / fs);
    double w2 = K0 * K0;
    double u  = tan(M_PI * bw / fs);
    double v  = u * (1.0 + w2);
    double K1 = (sqrt(v * v + 4.0 * w2) - v) / 2.0;   /* tan(pi f1/fs) */
    double K2 = w2 / K1;                              /* tan(pi f2/fs) */
    double B  = K2 - K1;
    int m, k = 0;

    for (m = 0; m < order / 2; m++) {
        double phi = M_PI * (2.0 * m + order + 1.0) / (2.0 * order);
        double complex p  = cos(phi) + I * sin(phi);
        double complex sq = csqrt(B * B - 4.0 * p * p * w2);
        double complex r1 = cabs(B + sq) >= cabs(B - sq) ? B + sq : B - sq;
        double complex s1 = r1 / (2.0 * p);
        double complex s2 = w2 / s1;

        design_section(&sec[k++], w2, -2.0 * creal(s1), creal(s1 * conj(s1)));
        design_section(&sec[k++], w2, -2.0 * creal(s2), creal(s2 * conj(s2)));
    }
    if (order & 1)
        design_section(&sec[k++], w2, B, w2);
}

static void design_notch(design_t *d, int order, int harmonics, double f0,
                         double q, double fs)
{
    int h;

    d->nsections = 0;
    for (h = 1; h <= harmonics; h++) {
        double fc = f0 * h, bw = fc / q;

        if (fc >= fs * MAX_FREQ) break;
        if (bw > fs * MAX_WIDTH) bw = fs * MAX_WIDTH;
        design_band(&d->sec[d->nsections], order, fc, bw, fs);
        d->nsections += order;
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-f freq_hz] [-Q q] [-n order] [-H harmonics]\n"
        "          [-r rate_hz] [-c channels] [-b block]\n"
        "\n"
        "  -f  frequency to remove, Hz             (default 50)\n"
        "  -Q  freq / -3 dB width                  (default 10)\n"
        "  -n  band-stop order, 1..%d               (default 2)\n"
        "      1 is the classic notch\n"
        "  -H  notch freq and its harmonics, 1..%d (default 1)\n"
        "  -r  sample rate in Hz                   (default 48000)\n"
        "  -c  interleaved channel count, 1..%d    (default 1)\n"
        "  -b  frames read per block               (default 128)\n"
        "\n"
        "Reads raw 32-bit float samples from stdin, writes them to stdout.\n",
        argv0, MAX_ORDER, MAX_HARMONICS, MAX_CHANNELS);
}

int main(int argc, char **argv)
{
    double freq      = 50.0;
    double q         = 10.0;
    double rate      = 48000.0;
    int    order     = 2;
    int    harmonics = 1;
    int    channels  = 1;
    int    block     = 128;
    int    i, ch, k;

    static design_t       design;
    static biquad_state_t state[MAX_CHANNELS][MAX_SECTIONS];
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
        if      (!strcmp(argv[i], "-f")) freq      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-Q")) q         = atof(argv[++i]);
        else if (!strcmp(argv[i], "-n")) order     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-H")) harmonics = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r")) rate      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c")) channels  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b")) block     = atoi(argv[++i]);
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
    if (freq <= 0.0 || freq >= rate * MAX_FREQ) {
        fprintf(stderr, "%s: freq must be between 0 and %g Hz\n",
                argv[0], rate * MAX_FREQ);
        return 2;
    }
    if (q <= 0.0 || freq / q > rate * MAX_WIDTH) {
        fprintf(stderr, "%s: Q must leave the width under %g Hz\n",
                argv[0], rate * MAX_WIDTH);
        return 2;
    }
    if (order < 1 || order > MAX_ORDER) {
        fprintf(stderr, "%s: order must be 1..%d\n", argv[0], MAX_ORDER);
        return 2;
    }
    if (harmonics < 1 || harmonics > MAX_HARMONICS) {
        fprintf(stderr, "%s: harmonics must be 1..%d\n", argv[0], MAX_HARMONICS);
        return 2;
    }
    if (channels < 1 || channels > MAX_CHANNELS) {
        fprintf(stderr, "%s: channels must be 1..%d\n", argv[0], MAX_CHANNELS);
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

    design_notch(&design, order, harmonics, freq, q, rate);
    memset(state, 0, sizeof state);

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels;
        size_t tail   = got % (size_t)channels;
        size_t n;

        /* One NaN in a recursive filter's state is NaN forever. */
        for (n = 0; n < got; n++)
            if (!isfinite(buf[n])) buf[n] = 0.0f;

        for (n = 0; n < frames; n++) {
            for (ch = 0; ch < channels; ch++) {
                double x = (double)buf[n * channels + ch];

                for (k = 0; k < design.nsections; k++) {
                    const biquad_t *c  = &design.sec[k];
                    biquad_state_t *st = &state[ch][k];
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
        fflush(stdout);

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
