/*
 * dsp.h -- the DSP primitives shared by the pw_* filters.
 *
 * Everything here is taken from the reference implementations in reference/
 * and is unchanged in substance: double-precision coefficients and state,
 * transposed direct form II, denormal flushing. The only difference is that
 * the coefficient structures are plain data with no pointers, so a designed
 * configuration can be published to the realtime thread by a single store.
 */

#ifndef PWF_DSP_H
#define PWF_DSP_H

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DSP_MAX_ORDER    16
#define DSP_MAX_SECTIONS (DSP_MAX_ORDER / 2)

/* Anything below this is inaudible at any sane gain, and zeroing it keeps
 * the filter state out of the denormal range where some CPUs stall badly. */
#define DSP_DENORM_FLOOR 1e-100

#define DSP_LOG10_20 (20.0 / M_LN10)
#define DSP_LN10_20  (M_LN10 / 20.0)

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { double s1, s2; }             bqstate_t;
typedef struct { double b0, b1, a1; }         onepole_t;
typedef struct { double x1, y1; }             opstate_t;

static inline double dsp_flush(double v)
{
    return (v > -DSP_DENORM_FLOOR && v < DSP_DENORM_FLOOR) ? 0.0 : v;
}

static inline double dsp_db2lin(double db) { return exp(db * DSP_LN10_20); }

static inline double dsp_lin2db(double lin, double floor_db)
{
    double d;
    if (lin <= 0.0) return floor_db;
    d = DSP_LOG10_20 * log(lin);
    return d < floor_db ? floor_db : d;
}

/* Transposed direct form II: the best rounding behaviour of the direct
 * forms for a fixed cascade. */
static inline double biquad_run(const biquad_t *c, bqstate_t *st, double x)
{
    double y = c->b0 * x + st->s1;
    st->s1 = dsp_flush(c->b1 * x - c->a1 * y + st->s2);
    st->s2 = dsp_flush(c->b2 * x - c->a2 * y);
    return y;
}

static inline double onepole_run(const onepole_t *c, opstate_t *st, double x)
{
    double y = c->b0 * x + c->b1 * st->x1 - c->a1 * st->y1;
    st->x1 = x;
    st->y1 = dsp_flush(y);
    return y;
}

static inline void biquad_identity(biquad_t *c)
{
    c->b0 = 1.0; c->b1 = 0.0; c->b2 = 0.0; c->a1 = 0.0; c->a2 = 0.0;
}

/* ---- RBJ-style bilinear second-order sections -------------------------- */

static inline void biquad_lp(biquad_t *s, double fc, double fs, double q)
{
    double w0    = 2.0 * M_PI * fc / fs;
    double cosw  = cos(w0);
    double alpha = sin(w0) / (2.0 * q);
    double a0    = 1.0 + alpha;
    double g     = (1.0 - cosw) / 2.0;

    s->b0 =  g       / a0;
    s->b1 =  2.0 * g / a0;
    s->b2 =  g       / a0;
    s->a1 = (-2.0 * cosw)  / a0;
    s->a2 = ( 1.0 - alpha) / a0;
}

static inline void biquad_hp(biquad_t *s, double fc, double fs, double q)
{
    double w0    = 2.0 * M_PI * fc / fs;
    double cosw  = cos(w0);
    double alpha = sin(w0) / (2.0 * q);
    double a0    = 1.0 + alpha;
    double g     = (1.0 + cosw) / 2.0;

    s->b0 =  g       / a0;
    s->b1 = -2.0 * g / a0;
    s->b2 =  g       / a0;
    s->a1 = (-2.0 * cosw)  / a0;
    s->a2 = ( 1.0 - alpha) / a0;
}

/* First-order sections: H(s) = 1/(s+1) and s/(s+1), bilinear with the
 * cutoff prewarped so the -3 dB point lands where it was asked for. */
static inline void onepole_lp(onepole_t *s, double fc, double fs)
{
    double k = tan(M_PI * fc / fs);
    double n = 1.0 / (1.0 + k);

    s->b0 = k * n;
    s->b1 = k * n;
    s->a1 = (k - 1.0) * n;
}

static inline void onepole_hp(onepole_t *s, double fc, double fs)
{
    double k = tan(M_PI * fc / fs);
    double n = 1.0 / (1.0 + k);

    s->b0 =  n;
    s->b1 = -n;
    s->a1 = (k - 1.0) * n;
}

/* ---- Butterworth cascade ------------------------------------------------
 * The order-N Butterworth poles sit evenly spaced on the unit circle at
 * angles phi_m = pi*(2m+N+1)/(2N). Each conjugate pair becomes one biquad
 * with Q = -1/(2*cos(phi_m)); for odd N the pole at phi = pi is real and
 * becomes the first-order section. Taking m = 0 .. N/2-1 picks exactly one
 * pole per pair and skips the real pole, for both even and odd N.
 */
