/*
 * limit.c -- high-quality true-peak brickwall limiter for 32-bit float audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the limited stream to stdout in the same format.
 *
 * Why this is not just a fast compressor
 * --------------------------------------
 * A compressor smooths gain with a one-pole attack, so the gain arrives at
 * its target only asymptotically and the overshoot past the threshold decays
 * as exp(-lookahead/attack) -- it never reaches zero. A limiter has to
 * *guarantee* the ceiling, so it needs a construction that provably reaches
 * the required gain before the peak does.
 *
 * The construction used here:
 *
 *   1. g_req[n]  -- instantaneous gain that would put sample n exactly on
 *                   the ceiling, expressed as gain reduction in dB.
 *   2. g_env[n]  -- g_req with an exponential release applied:
 *                     gr[n] = max(gr_req[n], alpha*gr[n-1])
 *                   Release only ever makes the gain smaller (more reduction)
 *                   than required, so it cannot break the guarantee.
 *   3. m[n]      -- sliding MINIMUM of g_env over a window of S samples
 *                   looking ahead.
 *   4. b[n]      -- m passed through two cascaded boxcar averages of length
 *                   A each, i.e. a triangular (Bartlett) FIR of support
 *                   S = 2A-1. Two boxcars rather than one make the gain
 *                   envelope C1 -- a single boxcar leaves slope jumps that
 *                   are audible as a faint chirp on dense material.
 *
 * With the audio delayed by D = 2A-2 = S-1 samples, b[n] is guaranteed to be
 * <= g_env[n-D]. Proof: b[n] is a convex combination of m[n-k] for
 * k in [0, 2A-2]. Each m[n-k] is the minimum of g_env over [n-k-S+1, n-k].
 * That interval contains n-D for every k in [0, 2A-2]: the upper end needs
 * k <= 2A-2, the lower end needs k >= 2A-1-S = 0. So every term in the
 * average is <= g_env[n-D], hence so is the average. No overshoot is
 * possible -- not "small", not "decaying", none.
 *
 * True peak
 * ---------
 * Sample peaks are not what clips. A D/A reconstruction filter, and every
 * lossy codec, can produce inter-sample peaks well above the highest sample,
 * which is why broadcast and streaming specs are written in dBTP. The
 * detector here oversamples by 4x (configurable) through a Kaiser-windowed
 * polyphase sinc interpolator, in the spirit of ITU-R BS.1770-4, and limits
 * the interpolated peak. The audio path itself is never oversampled or
 * filtered -- only the detector sees it.
 *
 * Other notes
 * -----------
 *  - Channel-linked by design. One gain for all channels; a limiter that
 *    moves channels independently rewrites the stereo image on every peak.
 *  - Latency is reported but NOT added: the delay is absorbed and the output
 *    is sample-aligned with the input and exactly as long.
 *  - The boxcar running sums are recomputed exactly at intervals so that
 *    floating-point drift cannot slowly erode the guarantee on a stream that
 *    runs for days.
 *  - Non-finite input is scrubbed at the front door; one NaN would otherwise
 *    wedge the detector permanently.
 *  - A final clamp at the ceiling is present as a safety net. It should
 *    never engage; --verbose reports how often it did, and that count is
 *    expected to be exactly zero.
 *
 * Build:  cc -O2 -o limit limit.c -lm
 * Usage:  limit [options]   (see --help)
 *
 * Example (stream delivery, 48 kHz stereo, -1 dBTP):
 *   ... | ./limit -r 48000 -c 2 -C -1 -i 6 -e 60
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

#define MAX_CHANNELS  64
#define MAX_BLOCK     4096
#ifndef TAPS
#define TAPS          24          /* interpolator taps per phase, even */
#endif
#define MAX_OS        16
#define RESUM_PERIOD  65536       /* exact boxcar re-sum interval */

/* ---- modified Bessel I0, for the Kaiser window ------------------------- */
static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0, h = x / 2.0;
    int k;
    for (k = 1; k < 60; k++) {
        t *= (h / k) * (h / k);
        s += t;
        if (t < 1e-18 * s) break;
    }
    return s;
}

/* ---- exact sliding minimum over a fixed window (monotonic deque) ------- */
typedef struct {
    double    *val;
    long long *idx;
    int        cap, head, count;
} slidemin_t;

static int slidemin_init(slidemin_t *s, int window)
{
    s->cap  = window + 2;
    s->val  = malloc((size_t)s->cap * sizeof *s->val);
    s->idx  = malloc((size_t)s->cap * sizeof *s->idx);
    s->head = s->count = 0;
    return s->val && s->idx;
}

