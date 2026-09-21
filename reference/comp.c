/*
 * comp.c -- high-quality dynamic range compressor for raw 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the compressed stream to stdout in the same format.
 *
 * Design notes
 * ------------
 *  - Log-domain topology throughout: the level is converted to dB once, the
 *    static curve is evaluated in dB, and the attack/release smoothing is
 *    applied to the gain-reduction signal in dB rather than to the linear
 *    envelope. This is the "smooth decoupled peak detector" arrangement from
 *    Giannoulis, Massberg & Reiss, "Digital Dynamic Range Compressor Design
 *    -- A Tutorial and Analysis", JAES 60(6), 2012. Smoothing gain reduction
 *    in dB is what makes the attack and release times behave the same way
 *    regardless of how far above threshold the signal sits; smoothing a
 *    linear envelope instead makes them level-dependent, which is the usual
 *    cause of a compressor that "breathes" differently on loud and quiet
 *    passages.
 *
 *        y1[n] = max(xL[n], aR*y1[n-1] + (1-aR)*xL[n])
 *        yL[n] = aA*yL[n-1] + (1-aA)*y1[n]
 *
 *    The branching max() decouples attack from release so a new transient
 *    during a release is caught instantly, while the second one-pole rounds
 *    the corner out of the release -- the audible benefit over the plain
 *    decoupled detector, which leaves a kink you can hear as a chirp.
 *  - Soft knee. Quadratic interpolation across a knee of configurable width,
 *    continuous in both value and slope at each end, so the transition into
 *    compression is gradual instead of a hard corner.
 *  - Lookahead. The audio is delayed while the detector runs on the
 *    undelayed signal, so gain reduction is already in place when the
 *    transient arrives instead of arriving one attack-time late. The delay
 *    is absorbed, not prepended: output is sample-aligned with the input and
 *    exactly as long.
 *  - Peak or RMS detection. RMS (a one-pole on the squared signal) tracks
 *    perceived loudness and ignores isolated peaks; peak catches everything.
 *  - Channel linking. By default one gain, derived from the loudest channel,
 *    is applied to all of them. Compressing channels independently pulls the
 *    stereo image toward whichever side is momentarily louder.
 *  - Sidechain high-pass. Keeps bass energy from pumping the whole mix. It
 *    affects detection only; the audio path is never filtered.
 *  - Detector, gain and filter state are double precision. The audio path
 *    stays float and is only ever multiplied, never accumulated, so the
 *    stream keeps full 32-bit float quality.
 *  - No output clipping. The stream stays float; put a limiter after this if
 *    you need a hard ceiling.
 *
 * Build:  cc -O2 -o comp comp.c -lm
 * Usage:  comp [options]   (see --help)
 *
 * Example (48 kHz stereo live voice):
 *   ... | ./comp -r 48000 -c 2 -t -18 -R 3 -a 10 -e 120 -m 6 | ...
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
#define DB_FLOOR     (-240.0)   /* level floor, below float32 resolution */

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

static double biquad_run(const biquad_t *c, bqstate_t *st, double x)
{
    double y = c->b0 * x + st->s1;
    st->s1 = c->b1 * x - c->a1 * y + st->s2;
    st->s2 = c->b2 * x - c->a2 * y;
    if (st->s1 > -1e-100 && st->s1 < 1e-100) st->s1 = 0.0;
    if (st->s2 > -1e-100 && st->s2 < 1e-100) st->s2 = 0.0;
    return y;
}

/* ---- static characteristic ---------------------------------------------
 * Returns the output level in dB for an input level in dB. Below the knee
 * the curve is the identity; above it the slope is 1/ratio; across the knee
 * the two are joined by a quadratic that matches value and slope at both
 * ends, so the curve is C1 everywhere.
 */
static double curve_db(double x, double thr, double ratio, double knee)
{
    double d = x - thr;

    if (knee > 0.0 && 2.0 * d > -knee && 2.0 * d < knee) {
        double t = d + knee / 2.0;
        return x + (1.0 / ratio - 1.0) * t * t / (2.0 * knee);
    }
    if (2.0 * d <= -knee)
        return x;
    return thr + d / ratio;
}

