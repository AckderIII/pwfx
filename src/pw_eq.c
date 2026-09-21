/*
 * pw_eq -- 5-band parametric EQ as a PipeWire filter node.
 *
 * The DSP is the one from reference/eq.c: low shelf, three peaking bells,
 * high shelf, cascaded as transposed direct form II biquads in double
 * precision. The bells use Orfanidis' prescribed-Nyquist-gain design so they
 * keep the shape they were asked for near the top of the band instead of
 * cramping; the shelves use the RBJ cookbook design, which is exact at
 * Nyquist by construction. See src/dsp.h for the design formulas.
 *
 * Where the reference takes one packed "FREQ:GAIN:Q" argument per band, this
 * exposes the three numbers as separate parameters, so each is one line of
 * the control protocol and can be nudged on its own while audio is flowing.
 *
 *   reference:  eq --ls 90:-4:0.7 --b3 2500:3:0.9
 *   here:       ls-freq 90 / ls-gain -4 / ls-slope 0.7
 *               b3-freq 2500 / b3-gain 3 / b3-q 0.9
 *
 * A band with 0 dB of gain is skipped entirely, so an all-flat EQ is a
 * bit-exact passthrough rather than five identity biquads' worth of
 * rounding.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "dsp.h"

#define NBANDS 5

static const char *const bell_kinds[] = { "orfanidis", "rbj", NULL };

/* Band k lives at parameter indices 3k..3k+2, in this order. */
enum {
    P_LS_F, P_LS_G, P_LS_S,
    P_B2_F, P_B2_G, P_B2_Q,
    P_B3_F, P_B3_G, P_B3_Q,
    P_B4_F, P_B4_G, P_B4_Q,
    P_HS_F, P_HS_G, P_HS_S,
    P_BELLS
};

static pwf_param params[] = {
    { .name = "ls-freq",  .type = PWF_DOUBLE, .min = 1, .max = 384000,
      .def = 100, .unit = "Hz", .help = "low shelf corner frequency" },
    { .name = "ls-gain",  .type = PWF_DOUBLE, .min = -40, .max = 40,
      .def = 0, .unit = "dB", .help = "low shelf gain; 0 skips the band" },
    { .name = "ls-slope", .type = PWF_DOUBLE, .min = 0.05, .max = 10,
      .def = 0.7, .help = "low shelf slope S; 1 is the steepest monotonic one" },

    { .name = "b2-freq",  .type = PWF_DOUBLE, .min = 1, .max = 384000,
      .def = 300, .unit = "Hz", .help = "bell 2 centre frequency" },
    { .name = "b2-gain",  .type = PWF_DOUBLE, .min = -40, .max = 40,
      .def = 0, .unit = "dB", .help = "bell 2 gain; 0 skips the band" },
    { .name = "b2-q",     .type = PWF_DOUBLE, .min = 0.05, .max = 40,
      .def = 1.0, .help = "bell 2 Q; bandwidth is freq/Q" },

    { .name = "b3-freq",  .type = PWF_DOUBLE, .min = 1, .max = 384000,
      .def = 1000, .unit = "Hz", .help = "bell 3 centre frequency" },
    { .name = "b3-gain",  .type = PWF_DOUBLE, .min = -40, .max = 40,
      .def = 0, .unit = "dB", .help = "bell 3 gain; 0 skips the band" },
    { .name = "b3-q",     .type = PWF_DOUBLE, .min = 0.05, .max = 40,
      .def = 1.0, .help = "bell 3 Q; bandwidth is freq/Q" },

    { .name = "b4-freq",  .type = PWF_DOUBLE, .min = 1, .max = 384000,
      .def = 3000, .unit = "Hz", .help = "bell 4 centre frequency" },
    { .name = "b4-gain",  .type = PWF_DOUBLE, .min = -40, .max = 40,
      .def = 0, .unit = "dB", .help = "bell 4 gain; 0 skips the band" },
    { .name = "b4-q",     .type = PWF_DOUBLE, .min = 0.05, .max = 40,
      .def = 1.0, .help = "bell 4 Q; bandwidth is freq/Q" },

    { .name = "hs-freq",  .type = PWF_DOUBLE, .min = 1, .max = 384000,
      .def = 8000, .unit = "Hz", .help = "high shelf corner frequency" },
    { .name = "hs-gain",  .type = PWF_DOUBLE, .min = -40, .max = 40,
      .def = 0, .unit = "dB", .help = "high shelf gain; 0 skips the band" },
    { .name = "hs-slope", .type = PWF_DOUBLE, .min = 0.05, .max = 10,
      .def = 0.7, .help = "high shelf slope S; 1 is the steepest monotonic one" },

    { .name = "bells", .type = PWF_ENUM, .choices = bell_kinds, .def = 0,
      .help = "bell design: Orfanidis (uncramped) or the RBJ cookbook" },
    PWF_PARAM_END
};

static const char *const bname[NBANDS] =
    { "low shelf", "bell 2", "bell 3", "bell 4", "high shelf" };

typedef struct {
    biquad_t sec[NBANDS];
    uint8_t  on[NBANDS];
} eq_coeffs;

typedef struct {
    bqstate_t *st;              /* channels * NBANDS */
    uint8_t    last_on[NBANDS];
    int        have_last;
} eq_state;

static int eq_init(void *user, uint32_t channels, double max_rate)
{
    eq_state *s = user;
    (void)max_rate;
    s->st = calloc((size_t)channels * NBANDS, sizeof *s->st);
    s->have_last = 0;
    return s->st ? 0 : -1;
}