typedef struct {
    int       order;
    int       nsections;
    int       has_first;
    biquad_t  sec[DSP_MAX_SECTIONS];
    onepole_t first;
} butter_t;

static inline void butter_design(butter_t *d, int order, double fc, double fs,
                                 int highpass)
{
    int m;

    memset(d, 0, sizeof *d);
    d->order     = order;
    d->nsections = order / 2;
    d->has_first = order & 1;

    for (m = 0; m < d->nsections; m++) {
        double phi = M_PI * (2.0 * m + order + 1.0) / (2.0 * order);
        double q   = -1.0 / (2.0 * cos(phi));
        if (highpass) biquad_hp(&d->sec[m], fc, fs, q);
        else          biquad_lp(&d->sec[m], fc, fs, q);
    }

    if (d->has_first) {
        if (highpass) onepole_hp(&d->first, fc, fs);
        else          onepole_lp(&d->first, fc, fs);
    }
}

static inline double butter_run(const butter_t *d, bqstate_t *bq, opstate_t *op,
                                double x)
{
    int k;
    if (d->has_first) x = onepole_run(&d->first, op, x);
    for (k = 0; k < d->nsections; k++) x = biquad_run(&d->sec[k], &bq[k], x);
    return x;
}


/* ---- parametric EQ sections --------------------------------------------
 * Cramping is the quality problem in digital parametric EQ. The bilinear
 * transform maps the infinite analog frequency axis onto the finite digital
 * one, so everything is squeezed as it approaches Nyquist: a textbook RBJ
 * bell at 10 kHz / 48 kHz is already noticeably narrower and taller on one
 * side than the analog prototype, and by 15 kHz the shape is badly wrong.
 * The bells here are therefore designed with Orfanidis' prescribed-Nyquist-
 * gain method (JAES 45(6), 1997), which imposes the analog prototype's value
 * at the real Nyquist frequency as a fourth design constraint. Same runtime
 * cost -- one biquad per band -- only the coefficient formulas differ.
 *
 * The shelves use the RBJ cookbook design, which is the right choice for
 * them: a shelf's asymptote *is* its Nyquist value, so it reaches exactly
 * the gain asked for by construction. Only the transition slope warps, and
 * much less visibly than a bell's skirt.
 */

/* Lower bound on the RBJ shelf discriminant. The shelf's alpha term is
 * sqrt(t)*sin(w0)/2, and alpha is what pulls the poles inside the unit
 * circle: at t = 0 the denominator's a2 equals a0 exactly, the poles land ON
 * the circle, and the section becomes an undamped oscillator that rings
 * forever. t goes negative when the slope S is pushed past the limit set by
 * the band's gain, so both a parameter check (dsp_shelf_smax) and this floor
 * are in place -- the check is the real fix, this is the backstop. */
#define DSP_SHELF_T_MIN 0.05

/* Largest shelf slope this gain can carry while keeping the poles strictly
 * inside the unit circle. The limit tightens as the gain grows: at +/-12 dB
 * it is about 4.6, at +/-24 dB about 2.3. S = 1 is the steepest monotonic
 * shelf and is always safe. */
static inline double dsp_shelf_smax(double gdb)
{
    double A = pow(10.0, fabs(gdb) / 40.0);
    double s = A + 1.0 / A;
    double d = 1.0 - (2.0 - DSP_SHELF_T_MIN) / s;
    return (d <= 1e-12) ? 1e9 : 1.0 / d;
}

static inline void biquad_lowshelf(biquad_t *c, double f0, double fs,
                                   double gdb, double S)
{
    double A  = pow(10.0, gdb / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0), sw = sin(w0);
    double t  = (A + 1.0 / A) * (1.0 / S - 1.0) + 2.0;
    double alpha, tsa, a0;

    if (t < DSP_SHELF_T_MIN) t = DSP_SHELF_T_MIN;
    alpha = sw / 2.0 * sqrt(t);
    tsa   = 2.0 * sqrt(A) * alpha;

    a0    =      (A + 1.0) + (A - 1.0) * cw + tsa;
    c->b0 =  A * ((A + 1.0) - (A - 1.0) * cw + tsa) / a0;
    c->b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw) / a0;
    c->b2 =  A * ((A + 1.0) - (A - 1.0) * cw - tsa) / a0;
    c->a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw)     / a0;
    c->a2 =      ((A + 1.0) + (A - 1.0) * cw - tsa) / a0;
}

