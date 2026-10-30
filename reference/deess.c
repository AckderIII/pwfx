/*
 * deess.c -- de-esser for raw 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the de-essed stream to stdout in the same format.
 *
 * Design notes
 * ------------
 *  - Split-band reduction as a subtractive dynamic EQ:
 *
 *        y = x - (1 - g) * bp(x)
 *
 *    bp is a band-pass at the sibilance frequency with a constant 0 dB peak
 *    (RBJ), so at its centre it has unity gain and zero phase and the cut
 *    there is exactly g; away from the centre the cut falls off with the
 *    band's skirt and the body of the voice is untouched. At g = 1 the
 *    second term is exactly zero, so when nothing is being cut the output
 *    IS the input, bit for bit. A crossover split -- the usual way to do
 *    split-band -- sums back to an allpass, not to the input, and smears
 *    the phase of everything whether a sibilant is there or not.
 *  - Wide mode (--mode wide) applies g to the whole signal instead: the
 *    classic broadband de-esser, gentler on material where a band cut makes
 *    the "s" sound lispy.
 *  - The detector listens to the band, as an RMS. A sibilant is noise; its
 *    peaks say little about how loud it sounds, its power does. The band
 *    and the whole signal are averaged over the same 5 ms, a choice between
 *    two faults. Shorter, the whole signal's level follows the waveform of
 *    the voice's fundamental -- a 200 Hz tone swings a 1 ms RMS by 2 dB --
 *    and the peak-holding smoothing below cuts by the top of each swing,
 *    deeper the lower the voice. Averaged over different times, the faster
 *    of the two leads at every onset, and every word starts by reading as
 *    sibilance. Over the same 5 ms both rise together and a 100 Hz
 *    fundamental moves the level by about 0.3 dB; the default lookahead
 *    covers the window, so the cut is still in place when the "s" arrives.
 *  - Relative detection, the default: the threshold is the band's level
 *    relative to the whole signal's. A loud open vowel with a bright edge
 *    has a lot of band energy in absolute terms but little relative to its
 *    own level, and does not trigger; a quiet "s" is nearly all band and
 *    does. --detect abs compares the band's level against the threshold in
 *    dBFS instead. In relative mode a band below -60 dBFS counts as
 *    silence: in silence both levels sit at the floor, their difference is
 *    0 dB, and without the floor the de-esser would clamp down in every
 *    pause and duck the first syllable after it.
 *  - The gain computer is a compressor's: a 6 dB soft knee (quadratic,
 *    C1 continuous), a ratio above the threshold, and the reduction capped
 *    at --range, so the hardest "s" is still cut by no more than asked.
 *  - Smoothing is the smooth decoupled peak detector of Giannoulis, Massberg
 *    and Reiss (JAES 2012), applied to the gain reduction in dB -- the same
 *    as comp.c, so attack and release behave the same at every level.
 *  - True lookahead: the audio is delayed by L samples while the detector
 *    runs on the undelayed signal, so the cut is in place when the sibilant
 *    arrives instead of an attack time after it. The delay is absorbed:
 *    the first L outputs are dropped and L frames of silence are flushed at
 *    EOF, so the output is sample-aligned with the input and as long.
 *  - Channel-linked: one detector on the loudest channel, one gain. Ducking
 *    one side of a stereo voice on each "s" would pull the image across.
 *  - --listen outputs the band alone, delayed like everything else: what
 *    the detector hears, for finding the frequency by ear.
 *  - Filter, detector and gain state are double precision, and so is the
 *    band's delay line, so the subtraction is done at full precision; the
 *    audio is float in and float out.
 *
 * Build:  cc -O2 -o deess deess.c -lm
 * Usage:  deess [options]   (see --help)
 *
 * Example (48 kHz mono voice):
 *   ... | ./deess -r 48000 -f 6500 -t -12 -R 4 -g 10 | ...
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
#define MAX_LOOK_MS  10.0
#define RMS_MS       5.0
#define KNEE_DB      6.0
#define REL_FLOOR    (-60.0)
#define DB_FLOOR     (-240.0)

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { double s1, s2; }             bqstate_t;

static double flush_denorm(double v)
{
    return (v > -1e-100 && v < 1e-100) ? 0.0 : v;
}

/* RBJ band-pass, constant 0 dB peak gain. */
static void biquad_bp(biquad_t *s, double f0, double fs, double q)
{
    double w0 = 2.0 * M_PI * f0 / fs, cw = cos(w0);
    double alpha = sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
    s->b0 = alpha / a0;  s->b1 = 0.0;  s->b2 = -alpha / a0;
    s->a1 = -2.0 * cw / a0;  s->a2 = (1.0 - alpha) / a0;
}

