/*
 * pw_level -- loudness leveler as a PipeWire filter node.
 *
 * The DSP is in src/level_dsp.h, ported from reference/level.c: BS.1770
 * loudness over a sliding rectangular window, a gain that rides it towards
 * a target -- up as well as down, capped both ways, frozen below a gate --
 * smoothed over seconds, and optional lookahead.
 *
 * The one change for realtime, as for the compressor: the reference
 * absorbs the lookahead so its output is sample-aligned with its input; a
 * live filter cannot, so the delay is real here and is reported to
 * PipeWire as process latency, which the graph compensates for.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "level_dsp.h"

static pwf_param params[] = {
    { .name = "loudness", .shortopt = "-l", .type = PWF_DOUBLE,
      .min = -50, .max = 0, .def = -18, .unit = "LUFS",
      .help = "the loudness to ride towards; speech -16..-19, broadcast -23" },
    { .name = "strength", .shortopt = "-s", .type = PWF_DOUBLE,
      .min = 0, .max = 100, .def = 100, .unit = "%",
      .help = "how much of the way there to go; 50 halves the distance" },
    { .name = "max-boost", .shortopt = "-B", .type = PWF_DOUBLE,
      .min = 0, .max = 30, .def = 12, .unit = "dB",
      .help = "the most it will ever turn up" },
    { .name = "max-cut", .shortopt = "-C", .type = PWF_DOUBLE,
      .min = 0, .max = 30, .def = 12, .unit = "dB",
      .help = "the most it will ever turn down" },
    { .name = "gate", .shortopt = "-g", .type = PWF_DOUBLE,
      .min = -80, .max = -10, .def = -50, .unit = "LUFS",
      .help = "below this the gain holds still, so pauses are not ridden up" },
    { .name = "window", .shortopt = "-w", .type = PWF_DOUBLE,
      .min = LEVEL_MIN_WINDOW_MS, .max = LEVEL_MAX_WINDOW_MS, .def = 400,
      .unit = "ms",
      .help = "loudness window; 400 is momentary, 3000 short-term" },
    { .name = "attack", .shortopt = "-a", .type = PWF_DOUBLE,
      .min = 10, .max = 30000, .def = 1000, .unit = "ms",
      .help = "time constant turning down" },
    { .name = "release", .shortopt = "-e", .type = PWF_DOUBLE,
      .min = 10, .max = 60000, .def = 3000, .unit = "ms",
      .help = "time constant turning up" },
    { .name = "lookahead", .shortopt = "-k", .type = PWF_DOUBLE,
      .min = 0, .max = LEVEL_MAX_LOOK_MS, .def = 0, .unit = "ms",
      .help = "audio delay feeding the detector; half the window centres it" },
    PWF_PARAM_END
};

static int level_pw_init(void *user, uint32_t channels, double max_rate)
{
    return level_init(user, channels, max_rate);
}

static void level_pw_fini(void *user)
{
    level_fini(user);
}

static uint32_t level_pw_design(void *user, const pwf_param *p, double rate,
                                uint32_t channels, void *coeffs)
{
    level_state *s = user;
    level_coeffs *c = coeffs;

    (void)channels;
    level_design(c, rate, pwf_value(p, "loudness"), pwf_value(p, "strength"),
                 pwf_value(p, "max-boost"), pwf_value(p, "max-cut"),
                 pwf_value(p, "gate"), pwf_value(p, "window"),
                 pwf_value(p, "attack"), pwf_value(p, "release"),
                 pwf_value(p, "lookahead"));

    /* The delay line was sized for max-rate; past that the lookahead is
     * capped rather than overrunning it. */
    if (c->L > s->max_L) c->L = s->max_L;
    return (uint32_t)c->L;
}

/* Nothing to reset on a change: the hop ring is written in full whatever
 * the window, the gain glides to a new target on its own, and a new
 * lookahead steps the delay line, which is accepted and documented. */
static void level_pw_process(void *user, const void *coeffs, int changed,
                             float *const *in, float *const *out,
                             uint32_t channels, uint32_t n)
{
    (void)changed;
    level_run(user, coeffs, in, out, channels, n);
}

static double level_pw_gain(void *user, const void *coeffs)
{
    (void)coeffs;
    return level_gain_db(user);
}

/* Output loudness against input loudness once the gain has settled, from
 * the gate up: below it the gain holds wherever it was, so there is no
 * curve to draw there. */
static size_t level_pw_curve(void *user, const void *coeffs, double rate,
                             double *x, double *y, size_t max)
{
    const level_coeffs *c = coeffs;
    size_t i, n = pwf_level_grid(x, max), m = 0;

    (void)user; (void)rate;
    for (i = 0; i < n; i++) {
        if (x[i] < c->gate) continue;
        x[m] = x[i];
        y[m] = level_curve_db(c, x[i]);
        m++;
    }
    return m;
}

/* The longest the lookahead can make it, for the bypass's dry delay. */
static uint32_t level_pw_max_latency(void *user)
{
    return (uint32_t)((level_state *)user)->max_L;
}

int main(int argc, char **argv)
{
    static level_state state;
    pwf_desc desc = {
        .name       = "pw_level",
        .summary    = "loudness leveler: rides the gain towards a target LUFS",
        .notes      =
            "A slow automatic gain, the hand on the fader: it turns a quiet\n"
            "speaker up and a loud one down over seconds, and leaves each\n"
            "syllable's own dynamics alone -- that is a compressor's job.\n"
            "\n"
            "Loudness is ITU-R BS.1770's, K-weighted with the channels'\n"
            "powers summed, over a rectangular window: 400 ms is momentary\n"
            "loudness, 3000 short-term. The gain is strength x (loudness -\n"
            "measured), capped at max-boost and max-cut, and glides there at\n"
            "the attack going down and the release going up. gain-applied\n"
            "shows where it is: positive is a boost.\n"
            "\n"
            "Below the gate the gain holds still, so a pause or the room is\n"
            "not ridden up to the target. Set it between the noise floor and\n"
            "the quietest speech; it starts at 0 dB until there is speech.\n"
            "\n"
            "It does not catch peaks: a boost turns transients up with\n"
            "everything else, so follow it with pw_limit. The curve shows\n"
            "output loudness against input loudness once settled, from the\n"
            "gate up.\n"
            "\n"
            "Starting point for speech: loudness -18, max-boost 12, max-cut\n"
            "12, gate -50, window 400, attack 1000, release 3000.",
        .params     = params,
        .coeff_size = sizeof(level_coeffs),
        .init       = level_pw_init,
        .fini       = level_pw_fini,
        .design     = level_pw_design,
        .process    = level_pw_process,
        .max_latency = level_pw_max_latency,
        .gain_applied = level_pw_gain,
        .curve_kind = PWF_CURVE_LEVEL,
        .curve      = level_pw_curve,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
