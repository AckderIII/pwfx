/*
 * gate_dsp.h -- the noise gate / expander core from reference/gate.c, shaped
 * so the PipeWire filter and the offline test can run exactly the same code.
 *
 * Design notes, unchanged from the reference:
 *
 *  - True lookahead. The audio is delayed by L samples while the detector
 *    runs on the undelayed signal, so the gate is already open by the time
 *    the transient reaches the output. Without this, any gate with a
 *    non-zero attack clips the leading consonant off every word. The
 *    detector uses an exact sliding maximum over the lookahead window,
 *    maintained by a monotonic deque (O(1) amortised per sample), not an
 *    approximation.
 *  - Hysteresis: separate open and close thresholds, so a signal hovering at
 *    the threshold cannot chatter the gate.
 *  - Hold time: once open, the gate stays open for at least the hold period
 *    after the signal falls below the close threshold. That is what stops it
 *    closing between syllables.
 *  - Sidechain filtering, so desk rumble, mic stand thumps and AC hum do not
 *    hold the gate open. Detection only; the audio path is untouched.
 *  - Channel-linked: one detector, one gain, applied to every channel.
 *    Per-channel gating would shift the stereo image every time it moves.
 *  - Raised-cosine gain ramps in the dB domain. The curve has zero slope at
 *    both ends, so there is no derivative discontinuity at the start or end
 *    of a ramp and therefore no click -- the usual failure mode of a linear
 *    or one-pole gain ramp.
 *  - Range rather than hard mute: the closed state attenuates by a set
 *    amount instead of slamming to digital black, which sounds far more
 *    natural on a live mic. At >= 90 dB of range the fully-closed state does
 *    snap to exact zero, so gated passages are true silence.
 *
 * The one thing that had to change for realtime: the reference absorbs the
 * lookahead delay, swallowing the first L samples and flushing L more at
 * EOF, so its output is sample-aligned with its input. A filter in a live
 * graph cannot do that -- looking ahead means being late -- so here the
 * output really is delayed by L samples and that latency is reported to
 * PipeWire, which compensates for it. Everything else is identical, which
 * the offline test checks by reinstating the absorb-and-flush around this
 * same code and diffing against the reference byte for byte.
 */

#ifndef GATE_DSP_H
#define GATE_DSP_H

#include <stdint.h>
#include <stdlib.h>

#include "dsp.h"

/* Below this the gate is considered fully closed and, if the range is deep
 * enough, the output is forced to exact zero. */
#define GATE_SILENCE_RANGE_DB 90.0

/* Lookahead is latency, and latency in a live graph is a cost paid on every
 * sample; the reference's 1000 ms ceiling makes no sense here. This bounds
 * the delay line, which is allocated once at startup for the worst case. */
#define GATE_MAX_LOOK_MS 200.0

typedef struct {
    int       L;              /* lookahead, samples */
    double    open_lin;
    double    close_lin;
    double    atk_inc;        /* ramp position per sample, up */
    double    rel_inc;        /* ramp position per sample, down */
    long long hold_samples;
    double    range_db;
    int       snap_silent;
    int       use_hp, use_lp;
    biquad_t  hp, lp;
} gate_coeffs;

typedef struct {
    uint32_t   channels;
    int        max_L;

    bqstate_t *hp_st, *lp_st;          /* channels */

    double    *sm_val;                 /* monotonic deque, max_L + 2 */
    long long *sm_idx;
    int        sm_cap, sm_head, sm_count;

    float     *ring;                   /* (max_L + 1) * channels */
    int        ring_frames, w;

    long long  n, hold_left;
    int        is_open;
    double     p, gain, last_p;
    double     blk_min;                /* least gain since gate_take_gr */
} gate_state;

static inline void gate_design(gate_coeffs *c, double rate,
                               double thresh_db, double close_db,
                               double attack_ms, double hold_ms,
                               double release_ms, double look_ms,
                               double range_db, double sc_hpf, double sc_lpf)
{
    memset(c, 0, sizeof *c);

    c->open_lin  = pow(10.0, thresh_db / 20.0);
    c->close_lin = pow(10.0, close_db  / 20.0);
    c->L            = (int)(look_ms * rate / 1000.0 + 0.5);
    c->hold_samples = (long long)(hold_ms * rate / 1000.0 + 0.5);
    c->atk_inc = attack_ms  > 0.0 ? 1.0 / (attack_ms  * rate / 1000.0) : 1.0;
    c->rel_inc = release_ms > 0.0 ? 1.0 / (release_ms * rate / 1000.0) : 1.0;
    if (c->atk_inc > 1.0) c->atk_inc = 1.0;
    if (c->rel_inc > 1.0) c->rel_inc = 1.0;
    c->range_db    = range_db;
    c->snap_silent = range_db >= GATE_SILENCE_RANGE_DB;

    c->use_hp = sc_hpf > 0.0 && sc_hpf < rate / 2.0;
    c->use_lp = sc_lpf > 0.0 && sc_lpf < rate / 2.0;
    if (c->use_hp) biquad_hp(&c->hp, sc_hpf, rate, M_SQRT1_2);
    if (c->use_lp) biquad_lp(&c->lp, sc_lpf, rate, M_SQRT1_2);
}

