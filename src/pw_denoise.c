/*
 * pw_denoise -- noise suppression for voice as a PipeWire filter node.
 *
 * The DSP is in src/denoise_dsp.h, ported from reference/denoise.c: RNNoise
 * (third_party/rnnoise, BSD-3) framed into the blocks the graph hands over,
 * with the reduction limited rather than total and an optional gate on its
 * voice-activity estimate.
 *
 * Two things differ from the reference, both because this one runs live:
 *
 *  - The reference reads a frame at a time and absorbs RNNoise's delay; a
 *    live filter takes whatever block the graph gives it and plays out from
 *    the frame before, so its delay is real -- DN_LATENCY, 20 ms -- and is
 *    reported to the graph as latency.
 *  - RNNoise runs at 48 kHz only. The node asks the graph for 48 kHz; if the
 *    graph runs at another rate anyway, the filter passes its input through
 *    untouched and says so on stderr, rather than resample or produce noise
 *    of its own.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "denoise_dsp.h"

static pwf_param params[] = {
    { .name = "reduction", .shortopt = "-n", .type = PWF_DOUBLE,
      .min = 0, .max = 100, .def = 30, .unit = "dB",
      .help = "how far noise is pulled down; 100 is as far as it goes" },
    { .name = "vad-threshold", .shortopt = "-V", .type = PWF_DOUBLE,
      .min = 0, .max = 1, .def = 0,
      .help = "gate what is less sure than this to be speech; 0 = no gate" },
    PWF_PARAM_END
};

static int denoise_pw_init(void *user, uint32_t channels, double max_rate)
{
    (void)max_rate;
    return denoise_init(user, channels);
}

static void denoise_pw_fini(void *user)
{
    denoise_fini(user);
}

static uint32_t denoise_pw_design(void *user, const pwf_param *p, double rate,
                                  uint32_t channels, void *coeffs)
{
    denoise_coeffs *c = coeffs;
    static double warned_at = -1.0;

    (void)user; (void)channels;
    denoise_design(c, rate, pwf_value(p, "reduction"), pwf_value(p, "vad-threshold"));
    if (c->pass && rate != warned_at) {
        fprintf(stderr, "pw_denoise: the graph runs at %g Hz and RNNoise only "
                "at %d; passing the input through untouched\n", rate, DN_RATE);
        warned_at = rate;
    }
    return c->pass ? 0 : DN_LATENCY;
}

static void denoise_pw_process(void *user, const void *coeffs, int changed,
                               float *const *in, float *const *out,
                               uint32_t channels, uint32_t n)
{
    (void)changed;
    denoise_run(user, coeffs, in, out, channels, n);
}

static double denoise_pw_gr(void *user, const void *coeffs)
{
    (void)coeffs;
    return denoise_take_gr(user);
}

static uint32_t denoise_pw_max_latency(void *user)
{
    (void)user;
    return DN_LATENCY;
}

int main(int argc, char **argv)
{
    static denoise_state state;
    pwf_desc desc = {
        .name       = "pw_denoise",
        .summary    = "noise suppression for voice (RNNoise)",
        .notes      =
            "RNNoise, a recurrent neural network trained to tell a voice from\n"
            "the noise around it -- fans, keyboards, traffic, hum, steady or\n"
            "not -- and pull the noise down. It works at 48 kHz only: the node\n"
            "asks the graph for it, and passes the input through untouched if\n"
            "it does not get it.\n"
            "\n"
            "reduction limits how far: the noise comes out at most that many\n"
            "dB down, so the room still sounds like a room. 100 lets RNNoise\n"
            "take out all it can; 0 changes nothing.\n"
            "\n"
            "vad-threshold gates what RNNoise is less sure than that is\n"
            "speech down to the same level, held 200 ms after the last word.\n"
            "Around 0.5 to 0.9 for a talker on their own; 0, the default,\n"
            "is no gate.\n"
            "\n"
            "gr shows how far the signal is being pulled down. The latency is\n"
            "20 ms: a 10 ms frame of framing and RNNoise's own 10 ms.",
        .params     = params,
        .coeff_size = sizeof(denoise_coeffs),
        .init       = denoise_pw_init,
        .fini       = denoise_pw_fini,
        .design     = denoise_pw_design,
        .process    = denoise_pw_process,
        .gain_reduction = denoise_pw_gr,
        .max_latency = denoise_pw_max_latency,
        .rate       = DN_RATE,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