static double slidemin_push(slidemin_t *s, long long n, double v, int window)
{
    int back;
    while (s->count > 0) {
        back = (s->head + s->count - 1) % s->cap;
        if (s->val[back] < v) break;
        s->count--;
    }
    back = (s->head + s->count) % s->cap;
    s->val[back] = v;
    s->idx[back] = n;
    s->count++;
    while (s->count > 0 && s->idx[s->head] <= n - window) {
        s->head = (s->head + 1) % s->cap;
        s->count--;
    }
    return s->val[s->head];
}

/* ---- boxcar moving average with periodic exact re-summation ------------ */
typedef struct {
    double *buf;
    int     len, pos;
    double  sum;
    long long since;
} boxcar_t;

static int boxcar_init(boxcar_t *b, int len, double fill)
{
    int i;
    b->buf = malloc((size_t)len * sizeof *b->buf);
    if (!b->buf) return 0;
    for (i = 0; i < len; i++) b->buf[i] = fill;
    b->len = len;
    b->pos = 0;
    b->sum = fill * len;
    b->since = 0;
    return 1;
}

static double boxcar_push(boxcar_t *b, double v)
{
    b->sum -= b->buf[b->pos];
    b->buf[b->pos] = v;
    b->sum += v;
    b->pos = (b->pos + 1) % b->len;

    if (++b->since >= RESUM_PERIOD) {      /* kill accumulated drift */
        int i;
        double s = 0.0;
        for (i = 0; i < b->len; i++) s += b->buf[i];
        b->sum = s;
        b->since = 0;
    }
    return b->sum / b->len;
}

static int opt_is(const char *a, const char *s, const char *l)
{
    return (s && !strcmp(a, s)) || (l && !strcmp(a, l));
}

static void usage(const char *p)
{
    fprintf(stderr,
"usage: %s [options]   --  32-bit float true-peak limiter, stdin -> stdout\n"
"\n"
"stream\n"
"  -r, --rate HZ         sample rate                        (48000)\n"
"  -c, --channels N      interleaved channels, 1..%d        (1)\n"
"  -b, --block N         frames read per block              (128)\n"
"                        smaller = lower live latency,\n"
"                        larger  = faster offline\n"
"\n"
"limiting\n"
"  -C, --ceiling DB      output ceiling in dBTP             (-1)\n"
"  -i, --input-gain DB   gain applied before limiting       (0)\n"
"                        this is how you drive the limiter\n"
"  -e, --release MS      gain-reduction recovery constant   (60)\n"
"  -k, --lookahead MS    attack window; the gain is fully   (5)\n"
"                        in place before the peak arrives,\n"
"                        so there is no overshoot at any\n"
"                        setting -- shorter only sounds\n"
"                        more aggressive\n"
"\n"
"detection\n"
"  -O, --oversample N    true-peak oversampling: 1,2,4,8,16 (8)\n"
"                        1 = sample peak only (not dBTP)\n"
"\n"
"  -v, --verbose         report settings and peak statistics to stderr\n"
"  -h, --help            this text\n"
"\n"
"Output is sample-aligned with the input and the same length: the lookahead\n"
"delay is absorbed, not prepended.\n"
"\n"
"Streaming delivery:   -C -1 -i 6 -e 60        (-1 dBTP leaves codec headroom)\n"
"Transparent safety:   -C -0.3 -i 0 -e 100 -k 10\n",
        p, MAX_CHANNELS);
}

