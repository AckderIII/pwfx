/*
 * pwf.h -- shared scaffolding for the pw_* PipeWire filter programs.
 *
 * Every filter in this tree is the same program with a different DSP core:
 *
 *   - a table of typed, range-checked parameters,
 *   - a command line that is generated from that table,
 *   - a line-oriented control protocol on stdin/stdout,
 *   - a pw_filter node with N mono DSP input ports and N mono DSP output
 *     ports, processed in the realtime thread.
 *
 * Threading model
 * ---------------
 * Parameters are edited on the main loop thread; audio is processed on the
 * PipeWire data (realtime) thread. Nothing is shared mutably between them.
 * Instead:
 *
 *   main thread   design()  -> writes a fresh POD coefficient blob into a
 *                              free slot of a ring, then publishes the slot
 *                              with a release store of a counter.
 *   data thread   process() -> acquire-loads the counter, reads that slot.
 *                              It never writes to it.
 *
 * The blob is plain data: no pointers, no allocation. Anything whose size
 * depends on a parameter (delay lines, lookahead windows) is allocated once
 * at startup for the worst case (max-rate x the largest legal setting) and
 * the blob carries only the currently used length. So a parameter change is
 * wait-free on the reader side and allocation-free on both, which is what
 * makes every parameter live-changeable rather than startup-only.
 *
 * The counter is the slot index, not a wrapped one: process() compares it
 * with the value it last saw to decide whether the configuration changed,
 * and the ring is large enough that a slot cannot be recycled underneath a
 * reader that is still inside a single process() call.
 */

#ifndef PWF_H
#define PWF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWF_MAX_CHANNELS 64
#define PWF_STRMAX       128

/* Parameter value types. Everything numeric is carried as a double and
 * converted on the way in and out; that keeps one table shape for all of
 * them and costs nothing outside the (rare) parameter path. */
typedef enum {
    PWF_DOUBLE,
    PWF_INT,
    PWF_BOOL,     /* value is 0 or 1 */
    PWF_ENUM,     /* value is an index into .choices */
    PWF_STRING    /* value lives in .str, not .value */
} pwf_type;

enum {
    PWF_F_NONE = 0,
    PWF_F_RO   = 1u << 0,   /* reported, never settable (rate, latency) */
    PWF_F_CLI  = 1u << 1    /* settable on the command line only, not live */
};

typedef struct {
    const char         *name;      /* --name on the CLI, "name value" on stdin */
    const char         *shortopt;  /* e.g. "-f", or NULL */
    pwf_type            type;
    double              min, max;  /* inclusive, numeric types only */
    const char *const  *choices;   /* PWF_ENUM: NULL-terminated */
    double              def;       /* numeric default */
    const char         *defstr;    /* PWF_STRING default, may be NULL */
    const char         *unit;      /* "Hz", "dB", "ms", ... or NULL */
    const char         *help;      /* one line, for --help */
    uint32_t            flags;
    /* --- runtime, owned by the framework --- */
    double              value;
    char                str[PWF_STRMAX];
} pwf_param;

/* Terminator for a parameter table. */
#define PWF_PARAM_END { NULL }

struct pwf;

/* What a response curve's axes are. */
typedef enum {
    PWF_CURVE_NONE = 0,
    PWF_CURVE_FREQ,       /* x = frequency in Hz, y = gain in dB */
    PWF_CURVE_LEVEL       /* x = input level in dB, y = output level in dB */
} pwf_curve_kind;

/* The most points a curve can have. */
#define PWF_CURVE_MAX 256

typedef struct {
    const char *name;         /* program and default node name, "pw_lpf" */
    const char *summary;      /* one line, shown at the top of --help */
    const char *notes;        /* free text, shown at the bottom of --help */

    pwf_param  *params;       /* filter parameters, PWF_PARAM_END terminated */
    size_t      coeff_size;   /* sizeof the coefficient blob design() fills */

    /* Allocate realtime state. Called once, on the main thread, before the
     * filter is connected. max_rate is the highest sample rate the state has
     * to cope with, so that nothing needs allocating later. */
    int  (*init)(void *user, uint32_t channels, double max_rate);
    void (*fini)(void *user);

    /* Optional cross-parameter check, main thread. Range checks are done by
     * the framework; this is for constraints between parameters (a gate's
     * close threshold against its open threshold, say). Return 0 to accept,
     * -1 and fill err to reject the change. */
    int  (*validate)(void *user, const pwf_param *p, double rate,
                     char *err, size_t errsz);

    /* Turn parameters into coefficients, main thread. Fills coeffs (a blob
     * of coeff_size bytes, zeroed beforehand) and returns the latency this
     * configuration introduces, in samples. */
    uint32_t (*design)(void *user, const pwf_param *p, double rate,
                       uint32_t channels, void *coeffs);

    /* Process one quantum, realtime thread. changed is nonzero on the first
     * call that sees a new coefficient blob, which is where a filter resets
     * any structure whose geometry depends on a parameter. in[] and out[]
     * hold one mono buffer per channel and never alias. */
    void (*process)(void *user, const void *coeffs, int changed,
                    float *const *in, float *const *out,
                    uint32_t channels, uint32_t n_samples);

    /* Optional, realtime thread: the most gain reduction the filter has
     * applied since the last call, in dB >= 0. Called after every process()
     * with the same coeffs, so it must be as cheap and as lock-free as that.
     * A filter that sets this gets a read-only "gr" meter, reported with
     * in-level and out-level; one that leaves it NULL does not have one. */
    double (*gain_reduction)(void *user, const void *coeffs);

    /* Optional, main thread: the response the coefficients produce, for
     * drawing. Fill x[] and y[] with up to max points and return how many.
     * The points are a path in drawing order, not a function of x -- a
     * gate's hysteresis is a loop. A filter that sets this answers "curve"
     * on stdin and --curve on the command line, and says so in --help. */
    pwf_curve_kind curve_kind;
    size_t (*curve)(void *user, const void *coeffs, double rate,
                    double *x, double *y, size_t max);

    void *user;
} pwf_desc;

/* Parse argv, set up PipeWire, run until interrupted. Returns an exit code. */
int pwf_run(pwf_desc *desc, int argc, char **argv);

/* Helpers a filter's validate()/design() can use to read the table by name;
 * both abort on an unknown name, since the table is a compile-time thing. */
double      pwf_value(const pwf_param *p, const char *name);
const char *pwf_string(const pwf_param *p, const char *name);

/* The x axes the curve callbacks share, so every curve of one kind is drawn
 * against the same grid. Both return how many points they wrote.
 *   frequency: log-spaced from 20 Hz to 20 kHz, or to just under Nyquist;
 *   level:     -90 to +6 dB in 1 dB steps. */
size_t pwf_freq_grid(double rate, double *x, size_t max);
size_t pwf_level_grid(double *x, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* PWF_H */
