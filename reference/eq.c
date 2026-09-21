/*
 * eq.c -- high-quality 5-band parametric EQ for raw 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the equalised stream to stdout in the same format.
 *
 * Band layout: low shelf, three peaking bells, high shelf.
 *
 * Design notes
 * ------------
 *  - Cramping is the quality problem in digital parametric EQ. The bilinear
 *    transform maps the infinite analog frequency axis onto the finite
 *    digital one, so everything gets squeezed as it approaches Nyquist: a
 *    textbook RBJ bell at 10 kHz / 48 kHz is noticeably narrower and taller
 *    on one side than the analog prototype you asked for, and by 15 kHz the
 *    shape is badly wrong. The bells here are therefore designed with
 *    Orfanidis' prescribed-Nyquist-gain method (S. J. Orfanidis, "Digital
 *    Parametric Equalizer Design with Prescribed Nyquist-Frequency Gain",
 *    JAES 45(6), 1997). The analog prototype is evaluated at the real
 *    Nyquist frequency and that value is imposed on the digital filter as a
 *    fourth design constraint, alongside DC gain, peak gain and bandwidth
 *    gain. The result tracks the analog magnitude far more closely near the
 *    top of the band, at exactly the same runtime cost -- it is still one
 *    biquad per band; only the coefficient formulas differ.
 *  - --rbj switches the bells back to the classic RBJ cookbook design, for
 *    comparison or if you specifically want that behaviour.
 *  - The shelves use the RBJ cookbook design, which is the right choice
 *    here: a shelf's asymptote *is* its Nyquist value, so the high shelf
 *    reaches exactly the gain you asked for at Nyquist by construction. Only
 *    the transition slope warps, and much less visibly than a bell's skirt.
 *  - Coefficients and state are double precision even though the stream is
 *    float. A low shelf at 100 Hz / 48 kHz has poles within 0.01 of z = 1;
 *    single-precision state accumulates audible noise and DC drift there.
 *  - Transposed direct form II per section, cascaded. Denormals are flushed
 *    out of the state so quiet passages cannot stall the CPU.
 *  - Zero latency. This is a causal IIR cascade: no lookahead, no delay.
 *  - Non-finite input is scrubbed at the front door. A recursive filter has
 *    no way back from a NaN: one bad sample would otherwise poison the
 *    feedback state and every output after it.
 *  - No output clamping. The stream stays float, so a boost that pushes past
 *    full scale is preserved rather than clipped -- put a limiter after this.
 *
 * Build:  cc -O2 -o eq eq.c -lm
 * Usage:  eq [options]   (see --help)
 *
 * Example (live voice: cut rumble, scoop mud, lift presence and air):
 *   ... | ./eq -r 48000 -c 2 --ls 90:-4:0.7 --b2 300:-3:1.0 \
 *              --b3 2500:3:0.9 --hs 9000:2.5:0.7
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
#define MAX_BLOCK    4096
#define NBANDS       5
#define DENORM_FLOOR 1e-100

/* Lower bound on the RBJ shelf discriminant. The shelf's alpha term is
 * sqrt(t)*sin(w0)/2, and alpha is what pulls the poles inside the unit
 * circle: at t = 0 the denominator's a2 equals a0 exactly, the poles land ON
 * the circle, and the section becomes an undamped oscillator that rings
 * forever. t goes negative when the slope S is pushed past the limit set by
 * the band's gain, so both a hard parameter check (shelf_smax below) and this
 * floor are in place -- the check is the real fix, this is the backstop. */
#define SHELF_T_MIN  0.05

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { double s1, s2; }             bqstate_t;

typedef struct {
    double f;       /* centre / corner frequency, Hz */
    double g;       /* gain, dB                      */
    double q;       /* Q for bells, slope S for shelves */
    int    active;  /* 0 => identity, skipped entirely  */
} band_t;

static double flush_denorm(double v)
{
    return (v > -DENORM_FLOOR && v < DENORM_FLOOR) ? 0.0 : v;
}

/* Largest shelf slope this gain can carry while keeping the poles strictly
 * inside the unit circle. The limit tightens as the gain grows: at +/-12 dB
 * it is about 4.6, at +/-24 dB about 2.3. S = 1 is the steepest monotonic
 * shelf and is always safe. */
static double shelf_smax(double gdb)
{
    double A = pow(10.0, fabs(gdb) / 40.0);
    double s = A + 1.0 / A;
    double d = 1.0 - (2.0 - SHELF_T_MIN) / s;
    return (d <= 1e-12) ? 1e9 : 1.0 / d;
}