static int opt_is(const char *a, const char *s, const char *l)
{
    return (s && !strcmp(a, s)) || (l && !strcmp(a, l));
}

static void usage(const char *p)
{
    fprintf(stderr,
"usage: %s [options]   --  32-bit float compressor, stdin -> stdout\n"
"\n"
"stream\n"
"  -r, --rate HZ         sample rate                        (48000)\n"
"  -c, --channels N      interleaved channels, 1..%d        (1)\n"
"  -b, --block N         frames read per block              (128)\n"
"                        smaller = lower live latency,\n"
"                        larger  = faster offline\n"
"\n"
"static curve\n"
"  -t, --threshold DB    level where compression starts     (-18)\n"
"  -R, --ratio X         input:output above threshold       (3)\n"
"                        1 = off, 20+ behaves as limiting\n"
"  -w, --knee DB         soft-knee width, 0 = hard knee     (6)\n"
"                        the knee straddles the threshold,\n"
"                        so it spans threshold +/- w/2\n"
"\n"
"dynamics\n"
"  -a, --attack MS       gain-reduction time constant, up   (10)\n"
"  -e, --release MS      gain-reduction time constant, down (120)\n"
"                        both are 63%% time constants; the\n"
"                        10-90%% time is about 2.2x these\n"
"  -k, --lookahead MS    audio delay feeding the detector   (5)\n"
"  -q, --rms MS          RMS averaging window, 0 = peak     (0)\n"
"\n"
"output\n"
"  -m, --makeup DB       fixed makeup gain                  (0)\n"
"  -M, --auto-makeup     makeup that maps 0 dBFS back to\n"
"                        0 dBFS; adds to --makeup\n"
"\n"
"detection\n"
"  -l, --link MODE       max | avg | off                    (max)\n"
"                        off compresses each channel alone\n"
"                        and will shift a stereo image\n"
"  -F, --sc-hpf HZ       high-pass before detection, 0=off  (0)\n"
"  -G, --sc-lpf HZ       low-pass before detection, 0=off   (0)\n"
"\n"
"  -v, --verbose         report settings and peak gain reduction to stderr\n"
"  -h, --help            this text\n"
"\n"
"Output is sample-aligned with the input and the same length: the lookahead\n"
"delay is absorbed, not prepended. Nothing is clipped -- the stream stays\n"
"float, so chain a limiter after this if you need a hard ceiling.\n"
"\n"
"Live voice starting point:  -t -18 -R 3 -a 10 -e 120 -w 6 -m 6\n"
"Gentle levelling:           -t -24 -R 2 -a 25 -e 250 -w 10\n",
        p, MAX_CHANNELS);
}

