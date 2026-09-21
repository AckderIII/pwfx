/*
 * pw_gate -- noise gate / expander as a PipeWire filter node.
 *
 * The DSP is in src/gate_dsp.h, ported unchanged from reference/gate.c:
 * lookahead with an exact sliding-maximum detector, hysteresis, hold, a
 * filtered sidechain, channel-linked gain, and raised-cosine ramps in the dB
 * domain so the gate cannot click.
 *
 * Two things differ from the reference's command line:
 *
 *  - The lookahead delay is real here. The reference absorbs it, swallowing
 *    the first L samples and flushing L more at EOF so its output is
 *    sample-aligned with its input; a live filter cannot, because looking
 *    ahead means being late. The delay is reported to PipeWire as process
 *    latency instead, so the graph compensates for it.
 *  - The reference takes an absolute close threshold (--close). Here it is
 *    "hysteresis", the gap below the open threshold, so moving the threshold
 *    carries the close point with it instead of silently leaving 40 dB of
 *    hysteresis behind. close = threshold - hysteresis, so --close -41 in
 *    the reference is threshold -35 with hysteresis 6 here.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "gate_dsp.h"

static pwf_param params[] = {
    { .name = "threshold", .shortopt = "-t", .type = PWF_DOUBLE,
      .min = -120, .max = 0, .def = -35, .unit = "dB",
      .help = "level that opens the gate, dBFS" },
    { .name = "hysteresis", .shortopt = "-T", .type = PWF_DOUBLE,
      .min = 0, .max = 60, .def = 6, .unit = "dB",
      .help = "how far below the threshold the gate closes again" },
    { .name = "attack", .shortopt = "-a", .type = PWF_DOUBLE,
      .min = 0, .max = 1000, .def = 2, .unit = "ms",
      .help = "fade-in time once the gate opens" },
    { .name = "hold", .shortopt = "-o", .type = PWF_DOUBLE,
      .min = 0, .max = 10000, .def = 150, .unit = "ms",
      .help = "minimum open time after the signal drops below the close point" },
    { .name = "release", .shortopt = "-e", .type = PWF_DOUBLE,
      .min = 0, .max = 10000, .def = 150, .unit = "ms",
      .help = "fade-out time" },
    { .name = "lookahead", .shortopt = "-k", .type = PWF_DOUBLE,
      .min = 0, .max = GATE_MAX_LOOK_MS, .def = 5, .unit = "ms",
      .help = "audio delay feeding the detector; keep >= attack" },
    { .name = "range", .shortopt = "-g", .type = PWF_DOUBLE,
      .min = 0, .max = 144, .def = 120, .unit = "dB",
      .help = "attenuation when closed; >= 90 snaps to exact silence" },
    { .name = "sc-hpf", .shortopt = "-F", .type = PWF_DOUBLE,
      .min = 0, .max = 384000, .def = 120, .unit = "Hz",
      .help = "high-pass before detection, 0 = off" },
    { .name = "sc-lpf", .shortopt = "-G", .type = PWF_DOUBLE,
      .min = 0, .max = 384000, .def = 0, .unit = "Hz",
      .help = "low-pass before detection, 0 = off" },
    PWF_PARAM_END
};

static int gate_pw_init(void *user, uint32_t channels, double max_rate)
{
    return gate_init(user, channels, max_rate);
}

static void gate_pw_fini(void *user)
{
    gate_fini(user);
}

static int gate_pw_validate(void *user, const pwf_param *p, double rate,
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

static uint32_t gate_pw_design(void *user, const pwf_param *p, double rate,
                               uint32_t channels, void *coeffs)
{
    gate_state *s = user;
    gate_coeffs *c = coeffs;
    double thr = pwf_value(p, "threshold");

    (void)channels;
    gate_design(c, rate,
                thr,
                thr - pwf_value(p, "hysteresis"),
                pwf_value(p, "attack"),
                pwf_value(p, "hold"),
                pwf_value(p, "release"),
                pwf_value(p, "lookahead"),
                pwf_value(p, "range"),
                pwf_value(p, "sc-hpf"),
                pwf_value(p, "sc-lpf"));

    /* The delay line was sized for max-rate; if the graph runs faster than
     * that, the lookahead is capped rather than overrunning the buffer. */
    if (c->L > s->max_L) c->L = s->max_L;
    return (uint32_t)c->L;
}

static void gate_pw_process(void *user, const void *coeffs, int changed,
                            float *const *in, float *const *out,
                            uint32_t channels, uint32_t n)
{
    (void)changed;      /* nothing to rebuild: the ring is sized for max-rate
                         * and a lookahead change only moves the read offset,
                         * which the sliding maximum recovers from on its own */
    gate_run(user, coeffs, in, out, channels, n);
}

int main(int argc, char **argv)
{
    static gate_state state;
    pwf_desc desc = {
        .name       = "pw_gate",
        .summary    = "noise gate with lookahead, hysteresis and hold",
        .notes      =
            "The gate is channel-linked: one detector, one gain, applied to\n"
            "every channel, because gating channels independently shifts the\n"
            "stereo image every time the gate moves.\n"
            "\n"
            "Lookahead is real latency here, reported to the graph so\n"
            "PipeWire can compensate. Keep it at or above the attack time or\n"
            "transients clip. Changing it while audio is flowing steps the\n"
            "delay line and is audible as a click; set it and leave it.\n"
            "\n"
            "For a live voice mic, start at threshold -35, hold 150,\n"
            "release 200, and lower the threshold until the gate stops\n"
            "clipping the ends of your words. The sidechain high-pass at\n"
            "120 Hz keeps desk rumble and mic stand thumps from holding it\n"
            "open; it affects detection only, never the audio path.",
        .params     = params,
        .coeff_size = sizeof(gate_coeffs),
        .init       = gate_pw_init,
        .fini       = gate_pw_fini,
        .validate   = gate_pw_validate,
        .design     = gate_pw_design,
        .process    = gate_pw_process,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
