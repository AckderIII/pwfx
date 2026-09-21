/*
 * gate.c -- high-quality noise gate / expander for raw 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the gated stream to stdout in the same format.
 *
 * Design notes
 * ------------
 *  - True lookahead. The audio is delayed by L samples while the detector
 *    runs on the undelayed signal, so the gate is already open by the time
 *    the transient reaches the output. Without this, any gate with a
 *    non-zero attack clips the leading consonant off every word. The
 *    detector uses an exact sliding maximum over the lookahead window,
 *    maintained by a monotonic deque (O(1) amortised per sample), not an
 *    approximation.
 *  - Latency is reported but NOT added to the stream: the first L samples
 *    of delay are swallowed and L extra samples are flushed at EOF, so the
 *    output is sample-aligned with the input and has the same length.
 *  - Hysteresis. Separate open and close thresholds, so a signal hovering
 *    at the threshold cannot chatter the gate.
 *  - Hold time. Once open, the gate stays open for at least the hold
 *    period after the signal falls below the close threshold. This is what
 *    stops the gate closing between syllables.
 *  - Sidechain filtering. The detector listens to a high-passed (and
 *    optionally low-passed) copy of the signal, so desk rumble, mic stand
 *    thumps and AC hum do not hold the gate open. The filters affect
 *    detection only; the audio path is untouched.
 *  - Channel-linked. One detector, one gain, applied to every channel.
 *    Per-channel gating would shift the stereo image every time the gate
 *    moves.
 *  - Raised-cosine gain ramps applied in the dB domain. The curve has zero
 *    slope at both ends (C1 continuous), so there is no derivative
 *    discontinuity at the start or end of a ramp and therefore no click --
 *    the usual failure mode of a linear or one-pole gain ramp.
 *  - Range rather than hard mute. The closed state attenuates by a set
 *    amount instead of slamming to digital black, which sounds far more
 *    natural on a live mic. At >= 90 dB of range the fully-closed state
 *    does snap to exact zero, so gated passages are true silence.
 *  - Detector and gain state are double precision; only the audio path is
 *    float, and it is only ever multiplied, never accumulated, so the
 *    stream keeps its full 32-bit float quality.
 *
 * Build:  cc -O2 -o gate gate.c -lm
 * Usage:  gate [options]   (see --help)
 *
 * Example (48 kHz stereo live mic):
 *   ... | ./gate -r 48000 -c 2 -t -35 --hold 150 --release 200 | ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_CHANNELS 64
#define MAX_BLOCK    4096

/* Below this the gate is considered fully closed and, if the range is deep
 * enough, the output is forced to exact zero. */
#define SILENCE_RANGE_DB 90.0

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { double s1, s2; }             bqstate_t;

static void biquad_hp(biquad_t *s, double fc, double fs, double q)
{
    double w0 = 2.0 * M_PI * fc / fs, cw = cos(w0);
    double alpha = sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
    double g = (1.0 + cw) / 2.0;
    s->b0 = g / a0;  s->b1 = -2.0 * g / a0;  s->b2 = g / a0;
    s->a1 = -2.0 * cw / a0;  s->a2 = (1.0 - alpha) / a0;
}

static void biquad_lp(biquad_t *s, double fc, double fs, double q)
{
    double w0 = 2.0 * M_PI * fc / fs, cw = cos(w0);
    double alpha = sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
    double g = (1.0 - cw) / 2.0;
    s->b0 = g / a0;  s->b1 = 2.0 * g / a0;  s->b2 = g / a0;
    s->a1 = -2.0 * cw / a0;  s->a2 = (1.0 - alpha) / a0;
}

/* Transposed direct form II. */
static double biquad_run(const biquad_t *c, bqstate_t *st, double x)
{
    double y = c->b0 * x + st->s1;
    st->s1 = c->b1 * x - c->a1 * y + st->s2;
    st->s2 = c->b2 * x - c->a2 * y;
    if (st->s1 > -1e-100 && st->s1 < 1e-100) st->s1 = 0.0;
    if (st->s2 > -1e-100 && st->s2 < 1e-100) st->s2 = 0.0;
    return y;
}

/* ---- exact sliding maximum over a fixed window (monotonic deque) ------- */

