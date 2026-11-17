/*
 * pw_reverb -- algorithmic reverb as a PipeWire filter node.
 *
 * The DSP is the one from reference/reverb.c, kept in reverb_dsp.h so the
 * test harness runs the very same code: a 16-channel Hadamard diffuser
 * feeding a 16-line feedback delay network with Householder feedback, prime
 * line lengths, a shelf per line that makes the decay time exact at both
 * ends of the band, and slow modulation read through allpasses so it does
 * not eat the decay. reverb_dsp.h has the detail.
 *
 * What differs from the stdin/stdout reference: audio from the graph, the
 * graph's rate, and everything live -- which is why the core glides its
 * delay lengths and its mix to new values rather than stepping them.
 *
 * Like pw_delay it reports no latency: the predelay and the tail are the
 * effect, and the dry signal passes undelayed.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "reverb_dsp.h"

static pwf_param params[] = {
    { .name = "mix", .shortopt = "-m", .type = PWF_DOUBLE,
      .min = 0, .max = 100, .def = 25, .unit = "%",
      .help = "wet share: 0 is dry, 100 the reverb alone" },
    { .name = "decay", .shortopt = "-d", .type = PWF_DOUBLE,
      .min = 0.1, .max = 60, .def = 2, .unit = "s",
      .help = "decay time, RT60, below damp-freq" },
    { .name = "hf-decay", .shortopt = "-H", .type = PWF_DOUBLE,
      .min = 5, .max = 100, .def = 50, .unit = "%",
      .help = "decay time above damp-freq, as a share of decay" },
    { .name = "damp-freq", .shortopt = "-F", .type = PWF_DOUBLE,
      .min = 100, .max = 20000, .def = 4000, .unit = "Hz",
      .help = "where the decay time crosses over" },
    { .name = "size", .shortopt = "-s", .type = PWF_DOUBLE,
      .min = 10, .max = 100, .def = 70, .unit = "%",
      .help = "room size: echo spacing and density, not decay" },
    { .name = "predelay", .shortopt = "-p", .type = PWF_DOUBLE,
      .min = 0, .max = RV_MAX_PREDELAY, .def = 10, .unit = "ms",
      .help = "gap before the reverb starts" },
    { .name = "modulation", .shortopt = "-M", .type = PWF_DOUBLE,
      .min = 0, .max = 100, .def = 30, .unit = "%",
      .help = "slow movement in the tail, up to 1 ms; keeps it from ringing" },
    { .name = "low-cut", .shortopt = "-L", .type = PWF_DOUBLE,
      .min = 0, .max = 1000, .def = 0, .unit = "Hz",
      .help = "high-pass on what goes into the reverb, 0 = off" },
    PWF_PARAM_END
};

static int rv_init(void *user, uint32_t channels, double max_rate)
{
    return reverb_init(user, channels, max_rate);
}

static void rv_fini(void *user)
{
    reverb_fini(user);
}

static uint32_t rv_design(void *user, const pwf_param *p, double rate,
                          uint32_t channels, void *coeffs)
{
    const reverb_state *st = user;

    (void)channels;
    /* The lengths are capped by what init() allocated for max-rate, so a
     * graph running faster than that gets a smaller room rather than an
     * overrun; damp-freq and low-cut are clamped below Nyquist in there. */
    reverb_design(coeffs, rate, pwf_value(p, "size"), pwf_value(p, "predelay"),
                  pwf_value(p, "decay"), pwf_value(p, "hf-decay"),
                  pwf_value(p, "damp-freq"), pwf_value(p, "modulation"),
                  pwf_value(p, "mix"), pwf_value(p, "low-cut"),
                  st->dcap, st->tcap);
    return 0;                   /* the predelay and the tail are the effect */
}

static void rv_process(void *user, const void *coeffs, int changed,
                       float *const *in, float *const *out,
                       uint32_t channels, uint32_t n)
{
    (void)changed;              /* the core glides to whatever it is given */
    reverb_run(user, coeffs, in, out, channels, n);
}

int main(int argc, char **argv)
{
    static reverb_state state;
    pwf_desc desc = {
        .name       = "pw_reverb",
        .summary    = "algorithmic reverb: diffuser and 16-line feedback delay network",
        .notes      =
            "decay is the time the tail takes to fall 60 dB, and it holds\n"
            "whatever size is: size sets how far apart the echoes are and how\n"
            "soon they thicken, from a small room at 10 to a hall at 100.\n"
            "hf-decay shortens the top end's tail, as air and soft walls do;\n"
            "100 is as bright as the low end. Each channel's tail comes out\n"
            "decorrelated from the others, so a mono source gets a wide one.\n"
            "On a send, set mix 100 and mix it in downstream; inline, 15-30\n"
            "is a room around a voice. size, predelay and modulation glide to\n"
            "a new value rather than jump, which is heard as a short swoop.\n"
            "No latency is reported: the predelay is the effect, as pw_delay's\n"
            "time is.",
        .params     = params,
        .coeff_size = sizeof(reverb_coeffs),
        .init       = rv_init,
        .fini       = rv_fini,
        .design     = rv_design,
        .process    = rv_process,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
