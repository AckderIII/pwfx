/*
 * comp_dsp.h -- the compressor core from reference/comp.c, shaped so the
 * PipeWire filter and the offline test can run exactly the same code.
 *
 * Design notes, unchanged from the reference:
 *
 *  - Log-domain topology throughout: the level is converted to dB once, the
 *    static curve is evaluated in dB, and the attack/release smoothing is
 *    applied to the gain-reduction signal in dB rather than to the linear
 *    envelope. This is the "smooth decoupled peak detector" from Giannoulis,
 *    Massberg & Reiss, JAES 60(6), 2012. Smoothing gain reduction in dB is
 *    what makes the attack and release times behave the same regardless of
 *    how far above threshold the signal sits; smoothing a linear envelope
 *    instead makes them level-dependent, which is the usual cause of a
 *    compressor that breathes differently on loud and quiet passages.
 *
 *        y1[n] = max(xL[n], aR*y1[n-1] + (1-aR)*xL[n])
 *        yL[n] = aA*yL[n-1] + (1-aA)*y1[n]
 *
 *    The branching max() decouples attack from release so a new transient
 *    during a release is caught instantly, while the second one-pole rounds
 *    the corner out of the release -- the audible benefit over the plain
 *    decoupled detector, which leaves a kink you can hear as a chirp.
 *  - Soft knee: a quadratic across a knee of configurable width, continuous
 *    in both value and slope at each end.
 *  - Lookahead: the detector runs on the undelayed signal, so gain reduction
 *    is already in place when the transient arrives instead of arriving one
 *    attack-time late.
 *  - Peak or RMS detection; channel linking by max, by average, or off.
 *  - Sidechain high-pass and low-pass, affecting detection only.
 *  - Detector, gain and filter state are double precision. The audio path
 *    stays float and is only ever multiplied, never accumulated, so the
 *    stream keeps full 32-bit float quality.
 *
 * As with the gate, the one change for realtime is that the lookahead delay
 * is real rather than absorbed, and is reported to the graph as latency.
 */

#ifndef COMP_DSP_H
#define COMP_DSP_H

#include <stdint.h>
#include <stdlib.h>

#include "dsp.h"

#define COMP_MAX_CHANNELS  64
#define COMP_MAX_LOOK_MS  200.0
#define COMP_DB_FLOOR    (-240.0)   /* level floor, below float32 resolution */

enum { COMP_LINK_MAX = 0, COMP_LINK_AVG = 1, COMP_LINK_OFF = 2 };

typedef struct {
    int      L;                 /* lookahead, samples */
    double   thr_db, ratio, knee;
    double   aA, aR, aRms;      /* one-pole coefficients */
    double   makeup_db, makeup_lin;
    int      link, ndet;
    int      use_hp, use_lp;
    biquad_t hp, lp;
} comp_coeffs;

typedef struct {
    uint32_t   channels;
    int        max_L;

    bqstate_t *hp_st, *lp_st;           /* channels */
    double    *y1, *yL, *rms_acc, *gain;/* channels */

    float     *ring;                    /* (max_L + 1) * channels */
    int        ring_frames, w;

    double     peak_gr;                 /* diagnostic, dB */
    double     blk_gr;                  /* most reduction since comp_take_gr, dB */
    int        last_link;               /* -1 until the first block runs */
} comp_state;

/* ---- static characteristic ---------------------------------------------
 * Output level in dB for an input level in dB. Below the knee the curve is
 * the identity; above it the slope is 1/ratio; across the knee the two are
 * joined by a quadratic that matches value and slope at both ends, so the
 * curve is C1 everywhere.
 */
static inline double comp_curve_db(double x, double thr, double ratio,
                                   double knee)
{
    double d = x - thr;

    if (knee > 0.0 && 2.0 * d > -knee && 2.0 * d < knee) {
        double t = d + knee / 2.0;
        return x + (1.0 / ratio - 1.0) * t * t / (2.0 * knee);
    }
    if (2.0 * d <= -knee)
        return x;
    return thr + d / ratio;
}

