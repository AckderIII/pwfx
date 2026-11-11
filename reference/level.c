/*
 * level.c -- loudness leveler (slow automatic gain) for raw 32-bit float
 * audio.
 *
 * Reads interleaved 32-bit IEEE-754 float samples (native endianness) from
 * stdin, writes the levelled stream to stdout in the same format.
 *
 * A leveler is not a compressor. A compressor reacts to peaks within
 * milliseconds and only ever turns down; a leveler listens to loudness over
 * a fraction of a second, turns up as readily as it turns down, and moves
 * over seconds -- the hand on the fader that rides a quiet speaker up and a
 * loud one down, and leaves every syllable's own dynamics alone.
 *
 * Design notes
 * ------------
 *  - Loudness, not level: the detector is ITU-R BS.1770's. Each channel is
 *    K-weighted -- a +4 dB high shelf around 1.7 kHz, for the head, and a
 *    high-pass at 38 Hz, the "RLB" curve -- and the channels' mean squares
 *    are summed, with weight 1 since a raw stream says nothing about which
 *    channel is a surround. Loudness is then -0.691 + 10 log10(sum), in LUFS:
 *    a 997 Hz sine at 0 dBFS in one channel reads -3.01. Both filters are
 *    designed for the actual rate from the analogue prototypes behind the
 *    standard's 48 kHz table (as libebur128 does), and at 48 kHz they
 *    reproduce that table.
 *  - The window is rectangular, as BS.1770's momentary (400 ms) and
 *    short-term (3 s) loudness are, and slides in hops of HOP_MS. Each hop's
 *    sum of squares goes into a ring of the last MAX_HOPS hops, and the
 *    window's sum is re-added from that ring every hop rather than kept
 *    running, so it cannot drift. The ring is always written in full, so
 *    the window's length can change at any time and is right from the next
 *    hop.
 *  - Gated per hop, as BS.1770's integrated loudness gates its blocks: a
 *    hop that fails the gates -- a pause, noise, a breath -- is left out of
 *    the window's average. The window's loudness is that of the sound in
 *    it, not of the sound diluted by the silence around it, so the end of a
 *    phrase does not read as the speaker getting quieter and the gain does
 *    not climb into every pause, and the start of one reads at its true
 *    loudness from its first hop instead of creeping up over a window and
 *    being boosted on the way. With no hop passing in the window, the gain
 *    freezes wherever it is, and picks up from there when the speaker
 *    does. At startup the window is silence, so the gain starts at 0 dB
 *    and stays there until there is something to level.
 *  - Two gates, as BS.1770 has. The absolute one, --gate, keeps silence and
 *    a quiet room out. The relative one drops a hop more than REL_GATE_DB
 *    under a reference: the mean power of the hops that passed the
 *    absolute gate over the last REF_MS or so, a one-pole at the hop rate.
 *    It is what makes the leveler independent of where --gate sits. A
 *    voice's room noise rises and falls with the voice, so no fixed gate
 *    suits the same voice 20 dB apart: the quiet copy's soft syllables fall
 *    under it or the loud copy's pauses get over it -- and a pause that is
 *    counted, once the window holds nothing else, is the noise floor being
 *    ridden up to the target. Judged against the voice of the last few
 *    seconds, the room stays out wherever --gate is. Six seconds of real
 *    speech, played 20 dB apart, settle 20.0 dB apart with --gate at -80
 *    and 19.9 at -65. The reference is long, and not the window's own
 *    mean, for exactly that reason: a window full of pause would be its own
 *    reference. A hop the relative gate drops still moves the reference,
 *    but at REF_SLOW_MS instead: moving it as fast as a counted hop, a
 *    steady room 30 dB under the voice had pulled the reference down to
 *    itself 7 s into a pause and was then ridden up to --max-boost; now
 *    that takes two minutes, and a room 25 dB under takes 40 s. Not moving
 *    it at all would shut out a speaker who has turned quieter for good
 *    with the room. They are followed: at once if less than 20 dB down,
 *    after about a minute of their speech at 25, and --gate is what holds
 *    through a longer silence. 20 dB rather than BS.1770's
 *    10 LU, because a 5 ms hop of speech swings much further than a 400 ms
 *    block, and 10 would drop its consonants.
 *  - So the loudness ridden to is that of the sound while it sounds --
 *    ITU-T P.56's "active" level, in LUFS. BS.1770's integrated loudness
 *    counts the short gaps within phrases, and reads the same speech 1 to
 *    2 dB lower.
 *  - Gain computer: the gain that would put the window at --loudness,
 *    times --strength (100 % levels fully; 50 % halves the distance to the
 *    target, a 2:1 ratio around it), capped at --max-boost up and --max-cut
 *    down. Updated once per hop.
 *  - Smoothing: a one-pole on the gain in dB, per sample, with a time
 *    constant of --attack while turning down and --release while turning
 *    up -- the 63 % time, as in the compressor. Turning up slower than down
 *    is what keeps a leveler from pumping the room back in after every
 *    phrase.
 *  - Lookahead: the audio is delayed by L samples while the detector runs
 *    on the undelayed signal, so a level change is answered before it is
 *    heard. Half the window centres the measurement on what is being heard.
 *    The delay is absorbed: the first L outputs are dropped and L frames of
 *    silence are flushed at EOF, so the output is sample-aligned with the
 *    input and as long.
 *  - Linked: one detector, one gain for every channel, so the image stays
 *    where it is.
 *  - At exactly 0 dB the gain is exactly 1, so a leveler that has nothing
 *    to do -- gated from the start, or capped at 0 both ways -- passes its
 *    input through bit for bit.
 *  - Filter, window and gain state are double precision; the audio is
 *    float in and float out.
 *
 * It does not catch peaks: a boost turns transients up with everything
 * else. Follow it with a limiter.
 *
 * Build:  cc -O2 -o level level.c -lm
 * Usage:  level [options]   (see --help)
 *
 * Example (48 kHz stereo speech to -16 LUFS):
 *   ... | ./level -r 48000 -c 2 -l -16 | ./limit -r 48000 -c 2 -C -1 | ...
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
#define HOP_MS        5.0
#define MAX_WINDOW_MS 3000.0
#define MIN_WINDOW_MS 100.0
#define MAX_HOPS      600            /* MAX_WINDOW_MS / HOP_MS */
#define MAX_LOOK_MS   500.0
#define LUFS_OFFSET   (-0.691)
#define REL_GATE_LIN  0.01           /* REL_GATE_DB, -20 dB, as a power */
#define REF_MS        5000.0         /* the relative gate's memory */
#define REF_SLOW_MS   60000.0        /* ... of the hops it dropped */
#define DB_FLOOR      (-240.0)

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { double s1, s2; }             bqstate_t;

