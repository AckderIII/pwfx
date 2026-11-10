/*
 * level_dsp.h -- the leveler core from reference/level.c, shaped so the
 * PipeWire filter and the offline test can run exactly the same code.
 *
 * Design notes, unchanged from the reference:
 *
 *  - A leveler, not a compressor: it listens to loudness over a fraction of
 *    a second, turns up as readily as it turns down, and moves over
 *    seconds, leaving each syllable's own dynamics alone.
 *  - Loudness is ITU-R BS.1770's: each channel K-weighted (a +4 dB shelf
 *    around 1.7 kHz, then the RLB high-pass at 38 Hz), the channels'
 *    mean squares summed with weight 1, -0.691 + 10 log10 of that in LUFS.
 *    Both stages are designed for the actual rate from the analogue
 *    prototypes behind the standard's 48 kHz table, which they reproduce.
 *  - A rectangular window, as BS.1770's momentary and short-term loudness,
 *    sliding in hops of LEVEL_HOP_MS. Each hop's sum of squares goes into a
 *    ring of the last LEVEL_MAX_HOPS, and the window is re-added from it
 *    every hop rather than kept running, so it cannot drift. The ring is
 *    always written in full, so a new window length is right from the next
 *    hop -- nothing to reset when it changes.
 *  - Gated per hop, as BS.1770's integrated loudness gates its blocks: a
 *    hop that fails the gates is left out of the window's average, so the
 *    window reads the loudness of the sound in it rather than of the sound
 *    diluted by the pauses around it. The end of a phrase is not the
 *    speaker getting quieter, the gain does not climb into every pause, and
 *    a phrase reads at its true loudness from its first hop. With no hop
 *    passing in the window the gain freezes where it is. At startup the
 *    window is silence, so the gain starts at 0 dB and stays there until
 *    there is something to level.
 *  - Two gates, as BS.1770 has. The absolute one, the "gate" parameter,
 *    keeps silence and a quiet room out. The relative one drops a hop more
 *    than LEVEL_REL_GATE_DB under a reference: the mean power of the hops
 *    that passed the absolute gate over the last LEVEL_REF_MS or so (a
 *    one-pole at the hop rate). It is what makes the leveler independent of
 *    where the absolute gate sits. A voice's own room noise rises and falls
 *    with the voice, so no fixed gate is right for the same voice 20 dB
 *    apart: the quiet copy's soft syllables fall under it, or the loud
 *    copy's pauses get over it -- and a pause counted, once the window is
 *    nothing but pause, is the noise floor being ridden up to the target.
 *    Measured against the voice of the last few seconds instead, the room
 *    stays out wherever the absolute gate is. The reference is long and
 *    not the window's own mean for exactly that reason: a window full of
 *    pause would be its own reference. It does follow a speaker who has
 *    turned quieter for good, in about one time constant of their speech.
 *    20 dB, not BS.1770's 10 LU, because a 5 ms hop of speech swings much
 *    further than a 400 ms block, and 10 would drop the consonants.
 *  - So the loudness ridden to is that of the sound while it sounds, ITU-T
 *    P.56's "active" level in LUFS. BS.1770's integrated loudness counts
 *    the short gaps within phrases, and reads the same speech 1-2 dB lower.
 *  - Gain computer, once per hop: strength x (target - window loudness),
 *    capped at max-boost up and max-cut down.
 *  - A one-pole on the gain in dB, per sample: attack while turning down,
 *    release while turning up, 63 % times as in the compressor.
 *  - Lookahead delays the audio against the detector; half the window
 *    centres the measurement on what is being heard.
 *  - One detector, one gain for every channel.
 *  - At exactly 0 dB the gain is exactly 1: nothing to do is bit-exact.
 *
 * The one change for realtime, as for the compressor: the lookahead delay
 * is real rather than absorbed, and is reported to the graph as latency.
 */

#ifndef LEVEL_DSP_H
#define LEVEL_DSP_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

#define LEVEL_MAX_CHANNELS  64
#define LEVEL_HOP_MS        5.0
#define LEVEL_MIN_WINDOW_MS 100.0
#define LEVEL_MAX_WINDOW_MS 3000.0
#define LEVEL_MAX_HOPS      600         /* LEVEL_MAX_WINDOW_MS / LEVEL_HOP_MS */
#define LEVEL_MAX_LOOK_MS   500.0
#define LEVEL_LUFS_OFFSET   (-0.691)
#define LEVEL_REL_GATE_DB   20.0
#define LEVEL_REL_GATE_LIN  0.01        /* LEVEL_REL_GATE_DB as a power */
#define LEVEL_REF_MS        5000.0      /* the relative gate's memory */
#define LEVEL_DB_FLOOR      (-240.0)

