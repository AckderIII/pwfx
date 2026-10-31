/*
 * denoise_dsp.h -- the noise suppressor core from reference/denoise.c,
 * shaped so the PipeWire filter and the offline test run the same code.
 *
 * The suppression itself is RNNoise (third_party/rnnoise, BSD-3), a
 * recurrent network that estimates a gain per band from the signal's
 * spectrum, pitch and the network's memory of what it has heard: it tells
 * a voice from a fan, a keyboard or traffic, steady or not, where a gate or
 * a spectral subtractor only knows loud from quiet. What is built around it
 * here:
 *
 *  - Framing. RNNoise takes frames of DN_FRAME samples at 48 kHz. A block
 *    the graph hands over is pushed into the frame being filled, and the
 *    output for it comes from the frame finished before -- so the output is
 *    one frame late, whatever the block size, and never has to wait.
 *    RNNoise's own output is DN_RNN_DELAY frames late on top of that -- two,
 *    measured: it synthesises the previous call's spectrum, and its
 *    overlapping 960-sample windows add another frame. Reading its code
 *    suggested one; tests/denoise.py correlates its output with real speech
 *    and found two. The total is DN_LATENCY, three frames: 30 ms, reported
 *    to the graph.
 *  - The reduction is a limit, not a switch:
 *
 *        y = wet + f * (dry - wet),    f = 10^(-reduction / 20)
 *
 *    with dry the input aligned with wet -- DN_RNN_DELAY frames back, or
 *    the mix is a comb. Where RNNoise takes the noise out
 *    entirely, what is left is the noise at -reduction dB: a room that
 *    still sounds like a room. Where it leaves the voice alone, wet is dry
 *    and so is y. At reduction 0 it is the input, delayed.
 *  - An optional gate on RNNoise's voice-activity estimate: frames it is
 *    less than vad-threshold sure are speech are let through at f * dry,
 *    held open for DN_HOLD_FRAMES after the last one that was, and ramped
 *    across a frame either way so the switch cannot click.
 *  - One RNNoise state per channel; the gate goes by the most voice-like
 *    channel, so it opens and closes on all of them together.
 *  - RNNoise expects samples on a 16-bit scale; they are scaled to it and
 *    back around each call.
 *
 * RNNoise allocates its state in rnnoise_create() and nothing per frame,
 * so all of it is allocated in denoise_init() and process() only computes.
 */

#ifndef DENOISE_DSP_H
#define DENOISE_DSP_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rnnoise.h"

#define DN_RATE         48000
#define DN_FRAME        480
#define DN_RNN_DELAY    2               /* frames RNNoise's output is late */
#define DN_LATENCY      ((1 + DN_RNN_DELAY) * DN_FRAME)
#define DN_HOLD_FRAMES  20              /* 200 ms */
#define DN_MAX_CHANNELS 64

typedef struct {
    double f;          /* what noise is left at: 10^(-reduction/20) */
    double vad_thr;    /* 0 = no gate */
    int    pass;       /* not at 48 kHz: pass the input through */
} denoise_coeffs;

typedef struct {
    uint32_t      channels;
    DenoiseState *st[DN_MAX_CHANNELS];
    float        *in_cur;     /* channels * DN_FRAME: the frame being filled */
    float        *in_prev;    /* channels * DN_FRAME * DN_RNN_DELAY: the frames
                               * before, oldest first -- the oldest is dry,
                               * aligned with wet */
    float        *out_frame;  /* what is played out while in_cur fills */
    float         scratch[2][DN_FRAME];
    int           pos;
    double        w;          /* gate: 1 open, 0 shut, at the last frame's end */
    int           hold;
    double        blk_gr;     /* most reduction since denoise_take_gr, dB */
} denoise_state;

static inline void denoise_design(denoise_coeffs *c, double rate,
                                  double reduction_db, double vad_thr)
{
    memset(c, 0, sizeof *c);
    c->f       = pow(10.0, -reduction_db / 20.0);
    c->vad_thr = vad_thr;
    c->pass    = rate != DN_RATE;
}

static inline void denoise_fini(denoise_state *s)
{
    uint32_t ch;
    for (ch = 0; ch < s->channels && ch < DN_MAX_CHANNELS; ch++)
        if (s->st[ch]) rnnoise_destroy(s->st[ch]);
    free(s->in_cur); free(s->in_prev); free(s->out_frame);
    memset(s, 0, sizeof *s);
}