static double flush_denorm(double v)
{
    return (v > -1e-100 && v < 1e-100) ? 0.0 : v;
}

/* Transposed direct form II. */
static double biquad_run(const biquad_t *c, bqstate_t *st, double x)
{
    double y = c->b0 * x + st->s1;
    st->s1 = flush_denorm(c->b1 * x - c->a1 * y + st->s2);
    st->s2 = flush_denorm(c->b2 * x - c->a2 * y);
    return y;
}

/* ---- K-weighting --------------------------------------------------------
 * BS.1770's two stages, from the analogue prototypes its 48 kHz table was
 * derived from: a high shelf of about +4 dB at 1682 Hz, then the RLB
 * high-pass at 38 Hz. The numerator of the high-pass is 1, -2, 1 as in the
 * standard, not normalised to unity gain at Nyquist.
 */
static void kweight(biquad_t *shelf, biquad_t *hp, double fs)
{
    double f0 = 1681.974450955533, G = 3.999843853973347;
    double Q = 0.7071752369554196;
    double K = tan(M_PI * f0 / fs);
    double Vh = pow(10.0, G / 20.0);
    double Vb = pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;

    shelf->b0 = (Vh + Vb * K / Q + K * K) / a0;
    shelf->b1 = 2.0 * (K * K - Vh) / a0;
    shelf->b2 = (Vh - Vb * K / Q + K * K) / a0;
    shelf->a1 = 2.0 * (K * K - 1.0) / a0;
    shelf->a2 = (1.0 - K / Q + K * K) / a0;

    f0 = 38.13547087602444;
    Q  = 0.5003270373238773;
    K  = tan(M_PI * f0 / fs);
    a0 = 1.0 + K / Q + K * K;
    hp->b0 = 1.0;
    hp->b1 = -2.0;
    hp->b2 = 1.0;
    hp->a1 = 2.0 * (K * K - 1.0) / a0;
    hp->a2 = (1.0 - K / Q + K * K) / a0;
}