typedef struct {
    biquad_t shelf, hp;                 /* K-weighting */
    double   loud, gate, k_str, boost, cut;
    double   aA, aR;
    double   aRef;                      /* the reference's one-pole, per hop */
    int      hop_len, n_hops;           /* samples per hop, hops per window */
    int      L;                         /* lookahead, samples */
} level_coeffs;

typedef struct {
    uint32_t   channels;
    int        max_L;
    bqstate_t *sh_st, *hp_st;           /* channels */
    double     hops[LEVEL_MAX_HOPS];    /* sum of squares per hop, a ring;
                                         * 0 for a hop the gates dropped */
    unsigned char live[LEVEL_MAX_HOPS]; /* 1 for a hop that passed them */
    int        hop_w, hop_n;
    double     hop_acc;
    double     ref;                     /* the relative gate's reference,
                                         * mean square; 0 until a first hop */
    double     want, g;                 /* gain asked for, gain applied, dB */
    int        gated;
    float     *ring;                    /* (max_L + 1) * channels */
    int        ring_frames, w;
} level_state;

/* BS.1770's K-weighting at fs: the high shelf, then the RLB high-pass,
 * whose numerator is 1, -2, 1 as in the standard. */
static inline void level_kweight(biquad_t *shelf, biquad_t *hp, double fs)
{
    double f0 = 1681.974450955533, G = 3.999843853973347;
    double Q = 0.7071752369554196;
    double K = tan(M_PI * f0 / fs);
    double Vh = pow(10.0, G / 20.0);
    double Vb = pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;

    shelf->b0 = (Vh + Vb * K / Q + K * K) / a0;
    shelf->b1 = 2.0 * (K * K - Vh) / a0;
    shelf->b2 = (Vh - Vb * K / Q + K * K) / a0;
    shelf->a1 = 2.0 * (K * K - 1.0) / a0;
    shelf->a2 = (1.0 - K / Q + K * K) / a0;

    f0 = 38.13547087602444;
    Q  = 0.5003270373238773;
    K  = tan(M_PI * f0 / fs);
    a0 = 1.0 + K / Q + K * K;
    hp->b0 = 1.0;
    hp->b1 = -2.0;
    hp->b2 = 1.0;
    hp->a1 = 2.0 * (K * K - 1.0) / a0;
    hp->a2 = (1.0 - K / Q + K * K) / a0;
}

static inline void level_design(level_coeffs *c, double rate, double loud,
                                double strength, double boost, double cut,
                                double gate, double window_ms,
                                double attack_ms, double release_ms,
                                double look_ms)
{
    memset(c, 0, sizeof *c);
    level_kweight(&c->shelf, &c->hp, rate);
    c->loud  = loud;
    c->gate  = gate;
    c->k_str = strength / 100.0;
    c->boost = boost;
    c->cut   = cut;

    c->hop_len = (int)(LEVEL_HOP_MS * rate / 1000.0 + 0.5);
    if (c->hop_len < 1) c->hop_len = 1;
    c->n_hops = (int)(window_ms / LEVEL_HOP_MS + 0.5);
    if (c->n_hops < 1) c->n_hops = 1;
    if (c->n_hops > LEVEL_MAX_HOPS) c->n_hops = LEVEL_MAX_HOPS;

    /* alpha = exp(-1 / (tau * fs)); a zero time gives alpha = 0, instant */
    c->aA = attack_ms  > 0.0 ? exp(-1000.0 / (attack_ms  * rate)) : 0.0;
    c->aR = release_ms > 0.0 ? exp(-1000.0 / (release_ms * rate)) : 0.0;
    c->aRef = exp(-1000.0 * (double)c->hop_len / (LEVEL_REF_MS * rate));
    c->L  = (int)(look_ms * rate / 1000.0 + 0.5);
}

static inline void level_reset(level_state *s)
{
    memset(s->sh_st, 0, s->channels * sizeof *s->sh_st);
    memset(s->hp_st, 0, s->channels * sizeof *s->hp_st);
    memset(s->hops, 0, sizeof s->hops);
    memset(s->live, 0, sizeof s->live);
    memset(s->ring, 0, (size_t)s->ring_frames * s->channels * sizeof *s->ring);
    s->hop_w = s->hop_n = 0;
    s->hop_acc = 0.0;
    s->ref = 0.0;
    s->want = s->g = 0.0;
    s->gated = 1;
    s->w = 0;
}

static inline int level_init(level_state *s, uint32_t channels, double max_rate)
{
    memset(s, 0, sizeof *s);
    s->channels    = channels;
    s->max_L       = (int)(LEVEL_MAX_LOOK_MS * max_rate / 1000.0 + 0.5);
    s->ring_frames = s->max_L + 1;
    s->sh_st = calloc(channels, sizeof *s->sh_st);
    s->hp_st = calloc(channels, sizeof *s->hp_st);
    s->ring  = calloc((size_t)s->ring_frames * channels, sizeof *s->ring);
    if (!s->sh_st || !s->hp_st || !s->ring) return -1;
    level_reset(s);
    return 0;
}

