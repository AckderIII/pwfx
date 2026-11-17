/*
 * reverb_dsp.h -- the reverb's core, shared by src/pw_reverb.c and
 * tests/t_reverb.c and held to reference/reverb.c byte for byte.
 *
 * The structure is a diffuser feeding a feedback delay network, both 16
 * channels wide:
 *
 *   in -> low-cut -> spread onto 16 -> diffuser (4 steps) -> + -> tank
 *                                                            ^      |
 *                                                            +-- Householder
 *
 *  - Spread: input channel c is laid onto the 16 channels along row
 *    1 + c % 15 of the 16x16 Hadamard matrix, scaled to unit norm and by
 *    1/sqrt(channels). Output channel c is read back along the same row.
 *    The rows are orthogonal, so the channels' tails come out decorrelated
 *    -- a mono source gets a wide tail -- and row 0, all ones, is left out:
 *    it is the Householder matrix's own eigenvector, the one direction the
 *    tank's mixing would not scramble. Past 15 channels the rows repeat.
 *
 *  - Diffuser: four steps, each 16 delays then a sign flip per channel and
 *    the orthonormal Hadamard mix. Lossless, so it changes the density and
 *    not the level: each step multiplies the number of echoes by 16, and the
 *    step lengths double (their sum is 40 ms at size 100) so the echoes
 *    spread out rather than pile up. The delays are stratified -- line j of
 *    a step sits in the j-th sixteenth of the step's span, at a fixed
 *    pseudo-random point within it -- so the spacing is even without being
 *    regular. The predelay is added to every delay of the first step: the
 *    delays and the mixing are linear, so that is the same as delaying the
 *    input, and it costs no line of its own.
 *
 *  - Tank: 16 lines, lengths spaced geometrically from 40 to 140 ms at
 *    size 100 and rounded up to primes, so no two share a factor and their
 *    resonances do not line up. Their order is shuffled so a line's length
 *    is not tied to its Hadamard row. The feedback matrix is Householder,
 *    I - (2/16) 1 1^T: orthogonal, so it is lossless and every mode decays
 *    by the lines' own losses alone. The tank's input and its output are
 *    the same vector, what is written into the lines.
 *
 *  - Decay: each line has a first-order shelf with gain 10^(-3 L/(fs T60))
 *    at DC for T60 = decay and at Nyquist for T60 = decay * hf-decay, and
 *    its half-power point at damp-freq. Per pass a line of length L then
 *    loses exactly 60 dB per T60 at both ends of the band (Jot), so the
 *    tail decays at the time asked for whatever size is. Away from those
 *    ends the lines agree closely rather than exactly: the per-pass gains
 *    are near 1, so each shelf's dB response is near proportional to L.
 *
 *  - Modulation: each line's length moves slowly (0.31 to 1.0 Hz, a rate
 *    per line) by up to 1 ms, which smears the tank's modes and keeps a
 *    long tail from ringing metallic. The fractional delay is read through
 *    a first-order allpass, not interpolated: an interpolator's high-
 *    frequency loss would be taken once per pass, hundreds of times over a
 *    long tail, and would shorten its top end; an allpass has unit gain at
 *    every frequency, so the decay stays what the shelves make it. Its
 *    fraction is kept in [0.5, 1.5), where its pole stays well inside the
 *    circle. At modulation 0 the fraction is 1 and the allpass is an exact
 *    integer delay. The oscillators are rotations, renormalised every 1024
 *    samples by count, so the output does not depend on block size.
 *
 *  - Live changes: every delay length, the modulation depth, the shelves,
 *    the low-cut and the mix glide to a new value rather than step. A delay
 *    that jumped would cut the signal at that point, and in a tank the
 *    click would ring for the whole decay; a shelf switched at once is a
 *    step in its output. The glide is two one-pole stages in series, so a
 *    length's rate of change ramps instead of stepping (a step is a kink in
 *    every waveform read through it), with a 50 ms time constant, held to
 *    half a sample per sample in the diffuser and a twentieth in the tank:
 *    the allpasses read a fast-moving delay badly, and at 0.2 a size change
 *    left -60 dB of hiss on a pure tone, at 0.05 under -75. So a big size
 *    change is a swoop of up to 5 % in pitch for a second or two, tape-like
 *    and never backwards. tests/reverb.py holds a change to 60 dB under a
 *    tone above 3 kHz, where an instant jump reaches -18. Settled, every
 *    length is an integer again and every read exact; the reference has
 *    fixed settings and no glide, and settled the two are the same
 *    arithmetic.
 *
 * Delay lines hold float, as the stream does; everything computed --
 * mixing, shelves, allpasses -- is double, and denormals are flushed, both
 * on the way into a line and in the filter state.
 */

