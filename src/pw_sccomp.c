/*
 * pw_sccomp -- sidechain compressor as a PipeWire filter node.
 *
 * pw_comp with its detector listening to a separate mono input, sc_in,
 * instead of to the audio it compresses: the classic use is ducking -- music
 * on in_N, a voice on sc_in, and the music gets out of the voice's way.
 *
 * The DSP is pw_comp's, in src/comp_dsp.h, run with a key: the same
 * detector, knee, lookahead, peak or RMS detection and sidechain filters,
 * with one detector for the one key and one gain for every channel, so there
 * is no linking to choose. A key that is the audio itself, mono, is the plain
 * compressor sample for sample, and tests/sccomp.py holds it to exactly that
 * against reference/comp.c.
 *
 * Lookahead delays the audio, not the key, so the gain is in place when the
 * key's onset arrives in the output; it is reported as latency like
 * pw_comp's. Nothing linked to sc_in is silence: the gain sits at makeup and
 * the audio passes untouched.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "comp_dsp.h"

static pwf_param params[] = {
    { .name = "threshold", .shortopt = "-t", .type = PWF_DOUBLE,
      .min = -80, .max = 0, .def = -30, .unit = "dB",
      .help = "sidechain level where compression starts" },
    { .name = "ratio", .shortopt = "-R", .type = PWF_DOUBLE,
      .min = 1, .max = 100, .def = 4,
      .help = "input:output above threshold; 1 is off, 20+ is limiting" },
    { .name = "knee", .shortopt = "-w", .type = PWF_DOUBLE,
      .min = 0, .max = 40, .def = 6, .unit = "dB",
      .help = "soft-knee width, straddling the threshold; 0 = hard knee" },
    { .name = "attack", .shortopt = "-a", .type = PWF_DOUBLE,
      .min = 0, .max = 1000, .def = 10, .unit = "ms",
      .help = "gain-reduction time constant going up (63%, not 10-90%)" },
    { .name = "release", .shortopt = "-e", .type = PWF_DOUBLE,
      .min = 0, .max = 10000, .def = 300, .unit = "ms",
      .help = "gain-reduction time constant coming down" },
    { .name = "lookahead", .shortopt = "-k", .type = PWF_DOUBLE,
      .min = 0, .max = COMP_MAX_LOOK_MS, .def = 5, .unit = "ms",
      .help = "audio delay against the sidechain" },
    { .name = "rms", .shortopt = "-q", .type = PWF_DOUBLE,
      .min = 0, .max = 1000, .def = 0, .unit = "ms",
      .help = "RMS averaging window; 0 = peak detection" },
    { .name = "makeup", .shortopt = "-m", .type = PWF_DOUBLE,
      .min = -40, .max = 40, .def = 0, .unit = "dB",
      .help = "fixed makeup gain" },
    { .name = "auto-makeup", .shortopt = "-M", .type = PWF_BOOL, .def = 0,
      .help = "makeup that maps 0 dBFS back to 0 dBFS; adds to makeup" },
    { .name = "sc-hpf", .shortopt = "-F", .type = PWF_DOUBLE,
      .min = 0, .max = 384000, .def = 0, .unit = "Hz",
      .help = "high-pass on the sidechain, 0 = off" },
    { .name = "sc-lpf", .shortopt = "-G", .type = PWF_DOUBLE,
      .min = 0, .max = 384000, .def = 0, .unit = "Hz",
      .help = "low-pass on the sidechain, 0 = off" },
    PWF_PARAM_END
};

static int sccomp_pw_init(void *user, uint32_t channels, double max_rate)
{
    return comp_init(user, channels, max_rate);
}

static void sccomp_pw_fini(void *user)
{
    comp_fini(user);
}

static int sccomp_pw_validate(void *user, const pwf_param *p, double rate,
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

static uint32_t sccomp_pw_design(void *user, const pwf_param *p, double rate,
                                 uint32_t channels, void *coeffs)
{
    comp_state *s = user;
    comp_coeffs *c = coeffs;

    /* Linked by max: with a key, comp_run runs one detector whatever this
     * says, and max over one channel is the key's own level. */
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
                COMP_LINK_MAX,
                pwf_value(p, "sc-hpf"),
                pwf_value(p, "sc-lpf"));

    if (c->L > s->max_L) c->L = s->max_L;
    return (uint32_t)c->L;
}

static void sccomp_pw_process(void *user, const void *coeffs, int changed,
                              float *const *in, float *const *out,
                              uint32_t channels, uint32_t n)
{
    (void)changed;
    /* in[channels] is sc_in: the framework puts the sidechain after the
     * audio inputs, and silence there when nothing is linked to it. */
    comp_run(user, coeffs, in, in[channels], out, channels, n);
}

static double sccomp_pw_gr(void *user, const void *coeffs)
{
    (void)coeffs;
    return comp_take_gr(user);
}

/* The static curve, as pw_comp's: the level a signal at x dB comes out at
 * while the sidechain is at x dB too. Against a key that is not the audio,
 * read y - x as the gain that key level sets, makeup included. */
static size_t sccomp_pw_curve(void *user, const void *coeffs, double rate,
                              double *x, double *y, size_t max)
{
    const comp_coeffs *c = coeffs;
    size_t i, n = pwf_level_grid(x, max);

    (void)user; (void)rate;
    for (i = 0; i < n; i++)
        y[i] = comp_curve_db(x[i], c->thr_db, c->ratio, c->knee) + c->makeup_db;
    return n;
}

static uint32_t sccomp_pw_max_latency(void *user)
{
    return (uint32_t)((comp_state *)user)->max_L;
}

int main(int argc, char **argv)
{
    static comp_state state;
    pwf_desc desc = {
        .name       = "pw_sccomp",
        .summary    = "sidechain compressor: the gain follows a separate mono input",
        .notes      =
            "The audio on in_N is compressed by what arrives on sc_in: one\n"
            "gain, from the sidechain's level, applied to every channel. The\n"
            "sidechain itself is never heard. Link the voice that should\n"
            "duck the music to sc_in, by hand:\n"
            "\n"
            "    pw-link voice:capture_MONO pw_sccomp:sc_in\n"
            "\n"
            "With nothing linked to sc_in the detector hears silence, and the\n"
            "audio passes with nothing but the makeup gain applied.\n"
            "\n"
            "Lookahead delays the audio against the sidechain, so the gain\n"
            "is already down when the voice's first syllable meets the\n"
            "music; it is reported to the graph as latency. The sidechain\n"
            "filters shape what the detector hears -- a high-pass at 150 Hz\n"
            "keeps a voice's plosives from pumping the music.\n"
            "\n"
            "Attack and release are 63% time constants, as in pw_comp.\n"
            "\n"
            "Ducking music under a voice: threshold -35, ratio 6, attack 10,\n"
            "                             release 400, knee 6, sc-hpf 150",
        .params     = params,
        .coeff_size = sizeof(comp_coeffs),
        .init       = sccomp_pw_init,
        .fini       = sccomp_pw_fini,
        .validate   = sccomp_pw_validate,
        .design     = sccomp_pw_design,
        .process    = sccomp_pw_process,
        .sidechain  = 1,
        .max_latency = sccomp_pw_max_latency,
        .gain_reduction = sccomp_pw_gr,
        .curve_kind = PWF_CURVE_LEVEL,
        .curve      = sccomp_pw_curve,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
