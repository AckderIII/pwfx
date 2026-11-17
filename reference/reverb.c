/*
 * reverb.c -- high-quality algorithmic reverb for raw 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the processed stream to stdout in the same format.
 *
 * Design notes
 * ------------
 *  - A 16-channel diffuser feeding a 16-line feedback delay network. Each
 *    input channel is laid onto the 16 channels along its own row of the
 *    16x16 Hadamard matrix (rows 1..15; row 0 is the feedback matrix's
 *    eigenvector), and each output channel read back along the same row, so
 *    the channels' tails are decorrelated.
 *  - Diffuser: four lossless steps, each 16 delays, a sign flip per channel
 *    and an orthonormal Hadamard mix; spans doubling to 40 ms in all at
 *    size 100, delays stratified within each span. The predelay is added to
 *    every delay of the first step.
 *  - Tank: 16 lines, geometric 40..140 ms at size 100, rounded up to primes,
 *    Householder feedback (orthogonal, lossless). The vector written into
 *    the lines is also the wet output.
 *  - Decay: a first-order shelf per line, 10^(-3 L/(fs T60)) at DC for
 *    T60 = -d and at Nyquist for T60 = -d * -H/100, half-power point at -F
 *    (Jot's absorption filters). The decay time holds whatever the size.
 *  - Modulation: each line's length swings by up to 1 ms (-M 100) at its own
 *    rate, 0.31..1.0 Hz, read through a first-order allpass so the tank's
 *    decay is untouched by it: an interpolator would lose top end on every
 *    pass. At -M 0 the allpass is an exact integer delay.
 *  - Lines hold float; mixing and filter state are double; denormals are
 *    flushed going into a line and in every filter.
 *
 * Build:  cc -O2 -o reverb reverb.c -lm
 * Usage:  reverb [-m mix%] [-d decay_s] [-H hf_decay%] [-F damp_hz]
 *                [-s size%] [-p predelay_ms] [-M modulation%] [-L lowcut_hz]
 *                [-r rate_hz] [-c channels] [-b block]
 */

#include <stdint.h>
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
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

#define MAX_CHANNELS  64
#define MAX_BLOCK     4096
#define N             16
#define STEPS         4
#define MAX_PREDELAY  250.0
#define DIFF_MS       40.0
#define TANK_MIN_MS   40.0
#define TANK_MAX_MS   140.0
#define MOD_MS        1.0
#define STORE_FLOOR   1e-30
#define DENORM_FLOOR  1e-100

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { double s1, s2; }             biquad_state_t;
typedef struct { double b0, b1, a1; }         onepole_t;
typedef struct { double x1, y1; }             onepole_state_t;

static double flush_denorm(double v)
{
    return (v > -DENORM_FLOOR && v < DENORM_FLOOR) ? 0.0 : v;
}

static float store(double v)
{
    return (v > -STORE_FLOOR && v < STORE_FLOOR) ? 0.0f : (float)v;
}

static uint32_t xrand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static double xrand01(uint32_t *s)
{
    return (double)(xrand(s) >> 8) / 16777216.0;
}

static int prime_at_least(int n)
{
    int d;
    if (n < 2) return 2;
    for (;; n++) {
        for (d = 2; d * d <= n; d++)
            if (n % d == 0) break;
        if (d * d > n) return n;
    }
}

static const int perm[N] = { 5, 12, 2, 9, 14, 0, 7, 11, 3, 15, 6, 1, 10, 4, 13, 8 };

static uint32_t pow2_above(int n)
{
    uint32_t p = 1;
    while (p < (uint32_t)n + 2) p <<= 1;
    return p;
}

/* settings */
static int       dd[STEPS][N];
static double    ds[STEPS][N];
static int       L[N];
static onepole_t shelf[N];
static double    depth, rot_c[N], rot_s[N], wet, dry;
static int       lowcut;
static biquad_t  hp;

/* state */
static float          *dbuf[STEPS][N], *tbuf[N];
static uint32_t        dmask[STEPS], tmask, pos, count;
static double          hs[N][N], ap[N], lfo_c[N], lfo_s[N];
static onepole_state_t op[N];
static biquad_state_t  hp_st[MAX_CHANNELS];