static inline void comp_design(comp_coeffs *c, double rate, uint32_t channels,
                               double thr_db, double ratio, double knee,
                               double attack_ms, double release_ms,
                               double look_ms, double rms_ms,
                               double makeup_db, int auto_makeup,
                               int link, double sc_hpf, double sc_lpf)
{
    memset(c, 0, sizeof *c);

    c->thr_db = thr_db;
    c->ratio  = ratio;
    c->knee   = knee;
    c->link   = link;
    c->ndet   = (link == COMP_LINK_OFF) ? (int)channels : 1;
    c->L      = (int)(look_ms * rate / 1000.0 + 0.5);

    /* alpha = exp(-1 / (tau * fs)); a zero time gives alpha = 0, i.e. instant */
    c->aA   = attack_ms  > 0.0 ? exp(-1000.0 / (attack_ms  * rate)) : 0.0;
    c->aR   = release_ms > 0.0 ? exp(-1000.0 / (release_ms * rate)) : 0.0;
    c->aRms = rms_ms     > 0.0 ? exp(-1000.0 / (rms_ms     * rate)) : 0.0;

    if (auto_makeup)
        makeup_db += 0.0 - comp_curve_db(0.0, thr_db, ratio, knee);
    c->makeup_db  = makeup_db;
    c->makeup_lin = exp(makeup_db * DSP_LN10_20);

    c->use_hp = sc_hpf > 0.0 && sc_hpf < rate / 2.0;
    c->use_lp = sc_lpf > 0.0 && sc_lpf < rate / 2.0;
    if (c->use_hp) biquad_hp(&c->hp, sc_hpf, rate, M_SQRT1_2);
    if (c->use_lp) biquad_lp(&c->lp, sc_lpf, rate, M_SQRT1_2);
}

static inline void comp_reset(comp_state *s)
{
    uint32_t d;

    memset(s->hp_st, 0, s->channels * sizeof *s->hp_st);
    memset(s->lp_st, 0, s->channels * sizeof *s->lp_st);
    memset(s->ring, 0, (size_t)s->ring_frames * s->channels * sizeof *s->ring);
    for (d = 0; d < s->channels; d++) {
        s->y1[d] = s->yL[d] = s->rms_acc[d] = 0.0;
        s->gain[d] = 1.0;
    }
    s->w = 0;
    s->peak_gr = 0.0;
    s->blk_gr = 0.0;
    s->last_link = -1;
}

static inline int comp_init(comp_state *s, uint32_t channels, double max_rate)
{
    memset(s, 0, sizeof *s);
    s->channels    = channels;
    s->max_L       = (int)(COMP_MAX_LOOK_MS * max_rate / 1000.0 + 0.5);
    s->ring_frames = s->max_L + 1;

    s->hp_st   = calloc(channels, sizeof *s->hp_st);
    s->lp_st   = calloc(channels, sizeof *s->lp_st);
    s->y1      = calloc(channels, sizeof *s->y1);
    s->yL      = calloc(channels, sizeof *s->yL);
    s->rms_acc = calloc(channels, sizeof *s->rms_acc);
    s->gain    = calloc(channels, sizeof *s->gain);
    s->ring    = calloc((size_t)s->ring_frames * channels, sizeof *s->ring);

    if (!s->hp_st || !s->lp_st || !s->y1 || !s->yL || !s->rms_acc ||
        !s->gain || !s->ring)
        return -1;
    comp_reset(s);
    return 0;
}

static inline void comp_fini(comp_state *s)
{
    free(s->hp_st); free(s->lp_st);
    free(s->y1); free(s->yL); free(s->rms_acc); free(s->gain);
    free(s->ring);
    memset(s, 0, sizeof *s);
}