static void biquad_identity(biquad_t *c)
{
    c->b0 = 1.0; c->b1 = 0.0; c->b2 = 0.0; c->a1 = 0.0; c->a2 = 0.0;
}

/* ---- RBJ cookbook shelves ---------------------------------------------
 * S is the shelf slope: S = 1 is the steepest monotonic shelf, smaller is
 * gentler, and larger adds resonance at the corner. How far above 1 you can
 * go depends on the gain -- see shelf_smax(); slopes past that limit are
 * rejected at startup rather than silently turned into an oscillator.
 */
static void design_lowshelf(biquad_t *c, double f0, double fs, double gdb, double S)
{
    double A  = pow(10.0, gdb / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0), sw = sin(w0);
    double t  = (A + 1.0 / A) * (1.0 / S - 1.0) + 2.0;
    double alpha, tsa, a0;

    if (t < SHELF_T_MIN) t = SHELF_T_MIN;
    alpha = sw / 2.0 * sqrt(t);
    tsa   = 2.0 * sqrt(A) * alpha;

    a0    =      (A + 1.0) + (A - 1.0) * cw + tsa;
    c->b0 =  A * ((A + 1.0) - (A - 1.0) * cw + tsa) / a0;
    c->b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw) / a0;
    c->b2 =  A * ((A + 1.0) - (A - 1.0) * cw - tsa) / a0;
    c->a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw)     / a0;
    c->a2 =      ((A + 1.0) + (A - 1.0) * cw - tsa) / a0;
}

static void design_highshelf(biquad_t *c, double f0, double fs, double gdb, double S)
{
    double A  = pow(10.0, gdb / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0), sw = sin(w0);
    double t  = (A + 1.0 / A) * (1.0 / S - 1.0) + 2.0;
    double alpha, tsa, a0;

    if (t < SHELF_T_MIN) t = SHELF_T_MIN;
    alpha = sw / 2.0 * sqrt(t);
    tsa   = 2.0 * sqrt(A) * alpha;

    a0    =      (A + 1.0) - (A - 1.0) * cw + tsa;
    c->b0 =  A * ((A + 1.0) + (A - 1.0) * cw + tsa) / a0;
    c->b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0;
    c->b2 =  A * ((A + 1.0) + (A - 1.0) * cw - tsa) / a0;
    c->a1 =  2.0 * ((A - 1.0) - (A + 1.0) * cw)     / a0;
    c->a2 =      ((A + 1.0) - (A - 1.0) * cw - tsa) / a0;
}