static void design(double fs, double size_pct, double predelay_ms,
                   double decay_s, double hf_pct, double damp_hz,
                   double mod_pct, double mix_pct, double lowcut_hz,
                   const int *dcap, int tcap)
{
    double size = size_pct / 100.0;
    double t_lo = decay_s, t_hi = decay_s * hf_pct / 100.0;
    double fc = damp_hz, K;
    int P = (int)(predelay_ms * fs / 1000.0 + 0.5);
    uint32_t seed = 0x9E3779B9u;
    int k, j, i;

    for (k = 0; k < STEPS; k++) {
        double span = size * DIFF_MS * (double)(1 << k) / 15.0 * fs / 1000.0;
        for (j = 0; j < N; j++) {
            double r = j == 0 ? 0.0 : xrand01(&seed);
            int d = (int)(span * ((double)j + r) / (double)N + 0.5);
            if (k == 0) d += P;
            if (d > dcap[k]) d = dcap[k];
            dd[k][j] = d;
            ds[k][j] = (xrand(&seed) & 0x100) ? -1.0 : 1.0;
        }
    }

    if (fc > fs * 0.49) fc = fs * 0.49;
    K = tan(M_PI * fc / fs);
    for (i = 0; i < N; i++) {
        double ms = size * TANK_MIN_MS
                  * pow(TANK_MAX_MS / TANK_MIN_MS, (double)i / (N - 1));
        int len = prime_at_least((int)(ms * fs / 1000.0 + 0.5));
        int line = perm[i];
        double g0, gi;
        onepole_t *s = &shelf[line];

        if (len > tcap) len = tcap;
        L[line] = len;
        g0 = pow(10.0, -3.0 * (double)len / (fs * t_lo));
        gi = pow(10.0, -3.0 * (double)len / (fs * t_hi));
        s->b0 = (gi + g0 * K) / (1.0 + K);
        s->b1 = (g0 * K - gi) / (1.0 + K);
        s->a1 = (K - 1.0) / (1.0 + K);

        rot_c[line] = cos(2.0 * M_PI * (0.31 + 0.046 * (double)i) / fs);
        rot_s[line] = sin(2.0 * M_PI * (0.31 + 0.046 * (double)i) / fs);
    }

    depth = mod_pct / 100.0 * MOD_MS * fs / 1000.0;
    wet = mix_pct / 100.0;
    dry = 1.0 - wet;
    if (lowcut_hz > 0.0) {
        double f0 = lowcut_hz < fs * 0.45 ? lowcut_hz : fs * 0.45;
        double w0 = 2.0 * M_PI * f0 / fs, cw = cos(w0);
        double alpha = sin(w0) / (2.0 * M_SQRT1_2), a0 = 1.0 + alpha;
        double g = (1.0 + cw) / 2.0;
        lowcut = 1;
        hp.b0 =  g / a0;
        hp.b1 = -2.0 * g / a0;
        hp.b2 =  g / a0;
        hp.a1 = (-2.0 * cw) / a0;
        hp.a2 = (1.0 - alpha) / a0;
    }
}

static void hadamard(double *v)
{
    int h, i, j;
    for (h = 1; h < N; h <<= 1)
        for (i = 0; i < N; i += 2 * h)
            for (j = i; j < i + h; j++) {
                double a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    for (i = 0; i < N; i++) v[i] *= 0.25;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-m mix%%] [-d decay_s] [-H hf_decay%%] [-F damp_hz]\n"
        "          [-s size%%] [-p predelay_ms] [-M modulation%%] [-L lowcut_hz]\n"
        "          [-r rate_hz] [-c channels] [-b block]\n"
        "\n"
        "  -m  wet share, 0..100 %%                 (default 25)\n"
        "  -d  decay time, RT60, s, 0.1..60        (default 2)\n"
        "  -H  RT60 above -F, %% of -d, 5..100      (default 50)\n"
        "  -F  where the decay crosses over, Hz    (default 4000)\n"
        "  -s  room size, 10..100 %%                (default 70)\n"
        "  -p  predelay, ms, 0..%g                (default 10)\n"
        "  -M  modulation depth, 0..100 %%          (default 30)\n"
        "  -L  low cut on the wet input, Hz, 0=off (default 0)\n"
        "  -r  sample rate in Hz                   (default 48000)\n"
        "  -c  interleaved channel count, 1..%d    (default 1)\n"
        "  -b  frames read per block               (default 128)\n",
        argv0, MAX_PREDELAY, MAX_CHANNELS);
}