#ifndef PWF_REVERB_DSP_H
#define PWF_REVERB_DSP_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

#define RV_N            16
#define RV_STEPS        4
#define RV_MAX_PREDELAY 250.0     /* ms */
#define RV_DIFF_MS      40.0      /* the diffuser's span at size 100 */
#define RV_TANK_MIN_MS  40.0
#define RV_TANK_MAX_MS  140.0
#define RV_MOD_MS       1.0       /* the largest excursion, at modulation 100 */
#define RV_GLIDE_MS     50.0
#define RV_SLEW         0.5       /* fastest glide, samples per sample */
#define RV_TANK_SLEW    0.05      /* and in the tank, read through allpasses */
#define RV_RAMP_MS      10.0
#define RV_STORE_FLOOR  1e-30     /* below this a line holds 0 */

typedef struct {
    int       dd[RV_STEPS][RV_N];  /* diffuser delays; step 0 has the predelay */
    double    ds[RV_STEPS][RV_N];  /* and their signs, +-1 */
    int       L[RV_N];             /* tank lengths */
    onepole_t shelf[RV_N];
    double    depth;               /* modulation excursion, samples */
    double    rot_c[RV_N], rot_s[RV_N];
    double    wet, dry;
    biquad_t  hp;                  /* the low-cut; the identity when off */
    double    glide_k, ramp_k;
} reverb_coeffs;

typedef struct {
    uint32_t  channels;
    float    *mem;
    float    *dbuf[RV_STEPS][RV_N];
    float    *tbuf[RV_N];
    uint32_t  dmask[RV_STEPS], tmask;
    int       dcap[RV_STEPS], tcap;    /* the longest delay each may hold */
    uint32_t  pos;
    double    hs[RV_N][RV_N];          /* Hadamard signs */
    double    inscale;
    /* what is in force, gliding towards the coefficients through a middle
     * stage */
    double    dcur[RV_STEPS][RV_N], tcur[RV_N];
    double    dmid[RV_STEPS][RV_N], tmid[RV_N];
    double    depth_cur, depth_mid, wet_cur, dry_cur;
    onepole_t sh_cur[RV_N], sh_mid[RV_N];
    biquad_t  hp_cur, hp_mid;
    int       started;
    /* filter state */
    double    ap[RV_N];
    opstate_t op[RV_N];
    double    lc[RV_N], ls[RV_N];
    uint32_t  count;
    bqstate_t *hp;                     /* channels */
} reverb_state;

static inline uint32_t rv_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static inline double rv_rand01(uint32_t *s)
{
    return (double)(rv_rand(s) >> 8) / 16777216.0;
}

static inline int rv_prime_at_least(int n)
{
    int d;
    if (n < 2) return 2;
    for (;; n++) {
        for (d = 2; d * d <= n; d++)
            if (n % d == 0) break;
        if (d * d > n) return n;
    }
}

/* The order the tank's lengths are dealt to its lines. */
static const int rv_perm[RV_N] = { 5, 12, 2, 9, 14, 0, 7, 11, 3, 15, 6, 1, 10, 4, 13, 8 };

/* The longest delays the state must hold at rate fs: the predelay and the
 * first step's span; each later step's span; the tank's longest prime plus
 * the modulation. Shared by init and by the reference. */
