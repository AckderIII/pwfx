/*
 * limit_dsp.h -- the true-peak brickwall limiter core from reference/limit.c,
 * shaped so the PipeWire filter and the offline test can run the same code.
 *
 * Why this is not just a fast compressor
 * --------------------------------------
 * A compressor smooths gain with a one-pole attack, so the gain arrives at
 * its target only asymptotically and the overshoot past the threshold decays
 * as exp(-lookahead/attack) -- it never reaches zero. A limiter has to
 * *guarantee* the ceiling, so it needs a construction that provably reaches
 * the required gain before the peak does.
 *
 *   1. gr_req[n] -- instantaneous gain reduction, in dB, that would put
 *                   sample n exactly on the ceiling.
 *   2. g_env[n]  -- gr_req with an exponential release applied:
 *                     gr[n] = max(gr_req[n], alpha*gr[n-1])
 *                   Release only ever makes the gain smaller (more
 *                   reduction) than required, so it cannot break the
 *                   guarantee.
 *   3. m[n]      -- sliding MINIMUM of g_env over a window of S samples
 *                   looking ahead.
 *   4. b[n]      -- m through two cascaded boxcar averages of length A each,
 *                   i.e. a triangular (Bartlett) FIR of support S = 2A-1.
 *                   Two boxcars rather than one make the gain envelope C1; a
 *                   single boxcar leaves slope jumps that are audible as a
 *                   faint chirp on dense material.
 *
 * With the audio delayed by D = 2A-2 = S-1 samples, b[n] is guaranteed to be
 * <= g_env[n-D]. Proof: b[n] is a convex combination of m[n-k] for
 * k in [0, 2A-2]. Each m[n-k] is the minimum of g_env over [n-k-S+1, n-k].
 * That interval contains n-D for every k in [0, 2A-2]: the upper end needs
 * k <= 2A-2, the lower end k >= 2A-1-S = 0. So every term in the average is
 * <= g_env[n-D], hence so is the average. No overshoot is possible -- not
 * "small", not "decaying", none.
 *
 * True peak
 * ---------
 * Sample peaks are not what clips. A D/A reconstruction filter, and every
 * lossy codec, can produce inter-sample peaks well above the highest sample,
 * which is why broadcast and streaming specs are written in dBTP. The
 * detector oversamples by a configurable factor through a Kaiser-windowed
 * polyphase sinc interpolator, in the spirit of ITU-R BS.1770-4, and limits
 * the interpolated peak. The audio path itself is never oversampled or
 * filtered -- only the detector sees it.
 *
 * Other notes
 * -----------
 *  - Channel-linked by design: one gain for all channels. A limiter that
 *    moves channels independently rewrites the stereo image on every peak.
 *  - The boxcar running sums are recomputed exactly at intervals so that
 *    floating-point drift cannot slowly erode the guarantee on a stream that
 *    runs for days.
 *  - The final clamp at the ceiling is a safety net that should never
 *    engage; limit_state::clamps counts the times it did, and that count is
 *    expected to be exactly zero.
 *
 * As with the gate and the compressor, the change for realtime is that the
 * lookahead delay is real rather than absorbed, and is reported to the graph
 * as latency.
 */

#ifndef LIMIT_DSP_H
#define LIMIT_DSP_H

#include <stdint.h>
#include <stdlib.h>

#include "dsp.h"

#define LIMIT_MAX_CHANNELS 64
#define LIMIT_TAPS         24        /* interpolator taps per phase, even */
#define LIMIT_MAX_OS       16
#define LIMIT_MAX_LOOK_MS  100.0
#define LIMIT_RESUM_PERIOD 65536     /* exact boxcar re-sum interval */

typedef struct {
    int    os;
    double h[LIMIT_MAX_OS][LIMIT_TAPS + 1];
    int    A, S, D, D_det, DT;
    double ceil_lin, in_lin, alpha;
} limit_coeffs;