static void eq_fini(void *user)
{
    eq_state *s = user;
    free(s->st);
    s->st = NULL;
}

static int eq_validate(void *user, const pwf_param *p, double rate,
                       char *err, size_t errsz)
{
    int k;

    (void)user;
    for (k = 0; k < NBANDS; k++) {
        double f = p[k * 3 + 0].value;
        double g = p[k * 3 + 1].value;
        double q = p[k * 3 + 2].value;

        if (g == 0.0) continue;         /* skipped band, nothing to check */

        if (f >= rate / 2.0) {
            snprintf(err, errsz, "%s frequency %g Hz must be below Nyquist "
                     "(%g Hz)", bname[k], f, rate / 2.0);
            return -1;
        }
        if (k == 0 || k == NBANDS - 1) {
            double smax = dsp_shelf_smax(g);
            if (q > smax) {
                snprintf(err, errsz, "%s slope S=%g is too steep for a %+g dB "
                         "shelf (max %.2f here); past that limit the poles "
                         "reach the unit circle and the section rings forever",
                         bname[k], q, g, smax);
                return -1;
            }
        }
    }
    return 0;
}

static uint32_t eq_design(void *user, const pwf_param *p, double rate,
                          uint32_t channels, void *coeffs)
{
    eq_coeffs *c = coeffs;
    int use_rbj = (int)pwf_value(p, "bells") == 1;
    int k;

    (void)user; (void)channels;

    for (k = 0; k < NBANDS; k++) {
        double f = p[k * 3 + 0].value;
        double g = p[k * 3 + 1].value;
        double q = p[k * 3 + 2].value;

        /* A band with no gain is the identity; skipping it outright keeps an
         * all-flat EQ a bit-exact passthrough. The graph can also drop to a
         * rate that puts a band above Nyquist, which validate() cannot see
         * coming, so such a band is skipped rather than designed wrong. */
        if (g == 0.0 || f >= rate / 2.0) {
            c->on[k] = 0;
            biquad_identity(&c->sec[k]);
            continue;
        }
        c->on[k] = 1;
        if (k == 0)
            biquad_lowshelf(&c->sec[k], f, rate, g, q);
        else if (k == NBANDS - 1)
            biquad_highshelf(&c->sec[k], f, rate, g, q);
        else if (use_rbj)
            biquad_peak_rbj(&c->sec[k], f, rate, g, q);
        else
            biquad_peak_orfanidis(&c->sec[k], f, rate, g, q);
    }
    return 0;                   /* a causal IIR cascade: zero latency */
}

static void eq_process(void *user, const void *coeffs, int changed,
                       float *const *in, float *const *out,
                       uint32_t channels, uint32_t n)
{
    eq_state *s = user;
    const eq_coeffs *c = coeffs;
    uint32_t ch, i;
    int k;

    /* State is indexed by band, not by position in the cascade, so turning a
     * band on or off never hands one band's history to another. A band that
     * has just come back on starts from silence instead of ringing out
     * whatever it was doing when it was switched off. */
    if (changed) {
        for (k = 0; k < NBANDS; k++) {
            if (!c->on[k] || (s->have_last && s->last_on[k])) continue;
            for (ch = 0; ch < channels; ch++)
                memset(&s->st[ch * NBANDS + k], 0, sizeof *s->st);
        }
        memcpy(s->last_on, c->on, sizeof s->last_on);
        s->have_last = 1;
    }

    for (ch = 0; ch < channels; ch++) {
        const float *x = in[ch];
        float *y = out[ch];
        bqstate_t *st = &s->st[ch * NBANDS];

        for (i = 0; i < n; i++) {
            double v = (double)x[i];
            if (!isfinite(v)) v = 0.0;
            for (k = 0; k < NBANDS; k++)
                if (c->on[k]) v = biquad_run(&c->sec[k], &st[k], v);
            y[i] = (float)v;
        }
    }
}

int main(int argc, char **argv)
{
    static eq_state state;
    pwf_desc desc = {
        .name       = "pw_eq",
        .summary    = "5-band parametric EQ (low shelf, 3 bells, high shelf)",
        .notes      =
            "Bands are numbered low to high: ls, b2, b3, b4, hs. Each has a\n"
            "frequency, a gain and a width -- Q for the bells (bandwidth is\n"
            "freq/Q, higher Q is narrower), slope S for the shelves (S = 1 is\n"
            "the steepest monotonic shelf, lower is gentler). A slope past\n"
            "what its gain can carry is rejected rather than turned into an\n"
            "oscillator.\n"
            "\n"
            "Setting a band's gain to 0 takes it out of the cascade entirely,\n"
            "so a flat EQ is a bit-exact passthrough.\n"
            "\n"
            "Zero latency: this is a causal IIR cascade. Nothing is clipped\n"
            "either, so put pw_limit after a boost.\n"
            "\n"
            "Live voice starting point:\n"
            "  ls-freq 90 / ls-gain -4 / b2-freq 300 / b2-gain -3\n"
            "  b3-freq 2500 / b3-gain 3 / b3-q 0.9\n"
            "  hs-freq 9000 / hs-gain 2.5",
        .params     = params,
        .coeff_size = sizeof(eq_coeffs),
        .init       = eq_init,
        .fini       = eq_fini,
        .validate   = eq_validate,
        .design     = eq_design,
        .process    = eq_process,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