typedef struct {
    double    *val;
    long long *idx;
    int        cap, head, count;
} slidemax_t;

static int slidemax_init(slidemax_t *s, int window)
{
    s->cap  = window + 2;
    s->val  = malloc((size_t)s->cap * sizeof *s->val);
    s->idx  = malloc((size_t)s->cap * sizeof *s->idx);
    s->head = s->count = 0;
    return s->val && s->idx;
}

static void slidemax_free(slidemax_t *s) { free(s->val); free(s->idx); }

/* Push sample n with magnitude v, then drop everything older than n-window.
 * Returns the maximum over the window [n-window, n]. */
static double slidemax_push(slidemax_t *s, long long n, double v, int window)
{
    int back;

    while (s->count > 0) {
        back = (s->head + s->count - 1) % s->cap;
        if (s->val[back] > v) break;
        s->count--;
    }
    back = (s->head + s->count) % s->cap;
    s->val[back] = v;
    s->idx[back] = n;
    s->count++;

    while (s->count > 0 && s->idx[s->head] < n - window) {
        s->head = (s->head + 1) % s->cap;
        s->count--;
    }
    return s->val[s->head];
}

/* ----------------------------------------------------------------------- */

static int opt_is(const char *a, const char *s, const char *l)
{
    return (s && !strcmp(a, s)) || (l && !strcmp(a, l));
}

static void usage(const char *p)
{
    fprintf(stderr,
"usage: %s [options]   --  32-bit float noise gate, stdin -> stdout\n"
"\n"
"stream\n"
"  -r, --rate HZ           sample rate                      (48000)\n"
"  -c, --channels N        interleaved channels, 1..%d      (1)\n"
"  -b, --block N           frames read per block            (128)\n"
"                          smaller = lower live latency,\n"
"                          larger  = faster offline\n"
"\n"
"thresholds\n"
"  -t, --threshold DB      level that opens the gate, dBFS  (-35)\n"
"  -T, --close DB          level that closes it, dBFS       (threshold-6)\n"
"                          the gap between the two is the hysteresis that\n"
"                          keeps a borderline signal from chattering\n"
"\n"
"envelope\n"
"  -a, --attack MS         fade-in time once opened         (2)\n"
"  -o, --hold MS           minimum open time after the      (150)\n"
"                          signal drops below --close\n"
"  -e, --release MS        fade-out time                    (150)\n"
"  -k, --lookahead MS      audio delay feeding the detector (5)\n"
"                          keep >= attack or transients clip\n"
"\n"
"depth\n"
"  -g, --range DB          attenuation when closed          (120)\n"
"                          >= %.0f snaps the closed state to exact silence\n"
"\n"
"sidechain (detection only, audio path is untouched)\n"
"  -F, --sc-hpf HZ         high-pass before detection       (120)\n"
"  -G, --sc-lpf HZ         low-pass before detection, 0=off (0)\n"
"\n"
"  -h, --help              this text\n"
"\n"
"Output is sample-aligned with the input and the same length: the lookahead\n"
"delay is absorbed, not prepended. For a live voice mic, start at\n"
"-t -35 --hold 150 --release 200 and lower the threshold until the gate\n"
"stops clipping the ends of your words.\n",
        p, MAX_CHANNELS, SILENCE_RANGE_DB);
}