static int opt_is(const char *a, const char *s, const char *l)
{
    return (s && !strcmp(a, s)) || (l && !strcmp(a, l));
}

static void usage(const char *p)
{
    fprintf(stderr,
"usage: %s [options]   --  32-bit float loudness leveler, stdin -> stdout\n"
"\n"
"stream\n"
"  -r, --rate HZ         sample rate                        (48000)\n"
"  -c, --channels N      interleaved channels, 1..%d        (1)\n"
"  -b, --block N         frames read per block              (128)\n"
"\n"
"target\n"
"  -l, --loudness LUFS   the loudness to ride towards       (-18)\n"
"                        speech -16..-19, broadcast -23\n"
"  -s, --strength PCT    how much of the way to go, 0..100  (100)\n"
"  -B, --max-boost DB    the most it will ever turn up      (12)\n"
"  -C, --max-cut DB      the most it will ever turn down    (12)\n"
"  -g, --gate LUFS       below this the gain holds still    (-50)\n"
"\n"
"timing\n"
"  -w, --window MS       loudness window, %g..%g          (400)\n"
"  -a, --attack MS       time constant turning down         (1000)\n"
"  -e, --release MS      time constant turning up           (3000)\n"
"  -k, --lookahead MS    audio delay feeding the detector   (0)\n"
"                        0..%g; half the window centres it\n"
"\n"
"  -v, --verbose         report settings and the gain range to stderr\n"
"  -h, --help            this text\n"
"\n"
"Loudness is ITU-R BS.1770's: K-weighted, channels' powers summed. The\n"
"window slides in %g ms hops. Output is sample-aligned with the input and the\n"
"same length: the lookahead delay is absorbed, not prepended. At 0 dB of\n"
"gain the input passes through bit for bit. It does not catch peaks.\n",
        p, MAX_CHANNELS, MIN_WINDOW_MS, MAX_WINDOW_MS, MAX_LOOK_MS, HOP_MS);
}

