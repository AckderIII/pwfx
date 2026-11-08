/*
 * pw_comp -- dynamic range compressor as a PipeWire filter node.
 *
 * The DSP is in src/comp_dsp.h, ported unchanged from reference/comp.c: a
 * log-domain smooth decoupled peak detector, a C1 soft knee, lookahead, peak
 * or RMS detection, channel linking, and a filtered sidechain.
 *
 * The one change for realtime: the reference absorbs the lookahead delay so
 * its output is sample-aligned with its input. A live filter cannot -- looking
 * ahead means being late -- so the delay is real here and is reported to
 * PipeWire as process latency, which the graph compensates for.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "comp_dsp.h"

static const char *const link_modes[] = { "max", "avg", "off", NULL };

static pwf_param params[] = {
    { .name = "threshold", .shortopt = "-t", .type = PWF_DOUBLE,
      .min = -80, .max = 0, .def = -18, .unit = "dB",
      .help = "level where compression starts" },
    { .name = "ratio", .shortopt = "-R", .type = PWF_DOUBLE,
      .min = 1, .max = 100, .def = 3,
      .help = "input:output above threshold; 1 is off, 20+ is limiting" },
    { .name = "knee", .shortopt = "-w", .type = PWF_DOUBLE,
      .min = 0, .max = 40, .def = 6, .unit = "dB",
      .help = "soft-knee width, straddling the threshold; 0 = hard knee" },
    { .name = "attack", .shortopt = "-a", .type = PWF_DOUBLE,
      .min = 0, .max = 1000, .def = 10, .unit = "ms",
      .help = "gain-reduction time constant going up (63%, not 10-90%)" },
    { .name = "release", .shortopt = "-e", .type = PWF_DOUBLE,
      .min = 0, .max = 10000, .def = 120, .unit = "ms",
      .help = "gain-reduction time constant coming down" },
    { .name = "lookahead", .shortopt = "-k", .type = PWF_DOUBLE,
      .min = 0, .max = COMP_MAX_LOOK_MS, .def = 5, .unit = "ms",
      .help = "audio delay feeding the detector" },
    { .name = "rms", .shortopt = "-q", .type = PWF_DOUBLE,
      .min = 0, .max = 1000, .def = 0, .unit = "ms",
      .help = "RMS averaging window; 0 = peak detection" },
    { .name = "makeup", .shortopt = "-m", .type = PWF_DOUBLE,
      .min = -40, .max = 40, .def = 0, .unit = "dB",
      .help = "fixed makeup gain" },
    { .name = "auto-makeup", .shortopt = "-M", .type = PWF_BOOL, .def = 0,
      .help = "makeup that maps 0 dBFS back to 0 dBFS; adds to makeup" },
    { .name = "link", .shortopt = "-l", .type = PWF_ENUM,
      .choices = link_modes, .def = 0,
      .help = "channel linking; off compresses each channel alone" },
    { .name = "sc-hpf", .shortopt = "-F", .type = PWF_DOUBLE,
      .min = 0, .max = 384000, .def = 0, .unit = "Hz",
      .help = "high-pass before detection, 0 = off" },
    { .name = "sc-lpf", .shortopt = "-G", .type = PWF_DOUBLE,
      .min = 0, .max = 384000, .def = 0, .unit = "Hz",
      .help = "low-pass before detection, 0 = off" },
    PWF_PARAM_END
};

static int comp_pw_init(void *user, uint32_t channels, double max_rate)
{
    return comp_init(user, channels, max_rate);
}

static void comp_pw_fini(void *user)
{
    comp_fini(user);
}

static int comp_pw_validate(void *user, const pwf_param *p, double rate,
                            char *err, size_t errsz)
{
    double hpf = pwf_value(p, "sc-hpf");
    double lpf = pwf_value(p, "sc-lpf");

    (void)user;
    if (hpf >= rate / 2.0 || lpf >= rate / 2.0) {
        snprintf(err, errsz, "sidechain frequencies must be below Nyquist "
                 "(%g Hz)", rate / 2.0);
        return -1;
    }
    if (lpf > 0.0 && hpf > 0.0 && lpf <= hpf) {
        snprintf(err, errsz, "sidechain low-pass (%g Hz) must be above the "
                 "high-pass (%g Hz) or the detector hears nothing", lpf, hpf);
        return -1;
    }
    return 0;
}

static uint32_t comp_pw_design(void *user, const pwf_param *p, double rate,
                               uint32_t channels, void *coeffs)
{
    comp_state *s = user;
    comp_coeffs *c = coeffs;

    comp_design(c, rate, channels,
                pwf_value(p, "threshold"),
                pwf_value(p, "ratio"),
                pwf_value(p, "knee"),
                pwf_value(p, "attack"),
                pwf_value(p, "release"),
                pwf_value(p, "lookahead"),
                pwf_value(p, "rms"),
                pwf_value(p, "makeup"),
                pwf_value(p, "auto-makeup") != 0,
                (int)pwf_value(p, "link"),
                pwf_value(p, "sc-hpf"),
                pwf_value(p, "sc-lpf"));

    /* The delay line was sized for max-rate; if the graph runs faster than
     * that, the lookahead is capped rather than overrunning the buffer. */
    if (c->L > s->max_L) c->L = s->max_L;
    return (uint32_t)c->L;
}