/* Transposed direct form II. */
static double biquad_run(const biquad_t *c, bqstate_t *st, double x)
{
    double y = c->b0 * x + st->s1;
    st->s1 = flush_denorm(c->b1 * x - c->a1 * y + st->s2);
    st->s2 = flush_denorm(c->b2 * x - c->a2 * y);
    return y;
}

/* ---- static characteristic ---------------------------------------------
 * Output level in dB for an input level in dB: identity below the knee,
 * slope 1/ratio above it, joined across the knee by a quadratic matching
 * value and slope at both ends.
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
"usage: %s [options]   --  32-bit float de-esser, stdin -> stdout\n"
"\n"
"stream\n"
"  -r, --rate HZ         sample rate                        (48000)\n"
"  -c, --channels N      interleaved channels, 1..%d        (1)\n"
"  -b, --block N         frames read per block              (128)\n"
"\n"
"band\n"
"  -f, --freq HZ         centre of the sibilance band       (6500)\n"
"                        most voices: 5000..8000\n"
"  -Q, --q Q             width of the band; higher is       (1.5)\n"
"                        narrower\n"
"\n"
"detection\n"
"  -d, --detect MODE     rel | abs                          (rel)\n"
"                        rel: the band's level relative to\n"
"                        the whole signal's; abs: in dBFS\n"
"  -t, --threshold DB    where cutting starts               (-12)\n"
"                        rel: -15..-5 is typical\n"
"                        abs: -40..-20 is typical\n"
"\n"
"reduction\n"
"  -R, --ratio X         band:output above threshold        (4)\n"
"  -g, --range DB        the most it will ever cut          (12)\n"
"  -m, --mode MODE       split | wide                       (split)\n"
"                        split cuts the band only; wide\n"
"                        turns the whole signal down\n"
"\n"
"dynamics\n"
"  -a, --attack MS       reduction time constant, up        (1)\n"
"  -e, --release MS      reduction time constant, down      (60)\n"
"  -k, --lookahead MS    audio delay feeding the detector   (5)\n"
"                        0..%g\n"
"\n"
"  -L, --listen          output the band alone, to tune -f by ear\n"
"  -v, --verbose         report settings and peak reduction to stderr\n"
"  -h, --help            this text\n"
"\n"
"Output is sample-aligned with the input and the same length: the lookahead\n"
"delay is absorbed, not prepended. With nothing to cut, split mode passes\n"
"the input through bit for bit.\n",
        p, MAX_CHANNELS, MAX_LOOK_MS);
}

int main(int argc, char **argv)
{
    double rate = 48000.0, freq = 6500.0, q = 1.5;
    double thr_db = -12.0, ratio = 4.0, range = 12.0;
    double attack_ms = 1.0, release_ms = 60.0, look_ms = 5.0;
    int    channels = 1, block = 128, detect_abs = 0, wide = 0;
    int    listen = 0, verbose = 0;
    int    i, ch;

    biquad_t  bp;
    bqstate_t bp_st[MAX_CHANNELS];
    double    acc_b = 0.0, acc_x = 0.0, y1 = 0.0, yL = 0.0, peak_gr = 0.0;
    double    aA, aR, aRms;
    const double LOG10_10 = 10.0 / M_LN10, LN10_20 = M_LN10 / 20.0;

    float    *ring_x = NULL;
    double   *ring_b = NULL;
    int       L, ring_frames, w = 0;
    long long primed = 0;

    static float buf[MAX_BLOCK * MAX_CHANNELS];
    static float out[MAX_BLOCK * MAX_CHANNELS];
    size_t got;
    int    rc = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (opt_is(a, "-h", "--help"))     { usage(argv[0]); return 0; }
        if (opt_is(a, "-L", "--listen"))   { listen = 1; continue; }
        if (opt_is(a, "-v", "--verbose"))  { verbose = 1; continue; }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], a);
            return 2;
        }
        if      (opt_is(a, "-b", "--block"))      block      = atoi(argv[++i]);
        else if (opt_is(a, "-r", "--rate"))       rate       = atof(argv[++i]);
        else if (opt_is(a, "-c", "--channels"))   channels   = atoi(argv[++i]);
        else if (opt_is(a, "-f", "--freq"))       freq       = atof(argv[++i]);
        else if (opt_is(a, "-Q", "--q"))          q          = atof(argv[++i]);
        else if (opt_is(a, "-t", "--threshold"))  thr_db     = atof(argv[++i]);
        else if (opt_is(a, "-R", "--ratio"))      ratio      = atof(argv[++i]);
        else if (opt_is(a, "-g", "--range"))      range      = atof(argv[++i]);
        else if (opt_is(a, "-a", "--attack"))     attack_ms  = atof(argv[++i]);
        else if (opt_is(a, "-e", "--release"))    release_ms = atof(argv[++i]);
        else if (opt_is(a, "-k", "--lookahead"))  look_ms    = atof(argv[++i]);
        else if (opt_is(a, "-d", "--detect")) {
            const char *v = argv[++i];
            if      (!strcmp(v, "rel")) detect_abs = 0;
            else if (!strcmp(v, "abs")) detect_abs = 1;
            else {
                fprintf(stderr, "%s: --detect must be rel or abs\n", argv[0]);
                return 2;
            }
        } else if (opt_is(a, "-m", "--mode")) {
            const char *v = argv[++i];
            if      (!strcmp(v, "split")) wide = 0;
            else if (!strcmp(v, "wide"))  wide = 1;
            else {
                fprintf(stderr, "%s: --mode must be split or wide\n", argv[0]);
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
    if (freq <= 0.0 || freq >= rate / 2.0) {
        fprintf(stderr, "%s: freq must be between 0 and %g Hz\n",
                argv[0], rate / 2.0);
        return 2;
    }
    if (!(q > 0.0)) {
        fprintf(stderr, "%s: q must be positive\n", argv[0]); return 2;
    }
    if (!(ratio >= 1.0)) {
        fprintf(stderr, "%s: ratio must be >= 1\n", argv[0]); return 2;
    }
    if (range < 0.0) {
        fprintf(stderr, "%s: range must not be negative\n", argv[0]); return 2;
    }
    if (attack_ms < 0.0 || release_ms < 0.0 || look_ms < 0.0) {
        fprintf(stderr, "%s: times must not be negative\n", argv[0]); return 2;
    }
    if (look_ms > MAX_LOOK_MS) {
        fprintf(stderr, "%s: lookahead must be <= %g ms\n", argv[0], MAX_LOOK_MS);
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    biquad_bp(&bp, freq, rate, q);
    memset(bp_st, 0, sizeof bp_st);

    /* alpha = exp(-1 / (tau * fs)); a zero time gives alpha = 0, instant */
    aA   = attack_ms  > 0.0 ? exp(-1000.0 / (attack_ms  * rate)) : 0.0;
    aR   = release_ms > 0.0 ? exp(-1000.0 / (release_ms * rate)) : 0.0;
    aRms = exp(-1000.0 / (RMS_MS * rate));

    L           = (int)(look_ms * rate / 1000.0 + 0.5);
    ring_frames = L + 1;
    ring_x = calloc((size_t)ring_frames * (size_t)channels, sizeof *ring_x);
    ring_b = calloc((size_t)ring_frames * (size_t)channels, sizeof *ring_b);
    if (!ring_x || !ring_b) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    if (verbose)
        fprintf(stderr,
            "deess: %g Hz, %d ch, band %g Hz Q %g, detect %s thr %g dB,\n"
            "       ratio %g:1, range %g dB, mode %s, attack %g ms,\n"
            "       release %g ms, lookahead %g ms (%d smp)%s\n",
            rate, channels, freq, q, detect_abs ? "abs" : "rel", thr_db,
            ratio, range, wide ? "wide" : "split", attack_ms, release_ms,
            look_ms, L, listen ? ", listening" : "");

    for (;;) {
        size_t frames, n, nout = 0;
        int    eof;

        got = fread(buf, sizeof(float), (size_t)block * (size_t)channels, stdin);

        /* Scrub non-finite input: the band-pass and the detector are both
         * recursive, and one NaN would sit in them for good. */
        for (i = 0; (size_t)i < got; i++)
            if (!isfinite(buf[i])) buf[i] = 0.0f;

        frames = got / (size_t)channels;
        eof    = (frames == 0);

        /* At EOF push L frames of silence to drain the delay lines, so the
         * output ends up exactly as long as the input. */
        if (eof) frames = (size_t)L;

        for (n = 0; n < frames; n++) {
            const float *in = eof ? NULL : &buf[n * (size_t)channels];
            double b[MAX_CHANNELS];
            double sb = 0.0, sx = 0.0, bdb, xdb, det, xL, g;
            int    rd;

            /* the band, per channel, and the loudest channel's squares */
            for (ch = 0; ch < channels; ch++) {
                double x = in ? (double)in[ch] : 0.0;
                b[ch] = biquad_run(&bp, &bp_st[ch], x);
                if (b[ch] * b[ch] > sb) sb = b[ch] * b[ch];
                if (x * x > sx) sx = x * x;
            }

            /* RMS of each, in dB */
            acc_b = flush_denorm(aRms * acc_b + (1.0 - aRms) * sb);
            acc_x = flush_denorm(aRms * acc_x + (1.0 - aRms) * sx);
            bdb = acc_b > 0.0 ? LOG10_10 * log(acc_b) : DB_FLOOR;
            xdb = acc_x > 0.0 ? LOG10_10 * log(acc_x) : DB_FLOOR;
            if (bdb < DB_FLOOR) bdb = DB_FLOOR;
            if (xdb < DB_FLOOR) xdb = DB_FLOOR;

            if (detect_abs)
                det = bdb;
            else
                det = bdb < REL_FLOOR ? DB_FLOOR : bdb - xdb;

            /* reduction the static curve asks for, capped at the range */
            xL = det - curve_db(det, thr_db, ratio, KNEE_DB);
            if (xL < 0.0) xL = 0.0;
            if (xL > range) xL = range;

            /* smooth decoupled peak detector, in the log domain */
            {
                double rel = aR * y1 + (1.0 - aR) * xL;
                y1 = xL > rel ? xL : rel;
                yL = aA * yL + (1.0 - aA) * y1;
            }
            if (yL < 1e-12) yL = 0.0;
            if (yL > peak_gr) peak_gr = yL;
            g = yL == 0.0 ? 1.0 : exp(-yL * LN10_20);

            /* delay lines: write the current frame, read the frame L back */
            for (ch = 0; ch < channels; ch++) {
                ring_x[(size_t)w * channels + ch] = in ? in[ch] : 0.0f;
                ring_b[(size_t)w * channels + ch] = b[ch];
            }
            rd = (w + ring_frames - L) % ring_frames;

            if (primed >= L) {
                for (ch = 0; ch < channels; ch++) {
                    double xd = (double)ring_x[(size_t)rd * channels + ch];
                    double bd = ring_b[(size_t)rd * channels + ch];
                    double y;

                    if (listen)
                        y = bd;
                    else if (wide)
                        y = xd * g;
                    else
                        y = xd - (1.0 - g) * bd;
                    out[nout * channels + ch] = (float)y;
                }
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
        /* Flush every block, or the last partial stdio buffer shows up as
         * latency in a live pipeline. */
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
        fprintf(stderr, "deess: peak reduction %.2f dB\n", peak_gr);

    free(ring_x);
    free(ring_b);
    fflush(stdout);
    return rc;
}