int main(int argc, char **argv)
{
    double rate = 48000.0, ceil_db = -1.0, in_db = 0.0, release_ms = 60.0;
    double look_ms = 5.0;
    int    channels = 1, block = 128, os = 8, verbose = 0, i, ch, p_, k;

    /* polyphase interpolator */
    double  h[MAX_OS][TAPS + 1];
    double  tpline[MAX_CHANNELS][TAPS + 1];
    int     tp_pos = 0;

    slidemin_t sm;
    boxcar_t   box1, box2;
    float     *ring = NULL;
    int        A, S, D, D_det, DT, ring_frames, w = 0;
    long long  n = 0, primed = 0;

    double ceil_lin, in_lin, alpha, gr_db = 0.0;
    double peak_gr = 0.0, tp_in = 0.0, max_excess = 0.0;
    long long clamps = 0;
    const double LOG10_20 = 20.0 / M_LN10, LN10_20 = M_LN10 / 20.0;

    static float buf[MAX_BLOCK * MAX_CHANNELS];
    static float out[MAX_BLOCK * MAX_CHANNELS];
    size_t got;
    int    rc = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (opt_is(a, "-h", "--help"))    { usage(argv[0]); return 0; }
        if (opt_is(a, "-v", "--verbose")) { verbose = 1; continue; }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], a);
            return 2;
        }
        if      (opt_is(a, "-b", "--block"))      block      = atoi(argv[++i]);
        else if (opt_is(a, "-r", "--rate"))       rate       = atof(argv[++i]);
        else if (opt_is(a, "-c", "--channels"))   channels   = atoi(argv[++i]);
        else if (opt_is(a, "-C", "--ceiling"))    ceil_db    = atof(argv[++i]);
        else if (opt_is(a, "-i", "--input-gain")) in_db      = atof(argv[++i]);
        else if (opt_is(a, "-e", "--release"))    release_ms = atof(argv[++i]);
        else if (opt_is(a, "-k", "--lookahead"))  look_ms    = atof(argv[++i]);
        else if (opt_is(a, "-O", "--oversample")) os         = atoi(argv[++i]);
        else {
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
    if (os < 1 || os > MAX_OS || (os & (os - 1))) {
        fprintf(stderr, "%s: oversample must be a power of two, 1..%d\n",
                argv[0], MAX_OS);
        return 2;
    }
    if (release_ms < 0.0 || look_ms < 0.0) {
        fprintf(stderr, "%s: times must not be negative\n", argv[0]); return 2;
    }
    if (look_ms > 100.0) {
        fprintf(stderr, "%s: lookahead must be <= 100 ms\n", argv[0]); return 2;
    }
    if (ceil_db > 0.0) {
        fprintf(stderr, "%s: ceiling must be <= 0 dBFS\n", argv[0]); return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /* ---- design the polyphase interpolator ----------------------------
     * Prototype: Kaiser-windowed sinc, length os*TAPS+1, cutoff at the
     * original Nyquist. Phase p is taken every os-th tap starting at p, and
     * each phase is normalised to unit sum so no phase carries a DC offset
     * relative to the others. Group delay is exactly TAPS/2 input samples.
     */
    {
        int    N   = os * TAPS + 1;
        double cen = os * TAPS / 2.0, beta = 8.0, i0b = bessel_i0(beta);
        double *hf = malloc((size_t)N * sizeof *hf);
        if (!hf) { fprintf(stderr, "%s: out of memory\n", argv[0]); return 1; }
        for (i = 0; i < N; i++) {
            double xx = (i - cen) / os;
            double s  = (fabs(xx) < 1e-12) ? 1.0 : sin(M_PI * xx) / (M_PI * xx);
            double t  = (N == 1) ? 0.0 : (2.0 * i / (N - 1) - 1.0);
            double t2 = 1.0 - t * t;
            if (t2 < 0.0) t2 = 0.0;
            hf[i] = s * bessel_i0(beta * sqrt(t2)) / i0b;
        }
        for (p_ = 0; p_ < os; p_++) {
            double sum = 0.0;
            for (k = 0; k <= TAPS; k++) {
                int idx = k * os + p_;
                h[p_][k] = (idx < N) ? hf[idx] : 0.0;
                sum += h[p_][k];
            }
            if (sum != 0.0)
                for (k = 0; k <= TAPS; k++) h[p_][k] /= sum;
        }
        free(hf);
    }
    D_det = TAPS / 2;
    memset(tpline, 0, sizeof tpline);

    ceil_lin = exp(ceil_db * LN10_20);
    in_lin   = exp(in_db   * LN10_20);
    alpha    = release_ms > 0.0 ? exp(-1000.0 / (release_ms * rate)) : 0.0;

    /* Smoother geometry: two boxcars of length A give a triangular FIR of
     * support S = 2A-1 and total delay D = S-1, which is exactly the window
     * the sliding minimum must cover for the no-overshoot proof to hold. */
    {
        int L = (int)(look_ms * rate / 1000.0 + 0.5);
        A = (L + 2) / 2;
        if (A < 1) A = 1;
    }
    S  = 2 * A - 1;
    D  = S - 1;
    DT = D + D_det;                 /* + detector group delay */

    ring_frames = DT + 1;
    ring = calloc((size_t)ring_frames * (size_t)channels, sizeof *ring);
    if (!ring || !slidemin_init(&sm, S) ||
        !boxcar_init(&box1, A, 1.0) || !boxcar_init(&box2, A, 1.0)) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    if (verbose)
        fprintf(stderr,
            "limit: %g Hz, %d ch, ceiling %g dBTP, input gain %+g dB,\n"
            "       release %g ms, lookahead %.2f ms (%d smp), oversample %dx,\n"
            "       total latency %d smp (absorbed, not added)\n",
            rate, channels, ceil_db, in_db, release_ms,
            1000.0 * D / rate, D, os, DT);

    for (;;) {
        size_t frames, fi, nout = 0;
        int    eof;

        got = fread(buf, sizeof(float), (size_t)block * (size_t)channels, stdin);

        /* Scrub non-finite input: one NaN would lose every comparison in the
         * detector and wedge the gain state for the rest of the stream. */
        for (i = 0; (size_t)i < got; i++)
            if (!isfinite(buf[i])) buf[i] = 0.0f;

        frames = got / (size_t)channels;
        eof    = (frames == 0);
        if (eof) frames = (size_t)DT;      /* drain the delay line */

        for (fi = 0; fi < frames; fi++) {
            const float *in = eof ? NULL : &buf[fi * (size_t)channels];
            double tp = 0.0, gr_req, g_env, m, b1, b2;
            int    rd;

            /* push into the per-channel interpolator lines, find true peak */
            for (ch = 0; ch < channels; ch++) {
                double v = in ? (double)in[ch] * in_lin : 0.0;
                tpline[ch][tp_pos] = v;
            }
            for (ch = 0; ch < channels; ch++) {
                for (p_ = 0; p_ < os; p_++) {
                    double acc = 0.0;
                    for (k = 0; k <= TAPS; k++) {
                        int q = tp_pos - k;
                        if (q < 0) q += TAPS + 1;
                        acc += h[p_][k] * tpline[ch][q];
                    }
                    acc = fabs(acc);
                    if (acc > tp) tp = acc;
                }
            }
            if (tp > tp_in) tp_in = tp;

            /* required reduction, in dB >= 0 */
            gr_req = tp > ceil_lin ? LOG10_20 * log(tp / ceil_lin) : 0.0;

            /* exponential release; only ever adds reduction, never removes */
            gr_db = alpha * gr_db;
            if (gr_req > gr_db) gr_db = gr_req;
            if (gr_db < 1e-12)  gr_db = 0.0;
            if (gr_db > peak_gr) peak_gr = gr_db;

            g_env = gr_db > 0.0 ? exp(-gr_db * LN10_20) : 1.0;

            /* sliding minimum, then triangular smoothing */
            m  = slidemin_push(&sm, n, g_env, S);
            b1 = boxcar_push(&box1, m);
            b2 = boxcar_push(&box2, b1);

            /* delay line: write current frame, read the frame DT back */
            for (ch = 0; ch < channels; ch++)
                ring[(size_t)w * channels + ch] =
                    (float)(in ? (double)in[ch] * in_lin : 0.0);
            rd = (w + ring_frames - DT) % ring_frames;

            if (primed >= DT) {
                for (ch = 0; ch < channels; ch++) {
                    double y = ring[(size_t)rd * channels + ch] * b2;
                    double ex = fabs(y) - ceil_lin;
                    /* Only a real breach counts. The construction is exact in
                     * exact arithmetic, so anything at the last-bit level is
                     * double-rounding, not overshoot, and would make the
                     * counter useless as a diagnostic. */
                    if (ex > 0.0) {
                        if (ex > max_excess) max_excess = ex;
                        if (ex > 1e-6 * ceil_lin) clamps++;
                        y = y > 0.0 ? ceil_lin : -ceil_lin;
                    }
                    out[nout * channels + ch] = (float)y;
                }
                nout++;
            } else {
                primed++;
            }

            tp_pos = (tp_pos + 1) % (TAPS + 1);
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
        fprintf(stderr,
            "limit: input true peak %+.2f dBTP (after input gain), "
            "peak gain reduction %.2f dB\n"
            "limit: ceiling breaches %lld (expected 0); largest excess %.2e "
            "(%.1f dB below a float32 LSB at the ceiling)\n",
            tp_in > 0.0 ? LOG10_20 * log(tp_in) : -999.0, peak_gr, clamps,
            max_excess,
            max_excess > 0.0
                ? 20.0 / M_LN10 * log(6e-8 * ceil_lin / max_excess) : 999.0);

    free(sm.val); free(sm.idx);
    free(box1.buf); free(box2.buf);
    free(ring);
    fflush(stdout);
    return rc;
}
