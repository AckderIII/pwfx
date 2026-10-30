/*
 * deess_dsp.h -- the de-esser core from reference/deess.c, shaped so the
 * PipeWire filter and the offline test can run exactly the same code.
 *
 * Design notes, unchanged from the reference:
 *
 *  - Split-band reduction as a subtractive dynamic EQ:
 *
 *        y = x - (1 - g) * bp(x)
 *
 *    where bp is a constant-0 dB-peak band-pass at the sibilance frequency
 *    and g the gain the detector asks for. At the band's centre bp has unity
 *    gain and zero phase, so the cut there is exactly g; away from it the
 *    cut falls off with the band's skirt, so the body of the voice is left
 *    alone. And at g = 1 the second term is exactly zero: a de-esser that is
 *    not working passes its input through bit for bit. A crossover split
 *    would instead put an allpass phase shift on everything, all the time.
 *  - "wide" mode applies g to the whole signal instead -- the classic
 *    broadband de-esser, for material where a band cut sounds lispy.
 *  - Detection on the band, as an RMS: a sibilant is noise, and its peaks
 *    say little about how loud it sounds. The band and the whole signal are
 *    averaged over the same DEESS_RMS_MS, and that is a choice between two
 *    faults. Shorter, the whole signal's level follows the waveform of the
 *    voice's fundamental -- a 200 Hz tone swings a 1 ms RMS by 2 dB -- and
 *    the peak-holding smoothing cuts by the top of each swing, deeper the
 *    lower the voice. Averaged over different times, the faster of the two
 *    leads at every onset, and every word starts by reading as sibilance.
 *    Over the same 5 ms both rise together, and a 100 Hz fundamental
 *    swings the level by about 0.3 dB; the default lookahead covers the
 *    window, so the cut is still in place when the "s" arrives.
 *  - Absolute or relative detection. "abs" compares the band's level against
 *    the threshold. "rel" compares the band's level *relative to the whole
 *    signal's*, so a loud vowel with a bright edge does not set it off and a
 *    quiet "s" still does -- the reason most good de-essers default to it.
 *    Below DEESS_REL_FLOOR the band is treated as silence in "rel" mode;
 *    otherwise silence, at the floor in both, would read as 0 dB of
 *    sibilance and hold the de-esser down into the next word.
 *  - The gain computer is the compressor's: a soft knee of DEESS_KNEE_DB, a
 *    ratio, and the reduction capped at "range", so even a violent "s" is
 *    cut by no more than asked.
 *  - The reduction is smoothed in dB by the compressor's smooth decoupled
 *    peak detector (attack and release as 63 % time constants), and the
 *    audio is delayed by the lookahead so the cut is in place when the
 *    sibilant arrives rather than an attack-time after.
 *  - Channel-linked: one detector on the loudest channel's band, one gain.
 *    A de-esser that ducked one side of a stereo voice on every "s" would
 *    pull the image across.
 *  - "listen" outputs the band alone, delayed like everything else: what
 *    the detector hears, for finding the frequency by ear.
 *  - Filter, detector and gain state are double; the band's delay line is
 *    double too, so the subtraction is done at full precision.
 *
 * The one change for realtime, as for the compressor: the lookahead delay
 * is real rather than absorbed, and is reported to the graph as latency.
 */

#ifndef DEESS_DSP_H
#define DEESS_DSP_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

#define DEESS_MAX_CHANNELS 64
#define DEESS_MAX_LOOK_MS  10.0
#define DEESS_RMS_MS       5.0
#define DEESS_KNEE_DB      6.0
#define DEESS_REL_FLOOR    (-60.0)
#define DEESS_DB_FLOOR     (-240.0)

enum { DEESS_DETECT_REL = 0, DEESS_DETECT_ABS = 1 };
enum { DEESS_MODE_SPLIT = 0, DEESS_MODE_WIDE = 1 };

typedef struct {
    biquad_t bp;
    double   thr_db, ratio, range;
    double   aA, aR, aRms;
    int      detect, mode, listen;
    int      L;                         /* lookahead, samples */
} deess_coeffs;

typedef struct {
    uint32_t   channels;
    int        max_L;
    bqstate_t *bp_st;                   /* channels */
    double     acc_b, acc_x;            /* mean squares, linked */
    double     y1, yL;                  /* gain reduction state, dB */
    float     *ring_x;                  /* (max_L + 1) * channels */
    double    *ring_b;                  /* (max_L + 1) * channels */
    int        ring_frames, w;
    double     blk_gr;                  /* most reduction since deess_take_gr */
} deess_state;

