/*
 * pw_notch -- notch filter as a PipeWire filter node: one tone, or a tone
 * and its harmonics, taken out and the band around it left alone.
 *
 * The DSP is the one from reference/notch.c, kept in notch_dsp.h so the test
 * harness runs the very same code: a Butterworth band-stop per notch, its
 * null exactly on the frequency asked for and its -3 dB edges exactly
 * freq/q Hz apart, solved for in the digital domain rather than prewarped
 * at the centre and approximated at the edges. notch_dsp.h has the
 * derivation. Order 1 is the classic second-order notch; higher orders keep
 * the same width at -3 dB with flat shoulders and a wider stop band.
 * harmonics stacks notches at 2 f, 3 f, ... with the same q, for hum.
 *
 * What differs from the stdin/stdout reference is what differs for every
 * filter here: audio from the graph, the graph's rate, everything live.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "notch_dsp.h"

static pwf_param params[] = {
    { .name = "freq", .shortopt = "-f", .type = PWF_DOUBLE,
      .min = 10, .max = 384000, .def = 50, .unit = "Hz",
      .help = "the frequency to remove; the first harmonic" },
    { .name = "q", .shortopt = "-Q", .type = PWF_DOUBLE,
      .min = 0.5, .max = 300, .def = 10,
      .help = "freq / width: the -3 dB edges are freq/q Hz apart" },
    { .name = "order", .shortopt = "-n", .type = PWF_INT,
      .min = 1, .max = NOTCH_MAX_ORDER, .def = 2,
      .help = "steepness; 1 is the classic notch, a V" },
    { .name = "harmonics", .shortopt = "-H", .type = PWF_INT,
      .min = 1, .max = NOTCH_MAX_HARMONICS, .def = 1,
      .help = "how many of freq, 2 freq, 3 freq ... to notch, each at q" },
    PWF_PARAM_END
};

typedef struct {
    notch_t n;
} notch_coeffs;

typedef struct {
    uint32_t   channels;
    bqstate_t *bq;      /* channels * NOTCH_MAX_SECTIONS */
    int        last_ns;
} notch_state;

static int notch_init(void *user, uint32_t channels, double max_rate)
{
    notch_state *st = user;

    (void)max_rate;
    st->channels = channels;
    st->bq = calloc((size_t)channels * NOTCH_MAX_SECTIONS, sizeof *st->bq);
    st->last_ns = -1;
    return st->bq ? 0 : -1;
}

static void notch_fini(void *user)
{
    notch_state *st = user;
    free(st->bq);
    st->bq = NULL;
}

static int notch_validate(void *user, const pwf_param *p, double rate,
                          char *err, size_t errsz)
{
    double f = pwf_value(p, "freq"), q = pwf_value(p, "q");

    (void)user;
    if (f >= rate * NOTCH_MAX_FREQ) {
        snprintf(err, errsz, "must be below %g Hz at %g Hz",
                 rate * NOTCH_MAX_FREQ, rate);
        return -1;
    }
    if (f / q > rate * NOTCH_MAX_WIDTH) {
        snprintf(err, errsz, "a width of %g Hz is wider than %g Hz allows "
                 "at %g Hz; raise q", f / q, rate * NOTCH_MAX_WIDTH, rate);
        return -1;
    }
    return 0;
}

static uint32_t notch_design_cb(void *user, const pwf_param *p, double rate,
                                uint32_t channels, void *coeffs)
{
    notch_coeffs *c = coeffs;

    (void)user; (void)channels;
    /* A frequency that was legal at the old rate can be above the new one's
     * top; notch_design then leaves out every harmonic that is, the first
     * included, and the filter passes the signal untouched. The width is
     * clamped there too. */
    notch_design(&c->n, (int)pwf_value(p, "order"),
                 (int)pwf_value(p, "harmonics"),
                 pwf_value(p, "freq"), pwf_value(p, "q"), rate);
    return 0;                       /* a causal IIR cascade: zero latency */
}

static void notch_process(void *user, const void *coeffs, int changed,
                          float *const *in, float *const *out,
                          uint32_t channels, uint32_t n)
{
    notch_state *st = user;
    const notch_coeffs *c = coeffs;
    uint32_t ch, i;

    /* Sections that were idle under the previous design hold what they last
     * ran with; clear exactly those, as pw_hpf does, and leave the running
     * ones to carry on into their new coefficients. */
    if (changed) {
        if (st->last_ns >= 0 && c->n.nsections > st->last_ns) {
            for (ch = 0; ch < channels; ch++)
                memset(&st->bq[ch * NOTCH_MAX_SECTIONS + st->last_ns], 0,
                       (size_t)(c->n.nsections - st->last_ns) * sizeof *st->bq);
        }
        st->last_ns = c->n.nsections;
    }

    for (ch = 0; ch < channels; ch++) {
        const float *x = in[ch];
        float *y = out[ch];
        bqstate_t *bq = &st->bq[ch * NOTCH_MAX_SECTIONS];

        for (i = 0; i < n; i++) {
            double v = (double)x[i];
            if (!isfinite(v)) v = 0.0;
            y[i] = (float)notch_run(&c->n, bq, v);
        }
    }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* The response of the cascade running, from its coefficients. The shared
 * grid has 16 points an octave, which a notch of q 30 falls between, so each
 * notch adds its centre -- where the curve reaches the floor -- its edges
 * and a point either side between them. */
static size_t notch_curve(void *user, const void *coeffs, double rate,
                          double *x, double *y, size_t max)
{
    const notch_coeffs *c = coeffs;
    const notch_t *d = &c->n;
    size_t i, k, n = pwf_freq_grid(rate, x, max);
    double top = x[n - 1];
    int h;

    (void)user;
    for (h = 0; h < d->nnotches; h++) {
        double pts[5] = { d->lo[h], sqrt(d->lo[h] * d->f0[h]), d->f0[h],
                          sqrt(d->f0[h] * d->hi[h]), d->hi[h] };
        for (k = 0; k < 5 && n < max; k++)
            if (pts[k] >= x[0] && pts[k] <= top) x[n++] = pts[k];
    }
    qsort(x, n, sizeof *x, cmp_double);
    for (i = k = 0; i < n; i++)
        if (k == 0 || x[i] != x[k - 1]) x[k++] = x[i];
    for (i = 0; i < k; i++) y[i] = notch_mag_db(d, x[i], rate);
    return k;
}

int main(int argc, char **argv)
{
    static notch_state state;
    pwf_desc desc = {
        .name       = "pw_notch",
        .summary    = "notch filter: a tone and its harmonics, removed",
        .notes      =
            "The null sits exactly on freq and is as deep as the arithmetic\n"
            "allows; the -3 dB edges are exactly freq/q Hz apart at any\n"
            "rate. order 1 is the classic notch, a V that starts cutting well\n"
            "outside its width; order 2 and up keep the same width at -3 dB\n"
            "with flat shoulders and a wider bottom, for a tone that wanders.\n"
            "For mains hum: freq 50 (or 60), q 10-30, harmonics 4-8. The\n"
            "narrower and steeper the notch, the longer it rings: a q 100\n"
            "notch at 50 Hz takes a good fraction of a second to settle, and\n"
            "a live change to freq on one that narrow rings briefly.",
        .params     = params,
        .coeff_size = sizeof(notch_coeffs),
        .init       = notch_init,
        .fini       = notch_fini,
        .validate   = notch_validate,
        .design     = notch_design_cb,
        .process    = notch_process,
        .curve_kind = PWF_CURVE_FREQ,
        .curve      = notch_curve,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
