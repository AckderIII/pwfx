/*
 * delay_dsp.h -- the delay core from reference/delay.c, shaped so the
 * PipeWire filter and the offline test can run exactly the same code.
 *
 * One ring per channel, holding w = x + feedback * d, where d is the ring
 * read D samples back. The output is dry * x + wet * d. With no feedback
 * and all wet that is a plain delay, for lining one source up with another;
 * with feedback it is an echo whose k-th repeat is wet * feedback^(k-1).
 *
 * The ring and the arithmetic are float, as in the reference: the stream is
 * float, and the only thing accumulated is the feedback, which is capped
 * below 1 so it always decays.
 *
 * The one change for realtime is that the time can move while audio flows.
 * Jumping the read position would click, so a change crossfades from the
 * old tap to the new one over DELAY_XFADE_MS. A change that lands mid-fade
 * starts the next fade from whichever tap is louder at that moment, which
 * keeps a dragged slider from stacking up steps; it is still a jump between
 * two points in the past, and is audible as one, as it is in any delay.
 */

#ifndef DELAY_DSP_H
#define DELAY_DSP_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

#define DELAY_MAX_MS   2000.0
#define DELAY_XFADE_MS 10.0

typedef struct {
    int    D;              /* delay, samples */
    double fb;             /* feedback, 0..0.95 */
    double wet, dry;
    int    xf;             /* crossfade length on a time change, samples */
} delay_coeffs;

typedef struct {
    uint32_t channels;
    int      max_D;
    float   *ring;         /* (max_D + 1) * channels */
    int      ring_frames, w;
    int      D_old;        /* tap being faded out */
    int      xf_left;      /* samples of crossfade still to go */
    int      last_D;       /* -1 before the first block */
} delay_state;

static inline void delay_design(delay_coeffs *c, double rate, double time_ms,
                                double feedback_pct, double mix_pct)
{
    memset(c, 0, sizeof *c);
    c->D   = (int)(time_ms * rate / 1000.0 + 0.5);
    c->fb  = feedback_pct / 100.0;
    c->wet = mix_pct / 100.0;
    c->dry = 1.0 - c->wet;
    c->xf  = (int)(DELAY_XFADE_MS * rate / 1000.0) + 1;
}

static inline void delay_reset(delay_state *s)
{
    memset(s->ring, 0, (size_t)s->ring_frames * s->channels * sizeof *s->ring);
    s->w = 0;
    s->xf_left = 0;
    s->last_D = -1;
}

static inline int delay_init(delay_state *s, uint32_t channels, double max_rate)
{
    memset(s, 0, sizeof *s);
    s->channels    = channels;
    s->max_D       = (int)(DELAY_MAX_MS * max_rate / 1000.0 + 0.5);
    s->ring_frames = s->max_D + 1;
    s->ring = calloc((size_t)s->ring_frames * channels, sizeof *s->ring);
    if (!s->ring) return -1;
    delay_reset(s);
    return 0;
}

static inline void delay_fini(delay_state *s)
{
    free(s->ring);
    memset(s, 0, sizeof *s);
}

/* What the ring holds D samples back for channel ch; at D = 0, the input
 * itself, so a zero time is a straight passthrough. */
static inline float delay_tap(const delay_state *s, int D, uint32_t ch,
                              float x)
{
    int rd;
    if (D == 0) return x;
    rd = (s->w + s->ring_frames - D) % s->ring_frames;
    return s->ring[(size_t)rd * s->channels + ch];
}

/* One block. A changed time starts a crossfade from the tap in use. */
static inline void delay_run(delay_state *s, const delay_coeffs *c,
                             float *const *in, float *const *out,
                             uint32_t channels, uint32_t n)
{
    uint32_t k, ch;
    int D = c->D;
    double fb;

    if (D > s->max_D) D = s->max_D;
    /* At no delay there is no loop to feed back around. */
    fb = D > 0 ? c->fb : 0.0;

    if (s->last_D < 0) {
        s->last_D = D;                 /* the first time is in force at once */
    } else if (D != s->last_D) {
        /* Mid-fade, the louder of the two taps is the one to leave from:
         * the old one while it still has at least half the weight, the one
         * being faded to otherwise. */
        if (!(s->xf_left > 0 && s->xf_left * 2 >= c->xf))
            s->D_old = s->last_D;
        s->xf_left = c->xf;
        s->last_D  = D;
    }

    for (k = 0; k < n; k++) {
        double a = s->xf_left > 0 ? (double)s->xf_left / (double)c->xf : 0.0;

        for (ch = 0; ch < channels; ch++) {
            float x = in[ch][k];
            float d;

            if (!isfinite(x)) x = 0.0f;
            d = delay_tap(s, D, ch, x);
            if (a > 0.0)
                d = (float)(a * delay_tap(s, s->D_old, ch, x) + (1.0 - a) * d);

            s->ring[(size_t)s->w * channels + ch] =
                (float)((double)x + fb * (double)d);
            out[ch][k] = (float)(c->dry * (double)x + c->wet * (double)d);
        }
        if (s->xf_left > 0) s->xf_left--;
        s->w = (s->w + 1) % s->ring_frames;
    }
}

#endif /* DELAY_DSP_H */