static inline int rv_need_diff(int step, double fs)
{
    double span = RV_DIFF_MS * (double)(1 << step) / 15.0;
    return (int)((span + (step == 0 ? RV_MAX_PREDELAY : 0.0)) * fs / 1000.0) + 4;
}

static inline int rv_need_tank(double fs)
{
    return rv_prime_at_least((int)(RV_TANK_MAX_MS * fs / 1000.0 + 0.5))
         + (int)(RV_MOD_MS * fs / 1000.0) + 4;
}

static inline uint32_t rv_pow2(int n)
{
    uint32_t p = 1;
    while (p < (uint32_t)n + 2) p <<= 1;
    return p;
}

static inline void reverb_design(reverb_coeffs *c, double fs, double size_pct,
                                 double predelay_ms, double decay_s,
                                 double hf_pct, double damp_hz, double mod_pct,
                                 double mix_pct, double lowcut_hz,
                                 const int *dcap, int tcap)
{
    double size = size_pct / 100.0;
    double t_lo = decay_s, t_hi = decay_s * hf_pct / 100.0;
    double fc = damp_hz, K;
    int P = (int)(predelay_ms * fs / 1000.0 + 0.5);
    uint32_t seed = 0x9E3779B9u;
    int k, j, i;

    memset(c, 0, sizeof *c);

    for (k = 0; k < RV_STEPS; k++) {
        double span = size * RV_DIFF_MS * (double)(1 << k) / 15.0 * fs / 1000.0;
        for (j = 0; j < RV_N; j++) {
            double r = j == 0 ? 0.0 : rv_rand01(&seed);
            int d = (int)(span * ((double)j + r) / (double)RV_N + 0.5);
            if (k == 0) d += P;
            if (d > dcap[k]) d = dcap[k];
            c->dd[k][j] = d;
            c->ds[k][j] = (rv_rand(&seed) & 0x100) ? -1.0 : 1.0;
        }
    }

    if (fc > fs * 0.49) fc = fs * 0.49;
    K = tan(M_PI * fc / fs);
    for (i = 0; i < RV_N; i++) {
        double ms = size * RV_TANK_MIN_MS
                  * pow(RV_TANK_MAX_MS / RV_TANK_MIN_MS, (double)i / (RV_N - 1));
        int L = rv_prime_at_least((int)(ms * fs / 1000.0 + 0.5));
        int line = rv_perm[i];
        double g0, gi;
        onepole_t *s = &c->shelf[line];

        if (L > tcap) L = tcap;
        c->L[line] = L;
        g0 = pow(10.0, -3.0 * (double)L / (fs * t_lo));
        gi = pow(10.0, -3.0 * (double)L / (fs * t_hi));
        /* H(s) = (gi s + g0 K)/(s + K), bilinear, s = (1-z^-1)/(1+z^-1) */
        s->b0 = (gi + g0 * K) / (1.0 + K);
        s->b1 = (g0 * K - gi) / (1.0 + K);
        s->a1 = (K - 1.0) / (1.0 + K);

        c->rot_c[line] = cos(2.0 * M_PI * (0.31 + 0.046 * (double)i) / fs);
        c->rot_s[line] = sin(2.0 * M_PI * (0.31 + 0.046 * (double)i) / fs);
    }

    c->depth = mod_pct / 100.0 * RV_MOD_MS * fs / 1000.0;
    c->wet = mix_pct / 100.0;
    c->dry = 1.0 - c->wet;
    if (lowcut_hz > 0.0)
        biquad_hp(&c->hp, lowcut_hz < fs * 0.45 ? lowcut_hz : fs * 0.45, fs,
                  M_SQRT1_2);
    else
        biquad_identity(&c->hp);
    c->glide_k = 1.0 - exp(-1000.0 / (RV_GLIDE_MS * fs));
    c->ramp_k  = 1.0 - exp(-1000.0 / (RV_RAMP_MS * fs));
}

static inline void reverb_hadamard_signs(double hs[RV_N][RV_N])
{
    int r, i;
    for (r = 0; r < RV_N; r++)
        for (i = 0; i < RV_N; i++)
            hs[r][i] = (__builtin_popcount((unsigned)(r & i)) & 1) ? -1.0 : 1.0;
}