static inline void level_fini(level_state *s)
{
    free(s->sh_st); free(s->hp_st); free(s->ring);
    memset(s, 0, sizeof *s);
}

/* One hop is complete: gate it, then the loudness of the hops in the window
 * that passed, and from it the gain to ride towards -- or, with none, no
 * change. */
static inline void level_hop(level_state *s, const level_coeffs *c)
{
    const double LOG10_10 = 10.0 / M_LN10;
    double sum = 0.0, ms, lufs;
    int j, r, cnt = 0, live = 0;

    ms   = s->hop_acc / (double)c->hop_len;
    lufs = ms > 0.0 ? LEVEL_LUFS_OFFSET + LOG10_10 * log(ms) : LEVEL_DB_FLOOR;
    if (lufs >= c->gate) {
        /* the absolute gate passed it: it moves the reference, and is
         * counted if it is not far under it */
        s->ref = s->ref > 0.0 ? c->aRef * s->ref + (1.0 - c->aRef) * ms : ms;
        live = ms >= s->ref * LEVEL_REL_GATE_LIN;
    }
    s->live[s->hop_w] = (unsigned char)live;
    s->hops[s->hop_w] = live ? s->hop_acc : 0.0;
    s->hop_w = (s->hop_w + 1) % LEVEL_MAX_HOPS;
    s->hop_acc = 0.0;
    s->hop_n = 0;

    for (j = 0, r = s->hop_w; j < c->n_hops; j++) {
        r = (r + LEVEL_MAX_HOPS - 1) % LEVEL_MAX_HOPS;
        sum += s->hops[r];
        cnt += s->live[r];
    }

    s->gated = cnt == 0;
    if (!s->gated) {
        double want;
        ms   = sum / ((double)cnt * (double)c->hop_len);
        lufs = LEVEL_LUFS_OFFSET + LOG10_10 * log(ms);
        want = c->k_str * (c->loud - lufs);
        if (want >  c->boost) want =  c->boost;
        if (want < -c->cut)   want = -c->cut;
        s->want = want;
    }
}

/* One block. out[] lags in[] by c->L samples. */
static inline void level_run(level_state *s, const level_coeffs *c,
                             float *const *in, float *const *out,
                             uint32_t channels, uint32_t n)
{
    uint32_t k, ch;
    int L = c->L;

    if (L > s->max_L) L = s->max_L;

    for (k = 0; k < n; k++) {
        double lin, a;
        int rd;

        /* K-weighted power, summed over the channels */
        for (ch = 0; ch < channels; ch++) {
            double x = (double)in[ch][k];
            double y;
            if (!isfinite(x)) x = 0.0;
            y = biquad_run(&c->hp, &s->hp_st[ch],
                           biquad_run(&c->shelf, &s->sh_st[ch], x));
            s->hop_acc += y * y;
        }
        if (++s->hop_n >= c->hop_len)
            level_hop(s, c);

        /* the gain, in dB: down at the attack, up at the release, and held
         * still while the gate is shut */
        if (!s->gated) {
            a = s->want < s->g ? c->aA : c->aR;
            s->g = a * s->g + (1.0 - a) * s->want;
            if (s->g > -1e-12 && s->g < 1e-12) s->g = 0.0;
        }
        lin = s->g == 0.0 ? 1.0 : exp(s->g * DSP_LN10_20);

        /* delay line: write the current frame, read the frame L back */
        for (ch = 0; ch < channels; ch++) {
            float x = in[ch][k];
            s->ring[(size_t)s->w * channels + ch] = isfinite(x) ? x : 0.0f;
        }
        rd = (s->w + s->ring_frames - L) % s->ring_frames;

        for (ch = 0; ch < channels; ch++)
            out[ch][k] = (float)((double)s->ring[(size_t)rd * channels + ch] * lin);

        s->w = (s->w + 1) % s->ring_frames;
    }
}

/* The gain being applied now, dB: positive turns up, negative down. */
static inline double level_gain_db(const level_state *s)
{
    return s->g;
}

/* The static characteristic: output loudness for a steady input loudness,
 * once the gain has settled. Below the gate there is none -- the gain holds
 * wherever it was -- so the caller leaves those points out. */
static inline double level_curve_db(const level_coeffs *c, double x)
{
    double want = c->k_str * (c->loud - x);
    if (want >  c->boost) want =  c->boost;
    if (want < -c->cut)   want = -c->cut;
    return x + want;
}

#endif /* LEVEL_DSP_H */