/* ---- RBJ cookbook peaking bell (the classic, cramped near Nyquist) ---- */
static void design_peak_rbj(biquad_t *c, double f0, double fs, double gdb, double Q)
{
    double A  = pow(10.0, gdb / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double alpha = sin(w0) / (2.0 * Q);
    double a0 = 1.0 + alpha / A;

    c->b0 = (1.0 + alpha * A) / a0;
    c->b1 = (-2.0 * cos(w0))  / a0;
    c->b2 = (1.0 - alpha * A) / a0;
    c->a1 = (-2.0 * cos(w0))  / a0;
    c->a2 = (1.0 - alpha / A) / a0;
}

/* ---- Orfanidis peaking bell with prescribed Nyquist gain ---------------
 * Four constraints are imposed: gain G0 at DC, gain G at w0, gain GB at the
 * band edges w0 +/- Dw/2, and gain G1 at Nyquist, where G1 is taken from the
 * analog prototype evaluated at the true Nyquist frequency. Matching that
 * last value is what stops the response collapsing as w0 climbs.
 *
 * Falls back to the RBJ design if the constraint set degenerates (which
 * happens when the analog Nyquist gain coincides with the peak gain, i.e.
 * when the centre frequency is pushed up against Nyquist).
 */
static void design_peak_orfanidis(biquad_t *c, double f0, double fs,
                                  double gdb, double Q)
{
    double G0 = 1.0;
    double G  = pow(10.0, gdb / 20.0);
    double GB = sqrt(G0 * G);                 /* midpoint in dB */
    double fny = fs / 2.0;
    double df  = f0 / Q;                      /* analog bandwidth, Hz */
    double w0  = 2.0 * M_PI * f0 / fs;
    double Dw  = 2.0 * M_PI * df / fs;

    double s1, s2, G1, F, G00, F00, G01, G11, F01, F11;
    double W2, DW, Cc, Dd, Aa, Bb, den;

    /* analog prototype magnitude at the true Nyquist frequency */
    s1 = (f0 * f0 - fny * fny);
    s2 = (df * fny);
    G1 = sqrt((G0 * G0 * s1 * s1 + G * G * s2 * s2) / (s1 * s1 + s2 * s2));

    F   = fabs(G * G   - GB * GB);
    G00 = fabs(G * G   - G0 * G0);
    F00 = fabs(GB * GB - G0 * G0);
    G01 = fabs(G * G   - G0 * G1);
    G11 = fabs(G * G   - G1 * G1);
    F01 = fabs(GB * GB - G0 * G1);
    F11 = fabs(GB * GB - G1 * G1);

    if (F < 1e-18 || G00 < 1e-18 || F11 < 1e-18 || G11 < 1e-18) {
        design_peak_rbj(c, f0, fs, gdb, Q);
        return;
    }

    W2 = sqrt(G11 / G00) * tan(w0 / 2.0) * tan(w0 / 2.0);
    DW = (1.0 + sqrt(F00 / F11) * W2) * tan(Dw / 2.0);

    Cc = F11 * DW * DW - 2.0 * W2 * (F01 - sqrt(F00 * F11));
    Dd = 2.0 * W2 * (G01 - sqrt(G00 * G11));

    if ((Cc + Dd) / F < 0.0 || (G * G * Cc + GB * GB * Dd) / F < 0.0) {
        design_peak_rbj(c, f0, fs, gdb, Q);
        return;
    }
    Aa = sqrt((Cc + Dd) / F);
    Bb = sqrt((G * G * Cc + GB * GB * Dd) / F);

    den = 1.0 + W2 + Aa;
    if (fabs(den) < 1e-18) {
        design_peak_rbj(c, f0, fs, gdb, Q);
        return;
    }

    c->b0 = (G1 + G0 * W2 + Bb)  / den;
    c->b1 = -2.0 * (G1 - G0 * W2) / den;
    c->b2 = (G1 - Bb + G0 * W2)  / den;
    c->a1 = -2.0 * (1.0 - W2)    / den;
    c->a2 = (1.0 + W2 - Aa)      / den;
}

/* Magnitude of one section at frequency f, for the --verbose report. */
static double biquad_mag_db(const biquad_t *c, double f, double fs)
{
    double w = 2.0 * M_PI * f / fs;
    double cw = cos(w), c2w = cos(2.0 * w);
    double sw = sin(w), s2w = sin(2.0 * w);
    double nr = c->b0 + c->b1 * cw + c->b2 * c2w;
    double ni =       -c->b1 * sw - c->b2 * s2w;
    double dr = 1.0   + c->a1 * cw + c->a2 * c2w;
    double di =       -c->a1 * sw - c->a2 * s2w;
    double n  = nr * nr + ni * ni, d = dr * dr + di * di;
    if (d <= 0.0) return -999.0;
    return 10.0 * log10(n / d);
}

static int opt_is(const char *a, const char *s, const char *l)
{
    return (s && !strcmp(a, s)) || (l && !strcmp(a, l));
}

/* Parse "freq:gain:q". Gain and q may be omitted: "1000", "1000:3". */
static int parse_band(const char *s, band_t *b)
{
    double f = 0.0, g = 0.0, q = 0.0;
    int n = sscanf(s, "%lf:%lf:%lf", &f, &g, &q);
    if (n < 1 || f <= 0.0) return 0;
    b->f = f;
    b->g = (n >= 2) ? g : 0.0;
    if (n >= 3) {
        if (q <= 0.0) return 0;
        b->q = q;
    }
    b->active = 1;
    return 1;
}

static void usage(const char *p)
{
    fprintf(stderr,
"usage: %s [options]   --  32-bit float 5-band parametric EQ, stdin -> stdout\n"
"\n"
"stream\n"
"  -r, --rate HZ       sample rate                          (48000)\n"
"  -c, --channels N    interleaved channels, 1..%d          (1)\n"
"  -b, --block N       frames read per block                (128)\n"
"                      smaller = lower live latency,\n"
"                      larger  = faster offline\n"
"\n"
"bands            each takes FREQ[:GAIN_DB[:Q]]\n"
"  --ls F[:G[:S]]    band 1, low shelf     (default  100:0:0.7)\n"
"  --b2 F[:G[:Q]]    band 2, peaking bell  (default  300:0:1.0)\n"
"  --b3 F[:G[:Q]]    band 3, peaking bell  (default 1000:0:1.0)\n"
"  --b4 F[:G[:Q]]    band 4, peaking bell  (default 3000:0:1.0)\n"
"  --hs F[:G[:S]]    band 5, high shelf    (default 8000:0:0.7)\n"
"\n"
"                    For the shelves the third field is the slope S:\n"
"                    S = 1 is the steepest monotonic shelf, lower is\n"
"                    gentler. For the bells it is Q: higher is narrower,\n"
"                    and the bandwidth is FREQ/Q.\n"
"                    A band with 0 dB gain is skipped entirely, so an\n"
"                    all-flat EQ is a bit-exact passthrough.\n"
"\n"
"design\n"
"      --rbj         use the classic RBJ cookbook bells instead of the\n"
"                    default Orfanidis prescribed-Nyquist-gain design.\n"
"                    RBJ bells cramp near Nyquist: at 10 kHz / 48 kHz the\n"
"                    shape already departs from the analog prototype, and\n"
"                    by 15 kHz it is badly off. Only affects the bells.\n"
"\n"
"  -v, --verbose     print coefficients and the resulting response to stderr\n"
"  -h, --help        this text\n"
"\n"
"Zero latency: this is a causal IIR cascade, nothing is delayed. Nothing is\n"
"clipped either -- the stream stays float, so put a limiter after a boost.\n"
"\n"
"Live voice starting point:\n"
"  --ls 90:-4:0.7 --b2 300:-3:1.0 --b3 2500:3:0.9 --hs 9000:2.5:0.7\n",
        p, MAX_CHANNELS);
}

int main(int argc, char **argv)
{
    double rate = 48000.0;
    int    channels = 1, block = 128, verbose = 0, use_rbj = 0;
    int    i, ch, k, nact = 0;

    band_t band[NBANDS] = {
        { 100.0, 0.0, 0.7, 0 },   /* low shelf  */
        { 300.0, 0.0, 1.0, 0 },   /* bell       */
        { 1000.0,0.0, 1.0, 0 },   /* bell       */
        { 3000.0,0.0, 1.0, 0 },   /* bell       */
        { 8000.0,0.0, 0.7, 0 },   /* high shelf */
    };
    static const char *bname[NBANDS] =
        { "low shelf", "bell 2", "bell 3", "bell 4", "high shelf" };

    biquad_t  sec[NBANDS];
    int       secmap[NBANDS];
    bqstate_t st[MAX_CHANNELS][NBANDS];

    static float buf[MAX_BLOCK * MAX_CHANNELS];
    size_t got;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (opt_is(a, "-h", "--help"))  { usage(argv[0]); return 0; }
        if (opt_is(a, "-v", "--verbose")) { verbose = 1; continue; }
        if (opt_is(a, NULL, "--rbj"))     { use_rbj = 1; continue; }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], a);
            return 2;
        }
        if      (opt_is(a, "-r", "--rate"))     rate     = atof(argv[++i]);
        else if (opt_is(a, "-c", "--channels")) channels = atoi(argv[++i]);
        else if (opt_is(a, "-b", "--block"))    block    = atoi(argv[++i]);
        else if (opt_is(a, NULL, "--ls") || opt_is(a, NULL, "--b1")) {
            if (!parse_band(argv[++i], &band[0])) goto badband;
        } else if (opt_is(a, NULL, "--b2")) {
            if (!parse_band(argv[++i], &band[1])) goto badband;
        } else if (opt_is(a, NULL, "--b3")) {
            if (!parse_band(argv[++i], &band[2])) goto badband;
        } else if (opt_is(a, NULL, "--b4")) {
            if (!parse_band(argv[++i], &band[3])) goto badband;
        } else if (opt_is(a, NULL, "--hs") || opt_is(a, NULL, "--b5")) {
            if (!parse_band(argv[++i], &band[4])) goto badband;
        } else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], a);
            usage(argv[0]);
            return 2;
        }
        continue;