static inline void reverb_lfo_start(double *lc, double *ls)
{
    int i;
    for (i = 0; i < RV_N; i++) {
        double ph = 2.0 * M_PI * 0.6180339887 * (double)i;
        lc[i] = cos(ph);
        ls[i] = sin(ph);
    }
}

static inline int reverb_init(reverb_state *s, uint32_t channels, double max_rate)
{
    size_t total = 0, off = 0;
    int k, i;

    memset(s, 0, sizeof *s);
    s->channels = channels;
    for (k = 0; k < RV_STEPS; k++) {
        s->dcap[k]  = rv_need_diff(k, max_rate);
        s->dmask[k] = rv_pow2(s->dcap[k]) - 1;
        total += (size_t)(s->dmask[k] + 1) * RV_N;
    }
    s->tcap  = rv_need_tank(max_rate);
    s->tmask = rv_pow2(s->tcap) - 1;
    total += (size_t)(s->tmask + 1) * RV_N;

    s->mem = calloc(total, sizeof *s->mem);
    s->hp  = calloc(channels, sizeof *s->hp);
    if (!s->mem || !s->hp) return -1;
    for (k = 0; k < RV_STEPS; k++)
        for (i = 0; i < RV_N; i++) {
            s->dbuf[k][i] = s->mem + off;
            off += s->dmask[k] + 1;
        }
    for (i = 0; i < RV_N; i++) {
        s->tbuf[i] = s->mem + off;
        off += s->tmask + 1;
    }
    reverb_hadamard_signs(s->hs);
    reverb_lfo_start(s->lc, s->ls);
    s->inscale = 1.0 / sqrt((double)channels);
    return 0;
}

static inline void reverb_fini(reverb_state *s)
{
    free(s->mem);
    free(s->hp);
    memset(s, 0, sizeof *s);
}