typedef struct {
    double *buf;
    int     len, pos;
    double  sum;
    long long since;
} limit_boxcar;

typedef struct {
    uint32_t   channels;
    int        max_A, max_S, max_DT;

    double   (*tpline)[LIMIT_TAPS + 1];   /* channels */
    int        tp_pos;

    double    *sm_val;                    /* sliding minimum deque */
    long long *sm_idx;
    int        sm_cap, sm_head, sm_count;

    limit_boxcar box1, box2;

    float     *ring;                      /* (max_DT + 1) * channels */
    int        ring_frames, w;

    long long  n;
    double     gr_db, last_b2;
    double     blk_min;                   /* least gain since limit_take_gr */
    int        last_A;

    /* diagnostics */
    double     peak_gr, tp_in, max_excess;
    long long  clamps;
} limit_state;

/* ---- modified Bessel I0, for the Kaiser window ------------------------- */
static inline double limit_bessel_i0(double x)
{
    double s = 1.0, t = 1.0, h = x / 2.0;
    int k;
    for (k = 1; k < 60; k++) {
        t *= (h / k) * (h / k);
        s += t;
        if (t < 1e-18 * s) break;
    }
    return s;
}

/* Design the polyphase interpolator. Prototype: Kaiser-windowed sinc of
 * length os*TAPS+1, cutoff at the original Nyquist. Phase p takes every
 * os-th tap starting at p, and each phase is normalised to unit sum so no
 * phase carries a DC offset relative to the others. Group delay is exactly
 * TAPS/2 input samples. */
static inline void limit_design_interp(limit_coeffs *c, int os)
{
    double hf[LIMIT_MAX_OS * LIMIT_TAPS + 1];
    int N   = os * LIMIT_TAPS + 1;
    double cen = os * LIMIT_TAPS / 2.0, beta = 8.0;
    double i0b = limit_bessel_i0(beta);
    int i, p, k;

    for (i = 0; i < N; i++) {
        double xx = (i - cen) / os;
        double s  = (fabs(xx) < 1e-12) ? 1.0 : sin(M_PI * xx) / (M_PI * xx);
        double t  = (N == 1) ? 0.0 : (2.0 * i / (N - 1) - 1.0);
        double t2 = 1.0 - t * t;
        if (t2 < 0.0) t2 = 0.0;
        hf[i] = s * limit_bessel_i0(beta * sqrt(t2)) / i0b;
    }
    for (p = 0; p < os; p++) {
        double sum = 0.0;
        for (k = 0; k <= LIMIT_TAPS; k++) {
            int idx = k * os + p;
            c->h[p][k] = (idx < N) ? hf[idx] : 0.0;
            sum += c->h[p][k];
        }
        if (sum != 0.0)
            for (k = 0; k <= LIMIT_TAPS; k++) c->h[p][k] /= sum;
    }
}

static inline void limit_design(limit_coeffs *c, double rate, double ceil_db,
                                double in_db, double release_ms,
                                double look_ms, int os)
{
    int L;

    memset(c, 0, sizeof *c);
    c->os = os;
    limit_design_interp(c, os);

    c->D_det    = LIMIT_TAPS / 2;
    c->ceil_lin = exp(ceil_db * DSP_LN10_20);
    c->in_lin   = exp(in_db   * DSP_LN10_20);
    c->alpha    = release_ms > 0.0 ? exp(-1000.0 / (release_ms * rate)) : 0.0;

    /* Two boxcars of length A give a triangular FIR of support S = 2A-1 and
     * total delay D = S-1, which is exactly the window the sliding minimum
     * must cover for the no-overshoot proof to hold. */
    L = (int)(look_ms * rate / 1000.0 + 0.5);
    c->A = (L + 2) / 2;
    if (c->A < 1) c->A = 1;
    c->S  = 2 * c->A - 1;
    c->D  = c->S - 1;
    c->DT = c->D + c->D_det;
}

