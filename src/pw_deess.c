/*
 * pw_deess -- de-esser as a PipeWire filter node.
 *
 * The DSP is in src/deess_dsp.h, ported from reference/deess.c: a
 * subtractive dynamic EQ on a band-pass at the sibilance frequency, which
 * passes its input through bit for bit whenever it is not cutting, with
 * relative or absolute RMS detection, a soft-knee gain computer capped at
 * a range, smooth decoupled smoothing in dB and true lookahead.
 *
 * The one change for realtime, as for the compressor: the reference
 * absorbs the lookahead so its output is sample-aligned with its input; a
 * live filter cannot, so the delay is real here and is reported to
 * PipeWire as process latency, which the graph compensates for.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "deess_dsp.h"

static const char *const detect_modes[] = { "rel", "abs", NULL };
static const char *const cut_modes[]    = { "split", "wide", NULL };

static pwf_param params[] = {
    { .name = "freq", .shortopt = "-f", .type = PWF_DOUBLE,
      .min = 1000, .max = 16000, .def = 6500, .unit = "Hz",
      .help = "centre of the sibilance band; most voices 5000..8000" },
    { .name = "q", .shortopt = "-Q", .type = PWF_DOUBLE,
      .min = 0.3, .max = 6, .def = 1.5,
      .help = "width of the band; higher is narrower" },
    { .name = "detect", .shortopt = "-d", .type = PWF_ENUM,
      .choices = detect_modes, .def = DEESS_DETECT_REL,
      .help = "rel: band level relative to the whole signal; abs: in dBFS" },
    { .name = "threshold", .shortopt = "-t", .type = PWF_DOUBLE,
      .min = -60, .max = 0, .def = -12, .unit = "dB",
      .help = "where cutting starts; rel -15..-5, abs -40..-20 typical" },
    { .name = "ratio", .shortopt = "-R", .type = PWF_DOUBLE,
      .min = 1, .max = 20, .def = 4,
      .help = "band:output above the threshold" },
    { .name = "range", .shortopt = "-g", .type = PWF_DOUBLE,
      .min = 0, .max = 30, .def = 12, .unit = "dB",
      .help = "the most it will ever cut" },
    { .name = "mode", .shortopt = "-m", .type = PWF_ENUM,
      .choices = cut_modes, .def = DEESS_MODE_SPLIT,
      .help = "split cuts the band only; wide turns everything down" },
    { .name = "attack", .shortopt = "-a", .type = PWF_DOUBLE,
      .min = 0, .max = 20, .def = 1, .unit = "ms",
      .help = "reduction time constant going up" },
    { .name = "release", .shortopt = "-e", .type = PWF_DOUBLE,
      .min = 5, .max = 500, .def = 60, .unit = "ms",
      .help = "reduction time constant coming down" },
    { .name = "lookahead", .shortopt = "-k", .type = PWF_DOUBLE,
      .min = 0, .max = DEESS_MAX_LOOK_MS, .def = 5, .unit = "ms",
      .help = "audio delay feeding the detector" },
    { .name = "listen", .shortopt = "-L", .type = PWF_BOOL, .def = 0,
      .help = "output the band alone, to tune freq by ear" },
    PWF_PARAM_END
};

static int deess_pw_init(void *user, uint32_t channels, double max_rate)
{
    return deess_init(user, channels, max_rate);
}

static void deess_pw_fini(void *user)
{
    deess_fini(user);
}

static int deess_pw_validate(void *user, const pwf_param *p, double rate,
                             char *err, size_t errsz)
{
    double f = pwf_value(p, "freq");

    (void)user;
    if (f >= rate / 2.0) {
        snprintf(err, errsz, "freq must be below Nyquist (%g Hz at %g Hz)",
                 rate / 2.0, rate);
        return -1;
    }
    return 0;
}

static uint32_t deess_pw_design(void *user, const pwf_param *p, double rate,
                                uint32_t channels, void *coeffs)
{
    deess_state *s = user;
    deess_coeffs *c = coeffs;
    double f = pwf_value(p, "freq");

    (void)channels;
    /* The graph can drop to a rate that puts a validated freq above
     * Nyquist; validate() cannot see that coming, so clamp here. */
    if (f > rate * 0.45) f = rate * 0.45;

    deess_design(c, rate, f, pwf_value(p, "q"), pwf_value(p, "threshold"),
                 pwf_value(p, "ratio"), pwf_value(p, "range"),
                 pwf_value(p, "attack"), pwf_value(p, "release"),
                 pwf_value(p, "lookahead"), (int)pwf_value(p, "detect"),
                 (int)pwf_value(p, "mode"), pwf_value(p, "listen") != 0);

    /* The delay lines were sized for max-rate; past that the lookahead is
     * capped rather than overrunning them. */
    if (c->L > s->max_L) c->L = s->max_L;
    return (uint32_t)c->L;
}

static void deess_pw_process(void *user, const void *coeffs, int changed,
                             float *const *in, float *const *out,
                             uint32_t channels, uint32_t n)
{
    (void)changed;
    deess_run(user, coeffs, in, out, channels, n);
}

static double deess_pw_gr(void *user, const void *coeffs)
{
    (void)coeffs;
    return deess_take_gr(user);
}

/* The response when cutting as hard as "range" lets it: where it acts and
 * how deep, from the band-pass actually running. */
static size_t deess_pw_curve(void *user, const void *coeffs, double rate,
                             double *x, double *y, size_t max)
{
    size_t i, n = pwf_freq_grid(rate, x, max);

    (void)user;
    for (i = 0; i < n; i++) y[i] = deess_full_cut_db(coeffs, x[i], rate);
    return n;
}

/* The longest the lookahead can make it, for the bypass's dry delay. */
static uint32_t deess_pw_max_latency(void *user)
{
    return (uint32_t)((deess_state *)user)->max_L;
}

int main(int argc, char **argv)
{
    static deess_state state;
    pwf_desc desc = {
        .name       = "pw_deess",
        .summary    = "de-esser: split-band or wide, relative or absolute",
        .notes      =
            "Split mode cuts only a band around freq, as a dynamic EQ: the\n"
            "cut at freq is exactly the reduction shown on gr, and when\n"
            "nothing is being cut the input passes through bit for bit.\n"
            "Wide mode turns the whole signal down instead, which can sound\n"
            "more natural on a voice that a band cut makes lispy.\n"
            "\n"
            "Relative detection (the default) compares the band with the\n"
            "whole signal, so a bright vowel does not trigger it and a\n"
            "quiet \"s\" does; the threshold is then in dB relative to the\n"
            "signal, -15..-5 typical. Absolute detection compares the band\n"
            "with the threshold in dBFS. A band below -60 dBFS never\n"
            "triggers relative detection.\n"
            "\n"
            "To find the frequency, turn listen on and sweep freq until the\n"
            "\"s\" sounds loudest and harshest, then turn it off again.\n"
            "\n"
            "The curve shows the response when cutting as hard as range\n"
            "allows -- where it acts and how deep.\n"
            "\n"
            "Starting point for a voice: freq 6500, q 1.5, detect rel,\n"
            "threshold -12, ratio 4, range 10.",
        .params     = params,
        .coeff_size = sizeof(deess_coeffs),
        .init       = deess_pw_init,
        .fini       = deess_pw_fini,
        .validate   = deess_pw_validate,
        .design     = deess_pw_design,
        .process    = deess_pw_process,
        .max_latency = deess_pw_max_latency,
        .gain_reduction = deess_pw_gr,
        .curve_kind = PWF_CURVE_FREQ,
        .curve      = deess_pw_curve,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