static inline void rv_hadamard(double *v)
{
    int h, i, j;
    for (h = 1; h < RV_N; h <<= 1)
        for (i = 0; i < RV_N; i += 2 * h)
            for (j = i; j < i + h; j++) {
                double a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    for (i = 0; i < RV_N; i++) v[i] *= 0.25;
}

static inline float rv_store(double v)
{
    return (v > -RV_STORE_FLOOR && v < RV_STORE_FLOOR) ? 0.0f : (float)v;
}

/* A glide is two one-pole stages in series, tgt -> mid -> cur: critically
 * damped, so the value's rate of change starts from zero and ramps rather
 * than stepping. A step in that rate is a kink in every waveform read
 * through the delay -- the pitch jumps -- and one stage alone gave one at
 * the start of every glide, twenty times the tone's own curvature. Both
 * stages snap to the target together once within snap of it. Returns
 * nonzero while still moving. */
static inline int rv_glide(double *cur, double *mid, double tgt, double k,
                           double snap)
{
    *mid += (tgt - *mid) * k;
    *cur += (*mid - *cur) * k;
    if (*cur - tgt > -snap && *cur - tgt < snap &&
        *mid - tgt > -snap && *mid - tgt < snap) {
        *cur = *mid = tgt;
        return 0;
    }
    return 1;
}

/* The rate for a glide with d still to go from cur, taken afresh every
 * sample: both stages are nearer their own targets than cur is to the
 * final one, so k = slew / d holds each to slew samples per sample --
 * never reading backwards -- and as d shrinks the rate returns to the
 * 50 ms time constant, so the last fraction of a sample does not take
 * twenty seconds. Taken from mid's distance instead, it let mid arrive
 * first and cur race the rest of the way at the full rate. */
static inline double rv_glide_k(double d, double k, double slew)
{
    double lim;
    if (d < 0) d = -d;
    if (d <= 0.0) return k;
    lim = slew / d;
    return lim < k ? lim : k;
}

static inline void reverb_run(reverb_state *s, const reverb_coeffs *c,
                              float *const *in, float *const *out,
                              uint32_t channels, uint32_t n)
{
    uint32_t f, ch;
    int k, i, gliding = 0;

    if (!s->started) {
        for (k = 0; k < RV_STEPS; k++)
            for (i = 0; i < RV_N; i++)
                s->dcur[k][i] = s->dmid[k][i] = (double)c->dd[k][i];
        for (i = 0; i < RV_N; i++) s->tcur[i] = s->tmid[i] = (double)c->L[i];
        s->depth_cur = s->depth_mid = c->depth;
        s->wet_cur = c->wet;
        s->dry_cur = c->dry;
        memcpy(s->sh_cur, c->shelf, sizeof s->sh_cur);
        memcpy(s->sh_mid, c->shelf, sizeof s->sh_mid);
        s->hp_cur = s->hp_mid = c->hp;
        s->started = 1;
    }

    for (k = 0; k < RV_STEPS && !gliding; k++)
        for (i = 0; i < RV_N; i++)
            if (s->dcur[k][i] != (double)c->dd[k][i]) gliding = 1;
    for (i = 0; i < RV_N; i++)
        if (s->tcur[i] != (double)c->L[i]) gliding = 1;
    if (s->depth_cur != c->depth || s->wet_cur != c->wet || s->dry_cur != c->dry ||
        memcmp(s->sh_cur, c->shelf, sizeof s->sh_cur) ||
        memcmp(&s->hp_cur, &c->hp, sizeof s->hp_cur))
        gliding = 1;

    for (f = 0; f < n; f++) {
        double u[RV_N], y[RV_N], w[RV_N], sum = 0.0, fb;

        if (gliding) {
            int still = 0;
            double tk[RV_N], dk;
            for (k = 0; k < RV_STEPS; k++)
                for (i = 0; i < RV_N; i++) {
                    double t = (double)c->dd[k][i];
                    dk = rv_glide_k(t - s->dcur[k][i], c->glide_k, RV_SLEW);
                    still |= rv_glide(&s->dcur[k][i], &s->dmid[k][i], t, dk, 1e-6);
                }
            for (i = 0; i < RV_N; i++) {
                double t = (double)c->L[i];
                tk[i] = rv_glide_k(t - s->tcur[i], c->glide_k, RV_TANK_SLEW);
                still |= rv_glide(&s->tcur[i], &s->tmid[i], t, tk[i], 1e-6);
            }
            dk = rv_glide_k(c->depth - s->depth_cur, c->glide_k, RV_TANK_SLEW);
            still |= rv_glide(&s->depth_cur, &s->depth_mid, c->depth, dk, 1e-6);
            /* A line's absorption is designed for its length, so its shelf
             * moves with it: switched at once, a line that is still long
             * would lose what a short one should, and the step in the
             * shelf's output is a click. The low-cut glides from the
             * identity and back when it is switched; both are interpolated
             * coefficient by coefficient, which keeps them stable -- the
             * stable denominators of a section form a convex set. */
            for (i = 0; i < RV_N; i++) {
                still |= rv_glide(&s->sh_cur[i].b0, &s->sh_mid[i].b0,
                                  c->shelf[i].b0, tk[i], 1e-12);
                still |= rv_glide(&s->sh_cur[i].b1, &s->sh_mid[i].b1,
                                  c->shelf[i].b1, tk[i], 1e-12);
                still |= rv_glide(&s->sh_cur[i].a1, &s->sh_mid[i].a1,
                                  c->shelf[i].a1, tk[i], 1e-12);
            }
            still |= rv_glide(&s->hp_cur.b0, &s->hp_mid.b0, c->hp.b0, c->glide_k, 1e-12);
            still |= rv_glide(&s->hp_cur.b1, &s->hp_mid.b1, c->hp.b1, c->glide_k, 1e-12);
            still |= rv_glide(&s->hp_cur.b2, &s->hp_mid.b2, c->hp.b2, c->glide_k, 1e-12);
            still |= rv_glide(&s->hp_cur.a1, &s->hp_mid.a1, c->hp.a1, c->glide_k, 1e-12);
            still |= rv_glide(&s->hp_cur.a2, &s->hp_mid.a2, c->hp.a2, c->glide_k, 1e-12);
            /* the mix is a gain, not a read position: one stage is enough */
            s->wet_cur += (c->wet - s->wet_cur) * c->ramp_k;
            s->dry_cur += (c->dry - s->dry_cur) * c->ramp_k;
            if (fabs(s->wet_cur - c->wet) < 1e-9 && fabs(s->dry_cur - c->dry) < 1e-9) {
                s->wet_cur = c->wet;
                s->dry_cur = c->dry;
            } else {
                still = 1;
            }
            gliding = still;
        }

        /* spread */
        for (i = 0; i < RV_N; i++) u[i] = 0.0;
        for (ch = 0; ch < channels; ch++) {
            double x = (double)in[ch][f], v;
            const double *row = s->hs[1 + ch % (RV_N - 1)];
            if (!isfinite(x)) x = 0.0;
            x = biquad_run(&s->hp_cur, &s->hp[ch], x);
            v = x * s->inscale * 0.25;
            for (i = 0; i < RV_N; i++) u[i] += v * row[i];
        }

        /* diffuse */
        for (k = 0; k < RV_STEPS; k++) {
            uint32_t m = s->dmask[k];
            for (i = 0; i < RV_N; i++) {
                float *b = s->dbuf[k][i];
                double t = s->dcur[k][i], a, r;
                int ip;

                b[s->pos & m] = rv_store(u[i]);
                ip = (int)t;
                a = t - (double)ip;
                if (a == 0.0)
                    r = (double)b[(s->pos - (uint32_t)ip) & m];
                else
                    r = (1.0 - a) * (double)b[(s->pos - (uint32_t)ip) & m]
                      + a * (double)b[(s->pos - (uint32_t)ip - 1) & m];
                u[i] = r * c->ds[k][i];
            }
            rv_hadamard(u);
        }

        /* tank: read, absorb, mix, write */
        for (i = 0; i < RV_N; i++) {
            const float *b = s->tbuf[i];
            double t = s->tcur[i] + s->depth_cur * s->ls[i], fr, eta, x0, x1, v;
            int ip;

            if (t < 2.0) t = 2.0;
            ip = (int)floor(t - 0.5);
            fr = t - (double)ip;
            eta = (1.0 - fr) / (1.0 + fr);
            x0 = (double)b[(s->pos - (uint32_t)ip) & s->tmask];
            x1 = (double)b[(s->pos - (uint32_t)ip - 1) & s->tmask];
            v = eta * x0 + x1 - eta * s->ap[i];
            s->ap[i] = dsp_flush(v);
            y[i] = onepole_run(&s->sh_cur[i], &s->op[i], v);
            sum += y[i];
        }
        fb = sum * (2.0 / RV_N);
        for (i = 0; i < RV_N; i++) {
            w[i] = u[i] + (y[i] - fb);
            s->tbuf[i][s->pos & s->tmask] = rv_store(w[i]);
        }

        /* out */
        for (ch = 0; ch < channels; ch++) {
            const double *row = s->hs[1 + ch % (RV_N - 1)];
            double x = (double)in[ch][f], acc = 0.0;
            if (!isfinite(x)) x = 0.0;
            for (i = 0; i < RV_N; i++) acc += row[i] * w[i];
            out[ch][f] = (float)(s->dry_cur * x + s->wet_cur * (acc * 0.25));
        }

        /* the oscillators */
        for (i = 0; i < RV_N; i++) {
            double nc = s->lc[i] * c->rot_c[i] - s->ls[i] * c->rot_s[i];
            double ns = s->ls[i] * c->rot_c[i] + s->lc[i] * c->rot_s[i];
            s->lc[i] = nc;
            s->ls[i] = ns;
        }
        if ((++s->count & 1023u) == 0)
            for (i = 0; i < RV_N; i++) {
                double g = 1.0 / sqrt(s->lc[i] * s->lc[i] + s->ls[i] * s->ls[i]);
                s->lc[i] *= g;
                s->ls[i] *= g;
            }
        s->pos++;
    }
}

#endif /* PWF_REVERB_DSP_H */
