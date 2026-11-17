/*
 * notch_dsp.h -- the notch filter's core, shared by src/pw_notch.c and
 * tests/t_notch.c and held to reference/notch.c byte for byte.
 *
 * A notch is a band-stop that is only as wide as it has to be, so what makes
 * one good is where its geometry lands, and the textbook designs put it in
 * the wrong place:
 *
 *  - The RBJ cookbook notch prewarps the centre and sets the width with a
 *    bandwidth-in-octaves approximation, so its -3 dB edges move off the
 *    width that was asked for as the centre climbs towards Nyquist. Here the
 *    edges are solved for in the digital domain: they lie exactly freq/q Hz
 *    apart, at exactly -3 dB, and the null sits exactly on freq, at any rate.
 *
 *    The bilinear transform maps an analog band-stop centred on W0 with
 *    edges W1 W2 = W0^2 onto the digital axis through W = tan(pi f/fs), so
 *    the digital edges f1, f2 have to satisfy tan(a) tan(a+d) = K0^2, with
 *    a = pi f1/fs, d = pi (f2-f1)/fs, K0 = tan(pi f0/fs). Expanding
 *    tan(a+d) gives a quadratic in t = tan(a):
 *        t^2 + u (1 + K0^2) t - K0^2 = 0,   u = tan(d),
 *    whose positive root is K1 = tan(pi f1/fs); K2 = K0^2/K1. Then the analog
 *    prototype is W0 = K0, B = K2 - K1, with s normalised so that the
 *    bilinear transform is s = (1 - z^-1)/(1 + z^-1).
 *
 *  - A second-order notch is a V: it starts taking level well outside its
 *    -3 dB width and is only deep in a sliver at the centre. Order N here is
 *    a Butterworth band-stop, the order-N low-pass prototype through
 *    s -> B s/(s^2 + W0^2): 2N poles, N second-order sections per notch,
 *    the shoulders maximally flat, so for the same -3 dB width the band
 *    around it is left alone and the stop band is wider. Order 1 is the
 *    classic notch, (s^2 + W0^2)/(s^2 + B s + W0^2).
 *
 *    Each prototype pole p gives the two band-stop poles that solve
 *    p s^2 - B s + p W0^2 = 0. Their product is W0^2, so the larger one is
 *    computed directly and the other as W0^2 divided by it: the quadratic
 *    formula's second root cancels catastrophically for a wide band. Every
 *    section has its zeros at +/- j W0, which the bilinear transform puts on
 *    the unit circle at exactly f0 -- an infinite null, by construction, at
 *    every order.
 *
 *  - Mains hum is not one tone. harmonics N notches f0, 2 f0, ... N f0 with
 *    the same q, so each is as wide relative to its centre as the first --
 *    which is also how far a harmonic drifts when the mains does. A harmonic
 *    at or above 0.49 fs is left out rather than folded.
 *
 * Double-precision coefficients and state, transposed direct form II and
 * denormal flushing, as everywhere else (biquad_run in dsp.h).
 */

#ifndef PWF_NOTCH_DSP_H
#define PWF_NOTCH_DSP_H

#include <complex.h>
#include "dsp.h"

#define NOTCH_MAX_ORDER     8
#define NOTCH_MAX_HARMONICS 16
#define NOTCH_MAX_SECTIONS  (NOTCH_MAX_ORDER * NOTCH_MAX_HARMONICS)

/* The highest a centre may sit, and the widest a band may be, as fractions
 * of the rate. A band as wide as Nyquist would put its lower edge at DC. */
#define NOTCH_MAX_FREQ  0.49
#define NOTCH_MAX_WIDTH 0.49

typedef struct {
    int      order;        /* prototype order: sections per notch */
    int      nnotches;     /* harmonics actually below the top */
    int      nsections;    /* order * nnotches */
    double   f0[NOTCH_MAX_HARMONICS];   /* each notch's centre, Hz */
    double   lo[NOTCH_MAX_HARMONICS];   /* and its -3 dB edges */
    double   hi[NOTCH_MAX_HARMONICS];
    biquad_t sec[NOTCH_MAX_SECTIONS];
} notch_t;

/* H(s) = (s^2 + w2)/(s^2 + a1 s + a0), bilinear, s = (1-z^-1)/(1+z^-1). */
static inline void notch_section(biquad_t *c, double w2, double a1, double a0)
{
    double d = 1.0 + a1 + a0;

    c->b0 = (1.0 + w2) / d;
    c->b1 = 2.0 * (w2 - 1.0) / d;
    c->b2 = (1.0 + w2) / d;
    c->a1 = 2.0 * (a0 - 1.0) / d;
    c->a2 = (1.0 - a1 + a0) / d;
}

/* One Butterworth band-stop of the given order, centred on f0, -3 dB edges
 * bw Hz apart; writes order sections and its edges. */
static inline void notch_band(biquad_t *sec, int order, double f0, double bw,
                              double fs, double *lo, double *hi)
{
    double K0 = tan(M_PI * f0 / fs);
    double w2 = K0 * K0;
    double u  = tan(M_PI * bw / fs);
    double v  = u * (1.0 + w2);
    double K1 = (sqrt(v * v + 4.0 * w2) - v) / 2.0;
    double K2 = w2 / K1;
    double B  = K2 - K1;
    int m, k = 0;

    *lo = atan(K1) * fs / M_PI;
    *hi = atan(K2) * fs / M_PI;

    /* the conjugate pairs of the prototype: one pole of each, two sections */
    for (m = 0; m < order / 2; m++) {
        double phi = M_PI * (2.0 * m + order + 1.0) / (2.0 * order);
        double complex p  = cos(phi) + I * sin(phi);
        double complex sq = csqrt(B * B - 4.0 * p * p * w2);
        double complex r1 = cabs(B + sq) >= cabs(B - sq) ? B + sq : B - sq;
        double complex s1 = r1 / (2.0 * p);
        double complex s2 = w2 / s1;

        notch_section(&sec[k++], w2, -2.0 * creal(s1), creal(s1 * conj(s1)));
        notch_section(&sec[k++], w2, -2.0 * creal(s2), creal(s2 * conj(s2)));
    }
    /* odd order: the real pole at -1, whose two band-stop poles are the
     * roots of s^2 + B s + W0^2 -- the classic notch */
    if (order & 1)
        notch_section(&sec[k++], w2, B, w2);
}

static inline void notch_design(notch_t *d, int order, int harmonics,
                                double f0, double q, double fs)
{
    int h;

    memset(d, 0, sizeof *d);
    d->order = order;
    for (h = 1; h <= harmonics; h++) {
        double fc = f0 * h, bw = fc / q;
        int n = d->nnotches;

        if (fc >= fs * NOTCH_MAX_FREQ) break;
        if (bw > fs * NOTCH_MAX_WIDTH) bw = fs * NOTCH_MAX_WIDTH;
        d->f0[n] = fc;
        notch_band(&d->sec[n * order], order, fc, bw, fs, &d->lo[n], &d->hi[n]);
        d->nnotches++;
    }
    d->nsections = d->nnotches * order;
}

static inline double notch_run(const notch_t *d, bqstate_t *bq, double x)
{
    int k;
    for (k = 0; k < d->nsections; k++) x = biquad_run(&d->sec[k], &bq[k], x);
    return x;
}

static inline double notch_mag_db(const notch_t *d, double f, double fs)
{
    double db = 0.0;
    int k;
    for (k = 0; k < d->nsections; k++) db += biquad_mag_db(&d->sec[k], f, fs);
    return db;
}

#endif /* PWF_NOTCH_DSP_H */