badband:
        fprintf(stderr, "%s: bad band spec '%s', expected FREQ[:GAIN[:Q]] "
                        "with FREQ > 0 and Q > 0\n", argv[0], argv[i]);
        return 2;
    }

    if (rate <= 0.0) {
        fprintf(stderr, "%s: sample rate must be positive\n", argv[0]);
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
    for (k = 0; k < NBANDS; k++) {
        if (!band[k].active || band[k].g == 0.0) continue;
        if (k == 0 || k == NBANDS - 1) {
            double smax = shelf_smax(band[k].g);
            if (band[k].q > smax) {
                fprintf(stderr,
                    "%s: %s slope S=%g is too steep for a %+g dB shelf "
                    "(max %.2f here).\n"
                    "%s: Past that limit the poles reach the unit circle and "
                    "the section rings forever.\n"
                    "%s: S=1 is the steepest monotonic shelf and is always "
                    "safe.\n",
                    argv[0], bname[k], band[k].q, band[k].g, smax,
                    argv[0], argv[0]);
                return 2;
            }
        }
    }
    for (k = 0; k < NBANDS; k++) {
        if (!band[k].active) continue;
        if (band[k].f >= rate / 2.0) {
            fprintf(stderr, "%s: %s frequency %g Hz must be below Nyquist "
                            "(%g Hz)\n", argv[0], bname[k], band[k].f,
                    rate / 2.0);
            return 2;
        }
    }

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /* Build the cascade, skipping bands that would be the identity. */
    for (k = 0; k < NBANDS; k++) {
        biquad_t c;
        if (!band[k].active || band[k].g == 0.0) continue;
        if (k == 0)
            design_lowshelf(&c, band[k].f, rate, band[k].g, band[k].q);
        else if (k == NBANDS - 1)
            design_highshelf(&c, band[k].f, rate, band[k].g, band[k].q);
        else if (use_rbj)
            design_peak_rbj(&c, band[k].f, rate, band[k].g, band[k].q);
        else
            design_peak_orfanidis(&c, band[k].f, rate, band[k].g, band[k].q);
        sec[nact]    = c;
        secmap[nact] = k;
        nact++;
    }
    for (k = nact; k < NBANDS; k++) biquad_identity(&sec[k]);
    memset(st, 0, sizeof st);

    if (verbose) {
        static const double probe[] = { 20, 50, 100, 200, 500, 1000, 2000,
                                        5000, 8000, 10000, 15000, 20000 };
        int p;
        fprintf(stderr, "eq: %g Hz, %d ch, %d active band(s), bells: %s\n",
                rate, channels, nact, use_rbj ? "RBJ" : "Orfanidis");
        for (k = 0; k < nact; k++)
            fprintf(stderr,
                "eq:   %-10s f=%-8g g=%+-6g q=%-5g  b=[% .9f % .9f % .9f] "
                "a=[1 % .9f % .9f]\n",
                bname[secmap[k]], band[secmap[k]].f, band[secmap[k]].g,
                band[secmap[k]].q, sec[k].b0, sec[k].b1, sec[k].b2,
                sec[k].a1, sec[k].a2);
        fprintf(stderr, "eq:   response:");
        for (p = 0; p < (int)(sizeof probe / sizeof *probe); p++) {
            double f = probe[p], sum = 0.0;
            if (f >= rate / 2.0) break;
            for (k = 0; k < nact; k++) sum += biquad_mag_db(&sec[k], f, rate);
            fprintf(stderr, "  %g:%+.2f", f, sum);
        }
        fprintf(stderr, " dB\n");
    }

    while ((got = fread(buf, sizeof(float), (size_t)block * channels, stdin)) > 0) {
        size_t frames = got / (size_t)channels;
        size_t tail   = got % (size_t)channels;
        size_t n;

        /* Scrub non-finite input: a recursive filter has no way back from a
         * NaN or Inf, so one bad sample would poison the state permanently. */
        for (n = 0; n < got; n++)
            if (!isfinite(buf[n])) buf[n] = 0.0f;

        for (n = 0; n < frames; n++) {
            for (ch = 0; ch < channels; ch++) {
                double x = (double)buf[n * channels + ch];
                for (k = 0; k < nact; k++) {
                    const biquad_t *c  = &sec[k];
                    bqstate_t      *s  = &st[ch][k];
                    double y = c->b0 * x + s->s1;
                    s->s1 = flush_denorm(c->b1 * x - c->a1 * y + s->s2);
                    s->s2 = flush_denorm(c->b2 * x - c->a2 * y);
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
        /* Flush every block, or the last partial stdio buffer sits here
         * instead of reaching the next stage -- latency, not corruption. */
        fflush(stdout);

        if (tail) {
            fwrite(&buf[frames * channels], sizeof(float), tail, stdout);
            fflush(stdout);
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