static inline int denoise_init(denoise_state *s, uint32_t channels)
{
    uint32_t ch;

    memset(s, 0, sizeof *s);
    if (channels > DN_MAX_CHANNELS) return -1;
    s->channels  = channels;
    s->in_cur    = calloc((size_t)channels * DN_FRAME, sizeof *s->in_cur);
    s->in_prev   = calloc((size_t)channels * DN_FRAME * DN_RNN_DELAY,
                          sizeof *s->in_prev);
    s->out_frame = calloc((size_t)channels * DN_FRAME, sizeof *s->out_frame);
    if (!s->in_cur || !s->in_prev || !s->out_frame) return -1;
    for (ch = 0; ch < channels; ch++)
        if (!(s->st[ch] = rnnoise_create(NULL))) return -1;
    s->w = 1.0;
    return 0;
}

/* One frame: denoise in_cur, mix it with the dry frame it lines up with,
 * DN_RNN_DELAY back, into out_frame. */
static inline void denoise_frame(denoise_state *s, const denoise_coeffs *c)
{
    uint32_t ch, n = s->channels;
    double vad = 0.0, target, ed = 0.0, eo = 0.0;
    int j;

    for (ch = 0; ch < n; ch++) {
        float *in = s->in_cur + (size_t)ch * DN_FRAME;
        float *wet = s->out_frame + (size_t)ch * DN_FRAME;   /* reused below */
        double v;
        for (j = 0; j < DN_FRAME; j++) s->scratch[0][j] = in[j] * 32768.0f;
        v = rnnoise_process_frame(s->st[ch], s->scratch[1], s->scratch[0]);
        if (v > vad) vad = v;
        for (j = 0; j < DN_FRAME; j++) wet[j] = s->scratch[1][j];
    }

    /* the gate, on the most voice-like channel */
    if (c->vad_thr > 0.0) {
        if (vad >= c->vad_thr) {
            s->hold = DN_HOLD_FRAMES;
            target = 1.0;
        } else if (s->hold > 0) {
            s->hold--;
            target = 1.0;
        } else {
            target = 0.0;
        }
    } else {
        target = 1.0;
    }

    for (ch = 0; ch < n; ch++) {
        float *out = s->out_frame + (size_t)ch * DN_FRAME;
        const float *dry_f = s->in_prev + (size_t)ch * DN_FRAME * DN_RNN_DELAY;
        for (j = 0; j < DN_FRAME; j++) {
            double wj  = s->w + (target - s->w) * (double)(j + 1) / DN_FRAME;
            double wet = (double)out[j] / 32768.0;
            double dry = (double)dry_f[j];
            double y   = wj * (wet + c->f * (dry - wet)) + (1.0 - wj) * c->f * dry;
            ed += dry * dry;
            eo += y * y;
            out[j] = (float)y;
        }
    }
    s->w = target;
    /* shift the history: drop the oldest frame, append in_cur */
    for (ch = 0; ch < n; ch++) {
        float *h = s->in_prev + (size_t)ch * DN_FRAME * DN_RNN_DELAY;
        memmove(h, h + DN_FRAME, (size_t)(DN_RNN_DELAY - 1) * DN_FRAME * sizeof *h);
        memcpy(h + (size_t)(DN_RNN_DELAY - 1) * DN_FRAME,
               s->in_cur + (size_t)ch * DN_FRAME, DN_FRAME * sizeof *h);
    }

    /* the frame's reduction, for the gr meter: how much quieter it came
     * out than the dry signal went in, never negative */
    if (ed > 1e-12 && eo > 0.0) {
        double gr = 10.0 * log10(ed / eo);
        if (gr > s->blk_gr) s->blk_gr = gr;
    }
}

/* One block. out[] lags in[] by DN_LATENCY samples, unless passing. */
static inline void denoise_run(denoise_state *s, const denoise_coeffs *c,
                               float *const *in, float *const *out,
                               uint32_t channels, uint32_t n)
{
    uint32_t k, ch;

    if (c->pass) {
        for (ch = 0; ch < channels; ch++)
            if (out[ch] != in[ch]) memcpy(out[ch], in[ch], n * sizeof **out);
        return;
    }
    for (k = 0; k < n; k++) {
        for (ch = 0; ch < channels; ch++) {
            float x = in[ch][k];
            if (!isfinite(x)) x = 0.0f;
            s->in_cur[(size_t)ch * DN_FRAME + s->pos] = x;
            out[ch][k] = s->out_frame[(size_t)ch * DN_FRAME + s->pos];
        }
        if (++s->pos == DN_FRAME) {
            denoise_frame(s, c);
            s->pos = 0;
        }
    }
}

static inline double denoise_take_gr(denoise_state *s)
{
    double g = s->blk_gr;
    s->blk_gr = 0.0;
    return g;
}

#endif /* DENOISE_DSP_H */