static inline void limit_boxcar_fill(limit_boxcar *b, int len, double fill)
{
    int i;
    for (i = 0; i < len; i++) b->buf[i] = fill;
    b->len = len;
    b->pos = 0;
    b->sum = fill * len;
    b->since = 0;
}

static inline double limit_boxcar_push(limit_boxcar *b, double v)
{
    b->sum -= b->buf[b->pos];
    b->buf[b->pos] = v;
    b->sum += v;
    b->pos = (b->pos + 1) % b->len;

    if (++b->since >= LIMIT_RESUM_PERIOD) {      /* kill accumulated drift */
        int i;
        double s = 0.0;
        for (i = 0; i < b->len; i++) s += b->buf[i];
        b->sum = s;
        b->since = 0;
    }
    return b->sum / b->len;
}

static inline double limit_slidemin(limit_state *s, long long n, double v,
                                    int window)
{
    int back;

    while (s->sm_count > 0) {
        back = (s->sm_head + s->sm_count - 1) % s->sm_cap;
        if (s->sm_val[back] < v) break;
        s->sm_count--;
    }
    back = (s->sm_head + s->sm_count) % s->sm_cap;
    s->sm_val[back] = v;
    s->sm_idx[back] = n;
    s->sm_count++;

    while (s->sm_count > 0 && s->sm_idx[s->sm_head] <= n - window) {
        s->sm_head = (s->sm_head + 1) % s->sm_cap;
        s->sm_count--;
    }
    return s->sm_val[s->sm_head];
}

static inline int limit_init(limit_state *s, uint32_t channels,
                             double max_rate)
{
    int max_L;

    memset(s, 0, sizeof *s);
    s->channels = channels;

    max_L       = (int)(LIMIT_MAX_LOOK_MS * max_rate / 1000.0 + 0.5);
    s->max_A    = (max_L + 2) / 2;
    if (s->max_A < 1) s->max_A = 1;
    s->max_S    = 2 * s->max_A - 1;
    s->max_DT   = s->max_S - 1 + LIMIT_TAPS / 2;
    s->ring_frames = s->max_DT + 1;
    s->sm_cap   = s->max_S + 2;

    s->tpline   = calloc(channels, sizeof *s->tpline);
    s->sm_val   = calloc((size_t)s->sm_cap, sizeof *s->sm_val);
    s->sm_idx   = calloc((size_t)s->sm_cap, sizeof *s->sm_idx);
    s->box1.buf = calloc((size_t)s->max_A, sizeof *s->box1.buf);
    s->box2.buf = calloc((size_t)s->max_A, sizeof *s->box2.buf);
    s->ring     = calloc((size_t)s->ring_frames * channels, sizeof *s->ring);

    if (!s->tpline || !s->sm_val || !s->sm_idx || !s->box1.buf ||
        !s->box2.buf || !s->ring)
        return -1;

    s->gr_db = 0.0;
    s->last_b2 = 1.0;
    s->blk_min = 1.0;
    s->last_A = 0;
    limit_boxcar_fill(&s->box1, s->max_A, 1.0);
    limit_boxcar_fill(&s->box2, s->max_A, 1.0);
    return 0;
}

static inline void limit_fini(limit_state *s)
{
    free(s->tpline);
    free(s->sm_val); free(s->sm_idx);
    free(s->box1.buf); free(s->box2.buf);
    free(s->ring);
    memset(s, 0, sizeof *s);
}