/* The compressor's static characteristic: output level in dB for an input
 * level in dB, identity below the knee, 1/ratio above, C1 across it. */
static inline double deess_curve_db(double x, double thr, double ratio,
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

/* RBJ band-pass, constant 0 dB peak gain: unity and zero phase at f0. */
static inline void deess_bp(biquad_t *s, double f0, double fs, double q)
{
    double w0 = 2.0 * M_PI * f0 / fs, cw = cos(w0);
    double alpha = sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
    s->b0 = alpha / a0;  s->b1 = 0.0;  s->b2 = -alpha / a0;
    s->a1 = -2.0 * cw / a0;  s->a2 = (1.0 - alpha) / a0;
}

static inline void deess_design(deess_coeffs *c, double rate,
                                double freq, double q, double thr_db,
                                double ratio, double range,
                                double attack_ms, double release_ms,
                                double look_ms, int detect, int mode,
                                int listen)
{
    memset(c, 0, sizeof *c);
    deess_bp(&c->bp, freq, rate, q);
    c->thr_db = thr_db;
    c->ratio  = ratio;
    c->range  = range;
    c->detect = detect;
    c->mode   = mode;
    c->listen = listen;
    c->L      = (int)(look_ms * rate / 1000.0 + 0.5);

    /* alpha = exp(-1 / (tau * fs)); a zero time gives alpha = 0, instant */
    c->aA   = attack_ms  > 0.0 ? exp(-1000.0 / (attack_ms  * rate)) : 0.0;
    c->aR   = release_ms > 0.0 ? exp(-1000.0 / (release_ms * rate)) : 0.0;
    c->aRms = exp(-1000.0 / (DEESS_RMS_MS * rate));
}

static inline void deess_reset(deess_state *s)
{
    memset(s->bp_st, 0, s->channels * sizeof *s->bp_st);
    memset(s->ring_x, 0, (size_t)s->ring_frames * s->channels * sizeof *s->ring_x);
    memset(s->ring_b, 0, (size_t)s->ring_frames * s->channels * sizeof *s->ring_b);
    s->acc_b = s->acc_x = 0.0;
    s->y1 = s->yL = 0.0;
    s->w = 0;
    s->blk_gr = 0.0;
}

static inline int deess_init(deess_state *s, uint32_t channels, double max_rate)
{
    memset(s, 0, sizeof *s);
    s->channels    = channels;
    s->max_L       = (int)(DEESS_MAX_LOOK_MS * max_rate / 1000.0 + 0.5);
    s->ring_frames = s->max_L + 1;
    s->bp_st  = calloc(channels, sizeof *s->bp_st);
    s->ring_x = calloc((size_t)s->ring_frames * channels, sizeof *s->ring_x);
    s->ring_b = calloc((size_t)s->ring_frames * channels, sizeof *s->ring_b);
    if (!s->bp_st || !s->ring_x || !s->ring_b) return -1;
    deess_reset(s);
    return 0;
}

static inline void deess_fini(deess_state *s)
{
    free(s->bp_st); free(s->ring_x); free(s->ring_b);
    memset(s, 0, sizeof *s);
}

/* One block. out[] lags in[] by c->L samples. */
static inline void deess_run(deess_state *s, const deess_coeffs *c,
                             float *const *in, float *const *out,
                             uint32_t channels, uint32_t n)
{
    const double LOG10_10 = 10.0 / M_LN10;
    double b[DEESS_MAX_CHANNELS];
    uint32_t k, ch;
    int L = c->L;

    if (L > s->max_L) L = s->max_L;

    for (k = 0; k < n; k++) {
        double sb = 0.0, sx = 0.0, bdb, xdb, det, xL, g;
        int rd;

        /* the band, per channel, and the loudest channel's squares */
        for (ch = 0; ch < channels; ch++) {
            double x = (double)in[ch][k];
            if (!isfinite(x)) x = 0.0;
            b[ch] = biquad_run(&c->bp, &s->bp_st[ch], x);
            if (b[ch] * b[ch] > sb) sb = b[ch] * b[ch];
            if (x * x > sx) sx = x * x;
        }

        /* RMS of each, in dB */
        s->acc_b = dsp_flush(c->aRms * s->acc_b + (1.0 - c->aRms) * sb);
        s->acc_x = dsp_flush(c->aRms * s->acc_x + (1.0 - c->aRms) * sx);
        bdb = s->acc_b > 0.0 ? LOG10_10 * log(s->acc_b) : DEESS_DB_FLOOR;
        xdb = s->acc_x > 0.0 ? LOG10_10 * log(s->acc_x) : DEESS_DB_FLOOR;
        if (bdb < DEESS_DB_FLOOR) bdb = DEESS_DB_FLOOR;
        if (xdb < DEESS_DB_FLOOR) xdb = DEESS_DB_FLOOR;

        if (c->detect == DEESS_DETECT_ABS)
            det = bdb;
        else
            det = bdb < DEESS_REL_FLOOR ? DEESS_DB_FLOOR : bdb - xdb;

        /* reduction the static curve asks for, capped at the range */
        xL = det - deess_curve_db(det, c->thr_db, c->ratio, DEESS_KNEE_DB);
        if (xL < 0.0) xL = 0.0;
        if (xL > c->range) xL = c->range;

        /* smooth decoupled peak detector, in the log domain */
        {
            double rel = c->aR * s->y1 + (1.0 - c->aR) * xL;
            s->y1 = xL > rel ? xL : rel;
            s->yL = c->aA * s->yL + (1.0 - c->aA) * s->y1;
        }
        if (s->yL < 1e-12) s->yL = 0.0;
        if (s->yL > s->blk_gr) s->blk_gr = s->yL;
        g = s->yL == 0.0 ? 1.0 : exp(-s->yL * DSP_LN10_20);

        /* delay lines: write the current frame, read the frame L back */
        for (ch = 0; ch < channels; ch++) {
            double x = (double)in[ch][k];
            if (!isfinite(x)) x = 0.0;
            s->ring_x[(size_t)s->w * channels + ch] = (float)x;
            s->ring_b[(size_t)s->w * channels + ch] = b[ch];
        }
        rd = (s->w + s->ring_frames - L) % s->ring_frames;

        for (ch = 0; ch < channels; ch++) {
            double xd = (double)s->ring_x[(size_t)rd * channels + ch];
            double bd = s->ring_b[(size_t)rd * channels + ch];
            double y;

            if (c->listen)
                y = bd;
            else if (c->mode == DEESS_MODE_WIDE)
                y = xd * g;
            else
                y = xd - (1.0 - g) * bd;
            out[ch][k] = (float)y;
        }

        s->w = (s->w + 1) % s->ring_frames;
    }
}

/* The most reduction applied since the last call, in dB >= 0. */
static inline double deess_take_gr(deess_state *s)
{
    double g = s->blk_gr;
    s->blk_gr = 0.0;
    return g;
}

/* The response at full reduction, for the curve: where it cuts and how
 * deep, when it is cutting as hard as "range" lets it. Split mode is
 * |1 - (1 - g) H(f)| with H the band-pass, complex, since the band's phase
 * away from the centre is what shapes the skirt; wide mode is flat at the
 * range; listening, it is the band itself. */
static inline double deess_full_cut_db(const deess_coeffs *c, double f,
                                       double fs)
{
    double gmin = exp(-c->range * DSP_LN10_20);
    double w = 2.0 * M_PI * f / fs;
    double cw = cos(w), c2w = cos(2.0 * w), sw = sin(w), s2w = sin(2.0 * w);
    double nr = c->bp.b0 + c->bp.b1 * cw + c->bp.b2 * c2w;
    double ni =          - c->bp.b1 * sw - c->bp.b2 * s2w;
    double dr = 1.0      + c->bp.a1 * cw + c->bp.a2 * c2w;
    double di =          - c->bp.a1 * sw - c->bp.a2 * s2w;
    double den = dr * dr + di * di;
    double hr = (nr * dr + ni * di) / den, hi = (ni * dr - nr * di) / den;
    double k = 1.0 - gmin, yr, yi;

    if (c->listen)
        return 10.0 * log10(hr * hr + hi * hi);
    if (c->mode == DEESS_MODE_WIDE)
        return -c->range;
    yr = 1.0 - k * hr;
    yi = -k * hi;
    return 10.0 * log10(yr * yr + yi * yi);
}

#endif /* DEESS_DSP_H */