int main(int argc, char **argv)
{
    double rate = 48000.0, thr_db = -18.0, ratio = 3.0, knee = 6.0;
    double attack_ms = 10.0, release_ms = 120.0, look_ms = 5.0, rms_ms = 0.0;
    double makeup_db = 0.0, sc_hpf = 0.0, sc_lpf = 0.0;
    int    channels = 1, block = 128, auto_makeup = 0, verbose = 0, link = 0;
    int    i, ch, d;

    biquad_t  hp, lp;
    bqstate_t hp_st[MAX_CHANNELS], lp_st[MAX_CHANNELS];
    int       use_hp, use_lp;

    double y1[MAX_CHANNELS], yL[MAX_CHANNELS], rms_acc[MAX_CHANNELS];
    double gain[MAX_CHANNELS];
    int    ndet;

    float    *ring = NULL;
    int       L, ring_frames, w = 0;
    long long primed = 0;

    double aA, aR, aRms, makeup_lin, peak_gr = 0.0;
    const double LOG10_20 = 20.0 / M_LN10, LN10_20 = M_LN10 / 20.0;

    static float buf[MAX_BLOCK * MAX_CHANNELS];
    static float out[MAX_BLOCK * MAX_CHANNELS];
    size_t got;
    int    rc = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (opt_is(a, "-h", "--help"))         { usage(argv[0]); return 0; }
        if (opt_is(a, "-M", "--auto-makeup"))  { auto_makeup = 1; continue; }
        if (opt_is(a, "-v", "--verbose"))      { verbose = 1; continue; }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], a);
            return 2;
        }
        if      (opt_is(a, "-b", "--block"))      block      = atoi(argv[++i]);
        else if (opt_is(a, "-r", "--rate"))       rate       = atof(argv[++i]);
        else if (opt_is(a, "-c", "--channels"))   channels   = atoi(argv[++i]);
        else if (opt_is(a, "-t", "--threshold"))  thr_db     = atof(argv[++i]);
        else if (opt_is(a, "-R", "--ratio"))      ratio      = atof(argv[++i]);
        else if (opt_is(a, "-w", "--knee"))       knee       = atof(argv[++i]);
        else if (opt_is(a, "-a", "--attack"))     attack_ms  = atof(argv[++i]);
        else if (opt_is(a, "-e", "--release"))    release_ms = atof(argv[++i]);
        else if (opt_is(a, "-k", "--lookahead"))  look_ms    = atof(argv[++i]);
        else if (opt_is(a, "-q", "--rms"))        rms_ms     = atof(argv[++i]);
        else if (opt_is(a, "-m", "--makeup"))     makeup_db  = atof(argv[++i]);
        else if (opt_is(a, "-F", "--sc-hpf"))     sc_hpf     = atof(argv[++i]);
        else if (opt_is(a, "-G", "--sc-lpf"))     sc_lpf     = atof(argv[++i]);
        else if (opt_is(a, "-l", "--link")) {
            const char *v = argv[++i];
            if      (!strcmp(v, "max")) link = 0;
            else if (!strcmp(v, "avg")) link = 1;
            else if (!strcmp(v, "off")) link = 2;
            else {
                fprintf(stderr, "%s: --link must be max, avg or off\n", argv[0]);
                return 2;
            }
        } else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], a);
            usage(argv[0]);
            return 2;
        }
    }

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
    if (!(ratio >= 1.0)) {
        fprintf(stderr, "%s: ratio must be >= 1\n", argv[0]); return 2;
    }
    if (knee < 0.0) {
        fprintf(stderr, "%s: knee must not be negative\n", argv[0]); return 2;
    }
    if (attack_ms < 0.0 || release_ms < 0.0 || look_ms < 0.0 || rms_ms < 0.0) {
        fprintf(stderr, "%s: times must not be negative\n", argv[0]); return 2;
    }
    if (look_ms > 1000.0 || rms_ms > 1000.0) {
        fprintf(stderr, "%s: lookahead and rms must be <= 1000 ms\n", argv[0]);
        return 2;
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

    ndet = (link == 2) ? channels : 1;
    for (d = 0; d < MAX_CHANNELS; d++) {
        y1[d] = yL[d] = rms_acc[d] = 0.0;
        gain[d] = 1.0;
    }

    /* alpha = exp(-1 / (tau * fs)); a zero time gives alpha = 0, i.e. instant */
    aA   = attack_ms  > 0.0 ? exp(-1000.0 / (attack_ms  * rate)) : 0.0;
    aR   = release_ms > 0.0 ? exp(-1000.0 / (release_ms * rate)) : 0.0;
    aRms = rms_ms     > 0.0 ? exp(-1000.0 / (rms_ms     * rate)) : 0.0;

    if (auto_makeup)
        makeup_db += 0.0 - curve_db(0.0, thr_db, ratio, knee);
    makeup_lin = exp(makeup_db * LN10_20);

    L           = (int)(look_ms * rate / 1000.0 + 0.5);
    ring_frames = L + 1;
    ring = calloc((size_t)ring_frames * (size_t)channels, sizeof *ring);
    if (!ring) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    if (verbose)
        fprintf(stderr,
            "comp: %g Hz, %d ch, thr %g dB, ratio %g:1, knee %g dB,\n"
            "      attack %g ms, release %g ms, lookahead %g ms (%d smp),\n"
            "      detector %s, link %s, makeup %+g dB\n",
            rate, channels, thr_db, ratio, knee, attack_ms, release_ms,
            look_ms, L, rms_ms > 0.0 ? "rms" : "peak",
            link == 0 ? "max" : link == 1 ? "avg" : "off", makeup_db);

    for (;;) {
        size_t frames, n, nout = 0;
        int    eof;

        got = fread(buf, sizeof(float), (size_t)block * (size_t)channels, stdin);

        /* Scrub non-finite input. A single NaN or Inf from a glitching capture
         * device or an upstream plugin would otherwise enter the detector's
         * feedback path and wedge it permanently: NaN loses every comparison
         * so the gain state never recovers, and Inf pins gain reduction at
         * infinity, muting the stream for good. One pass here keeps every
         * downstream state variable finite by construction. */
        for (i = 0; (size_t)i < got; i++)
            if (!isfinite(buf[i])) buf[i] = 0.0f;

        frames = got / (size_t)channels;
        eof    = (frames == 0);

        /* At EOF push L frames of silence to drain the delay line, so the
         * output ends up exactly as long as the input. */
        if (eof) frames = (size_t)L;

        for (n = 0; n < frames; n++) {
            const float *in = eof ? NULL : &buf[n * (size_t)channels];
            double sc[MAX_CHANNELS];
            int    rd;

            /* sidechain conditioning, per channel */
            for (ch = 0; ch < channels; ch++) {
                double v = in ? (double)in[ch] : 0.0;
                if (use_hp) v = biquad_run(&hp, &hp_st[ch], v);
                if (use_lp) v = biquad_run(&lp, &lp_st[ch], v);
                sc[ch] = v;
            }

            for (d = 0; d < ndet; d++) {
                double det, xdb, xL, gdb;

                if (link == 2) {
                    det = fabs(sc[d]);
                    if (aRms > 0.0) {
                        rms_acc[d] = aRms * rms_acc[d] + (1.0 - aRms) * det * det;
                        det = sqrt(rms_acc[d]);
                    }
                } else if (link == 0) {          /* max */
                    det = 0.0;
                    for (ch = 0; ch < channels; ch++) {
                        double v = fabs(sc[ch]);
                        if (v > det) det = v;
                    }
                    if (aRms > 0.0) {
                        rms_acc[0] = aRms * rms_acc[0] + (1.0 - aRms) * det * det;
                        det = sqrt(rms_acc[0]);
                    }
                } else {                          /* avg */
                    double s = 0.0;
                    for (ch = 0; ch < channels; ch++) s += sc[ch] * sc[ch];
                    s /= (double)channels;
                    if (aRms > 0.0) {
                        rms_acc[0] = aRms * rms_acc[0] + (1.0 - aRms) * s;
                        s = rms_acc[0];
                    }
                    det = sqrt(s);
                }

                xdb = det > 0.0 ? LOG10_20 * log(det) : DB_FLOOR;
                if (xdb < DB_FLOOR) xdb = DB_FLOOR;

                /* gain reduction demanded by the static curve, in dB >= 0 */
                xL = xdb - curve_db(xdb, thr_db, ratio, knee);
                if (xL < 0.0) xL = 0.0;

                /* smooth decoupled peak detector, in the log domain */
                {
                    double rel = aR * y1[d] + (1.0 - aR) * xL;
                    y1[d] = xL > rel ? xL : rel;
                    yL[d] = aA * yL[d] + (1.0 - aA) * y1[d];
                }
                if (yL[d] < 1e-12) yL[d] = 0.0;
                if (yL[d] > peak_gr) peak_gr = yL[d];

                gdb = makeup_db - yL[d];
                gain[d] = yL[d] == 0.0 ? makeup_lin : exp(gdb * LN10_20);
            }

            /* delay line: write current frame, read the frame L back */
            for (ch = 0; ch < channels; ch++)
                ring[(size_t)w * channels + ch] = in ? in[ch] : 0.0f;
            rd = (w + ring_frames - L) % ring_frames;

            if (primed >= L) {
                for (ch = 0; ch < channels; ch++)
                    out[nout * channels + ch] =
                        (float)(ring[(size_t)rd * channels + ch] *
                                gain[link == 2 ? ch : 0]);
                nout++;
            } else {
                primed++;
            }

            w = (w + 1) % ring_frames;
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

    if (verbose)
        fprintf(stderr, "comp: peak gain reduction %.2f dB\n", peak_gr);

    free(ring);
    fflush(stdout);
    return rc;
}
