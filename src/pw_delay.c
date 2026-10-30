/*
 * pw_delay -- delay and echo as a PipeWire filter node.
 *
 * The DSP is in src/delay_dsp.h, ported from reference/delay.c: a ring per
 * channel holding the input plus the fed-back delayed signal, mixed with
 * the dry input on the way out.
 *
 * Two things differ from the reference, both because this one runs live:
 *
 *  - The delay is NOT reported as latency, although it is one. Every other
 *    lookahead filter here reports its delay so the graph can compensate
 *    for it; a delay reported the same way would be compensated away, and
 *    lining a close mic up with a room mic is the whole point of a plain
 *    one. Latency reported: 0.
 *  - The time can change while audio flows, which is a crossfade from the
 *    old tap to the new one over 10 ms rather than a jump. The ring is
 *    sized once, at startup, for the longest time at max-rate.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "delay_dsp.h"

static pwf_param params[] = {
    { .name = "time", .shortopt = "-t", .type = PWF_DOUBLE,
      .min = 0, .max = DELAY_MAX_MS, .def = 0, .unit = "ms",
      .help = "how late the delayed signal is; 0 passes the input through" },
    { .name = "feedback", .shortopt = "-f", .type = PWF_DOUBLE,
      .min = 0, .max = 95, .def = 0, .unit = "%",
      .help = "how much of each repeat comes round again; 0 is one repeat" },
    { .name = "mix", .shortopt = "-x", .type = PWF_DOUBLE,
      .min = 0, .max = 100, .def = 100, .unit = "%",
      .help = "the delayed share of the output; 100 is the delay alone" },
    PWF_PARAM_END
};

static int delay_pw_init(void *user, uint32_t channels, double max_rate)
{
    return delay_init(user, channels, max_rate);
}

static void delay_pw_fini(void *user)
{
    delay_fini(user);
}

static uint32_t delay_pw_design(void *user, const pwf_param *p, double rate,
                                uint32_t channels, void *coeffs)
{
    delay_state *s = user;
    delay_coeffs *c = coeffs;

    (void)channels;
    delay_design(c, rate, pwf_value(p, "time"), pwf_value(p, "feedback"),
                 pwf_value(p, "mix"));

    /* The ring was sized for max-rate; if the graph runs faster than that,
     * the time is capped rather than overrunning it. */
    if (c->D > s->max_D) c->D = s->max_D;
    return 0;            /* deliberately: see the top of this file */
}

static void delay_pw_process(void *user, const void *coeffs, int changed,
                             float *const *in, float *const *out,
                             uint32_t channels, uint32_t n)
{
    (void)changed;       /* delay_run notices a new time by itself */
    delay_run(user, coeffs, in, out, channels, n);
}

int main(int argc, char **argv)
{
    static delay_state state;
    pwf_desc desc = {
        .name       = "pw_delay",
        .summary    = "delay and echo",
        .notes      =
            "With feedback 0 and mix 100 this is a plain delay: the input,\n"
            "late by the time and otherwise untouched -- for lining one\n"
            "source up with another. The delay is the effect, so it is not\n"
            "reported as latency, which would have the graph compensate it\n"
            "away.\n"
            "\n"
            "For an echo, add feedback and pull the mix down: the k-th\n"
            "repeat is at k x time, at mix x feedback^(k-1). Feedback is\n"
            "capped at 95 %, so it always dies away.\n"
            "\n"
            "Changing the time while audio flows crossfades to the new tap\n"
            "over 10 ms instead of clicking; it is still a jump between two\n"
            "moments in the past, and is heard as one.",
        .params     = params,
        .coeff_size = sizeof(delay_coeffs),
        .init       = delay_pw_init,
        .fini       = delay_pw_fini,
        .design     = delay_pw_design,
        .process    = delay_pw_process,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