/* One block. out[] lags in[] by c->DT samples. */
static inline void limit_run(limit_state *s, const limit_coeffs *c,
                             float *const *in, float *const *out,
                             uint32_t channels, uint32_t n)
{
    uint32_t k, ch;
    int p, t, A = c->A, S = c->S, DT = c->DT, os = c->os;

    if (A > s->max_A) { A = s->max_A; S = 2 * A - 1; DT = S - 1 + c->D_det; }

    /* The boxcar length is the lookahead. When it changes, the running sums
     * have to be rebuilt -- refilled with the gain currently in use, not
     * with unity, so the gain stays continuous across the change and the
     * ceiling keeps holding while the new window fills. */
    if (A != s->last_A) {
        limit_boxcar_fill(&s->box1, A, s->last_b2);
        limit_boxcar_fill(&s->box2, A, s->last_b2);
        s->last_A = A;
    }

    for (k = 0; k < n; k++) {
        double tp = 0.0, gr_req, g_env, m, b1, b2;
        int rd;

        /* push into the per-channel interpolator lines, find the true peak */
        for (ch = 0; ch < channels; ch++) {
            double v = (double)in[ch][k];
            if (!isfinite(v)) v = 0.0;
            s->tpline[ch][s->tp_pos] = v * c->in_lin;
        }
        for (ch = 0; ch < channels; ch++) {
            for (p = 0; p < os; p++) {
                double acc = 0.0;
                for (t = 0; t <= LIMIT_TAPS; t++) {
                    int q = s->tp_pos - t;
                    if (q < 0) q += LIMIT_TAPS + 1;
                    acc += c->h[p][t] * s->tpline[ch][q];
                }
                acc = fabs(acc);
                if (acc > tp) tp = acc;
            }
        }
        if (tp > s->tp_in) s->tp_in = tp;

        /* required reduction, in dB >= 0 */
        gr_req = tp > c->ceil_lin
               ? DSP_LOG10_20 * log(tp / c->ceil_lin) : 0.0;

        /* exponential release; only ever adds reduction, never removes */
        s->gr_db = c->alpha * s->gr_db;
        if (gr_req > s->gr_db) s->gr_db = gr_req;
        if (s->gr_db < 1e-12)  s->gr_db = 0.0;
        if (s->gr_db > s->peak_gr) s->peak_gr = s->gr_db;

        g_env = s->gr_db > 0.0 ? exp(-s->gr_db * DSP_LN10_20) : 1.0;

        /* sliding minimum, then triangular smoothing */
        m  = limit_slidemin(s, s->n, g_env, S);
        b1 = limit_boxcar_push(&s->box1, m);
        b2 = limit_boxcar_push(&s->box2, b1);
        s->last_b2 = b2;
        if (b2 < s->blk_min) s->blk_min = b2;

        /* delay line: write the current frame, read the frame DT back */
        for (ch = 0; ch < channels; ch++) {
            double v = (double)in[ch][k];
            if (!isfinite(v)) v = 0.0;
            s->ring[(size_t)s->w * channels + ch] = (float)(v * c->in_lin);
        }
        rd = (s->w + s->ring_frames - DT) % s->ring_frames;

        for (ch = 0; ch < channels; ch++) {
            double y = s->ring[(size_t)rd * channels + ch] * b2;
            double ex = fabs(y) - c->ceil_lin;
            /* Only a real breach counts. The construction is exact in exact
             * arithmetic, so anything at the last-bit level is
             * double-rounding, not overshoot, and would make the counter
             * useless as a diagnostic. */
            if (ex > 0.0) {
                if (ex > s->max_excess) s->max_excess = ex;
                if (ex > 1e-6 * c->ceil_lin) s->clamps++;
                y = y > 0.0 ? c->ceil_lin : -c->ceil_lin;
            }
            out[ch][k] = (float)y;
        }

        s->tp_pos = (s->tp_pos + 1) % (LIMIT_TAPS + 1);
        s->w = (s->w + 1) % s->ring_frames;
        s->n++;
    }
}

/* The most gain reduction applied since the last call, in dB >= 0. This is
 * the smoothed gain the audio is actually multiplied by, not gr_db, the
 * reduction the detector asked for before it was spread over the lookahead. */
static inline double limit_take_gr(limit_state *s)
{
    double g = s->blk_min;
    s->blk_min = 1.0;
    return g > 0.0 && g < 1.0 ? -DSP_LOG10_20 * log(g) : 0.0;
}

#endif /* LIMIT_DSP_H */