int main(int argc, char **argv)
{
    double rate = 48000.0, thresh_db = -35.0, close_db = 1.0; /* 1 = unset */
    double attack_ms = 2.0, hold_ms = 150.0, release_ms = 150.0;
    double look_ms = 5.0, range_db = 120.0;
    double sc_hpf = 120.0, sc_lpf = 0.0;
    int    channels = 1, block = 128, i, ch;

    biquad_t  hp, lp;
    bqstate_t hp_st[MAX_CHANNELS], lp_st[MAX_CHANNELS];
    int       use_hp, use_lp;

    slidemax_t sm;
    float     *ring = NULL;
    int        L, ring_frames, w = 0;
    long long  n = 0;
    long long  primed = 0;

    double open_lin, close_lin;
    double atk_inc, rel_inc, p = 0.0, gain = 0.0, last_p = -1.0;
    long long hold_samples, hold_left = 0;
    int    is_open = 0, snap_silent;

    static float buf[MAX_BLOCK * MAX_CHANNELS];
    static float out[MAX_BLOCK * MAX_CHANNELS];
    size_t got;
    int    rc = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (opt_is(a, "-h", "--help")) { usage(argv[0]); return 0; }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], a);
            return 2;
        }
        if      (opt_is(a, "-b", "--block"))     block      = atoi(argv[++i]);
        else if (opt_is(a, "-r", "--rate"))      rate       = atof(argv[++i]);
        else if (opt_is(a, "-c", "--channels"))  channels   = atoi(argv[++i]);
        else if (opt_is(a, "-t", "--threshold")) thresh_db  = atof(argv[++i]);
        else if (opt_is(a, "-T", "--close"))     close_db   = atof(argv[++i]);
        else if (opt_is(a, "-a", "--attack"))    attack_ms  = atof(argv[++i]);
        else if (opt_is(a, "-o", "--hold"))      hold_ms    = atof(argv[++i]);
        else if (opt_is(a, "-e", "--release"))   release_ms = atof(argv[++i]);
        else if (opt_is(a, "-k", "--lookahead")) look_ms    = atof(argv[++i]);
        else if (opt_is(a, "-g", "--range"))     range_db   = atof(argv[++i]);
        else if (opt_is(a, "-F", "--sc-hpf"))    sc_hpf     = atof(argv[++i]);
        else if (opt_is(a, "-G", "--sc-lpf"))    sc_lpf     = atof(argv[++i]);
        else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], a);
            usage(argv[0]);
            return 2;
        }
    }

    if (close_db > 0.0) close_db = thresh_db - 6.0;   /* default hysteresis */

    if (rate <= 0.0) {
        fprintf(stderr, "%s: sample rate must be positive\n", argv[0]); return 2;
    }
    if (channels < 1 || channels > MAX_CHANNELS) {
        fprintf(stderr, "%s: channels must be 1..%d\n", argv[0], MAX_CHANNELS);
        return 2;
    }
    if (block < 1 || block > MAX_BLOCK) {
        fprintf(stderr, "%s: block must be 1..%d frames\n", argv[0], MAX_BLOCK);
        return 2;
    }
    if (close_db > thresh_db) {
        fprintf(stderr, "%s: --close (%g) must not exceed --threshold (%g)\n",
                argv[0], close_db, thresh_db);
        return 2;
    }
    if (attack_ms < 0.0 || hold_ms < 0.0 || release_ms < 0.0 || look_ms < 0.0) {
        fprintf(stderr, "%s: times must not be negative\n", argv[0]); return 2;
    }
    if (look_ms > 1000.0) {
        fprintf(stderr, "%s: lookahead must be <= 1000 ms\n", argv[0]); return 2;
    }
    if (range_db < 0.0) {
        fprintf(stderr, "%s: range must not be negative\n", argv[0]); return 2;
    }
    if (sc_hpf < 0.0 || sc_hpf >= rate / 2.0 ||
        sc_lpf < 0.0 || sc_lpf >= rate / 2.0) {
        fprintf(stderr, "%s: sidechain frequencies must be 0..%g Hz\n",
                argv[0], rate / 2.0);
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    use_hp = sc_hpf > 0.0;
    use_lp = sc_lpf > 0.0;
    if (use_hp) biquad_hp(&hp, sc_hpf, rate, M_SQRT1_2);
    if (use_lp) biquad_lp(&lp, sc_lpf, rate, M_SQRT1_2);
    memset(hp_st, 0, sizeof hp_st);
    memset(lp_st, 0, sizeof lp_st);

    open_lin  = pow(10.0, thresh_db / 20.0);
    close_lin = pow(10.0, close_db  / 20.0);

    L            = (int)(look_ms   * rate / 1000.0 + 0.5);
    hold_samples = (long long)(hold_ms * rate / 1000.0 + 0.5);
    atk_inc = attack_ms  > 0.0 ? 1.0 / (attack_ms  * rate / 1000.0) : 1.0;
    rel_inc = release_ms > 0.0 ? 1.0 / (release_ms * rate / 1000.0) : 1.0;
    if (atk_inc > 1.0) atk_inc = 1.0;
    if (rel_inc > 1.0) rel_inc = 1.0;
    snap_silent = range_db >= SILENCE_RANGE_DB;

    ring_frames = L + 1;
    ring = calloc((size_t)ring_frames * (size_t)channels, sizeof *ring);
    if (!ring || !slidemax_init(&sm, L)) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        free(ring);
        return 1;
    }

    for (;;) {
        size_t frames, k, nout = 0;
        int    eof;

        got = fread(buf, sizeof(float), (size_t)block * (size_t)channels, stdin);

        /* Scrub non-finite input. A single NaN or Inf from a glitching capture
         * device or an upstream plugin would otherwise poison the sidechain
         * biquad state and the detector: NaN loses every comparison, so the
         * gate would sit shut forever with no way back. One pass here keeps
         * every downstream state variable finite by construction. */
        for (i = 0; (size_t)i < got; i++)
            if (!isfinite(buf[i])) buf[i] = 0.0f;

        frames = got / (size_t)channels;
        eof    = (frames == 0);

        /* At EOF, push L frames of silence to drain the delay line so the
         * output ends up exactly as long as the input. */
        if (eof) frames = (size_t)L;

        for (k = 0; k < frames; k++) {
            const float *in = eof ? NULL : &buf[k * (size_t)channels];
            double det = 0.0, s, gdb;
            int    rd;

            /* detector: sidechain-filtered peak, linked across channels */
            for (ch = 0; ch < channels; ch++) {
                double v = in ? (double)in[ch] : 0.0;
                if (use_hp) v = biquad_run(&hp, &hp_st[ch], v);
                if (use_lp) v = biquad_run(&lp, &lp_st[ch], v);
                v = fabs(v);
                if (v > det) det = v;
            }
            det = slidemax_push(&sm, n, det, L);

            /* state machine: hysteresis + hold */
            if (det >= open_lin) {
                is_open = 1;
                hold_left = hold_samples;
            } else if (det >= close_lin) {
                if (is_open) hold_left = hold_samples;
            } else if (is_open) {
                if (hold_left > 0) hold_left--;
                else is_open = 0;
            }

            /* raised-cosine ramp in the dB domain */
            p += is_open ? atk_inc : -rel_inc;
            if (p > 1.0) p = 1.0;
            if (p < 0.0) p = 0.0;
            if (p != last_p) {
                if (p <= 0.0 && snap_silent) {
                    gain = 0.0;
                } else {
                    s   = 0.5 - 0.5 * cos(M_PI * p);
                    gdb = -range_db * (1.0 - s);
                    gain = exp(gdb * (M_LN10 / 20.0));
                }
                last_p = p;
            }

            /* delay line: write current frame, read the frame L back */
            for (ch = 0; ch < channels; ch++)
                ring[(size_t)w * channels + ch] = in ? in[ch] : 0.0f;
            rd = (w + ring_frames - L) % ring_frames;

            if (primed >= L) {
                for (ch = 0; ch < channels; ch++)
                    out[nout * channels + ch] =
                        (float)(ring[(size_t)rd * channels + ch] * gain);
                nout++;
            } else {
                primed++;
            }

            w = (w + 1) % ring_frames;
            n++;
        }

        if (nout && fwrite(out, sizeof(float), nout * (size_t)channels, stdout)
                    != nout * (size_t)channels) {
            fprintf(stderr, "%s: write failed\n", argv[0]);
            rc = 1;
            break;
        }
        /* Flush every block. Without this the last partial stdio buffer sits
         * here instead of reaching the next stage, which shows up as latency
         * in a live pipeline rather than as a correctness bug. */
        fflush(stdout);

        if (eof) break;

        /* A torn frame can only appear at EOF on a malformed stream; pass the
         * stray samples through rather than dropping them. */
        if (got % (size_t)channels) {
            size_t tail = got % (size_t)channels;
            fwrite(&buf[frames * (size_t)channels], sizeof(float), tail, stdout);
        }
    }

    if (ferror(stdin)) {
        fprintf(stderr, "%s: read failed\n", argv[0]);
        rc = 1;
    }

    slidemax_free(&sm);
    free(ring);
    fflush(stdout);
    return rc;
}