int main(int argc, char **argv)
{
    double rate = 48000.0, loud = -18.0, strength = 100.0;
    double boost = 12.0, cut = 12.0, gate = -50.0;
    double window_ms = 400.0, attack_ms = 1000.0, release_ms = 3000.0;
    double look_ms = 0.0;
    int    channels = 1, block = 128, verbose = 0;
    int    i, ch;

    biquad_t  shelf, hp;
    bqstate_t sh_st[MAX_CHANNELS], hp_st[MAX_CHANNELS];
    double    hops[MAX_HOPS];        /* sum of squares, 0 if under the gate */
    unsigned char live[MAX_HOPS];    /* 1 if over the gate */
    int       hop_len, n_hops, hop_w = 0, hop_n = 0;
    double    hop_acc = 0.0, want = 0.0, g = 0.0, gmin = 0.0, gmax = 0.0;
    double    aA, aR, aRef, aRefSlow, k_str, ref = 0.0;
    int       gated = 1;
    const double LOG10_10 = 10.0 / M_LN10, LN10_20 = M_LN10 / 20.0;

    float    *ring = NULL;
    int       L, ring_frames, w = 0;
    long long primed = 0;

    static float buf[MAX_BLOCK * MAX_CHANNELS];
    static float out[MAX_BLOCK * MAX_CHANNELS];
    size_t got;
    int    rc = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (opt_is(a, "-h", "--help"))     { usage(argv[0]); return 0; }
        if (opt_is(a, "-v", "--verbose"))  { verbose = 1; continue; }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s: missing value for %s\n", argv[0], a);
            return 2;
        }
        if      (opt_is(a, "-b", "--block"))      block      = atoi(argv[++i]);
        else if (opt_is(a, "-r", "--rate"))       rate       = atof(argv[++i]);
        else if (opt_is(a, "-c", "--channels"))   channels   = atoi(argv[++i]);
        else if (opt_is(a, "-l", "--loudness"))   loud       = atof(argv[++i]);
        else if (opt_is(a, "-s", "--strength"))   strength   = atof(argv[++i]);
        else if (opt_is(a, "-B", "--max-boost"))  boost      = atof(argv[++i]);
        else if (opt_is(a, "-C", "--max-cut"))    cut        = atof(argv[++i]);
        else if (opt_is(a, "-g", "--gate"))       gate       = atof(argv[++i]);
        else if (opt_is(a, "-w", "--window"))     window_ms  = atof(argv[++i]);
        else if (opt_is(a, "-a", "--attack"))     attack_ms  = atof(argv[++i]);
        else if (opt_is(a, "-e", "--release"))    release_ms = atof(argv[++i]);
        else if (opt_is(a, "-k", "--lookahead"))  look_ms    = atof(argv[++i]);
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
    if (!(strength >= 0.0 && strength <= 100.0)) {
        fprintf(stderr, "%s: strength must be 0..100\n", argv[0]); return 2;
    }
    if (boost < 0.0 || cut < 0.0) {
        fprintf(stderr, "%s: boost and cut must not be negative\n", argv[0]);
        return 2;
    }
    if (window_ms < MIN_WINDOW_MS || window_ms > MAX_WINDOW_MS) {
        fprintf(stderr, "%s: window must be %g..%g ms\n", argv[0],
                MIN_WINDOW_MS, MAX_WINDOW_MS);
        return 2;
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

    kweight(&shelf, &hp, rate);
    memset(sh_st, 0, sizeof sh_st);
    memset(hp_st, 0, sizeof hp_st);
    memset(hops, 0, sizeof hops);
    memset(live, 0, sizeof live);

    /* The hop is HOP_MS at this rate, the window a whole number of hops. */
    hop_len = (int)(HOP_MS * rate / 1000.0 + 0.5);
    if (hop_len < 1) hop_len = 1;
    n_hops = (int)(window_ms / HOP_MS + 0.5);
    if (n_hops < 1) n_hops = 1;
    if (n_hops > MAX_HOPS) n_hops = MAX_HOPS;
    k_str = strength / 100.0;

    /* alpha = exp(-1 / (tau * fs)); a zero time gives alpha = 0, instant */
    aA = attack_ms  > 0.0 ? exp(-1000.0 / (attack_ms  * rate)) : 0.0;
    aR = release_ms > 0.0 ? exp(-1000.0 / (release_ms * rate)) : 0.0;
    aRef = exp(-1000.0 * (double)hop_len / (REF_MS * rate));
    aRefSlow = exp(-1000.0 * (double)hop_len / (REF_SLOW_MS * rate));

    L           = (int)(look_ms * rate / 1000.0 + 0.5);
    ring_frames = L + 1;
    ring = calloc((size_t)ring_frames * (size_t)channels, sizeof *ring);
    if (!ring) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    if (verbose)
        fprintf(stderr,
            "level: %g Hz, %d ch, loudness %g LUFS, strength %g %%,\n"
            "       boost %g dB, cut %g dB, gate %g LUFS, window %d x %d smp,\n"
            "       attack %g ms, release %g ms, lookahead %g ms (%d smp)\n",
            rate, channels, loud, strength, boost, cut, gate, n_hops, hop_len,
            attack_ms, release_ms, look_ms, L);

    for (;;) {
        size_t frames, n, nout = 0;
        int    eof;

        got = fread(buf, sizeof(float), (size_t)block * (size_t)channels, stdin);

        /* Scrub non-finite input: the K-weighting is recursive, and one NaN
         * would sit in it, and in the window, for good. */
        for (i = 0; (size_t)i < got; i++)
            if (!isfinite(buf[i])) buf[i] = 0.0f;

        frames = got / (size_t)channels;
        eof    = (frames == 0);

        /* At EOF push L frames of silence to drain the delay line, so the
         * output ends up exactly as long as the input. */
        if (eof) frames = (size_t)L;

        for (n = 0; n < frames; n++) {
            const float *in = eof ? NULL : &buf[n * (size_t)channels];
            double lin, a;
            int    rd;

            /* K-weighted power, summed over the channels */
            for (ch = 0; ch < channels; ch++) {
                double x = in ? (double)in[ch] : 0.0;
                double y = biquad_run(&hp, &hp_st[ch],
                                      biquad_run(&shelf, &sh_st[ch], x));
                hop_acc += y * y;
            }

            /* a hop is complete: gate it, then the loudness of the hops in
             * the window that passed, and from it the gain to ride towards
             * -- or, with none, no change */
            if (++hop_n >= hop_len) {
                double sum = 0.0, ms, lufs;
                int    j, r, cnt = 0, ok = 0;

                ms   = hop_acc / (double)hop_len;
                lufs = ms > 0.0 ? LUFS_OFFSET + LOG10_10 * log(ms) : DB_FLOOR;
                if (lufs >= gate) {
                    /* past the absolute gate: it counts if it is not far
                     * under the reference as it would move it, and then
                     * does move it; one that is dropped moves it too, but
                     * far more slowly */
                    double moved = ref > 0.0 ? aRef * ref + (1.0 - aRef) * ms
                                             : ms;
                    ok = ms >= moved * REL_GATE_LIN;
                    ref = ok ? moved : aRefSlow * ref + (1.0 - aRefSlow) * ms;
                }
                live[hop_w] = (unsigned char)ok;
                hops[hop_w] = ok ? hop_acc : 0.0;
                hop_w = (hop_w + 1) % MAX_HOPS;
                hop_acc = 0.0;
                hop_n = 0;

                for (j = 0, r = hop_w; j < n_hops; j++) {
                    r = (r + MAX_HOPS - 1) % MAX_HOPS;
                    sum += hops[r];
                    cnt += live[r];
                }

                gated = cnt == 0;
                if (!gated) {
                    ms   = sum / ((double)cnt * (double)hop_len);
                    lufs = LUFS_OFFSET + LOG10_10 * log(ms);
                    want = k_str * (loud - lufs);
                    if (want >  boost) want =  boost;
                    if (want < -cut)   want = -cut;
                }
            }

            /* the gain, in dB: down at the attack, up at the release, and
             * held still while the gate is shut */
            if (!gated) {
                a = want < g ? aA : aR;
                g = a * g + (1.0 - a) * want;
                if (g > -1e-12 && g < 1e-12) g = 0.0;
            }
            if (g < gmin) gmin = g;
            if (g > gmax) gmax = g;
            lin = g == 0.0 ? 1.0 : exp(g * LN10_20);

            /* delay line: write the current frame, read the frame L back */
            for (ch = 0; ch < channels; ch++)
                ring[(size_t)w * channels + ch] = in ? in[ch] : 0.0f;
            rd = (w + ring_frames - L) % ring_frames;

            if (primed >= L) {
                for (ch = 0; ch < channels; ch++)
                    out[nout * channels + ch] =
                        (float)((double)ring[(size_t)rd * channels + ch] * lin);
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
        fprintf(stderr, "level: gain from %+.2f to %+.2f dB, ending at %+.2f\n",
                gmin, gmax, g);

    free(ring);
    fflush(stdout);
    return rc;
}