int main(int argc, char **argv)
{
    double mix = 25, decay = 2, hf = 50, damp = 4000, size = 70,
           predelay = 10, mod = 30, lc = 0, rate = 48000;
    int channels = 1, block = 128, i, k, ch;
    int dcap[STEPS], tcap;
    size_t total = 0;
    float *mem;
    double inscale;
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
        if      (!strcmp(argv[i], "-m")) mix      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-d")) decay    = atof(argv[++i]);
        else if (!strcmp(argv[i], "-H")) hf       = atof(argv[++i]);
        else if (!strcmp(argv[i], "-F")) damp     = atof(argv[++i]);
        else if (!strcmp(argv[i], "-s")) size     = atof(argv[++i]);
        else if (!strcmp(argv[i], "-p")) predelay = atof(argv[++i]);
        else if (!strcmp(argv[i], "-M")) mod      = atof(argv[++i]);
        else if (!strcmp(argv[i], "-L")) lc       = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r")) rate     = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c")) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b")) block    = atoi(argv[++i]);
        else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (rate <= 0.0 || mix < 0 || mix > 100 || decay < 0.1 || decay > 60 ||
        hf < 5 || hf > 100 || damp <= 0 || size < 10 || size > 100 ||
        predelay < 0 || predelay > MAX_PREDELAY || mod < 0 || mod > 100 ||
        lc < 0) {
        fprintf(stderr, "%s: a setting is out of range\n", argv[0]);
        usage(argv[0]);
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

    for (k = 0; k < STEPS; k++) {
        double span = DIFF_MS * (double)(1 << k) / 15.0;
        dcap[k]  = (int)((span + (k == 0 ? MAX_PREDELAY : 0.0)) * rate / 1000.0) + 4;
        dmask[k] = pow2_above(dcap[k]) - 1;
        total += (size_t)(dmask[k] + 1) * N;
    }
    tcap  = prime_at_least((int)(TANK_MAX_MS * rate / 1000.0 + 0.5))
          + (int)(MOD_MS * rate / 1000.0) + 4;
    tmask = pow2_above(tcap) - 1;
    total += (size_t)(tmask + 1) * N;
    if (!(mem = calloc(total, sizeof *mem))) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }
    for (k = 0; k < STEPS; k++)
        for (i = 0; i < N; i++) {
            dbuf[k][i] = mem;
            mem += dmask[k] + 1;
        }
    for (i = 0; i < N; i++) {
        tbuf[i] = mem;
        mem += tmask + 1;
    }

    for (k = 0; k < N; k++)
        for (i = 0; i < N; i++)
            hs[k][i] = (__builtin_popcount((unsigned)(k & i)) & 1) ? -1.0 : 1.0;
    for (i = 0; i < N; i++) {
        double ph = 2.0 * M_PI * 0.6180339887 * (double)i;
        lfo_c[i] = cos(ph);
        lfo_s[i] = sin(ph);
    }
    inscale = 1.0 / sqrt((double)channels);
    design(rate, size, predelay, decay, hf, damp, mod, mix, lc, dcap, tcap);

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels;
        size_t tail   = got % (size_t)channels;
        size_t n;

        for (n = 0; n < got; n++)
            if (!isfinite(buf[n])) buf[n] = 0.0f;

        for (n = 0; n < frames; n++) {
            double xin[MAX_CHANNELS], u[N], y[N], w[N], sum = 0.0, fb;

            for (ch = 0; ch < channels; ch++) xin[ch] = (double)buf[n * channels + ch];

            for (i = 0; i < N; i++) u[i] = 0.0;
            for (ch = 0; ch < channels; ch++) {
                const double *row = hs[1 + ch % (N - 1)];
                double x = xin[ch], v;
                if (lowcut) {
                    biquad_state_t *st = &hp_st[ch];
                    double yy = hp.b0 * x + st->s1;
                    st->s1 = flush_denorm(hp.b1 * x - hp.a1 * yy + st->s2);
                    st->s2 = flush_denorm(hp.b2 * x - hp.a2 * yy);
                    x = yy;
                }
                v = x * inscale * 0.25;
                for (i = 0; i < N; i++) u[i] += v * row[i];
            }

            for (k = 0; k < STEPS; k++) {
                for (i = 0; i < N; i++) {
                    float *b = dbuf[k][i];
                    b[pos & dmask[k]] = store(u[i]);
                    u[i] = (double)b[(pos - (uint32_t)dd[k][i]) & dmask[k]] * ds[k][i];
                }
                hadamard(u);
            }

            for (i = 0; i < N; i++) {
                const float *b = tbuf[i];
                double t = (double)L[i] + depth * lfo_s[i], fr, eta, x0, x1, v, yy;
                int ip;
                onepole_state_t *st = &op[i];

                if (t < 2.0) t = 2.0;
                ip = (int)floor(t - 0.5);
                fr = t - (double)ip;
                eta = (1.0 - fr) / (1.0 + fr);
                x0 = (double)b[(pos - (uint32_t)ip) & tmask];
                x1 = (double)b[(pos - (uint32_t)ip - 1) & tmask];
                v = eta * x0 + x1 - eta * ap[i];
                ap[i] = flush_denorm(v);
                yy = shelf[i].b0 * v + shelf[i].b1 * st->x1 - shelf[i].a1 * st->y1;
                st->x1 = v;
                st->y1 = flush_denorm(yy);
                y[i] = yy;
                sum += y[i];
            }
            fb = sum * (2.0 / N);
            for (i = 0; i < N; i++) {
                w[i] = u[i] + (y[i] - fb);
                tbuf[i][pos & tmask] = store(w[i]);
            }

            for (ch = 0; ch < channels; ch++) {
                const double *row = hs[1 + ch % (N - 1)];
                double acc = 0.0;
                for (i = 0; i < N; i++) acc += row[i] * w[i];
                buf[n * channels + ch] = (float)(dry * xin[ch] + wet * (acc * 0.25));
            }

            for (i = 0; i < N; i++) {
                double nc = lfo_c[i] * rot_c[i] - lfo_s[i] * rot_s[i];
                double ns = lfo_s[i] * rot_c[i] + lfo_c[i] * rot_s[i];
                lfo_c[i] = nc;
                lfo_s[i] = ns;
            }
            if ((++count & 1023u) == 0)
                for (i = 0; i < N; i++) {
                    double g = 1.0 / sqrt(lfo_c[i] * lfo_c[i] + lfo_s[i] * lfo_s[i]);
                    lfo_c[i] *= g;
                    lfo_s[i] *= g;
                }
            pos++;
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
