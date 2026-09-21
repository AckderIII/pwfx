/*
 * pw_lpf -- Butterworth low-pass filter as a PipeWire filter node.
 *
 * The DSP is the one from reference/lpf.c: an order-N Butterworth built as a
 * cascade of second-order sections (plus a first-order section for odd N),
 * each designed analog-prototype -> bilinear transform with the cutoff
 * prewarped, so the -3 dB point lands exactly where it was asked for.
 * Coefficients and state are double precision: at a low cutoff the numerator
 * gain (1 - cos w0)/2 shrinks as w0^2/4 while the poles sit within 0.01 of
 * z = 1, and a tiny numerator feeding a near-integrating denominator is
 * exactly the arrangement that loses precision in single precision.
 *
 * What differs from the stdin/stdout reference: audio comes from the graph
 * rather than a pipe, the sample rate is whatever the graph is running at
 * rather than a command-line number, and cutoff and order can be changed
 * while audio is flowing.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "dsp.h"

static pwf_param params[] = {
    { .name = "cutoff", .shortopt = "-f", .type = PWF_DOUBLE,
      .min = 1, .max = 384000, .def = 12000, .unit = "Hz",
      .help = "cutoff frequency, the -3 dB point" },
    { .name = "order", .shortopt = "-n", .type = PWF_INT,
      .min = 1, .max = DSP_MAX_ORDER, .def = 4,
      .help = "filter order; rolloff is 6 dB/octave per order" },
    PWF_PARAM_END
};

typedef struct {
    butter_t b;
} lpf_coeffs;

typedef struct {
    uint32_t   channels;
    bqstate_t *bq;      /* channels * DSP_MAX_SECTIONS */
    opstate_t *op;      /* channels */
    int        last_ns, last_first;
} lpf_state;

static int lpf_init(void *user, uint32_t channels, double max_rate)
{
    lpf_state *st = user;

    (void)max_rate;
    st->channels = channels;
    st->bq = calloc((size_t)channels * DSP_MAX_SECTIONS, sizeof *st->bq);
    st->op = calloc(channels, sizeof *st->op);
    st->last_ns = -1;
    st->last_first = -1;
    return (st->bq && st->op) ? 0 : -1;
}

static void lpf_fini(void *user)
{
    lpf_state *st = user;
    free(st->bq);
    free(st->op);
    st->bq = NULL;
    st->op = NULL;
}

static int lpf_validate(void *user, const pwf_param *p, double rate,
                        char *err, size_t errsz)
{
    double fc = pwf_value(p, "cutoff");

    (void)user;
    if (fc >= rate / 2.0) {
        snprintf(err, errsz, "must be below Nyquist (%g Hz at %g Hz)",
                 rate / 2.0, rate);
        return -1;
    }
    return 0;
}

static uint32_t lpf_design(void *user, const pwf_param *p, double rate,
                           uint32_t channels, void *coeffs)
{
    lpf_coeffs *c = coeffs;
    double fc = pwf_value(p, "cutoff");
    int order = (int)pwf_value(p, "order");

    (void)user; (void)channels;

    /* The graph can move to a lower rate under a cutoff that was legal at
     * the old one; validate() cannot catch that, so clamp here. */
    if (fc > rate * 0.49) fc = rate * 0.49;

    butter_design(&c->b, order, fc, rate, 0);
    return 0;                       /* a causal IIR cascade: zero latency */
}

static void lpf_process(void *user, const void *coeffs, int changed,
                        float *const *in, float *const *out,
                        uint32_t channels, uint32_t n)
{
    lpf_state *st = user;
    const lpf_coeffs *c = coeffs;
    uint32_t ch, i;

    /* A section that was inactive under the previous order holds whatever it
     * last ran with. Starting it from that state would ring out an old
     * signal; starting the already-active ones from zero would cut the
     * current one off. So clear exactly the sections that just came back. */
    if (changed) {
        if (st->last_ns >= 0 && c->b.nsections > st->last_ns) {
            for (ch = 0; ch < channels; ch++)
                memset(&st->bq[ch * DSP_MAX_SECTIONS + st->last_ns], 0,
                       (size_t)(c->b.nsections - st->last_ns) * sizeof *st->bq);
        }
        if (c->b.has_first && st->last_first == 0)
            memset(st->op, 0, channels * sizeof *st->op);
        st->last_ns    = c->b.nsections;
        st->last_first = c->b.has_first;
    }

    for (ch = 0; ch < channels; ch++) {
        const float *x = in[ch];
        float *y = out[ch];
        bqstate_t *bq = &st->bq[ch * DSP_MAX_SECTIONS];
        opstate_t *op = &st->op[ch];

        for (i = 0; i < n; i++) {
            double v = (double)x[i];
            if (!isfinite(v)) v = 0.0;
            y[i] = (float)butter_run(&c->b, bq, op, v);
        }
    }
}

int main(int argc, char **argv)
{
    static lpf_state state;
    pwf_desc desc = {
        .name       = "pw_lpf",
        .summary    = "Butterworth low-pass filter",
        .notes      =
            "Rolloff is 6 dB/octave per order: order 2 gives 12 dB/oct,\n"
            "order 4 gives 24 dB/oct. The response follows the analog\n"
            "Butterworth curve 1/sqrt(1+(f/fc)^2N) closely up to about half\n"
            "of Nyquist and then falls away faster, reaching -inf at Nyquist\n"
            "itself; that is inherent to the bilinear transform. Nothing is\n"
            "clipped -- the stream stays float, so put pw_limit after a\n"
            "stage that can overshoot.",
        .params     = params,
        .coeff_size = sizeof(lpf_coeffs),
        .init       = lpf_init,
        .fini       = lpf_fini,
        .validate   = lpf_validate,
        .design     = lpf_design,
        .process    = lpf_process,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