/* One block. out[] lags in[] by c->L samples. */
static inline void comp_run(comp_state *s, const comp_coeffs *c,
                            float *const *in, float *const *out,
                            uint32_t channels, uint32_t n)
{
    double sc[COMP_MAX_CHANNELS];
    uint32_t k, ch;
    int d, L = c->L, ndet = c->ndet;

    if (L > s->max_L) L = s->max_L;
    if (ndet > (int)channels) ndet = (int)channels;

    for (k = 0; k < n; k++) {
        int rd;

        /* sidechain conditioning, per channel */
        for (ch = 0; ch < channels; ch++) {
            double v = (double)in[ch][k];
            if (!isfinite(v)) v = 0.0;
            if (c->use_hp) v = biquad_run(&c->hp, &s->hp_st[ch], v);
            if (c->use_lp) v = biquad_run(&c->lp, &s->lp_st[ch], v);
            sc[ch] = v;
        }

        for (d = 0; d < ndet; d++) {
            double det, xdb, xL, gdb;

            if (c->link == COMP_LINK_OFF) {
                det = fabs(sc[d]);
                if (c->aRms > 0.0) {
                    s->rms_acc[d] = c->aRms * s->rms_acc[d]
                                  + (1.0 - c->aRms) * det * det;
                    det = sqrt(s->rms_acc[d]);
                }
            } else if (c->link == COMP_LINK_MAX) {
                det = 0.0;
                for (ch = 0; ch < channels; ch++) {
                    double v = fabs(sc[ch]);
                    if (v > det) det = v;
                }
                if (c->aRms > 0.0) {
                    s->rms_acc[0] = c->aRms * s->rms_acc[0]
                                  + (1.0 - c->aRms) * det * det;
                    det = sqrt(s->rms_acc[0]);
                }
            } else {                              /* avg */
                double sum = 0.0;
                for (ch = 0; ch < channels; ch++) sum += sc[ch] * sc[ch];
                sum /= (double)channels;
                if (c->aRms > 0.0) {
                    s->rms_acc[0] = c->aRms * s->rms_acc[0]
                                  + (1.0 - c->aRms) * sum;
                    sum = s->rms_acc[0];
                }
                det = sqrt(sum);
            }

            xdb = det > 0.0 ? DSP_LOG10_20 * log(det) : COMP_DB_FLOOR;
            if (xdb < COMP_DB_FLOOR) xdb = COMP_DB_FLOOR;

            /* gain reduction demanded by the static curve, in dB >= 0 */
            xL = xdb - comp_curve_db(xdb, c->thr_db, c->ratio, c->knee);
            if (xL < 0.0) xL = 0.0;

            /* smooth decoupled peak detector, in the log domain */
            {
                double rel = c->aR * s->y1[d] + (1.0 - c->aR) * xL;
                s->y1[d] = xL > rel ? xL : rel;
                s->yL[d] = c->aA * s->yL[d] + (1.0 - c->aA) * s->y1[d];
            }
            if (s->yL[d] < 1e-12) s->yL[d] = 0.0;
            if (s->yL[d] > s->peak_gr) s->peak_gr = s->yL[d];
            if (s->yL[d] > s->blk_gr)  s->blk_gr  = s->yL[d];

            gdb = c->makeup_db - s->yL[d];
            s->gain[d] = s->yL[d] == 0.0 ? c->makeup_lin
                                         : exp(gdb * DSP_LN10_20);
        }

        /* delay line: write the current frame, read the frame L back */
        for (ch = 0; ch < channels; ch++) {
            double v = (double)in[ch][k];
            if (!isfinite(v)) v = 0.0;
            s->ring[(size_t)s->w * channels + ch] = (float)v;
        }
        rd = (s->w + s->ring_frames - L) % s->ring_frames;

        for (ch = 0; ch < channels; ch++)
            out[ch][k] = (float)(s->ring[(size_t)rd * channels + ch] *
                                 s->gain[c->link == COMP_LINK_OFF ? ch : 0]);

        s->w = (s->w + 1) % s->ring_frames;
    }
}

/* The most gain reduction applied since the last call, in dB >= 0, makeup
 * not counted: what the meter shows. Linked or not, the worst detector. */
static inline double comp_take_gr(comp_state *s)
{
    double g = s->blk_gr;
    s->blk_gr = 0.0;
    return g;
}

#endif /* COMP_DSP_H */