static inline void gate_reset(gate_state *s)
{
    memset(s->hp_st, 0, s->channels * sizeof *s->hp_st);
    memset(s->lp_st, 0, s->channels * sizeof *s->lp_st);
    memset(s->ring, 0, (size_t)s->ring_frames * s->channels * sizeof *s->ring);
    s->sm_head = s->sm_count = 0;
    s->w = 0;
    s->n = 0;
    s->hold_left = 0;
    s->is_open = 0;
    s->p = 0.0;
    s->gain = 0.0;
    s->last_p = -1.0;
    s->blk_min = 1.0;
}

static inline int gate_init(gate_state *s, uint32_t channels, double max_rate)
{
    memset(s, 0, sizeof *s);
    s->channels = channels;
    s->max_L    = (int)(GATE_MAX_LOOK_MS * max_rate / 1000.0 + 0.5);
    s->ring_frames = s->max_L + 1;
    s->sm_cap      = s->max_L + 2;

    s->hp_st  = calloc(channels, sizeof *s->hp_st);
    s->lp_st  = calloc(channels, sizeof *s->lp_st);
    s->sm_val = calloc((size_t)s->sm_cap, sizeof *s->sm_val);
    s->sm_idx = calloc((size_t)s->sm_cap, sizeof *s->sm_idx);
    s->ring   = calloc((size_t)s->ring_frames * channels, sizeof *s->ring);

    if (!s->hp_st || !s->lp_st || !s->sm_val || !s->sm_idx || !s->ring)
        return -1;
    gate_reset(s);
    return 0;
}

static inline void gate_fini(gate_state *s)
{
    free(s->hp_st); free(s->lp_st);
    free(s->sm_val); free(s->sm_idx);
    free(s->ring);
    memset(s, 0, sizeof *s);
}

/* Push sample n with magnitude v, drop everything older than n-window, and
 * return the maximum over [n-window, n]. */
static inline double gate_slidemax(gate_state *s, long long n, double v,
                                   int window)
{
    int back;

    while (s->sm_count > 0) {
        back = (s->sm_head + s->sm_count - 1) % s->sm_cap;
        if (s->sm_val[back] > v) break;
        s->sm_count--;
    }
    back = (s->sm_head + s->sm_count) % s->sm_cap;
    s->sm_val[back] = v;
    s->sm_idx[back] = n;
    s->sm_count++;

    while (s->sm_count > 0 && s->sm_idx[s->sm_head] < n - window) {
        s->sm_head = (s->sm_head + 1) % s->sm_cap;
        s->sm_count--;
    }
    return s->sm_val[s->sm_head];
}

/* One block. out[] lags in[] by c->L samples; the caller reports that as
 * latency (live) or compensates for it (offline). */
static inline void gate_run(gate_state *s, const gate_coeffs *c,
                            float *const *in, float *const *out,
                            uint32_t channels, uint32_t n)
{
    uint32_t k, ch;
    int L = c->L;

    if (L > s->max_L) L = s->max_L;

    for (k = 0; k < n; k++) {
        double det = 0.0;
        int rd;

        /* detector: sidechain-filtered peak, linked across channels */
        for (ch = 0; ch < channels; ch++) {
            double v = (double)in[ch][k];
            if (!isfinite(v)) v = 0.0;
            if (c->use_hp) v = biquad_run(&c->hp, &s->hp_st[ch], v);
            if (c->use_lp) v = biquad_run(&c->lp, &s->lp_st[ch], v);
            v = fabs(v);
            if (v > det) det = v;
        }
        det = gate_slidemax(s, s->n, det, L);

        /* state machine: hysteresis + hold */
        if (det >= c->open_lin) {
            s->is_open = 1;
            s->hold_left = c->hold_samples;
        } else if (det >= c->close_lin) {
            if (s->is_open) s->hold_left = c->hold_samples;
        } else if (s->is_open) {
            if (s->hold_left > 0) s->hold_left--;
            else s->is_open = 0;
        }

        /* raised-cosine ramp in the dB domain */
        s->p += s->is_open ? c->atk_inc : -c->rel_inc;
        if (s->p > 1.0) s->p = 1.0;
        if (s->p < 0.0) s->p = 0.0;
        if (s->p != s->last_p) {
            if (s->p <= 0.0 && c->snap_silent) {
                s->gain = 0.0;
            } else {
                double sh  = 0.5 - 0.5 * cos(M_PI * s->p);
                double gdb = -c->range_db * (1.0 - sh);
                s->gain = exp(gdb * DSP_LN10_20);
            }
            s->last_p = s->p;
        }

        /* delay line: write the current frame, read the frame L back */
        for (ch = 0; ch < channels; ch++) {
            double v = (double)in[ch][k];
            if (!isfinite(v)) v = 0.0;
            s->ring[(size_t)s->w * channels + ch] = (float)v;
        }
        rd = (s->w + s->ring_frames - L) % s->ring_frames;
        for (ch = 0; ch < channels; ch++)
            out[ch][k] = (float)(s->ring[(size_t)rd * channels + ch] * s->gain);
        if (s->gain < s->blk_min) s->blk_min = s->gain;

        s->w = (s->w + 1) % s->ring_frames;
        s->n++;
    }
}

/* The most gain reduction applied since the last call, in dB >= 0. A gate
 * snapped shut has a gain of exactly zero, which is no finite number of dB;
 * it reports its range, which is what it was asked to close by. */
static inline double gate_take_gr(gate_state *s, const gate_coeffs *c)
{
    double g = s->blk_min;
    s->blk_min = 1.0;
    if (g <= 0.0) return c->range_db;
    return g < 1.0 ? -DSP_LOG10_20 * log(g) : 0.0;
}

#endif /* GATE_DSP_H */