static inline void biquad_highshelf(biquad_t *c, double f0, double fs,
                                    double gdb, double S)
{
    double A  = pow(10.0, gdb / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0), sw = sin(w0);
    double t  = (A + 1.0 / A) * (1.0 / S - 1.0) + 2.0;
    double alpha, tsa, a0;

    if (t < DSP_SHELF_T_MIN) t = DSP_SHELF_T_MIN;
    alpha = sw / 2.0 * sqrt(t);
    tsa   = 2.0 * sqrt(A) * alpha;

    a0    =      (A + 1.0) - (A - 1.0) * cw + tsa;
    c->b0 =  A * ((A + 1.0) + (A - 1.0) * cw + tsa) / a0;
    c->b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0;
    c->b2 =  A * ((A + 1.0) + (A - 1.0) * cw - tsa) / a0;
    c->a1 =  2.0 * ((A - 1.0) - (A + 1.0) * cw)     / a0;
    c->a2 =      ((A + 1.0) - (A - 1.0) * cw - tsa) / a0;
}

/* The classic RBJ cookbook bell, cramped near Nyquist. */
static inline void biquad_peak_rbj(biquad_t *c, double f0, double fs,
                                   double gdb, double Q)
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

/* Orfanidis bell with prescribed Nyquist gain. Four constraints are imposed:
 * gain G0 at DC, G at w0, GB at the band edges w0 +/- Dw/2, and G1 at
 * Nyquist, where G1 comes from the analog prototype evaluated at the true
 * Nyquist frequency. Matching that last value is what stops the response
 * collapsing as w0 climbs. Falls back to RBJ if the constraint set
 * degenerates, which happens when the centre frequency is pushed right up
 * against Nyquist. */
static inline void biquad_peak_orfanidis(biquad_t *c, double f0, double fs,
                                         double gdb, double Q)
{
    double G0 = 1.0;
    double G  = pow(10.0, gdb / 20.0);
    double GB = sqrt(G0 * G);
    double fny = fs / 2.0;
    double df  = f0 / Q;
    double w0  = 2.0 * M_PI * f0 / fs;
    double Dw  = 2.0 * M_PI * df / fs;

    double s1, s2, G1, F, G00, F00, G01, G11, F01, F11;
    double W2, DW, Cc, Dd, Aa, Bb, den;

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
        biquad_peak_rbj(c, f0, fs, gdb, Q);
        return;
    }

    W2 = sqrt(G11 / G00) * tan(w0 / 2.0) * tan(w0 / 2.0);
    DW = (1.0 + sqrt(F00 / F11) * W2) * tan(Dw / 2.0);

    Cc = F11 * DW * DW - 2.0 * W2 * (F01 - sqrt(F00 * F11));
    Dd = 2.0 * W2 * (G01 - sqrt(G00 * G11));

    if ((Cc + Dd) / F < 0.0 || (G * G * Cc + GB * GB * Dd) / F < 0.0) {
        biquad_peak_rbj(c, f0, fs, gdb, Q);
        return;
    }
    Aa = sqrt((Cc + Dd) / F);
    Bb = sqrt((G * G * Cc + GB * GB * Dd) / F);

    den = 1.0 + W2 + Aa;
    if (fabs(den) < 1e-18) {
        biquad_peak_rbj(c, f0, fs, gdb, Q);
        return;
    }

    c->b0 = (G1 + G0 * W2 + Bb)   / den;
    c->b1 = -2.0 * (G1 - G0 * W2) / den;
    c->b2 = (G1 - Bb + G0 * W2)   / den;
    c->a1 = -2.0 * (1.0 - W2)     / den;
    c->a2 = (1.0 + W2 - Aa)       / den;
}

/* Magnitude of one section at f: the self-test tools, and the response
 * curves the filters draw from their own coefficients. */
static inline double biquad_mag_db(const biquad_t *c, double f, double fs)
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

/* Magnitude of a first-order section at f. */
static inline double onepole_mag_db(const onepole_t *c, double f, double fs)
{
    double w = 2.0 * M_PI * f / fs;
    double nr = c->b0 + c->b1 * cos(w), ni = -c->b1 * sin(w);
    double dr = 1.0   + c->a1 * cos(w), di = -c->a1 * sin(w);
    double n  = nr * nr + ni * ni, d = dr * dr + di * di;
    if (d <= 0.0) return -999.0;
    return 10.0 * log10(n / d);
}

/* Magnitude of a whole Butterworth cascade at f: what the filter does, from
 * the coefficients it runs, for the response curve. */
static inline double butter_mag_db(const butter_t *d, double f, double fs)
{
    double db = d->has_first ? onepole_mag_db(&d->first, f, fs) : 0.0;
    int k;
    for (k = 0; k < d->nsections; k++) db += biquad_mag_db(&d->sec[k], f, fs);
    return db;
}

/* Scrub non-finite input. A recursive filter has no way back from a NaN or
 * an Inf: one bad sample from a glitching capture device or an upstream
 * plugin enters the feedback state and every output after it is NaN for the
 * rest of the stream. */
static inline void dsp_scrub(float *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (!isfinite(b[i])) b[i] = 0.0f;
}

#endif /* PWF_DSP_H */
