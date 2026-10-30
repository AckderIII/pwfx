/*
 * pw_gain -- gain, mute, polarity and balance as a PipeWire filter node.
 *
 * The one filter in the tree with no reference implementation behind it,
 * and deliberately: there is no DSP to port, only a multiplication per
 * sample. What a reference would pin down is pinned down by theory instead
 * -- tests/curves.py for the curve, tests/all.sh and tests/gain.py for the
 * level, the mute, the balance and the polarity measured in the graph.
 *
 * Each channel's gain is worked out on the main thread as one signed linear
 * factor: the gain in dB, times 0 if muted, times -1 if that channel is
 * inverted, times the balance. Every change is a 10 ms linear ramp from the
 * factor in use to the new one, like the bypass crossfade, so nothing here
 * can click -- not the mute, and not a polarity flip, which passes through
 * zero on its way rather than jumping across it.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pwf.h"
#include "dsp.h"

#define GAIN_RAMP_MS 10.0

static const char *const invert_modes[] = { "off", "all", "left", "right", NULL };
enum { INV_OFF, INV_ALL, INV_LEFT, INV_RIGHT };

static pwf_param params[] = {
    { .name = "gain", .shortopt = "-g", .type = PWF_DOUBLE,
      .min = -60, .max = 24, .def = 0, .unit = "dB",
      .help = "gain; -60 is quiet, not silent -- that is what mute is for" },
    { .name = "mute", .shortopt = "-m", .type = PWF_BOOL, .def = 0,
      .help = "silence, faded over 10 ms" },
    { .name = "invert", .shortopt = "-i", .type = PWF_ENUM,
      .choices = invert_modes, .def = INV_OFF,
      .help = "flip the polarity of every channel, or of channel 1 or 2" },
    { .name = "balance", .shortopt = "-b", .type = PWF_DOUBLE,
      .min = -100, .max = 100, .def = 0, .unit = "%",
      .help = "turn channel 2 (right, +) or channel 1 (left, -) down" },
    PWF_PARAM_END
};

typedef struct {
    double   target[PWF_MAX_CHANNELS];  /* signed linear factor per channel */
    uint32_t ramp;                      /* samples to reach it */
} gain_coeffs;

typedef struct {
    double   cur[PWF_MAX_CHANNELS];     /* factor in use */
    double   from[PWF_MAX_CHANNELS];    /* where the ramp started */
    uint32_t left;                      /* samples of ramp still to go */
    int      started;
} gain_state;

static int gain_init(void *user, uint32_t channels, double max_rate)
{
    (void)channels; (void)max_rate;
    memset(user, 0, sizeof(gain_state));
    return 0;
}

static uint32_t gain_design(void *user, const pwf_param *p, double rate,
                            uint32_t channels, void *coeffs)
{
    gain_coeffs *c = coeffs;
    double g   = pwf_value(p, "mute") != 0 ? 0.0 : dsp_db2lin(pwf_value(p, "gain"));
    double bal = pwf_value(p, "balance") / 100.0;
    int inv    = (int)pwf_value(p, "invert");
    uint32_t ch;

    (void)user;
    for (ch = 0; ch < channels && ch < PWF_MAX_CHANNELS; ch++) {
        double k = g;
        /* Balance only ever turns a side down: at 0 both are untouched,
         * which is what makes it safe to leave in a chain. Channels past
         * the first two have no side and are left alone. */
        if (channels >= 2 && ch == 0 && bal > 0.0) k *= 1.0 - bal;
        if (channels >= 2 && ch == 1 && bal < 0.0) k *= 1.0 + bal;
        if (inv == INV_ALL || (inv == INV_LEFT && ch == 0) ||
            (inv == INV_RIGHT && ch == 1))
            k = -k;
        c->target[ch] = k;
    }
    c->ramp = (uint32_t)(GAIN_RAMP_MS * rate / 1000.0) + 1;
    return 0;
}

static void gain_process(void *user, const void *coeffs, int changed,
                         float *const *in, float *const *out,
                         uint32_t channels, uint32_t n)
{
    gain_state *s = user;
    const gain_coeffs *c = coeffs;
    uint32_t ch, i, left = 0;

    /* The first configuration is the one the filter starts in, already in
     * force on the first sample -- a stage that fades itself in every time
     * it is started would be a strange thing to put in a chain. Every later
     * one is ramped to, from wherever the last ramp had got to. */
    if (!s->started) {
        for (ch = 0; ch < channels; ch++) s->cur[ch] = c->target[ch];
        s->started = 1;
        s->left = 0;
    } else if (changed) {
        for (ch = 0; ch < channels; ch++) s->from[ch] = s->cur[ch];
        s->left = c->ramp;
    }

    for (ch = 0; ch < channels; ch++) {
        const float *x = in[ch];
        float *y = out[ch];
        double t = c->target[ch], f = s->from[ch];

        left = s->left;
        for (i = 0; i < n; i++) {
            double k = t;
            if (left > 0) {
                k = t + (f - t) * ((double)left / (double)c->ramp);
                left--;
            }
            y[i] = (float)((double)x[i] * k);
        }
        s->cur[ch] = left > 0 ? t + (f - t) * ((double)left / (double)c->ramp) : t;
    }
    s->left = left;
}

/* Output level against input level, from the factors running: a straight
 * line offset by the gain, or nothing at all when muted. With the balance
 * off centre the channels differ, and the line is the louder one -- the
 * level the stage can reach. */
static size_t gain_curve(void *user, const void *coeffs, double rate,
                         double *x, double *y, size_t max)
{
    const gain_coeffs *c = coeffs;
    double k = 0.0, db;
    size_t i, n = pwf_level_grid(x, max);
    uint32_t ch;

    (void)user; (void)rate;
    for (ch = 0; ch < PWF_MAX_CHANNELS; ch++)
        if (fabs(c->target[ch]) > k) k = fabs(c->target[ch]);
    db = k > 0.0 ? DSP_LOG10_20 * log(k) : -INFINITY;
    for (i = 0; i < n; i++) y[i] = x[i] + db;
    return n;
}

int main(int argc, char **argv)
{
    static gain_state state;
    pwf_desc desc = {
        .name       = "pw_gain",
        .summary    = "gain, mute, polarity and balance",
        .notes      =
            "Every change is a 10 ms ramp, so none of them clicks: a mute\n"
            "fades, and a polarity flip passes through silence rather than\n"
            "jumping across it.\n"
            "\n"
            "Balance only ever turns one side down, by the percentage: at\n"
            "+50 the left channel is at half (-6 dB) and the right is\n"
            "untouched. It applies to channels 1 and 2 and leaves any\n"
            "others alone; on a mono stage it does nothing.\n"
            "\n"
            "Nothing is clipped -- the stream stays float -- so put pw_limit\n"
            "after a stage that adds gain if what follows cannot take it.",
        .params     = params,
        .coeff_size = sizeof(gain_coeffs),
        .init       = gain_init,
        .design     = gain_design,
        .process    = gain_process,
        .curve_kind = PWF_CURVE_LEVEL,
        .curve      = gain_curve,
        .user       = &state,
    };
    return pwf_run(&desc, argc, argv);
}
