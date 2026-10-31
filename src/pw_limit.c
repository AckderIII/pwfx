/*
 * pw_limit -- true-peak brickwall limiter as a PipeWire filter node.
 *
 * The DSP is in src/limit_dsp.h, ported unchanged from reference/limit.c: a
 * sliding minimum over the lookahead window followed by two cascaded boxcars,
 * which is what turns "usually below the ceiling" into a proof that the
 * ceiling holds, and a Kaiser-windowed polyphase interpolator so the peak
 * being limited is the inter-sample one a D/A or a codec will produce, not
 * just the highest sample.
 *
 * The one change for realtime: the reference absorbs the lookahead delay so
 * its output is sample-aligned with its input. A live filter cannot -- looking
 * ahead means being late -- so the delay is real here and is reported to
 * PipeWire as process latency, which the graph compensates for. Note that a
 * limiter's latency is the lookahead plus the interpolator's group delay of
 * TAPS/2 samples, so it is never zero even at lookahead 0.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "limit_dsp.h"

static const char *const os_choices[] = { "1", "2", "4", "8", "16", NULL };

static pwf_param params[] = {
    { .name = "ceiling", .shortopt = "-C", .type = PWF_DOUBLE,
      .min = -60, .max = 0, .def = -1, .unit = "dB",
      .help = "output ceiling in dBTP" },
    { .name = "input-gain", .shortopt = "-i", .type = PWF_DOUBLE,
      .min = -40, .max = 40, .def = 0, .unit = "dB",
      .help = "gain applied before limiting; this is how you drive it" },
    { .name = "release", .shortopt = "-e", .type = PWF_DOUBLE,
      .min = 0, .max = 5000, .def = 60, .unit = "ms",
      .help = "gain-reduction recovery constant" },
    { .name = "lookahead", .shortopt = "-k", .type = PWF_DOUBLE,
      .min = 0, .max = LIMIT_MAX_LOOK_MS, .def = 5, .unit = "ms",
      .help = "attack window; shorter only sounds more aggressive, it never "
              "overshoots" },
    { .name = "oversample", .shortopt = "-O", .type = PWF_ENUM,
      .choices = os_choices, .def = 3,
      .help = "true-peak oversampling factor; 1 is sample peak, not dBTP" },
    PWF_PARAM_END
};

static int limit_pw_init(void *user, uint32_t channels, double max_rate)
{
    return limit_init(user, channels, max_rate);
}

static void limit_pw_fini(void *user)
{
    limit_fini(user);
}

static uint32_t limit_pw_design(void *user, const pwf_param *p, double rate,
                                uint32_t channels, void *coeffs)
{
    limit_state *s = user;
    limit_coeffs *c = coeffs;
    int os = 1 << (int)pwf_value(p, "oversample");

    (void)channels;
    limit_design(c, rate,
                 pwf_value(p, "ceiling"),
                 pwf_value(p, "input-gain"),
                 pwf_value(p, "release"),
                 pwf_value(p, "lookahead"),
                 os);

    /* The delay line was sized for max-rate; if the graph runs faster than
     * that, the lookahead is capped rather than overrunning the buffer. */
    if (c->A > s->max_A) {
        c->A  = s->max_A;
        c->S  = 2 * c->A - 1;
        c->D  = c->S - 1;
        c->DT = c->D + c->D_det;
    }
    return (uint32_t)c->DT;
}

static void limit_pw_process(void *user, const void *coeffs, int changed,
                             float *const *in, float *const *out,
                             uint32_t channels, uint32_t n)
{
    (void)changed;      /* limit_run rebuilds the boxcars itself when the
                         * lookahead moves, refilling them with the gain
                         * already in use so the change stays continuous */
    limit_run(user, coeffs, in, out, channels, n);
}

static double limit_pw_gr(void *user, const void *coeffs)
{
    (void)coeffs;
    return limit_take_gr(user);
}

/* The static curve: input gain, then nothing over the ceiling. */
static size_t limit_pw_curve(void *user, const void *coeffs, double rate,
                             double *x, double *y, size_t max)
{
    const limit_coeffs *c = coeffs;
    double in_db   = DSP_LOG10_20 * log(c->in_lin);
    double ceil_db = DSP_LOG10_20 * log(c->ceil_lin);
    size_t i, n = pwf_level_grid(x, max);

    (void)user; (void)rate;
    for (i = 0; i < n; i++)
        y[i] = x[i] + in_db < ceil_db ? x[i] + in_db : ceil_db;
    return n;
}

/* The longest the lookahead can make it, for the bypass's dry delay. */
static uint32_t limit_pw_max_latency(void *user)
{
    return (uint32_t)((limit_state *)user)->max_DT;
}

int main(int argc, char **argv)
{
    static limit_state state;
    pwf_desc desc = {
        .name       = "pw_limit",
        .summary    = "true-peak brickwall limiter",
        .notes      =
            "The ceiling is guaranteed, not approximated: the gain envelope\n"
            "is a sliding minimum over the lookahead window smoothed by two\n"
            "boxcars, which provably reaches the required gain before the\n"
            "peak does. There is no overshoot at any setting; a shorter\n"
            "lookahead only sounds more aggressive.\n"
            "\n"
            "Oversampling is what makes the ceiling a true-peak one. Sample\n"
            "peaks are not what clips -- a D/A reconstruction filter, and\n"
            "every lossy codec, can produce inter-sample peaks well above the\n"
            "highest sample, which is why broadcast and streaming specs are\n"
            "written in dBTP. Set oversample 1 to limit sample peaks only.\n"
            "\n"
            "Latency is the lookahead plus the interpolator's group delay of\n"
            "12 samples, so it is never zero. It is reported to the graph.\n"
            "Changing the lookahead live steps the delay line and is audible;\n"
            "set it and leave it.\n"
            "\n"
            "Streaming delivery: ceiling -1, input-gain 6, release 60\n"
            "Transparent safety: ceiling -0.3, input-gain 0, release 100,\n"
            "                    lookahead 10",
        .params     = params,
        .coeff_size = sizeof(limit_coeffs),
        .init       = limit_pw_init,
        .fini       = limit_pw_fini,
        .design     = limit_pw_design,
        .process    = limit_pw_process,
        .max_latency = limit_pw_max_latency,
        .gain_reduction = limit_pw_gr,
        .curve_kind = PWF_CURVE_LEVEL,
        .curve      = limit_pw_curve,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