static void comp_pw_process(void *user, const void *coeffs, int changed,
                            float *const *in, float *const *out,
                            uint32_t channels, uint32_t n)
{
    comp_state *s = user;
    const comp_coeffs *c = coeffs;

    /* Under max or avg linking only detector 0 runs, so the per-channel
     * detectors hold whatever they last converged to. Switching to link off
     * would apply that stale gain as a step on every channel but the first;
     * seeding them from the linked detector makes the changeover continuous. */
    if (changed && s->last_link != c->link) {
        if (c->link == COMP_LINK_OFF) {
            uint32_t d;
            for (d = 1; d < channels; d++) {
                s->y1[d]      = s->y1[0];
                s->yL[d]      = s->yL[0];
                s->rms_acc[d] = s->rms_acc[0];
                s->gain[d]    = s->gain[0];
            }
        }
        s->last_link = c->link;
    }

    comp_run(s, c, in, NULL, out, channels, n);
}

static double comp_pw_gr(void *user, const void *coeffs)
{
    (void)coeffs;
    return comp_take_gr(user);
}

/* The static curve: output level for a steady input level, makeup
 * included. Attack, release, RMS and the sidechain filters shape how fast
 * and on what it gets there, not where it ends up. */
static size_t comp_pw_curve(void *user, const void *coeffs, double rate,
                            double *x, double *y, size_t max)
{
    const comp_coeffs *c = coeffs;
    size_t i, n = pwf_level_grid(x, max);

    (void)user; (void)rate;
    for (i = 0; i < n; i++)
        y[i] = comp_curve_db(x[i], c->thr_db, c->ratio, c->knee) + c->makeup_db;
    return n;
}

/* The longest the lookahead can make it, for the bypass's dry delay. */
static uint32_t comp_pw_max_latency(void *user)
{
    return (uint32_t)((comp_state *)user)->max_L;
}

int main(int argc, char **argv)
{
    static comp_state state;
    pwf_desc desc = {
        .name       = "pw_comp",
        .summary    = "dynamic range compressor with lookahead and soft knee",
        .notes      =
            "Attack and release are 63% time constants; the 10-90% time is\n"
            "about 2.2x these. The knee straddles the threshold, so a knee\n"
            "of 6 dB spans threshold +/- 3 dB.\n"
            "\n"
            "Linking is on by default: one gain, derived from the loudest\n"
            "channel, applied to all of them. Compressing channels\n"
            "independently (link off) pulls the stereo image toward whichever\n"
            "side is momentarily louder.\n"
            "\n"
            "The sidechain filters affect detection only; the audio path is\n"
            "never filtered. A sidechain high-pass keeps bass energy from\n"
            "pumping the whole mix.\n"
            "\n"
            "Nothing is clipped -- the stream stays float -- so chain\n"
            "pw_limit after this if you need a hard ceiling.\n"
            "\n"
            "Live voice:       threshold -18, ratio 3, attack 10,\n"
            "                  release 120, knee 6, makeup 6\n"
            "Gentle levelling: threshold -24, ratio 2, attack 25,\n"
            "                  release 250, knee 10",
        .params     = params,
        .coeff_size = sizeof(comp_coeffs),
        .init       = comp_pw_init,
        .fini       = comp_pw_fini,
        .validate   = comp_pw_validate,
        .design     = comp_pw_design,
        .process    = comp_pw_process,
        .max_latency = comp_pw_max_latency,
        .gain_reduction = comp_pw_gr,
        .curve_kind = PWF_CURVE_LEVEL,
        .curve      = comp_pw_curve,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
