/*
 * pwf.c -- shared scaffolding for the pw_* PipeWire filters. See pwf.h for
 * the threading model; this file is the plumbing around it.
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <strings.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/support/system.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include "pwf.h"

/* Coefficient slots. A slot is recycled only after this many parameter
 * changes, which cannot happen while a single process() call is in flight,
 * so the reader never sees a slot rewritten underneath it. Power of two. */
#define PWF_SLOTS     16

/* Largest quantum the silence/discard scratch buffers cover. PipeWire's
 * default maximum is 8192 frames; anything above this is clamped rather
 * than risking a write past the end of the scratch. */
#define PWF_MAX_QUANTUM 16384

/* Bypass is crossfaded rather than switched, so toggling it cannot click. */
#define PWF_XFADE_MS  10.0

/* Level floor. Below this is past float32 resolution anyway, and a finite
 * number keeps the meters parseable by anything reading the protocol. */
#define PWF_DB_FLOOR  (-240.0)

/* How fast a peak meter falls back when the signal drops. Without a fallback
 * the reported number is the maximum of whatever block happened to be under
 * the timer, which flickers; with one it reads like a meter. */
#define PWF_METER_FALL_DB_S 20.0

enum {
    C_CHANNELS, C_RATE, C_LATENCY, C_BYPASS,
    C_METER, C_IN_LEVEL, C_OUT_LEVEL,
    C_MAX_RATE, C_NODE_NAME, C_TARGET, C_NODE_LATENCY,
    C_N
};

static const pwf_param core_template[C_N] = {
    { .name = "channels", .shortopt = "-c", .type = PWF_INT,
      .min = 1, .max = PWF_MAX_CHANNELS, .def = 2, .flags = PWF_F_CLI,
      .help = "number of mono port pairs to create" },
    { .name = "rate", .type = PWF_INT,
      .min = 1, .max = 768000, .def = 48000, .flags = PWF_F_RO, .unit = "Hz",
      .help = "graph sample rate (reported by the server once running)" },
    { .name = "latency", .type = PWF_INT,
      .min = 0, .max = 1u << 24, .def = 0, .flags = PWF_F_RO, .unit = "samples",
      .help = "latency this filter adds, reported to the graph" },
    { .name = "bypass", .type = PWF_BOOL, .def = 0,
      .help = "pass the input through, crossfaded over 10 ms" },
    { .name = "meter", .type = PWF_INT,
      .min = 0, .max = 10000, .def = 0, .unit = "ms",
      .help = "how often to report in-level and out-level, 0 = off" },
    { .name = "in-level", .type = PWF_DOUBLE,
      .min = PWF_DB_FLOOR, .max = 60, .def = PWF_DB_FLOOR,
      .flags = PWF_F_RO, .unit = "dB",
      .help = "peak level arriving at the input ports, dBFS" },
    { .name = "out-level", .type = PWF_DOUBLE,
      .min = PWF_DB_FLOOR, .max = 60, .def = PWF_DB_FLOOR,
      .flags = PWF_F_RO, .unit = "dB",
      .help = "peak level leaving the output ports, dBFS" },
    { .name = "max-rate", .type = PWF_INT,
      .min = 8000, .max = 768000, .def = 192000, .flags = PWF_F_CLI, .unit = "Hz",
      .help = "highest rate to size delay lines for, at startup" },
    { .name = "node-name", .type = PWF_STRING, .flags = PWF_F_CLI,
      .help = "PipeWire node.name (default: the program name)" },
    { .name = "target", .type = PWF_STRING, .flags = PWF_F_CLI,
      .help = "node to auto-connect to (target.object)" },
    { .name = "node-latency", .type = PWF_STRING, .flags = PWF_F_CLI,
      .help = "requested quantum as FRAMES/RATE, e.g. 256/48000" },
};

struct pwf_port {
    struct pwf *s;
    uint32_t    idx;
};

struct pwf {
    pwf_desc  *desc;
    pwf_param *fp;                  /* filter parameter table */
    pwf_param  core[C_N];

    struct pw_main_loop *mloop;
    struct pw_loop      *loop;
    struct pw_filter    *filter;

    struct pwf_port *in[PWF_MAX_CHANNELS];
    struct pwf_port *out[PWF_MAX_CHANNELS];

    uint32_t channels;
    double   rate;
    uint32_t latency;               /* samples, as last designed */

    uint8_t *slots;                 /* PWF_SLOTS * desc->coeff_size */
    _Atomic uint32_t pub;           /* published slot counter */

    _Atomic uint32_t rt_rate;       /* rate seen by the data thread */
    _Atomic uint32_t rt_bypass;
    _Atomic uint32_t rt_xfade;      /* crossfade length in samples */
    struct spa_source *rate_ev;

    /* Meters run the other way to everything else here: the data thread
     * measures and the main thread reads. One scalar each, and losing an
     * update costs nothing, so a plain relaxed store is the whole channel --
     * no ring, no sequence counter. Float bits rather than _Atomic double
     * because an 8-byte atomic is not lock-free everywhere, and nothing in
     * process() may take a lock. */
    _Atomic uint32_t rt_meter;      /* nonzero while metering is on */
    _Atomic uint32_t rt_in_level;   /* float bits, dBFS */
    _Atomic uint32_t rt_out_level;  /* float bits, dBFS */
    _Atomic uint32_t rt_fall;       /* float bits, dB of fallback per sample */
    struct spa_source *meter_timer;

    struct spa_source *stdin_src;
    char   lbuf[4096];
    size_t llen;
    int    loverflow;

    float *zero;                    /* silence, for unconnected inputs */
    float *sink;                    /* scratch, for unconnected outputs */

    /* data thread only */
    uint32_t rt_last_pub;
    uint32_t rt_rate_seen;
    double   rt_mix;                /* 1 = fully processed, 0 = fully dry */
    double   rt_in_db, rt_out_db;   /* held peaks */

    int meter_first;                /* report once on arming, even if idle */
    int started;                    /* past the initial parameter dump */
    int quit;
};

/* ---- parameter table helpers ------------------------------------------ */

static size_t table_len(const pwf_param *p)
{
    size_t n = 0;
    while (p && p[n].name) n++;
    return n;
}

static pwf_param *find_param(struct pwf *s, const char *name)
{
    size_t i;
    for (i = 0; i < C_N; i++)
        if (!strcmp(s->core[i].name, name)) return &s->core[i];
    for (i = 0; s->fp[i].name; i++)
        if (!strcmp(s->fp[i].name, name)) return &s->fp[i];
    return NULL;
}

static pwf_param *find_short(struct pwf *s, const char *opt)
{
    size_t i;
    for (i = 0; i < C_N; i++)
        if (s->core[i].shortopt && !strcmp(s->core[i].shortopt, opt))
            return &s->core[i];
    for (i = 0; s->fp[i].name; i++)
        if (s->fp[i].shortopt && !strcmp(s->fp[i].shortopt, opt))
            return &s->fp[i];
    return NULL;
}

static void fmt_value(const pwf_param *p, char *buf, size_t n)
{
    switch (p->type) {
    case PWF_DOUBLE: snprintf(buf, n, "%.6g", p->value);                 break;
    case PWF_INT:    snprintf(buf, n, "%ld", (long)llround(p->value));   break;
    case PWF_BOOL:   snprintf(buf, n, "%s", p->value != 0 ? "on" : "off"); break;
    case PWF_ENUM:   snprintf(buf, n, "%s", p->choices[(int)p->value]);  break;
    case PWF_STRING: snprintf(buf, n, "%s", p->str);                     break;
    }
}

/* The whole stdout protocol: one "name value" line per parameter, plus
 * "#"-prefixed comments that a consumer can skip. */
static void report(const pwf_param *p)
{
    char v[PWF_STRMAX];
    fmt_value(p, v, sizeof v);
    printf("%s %s\n", p->name, v);
    fflush(stdout);
}

/* A float carried through an atomic as its bit pattern. */
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static float    u2f(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

/* Peak hold with a linear fallback in dB. Holding the level in dB rather
 * than linear is what keeps this out of the per-sample path: the fallback is
 * a subtraction proportional to the block length, and the only transcendental
 * is one log per block. */
static double meter_step(double held, float peak, double fall)
{
    double now = peak > 0.0f ? 20.0 * log10((double)peak) : PWF_DB_FLOOR;

    held -= fall;
    if (now > held)          held = now;
    if (held < PWF_DB_FLOOR) held = PWF_DB_FLOOR;
    return held;
}

/* Copy what the data thread last measured into the parameter table, rounded
 * to a tenth of a dB so the reported number is a level and not a stream of
 * meter noise. Returns nonzero if either value moved. */
static int refresh_meters(struct pwf *s)
{
    double in  = u2f(atomic_load_explicit(&s->rt_in_level,  memory_order_relaxed));
    double out = u2f(atomic_load_explicit(&s->rt_out_level, memory_order_relaxed));

    in  = round(in  * 10.0) / 10.0;
    out = round(out * 10.0) / 10.0;

    if (in == s->core[C_IN_LEVEL].value && out == s->core[C_OUT_LEVEL].value)
        return 0;

    s->core[C_IN_LEVEL].value  = in;
    s->core[C_OUT_LEVEL].value = out;
    return 1;
}

static void report_all(struct pwf *s)
{
    size_t i;

    refresh_meters(s);
    for (i = 0; i < C_N; i++) report(&s->core[i]);
    for (i = 0; s->fp[i].name; i++) report(&s->fp[i]);
}

static void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

double pwf_value(const pwf_param *p, const char *name)
{
    size_t i;
    for (i = 0; p[i].name; i++)
        if (!strcmp(p[i].name, name)) return p[i].value;
    fprintf(stderr, "pwf: internal error: no parameter '%s'\n", name);
    abort();
}

const char *pwf_string(const pwf_param *p, const char *name)
{
    size_t i;
    for (i = 0; p[i].name; i++)
        if (!strcmp(p[i].name, name)) return p[i].str;
    fprintf(stderr, "pwf: internal error: no parameter '%s'\n", name);
    abort();
}

/* ---- parsing one value ------------------------------------------------- */

/* Writes the parsed value into *out (numeric) or leaves it to the caller for
 * strings. Returns 0 on success, -1 with a message on stderr otherwise. */
static int parse_value(const pwf_param *p, const char *v, double *out)
{
    char *end;
    double d;
    int i;

    switch (p->type) {
    case PWF_BOOL:
        if (!strcasecmp(v, "on")  || !strcasecmp(v, "true")  ||
            !strcasecmp(v, "yes") || !strcmp(v, "1"))
            *out = 1;
        else if (!strcasecmp(v, "off") || !strcasecmp(v, "false") ||
                 !strcasecmp(v, "no")  || !strcmp(v, "0"))
            *out = 0;
        else if (!strcasecmp(v, "toggle"))
            *out = p->value != 0 ? 0 : 1;
        else {
            err("%s: expected on/off, got '%s'", p->name, v);
            return -1;
        }
        return 0;

    case PWF_ENUM:
        for (i = 0; p->choices[i]; i++)
            if (!strcasecmp(p->choices[i], v)) { *out = i; return 0; }
        {
            char list[256] = "";
            for (i = 0; p->choices[i]; i++) {
                if (i) strncat(list, "|", sizeof list - strlen(list) - 1);
                strncat(list, p->choices[i], sizeof list - strlen(list) - 1);
            }
            err("%s: expected one of %s, got '%s'", p->name, list, v);
        }
        return -1;

    case PWF_DOUBLE:
    case PWF_INT:
        errno = 0;
        d = strtod(v, &end);
        if (end == v || *end != '\0' || errno == ERANGE || !isfinite(d)) {
            err("%s: '%s' is not a number", p->name, v);
            return -1;
        }
        if (p->type == PWF_INT) d = (double)llround(d);
        if (d < p->min || d > p->max) {
            err("%s: %g is out of range [%g, %g]", p->name, d, p->min, p->max);
            return -1;
        }
        *out = d;
        return 0;

    case PWF_STRING:
        if (strlen(v) >= PWF_STRMAX) {
            err("%s: value is too long (max %d characters)",
                p->name, PWF_STRMAX - 1);
            return -1;
        }
        return 0;
    }
    return -1;
}

/* ---- design and publish ------------------------------------------------ */

static void update_latency_param(struct pwf *s)
{
    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const struct spa_pod *params[1];
    struct spa_process_latency_info info;
    int res;

    if (!s->filter) return;

    spa_zero(info);
    info.rate = (int32_t)s->latency;
    info.ns   = s->rate > 0 ? (int64_t)(s->latency * 1000000000.0 / s->rate) : 0;

    params[0] = spa_process_latency_build(&b, SPA_PARAM_ProcessLatency, &info);
    if ((res = pw_filter_update_params(s->filter, NULL, params, 1)) < 0)
        fprintf(stderr, "# warning: cannot report latency: %s\n",
                spa_strerror(res));
}

/* Designs into slot (idx % PWF_SLOTS) without publishing it. */
static void design_into(struct pwf *s, uint32_t idx)
{
    void *slot = s->slots + (size_t)(idx % PWF_SLOTS) * s->desc->coeff_size;
    uint32_t lat;

    memset(slot, 0, s->desc->coeff_size);
    lat = s->desc->design(s->desc->user, s->fp, s->rate, s->channels, slot);

    if (lat != s->latency) {
        s->latency = lat;
        s->core[C_LATENCY].value = lat;
        if (s->started) report(&s->core[C_LATENCY]);
        update_latency_param(s);
    }
}

/* Metering is periodic, and everything else on stdout is event-driven: a line
 * appears because something changed or was asked for. So it is off unless
 * asked for, and a consumer that never sets "meter" sees exactly the stream
 * it saw before this existed. */
static void arm_meter(struct pwf *s)
{
    struct timespec value = { 0, 0 }, interval = { 0, 0 };
    long ms = (long)llround(s->core[C_METER].value);

    atomic_store_explicit(&s->rt_meter, ms > 0, memory_order_relaxed);
    /* Report once as soon as it is armed. Otherwise turning the meter on
     * over a silent or steady signal produces nothing at all, which is
     * indistinguishable from it not working. */
    s->meter_first = ms > 0;
    if (!s->meter_timer)
        return;
    if (ms > 0) {
        value.tv_sec  = ms / 1000;
        value.tv_nsec = (ms % 1000) * 1000000L;
        interval = value;
    }
    pw_loop_update_timer(s->loop, s->meter_timer, &value, &interval, false);
}

static void on_meter_timer(void *data, uint64_t expirations)
{
    struct pwf *s = data;

    (void)expirations;
    if (!refresh_meters(s) && !s->meter_first)
        return;                 /* nothing moved: an idle filter stays quiet */
    s->meter_first = 0;
    report(&s->core[C_IN_LEVEL]);
    report(&s->core[C_OUT_LEVEL]);
}

/* The scalars the data thread reads outside the coefficient blob. All of
 * them depend on the rate, so they are refreshed wherever the blob is. */
static void publish_scalars(struct pwf *s)
{
    double rate = s->rate > 0.0 ? s->rate : 48000.0;

    atomic_store_explicit(&s->rt_bypass,
            s->core[C_BYPASS].value != 0, memory_order_relaxed);
    atomic_store_explicit(&s->rt_xfade,
            (uint32_t)(PWF_XFADE_MS * rate / 1000.0) + 1,
            memory_order_relaxed);
    atomic_store_explicit(&s->rt_fall,
            f2u((float)(PWF_METER_FALL_DB_S / rate)), memory_order_relaxed);
}

static void publish(struct pwf *s)
{
    uint32_t next = atomic_load_explicit(&s->pub, memory_order_relaxed) + 1;
    design_into(s, next);
    atomic_store_explicit(&s->pub, next, memory_order_release);

    publish_scalars(s);
    arm_meter(s);
}


/* ---- applying a change -------------------------------------------------- */

/* Applies v (or sv, for strings) to p, running the filter's cross-parameter
 * check and rolling back if it fails. Reports the result. */
static int apply(struct pwf *s, pwf_param *p, double v, const char *sv)
{
    char errbuf[256] = "";
    double old = p->value;
    char oldstr[PWF_STRMAX];

    memcpy(oldstr, p->str, sizeof oldstr);
    if (p->type == PWF_STRING) snprintf(p->str, sizeof p->str, "%s", sv);
    else                       p->value = v;

    if (s->desc->validate &&
        s->desc->validate(s->desc->user, s->fp, s->rate,
                          errbuf, sizeof errbuf) < 0) {
        p->value = old;
        memcpy(p->str, oldstr, sizeof p->str);
        err("%s: %s", p->name, errbuf[0] ? errbuf : "rejected");
        return -1;
    }
    return 0;
}

/* ---- the stdin control protocol ---------------------------------------- */

static void usage(struct pwf *s, FILE *f, const char *prefix);

static void command(struct pwf *s, char *line)
{
    char *name, *val, *p;
    pwf_param *par;
    double v = 0;

    while (*line && isspace((unsigned char)*line)) line++;
    p = line + strlen(line);
    while (p > line && isspace((unsigned char)p[-1])) *--p = '\0';

    if (*line == '\0' || *line == '#') return;

    name = line;
    val  = NULL;
    for (p = line; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '=' || *p == ':') {
            *p++ = '\0';
            while (*p && (isspace((unsigned char)*p) || *p == '=' || *p == ':'))
                p++;
            val = p;
            break;
        }
    }

    if (!val || !*val) {
        /* bare word: either a query for one parameter, or a command */
        if (!strcmp(name, "dump") || !strcmp(name, "?") ||
            !strcmp(name, "params")) {
            report_all(s);
            return;
        }
        if (!strcmp(name, "help")) { usage(s, stdout, "# "); return; }
        if (!strcmp(name, "quit") || !strcmp(name, "exit")) {
            s->quit = 1;
            pw_main_loop_quit(s->mloop);
            return;
        }
        if ((par = find_param(s, name)) != NULL) {
            if (par == &s->core[C_IN_LEVEL] || par == &s->core[C_OUT_LEVEL])
                refresh_meters(s);
            report(par);
            return;
        }
        err("unknown command '%s' (try 'help')", name);
        return;
    }

    if ((par = find_param(s, name)) == NULL) {
        err("unknown parameter '%s' (try 'dump')", name);
        return;
    }
    if (par->flags & PWF_F_RO) {
        err("%s is read-only", par->name);
        return;
    }
    if (par->flags & PWF_F_CLI) {
        err("%s can only be set on the command line", par->name);
        return;
    }
    if (parse_value(par, val, &v) < 0) return;
    if (apply(s, par, v, val) < 0)     return;

    report(par);
    publish(s);
}

static void feed(struct pwf *s, const char *buf, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            if (s->loverflow) {
                err("input line too long, ignored");
                s->loverflow = 0;
            } else {
                s->lbuf[s->llen] = '\0';
                command(s, s->lbuf);
            }
            s->llen = 0;
            if (s->quit) return;
        } else if (s->llen + 1 < sizeof s->lbuf) {
            s->lbuf[s->llen++] = buf[i];
        } else {
            s->loverflow = 1;
            s->llen = 0;
        }
    }
}

static void on_stdin(void *data, int fd, uint32_t mask)
{
    struct pwf *s = data;
    char buf[1024];
    ssize_t r;

    if (mask & SPA_IO_IN) {
        r = read(fd, buf, sizeof buf);
        if (r > 0) {
            feed(s, buf, (size_t)r);
            return;
        }
        if (r < 0 && (errno == EINTR || errno == EAGAIN))
            return;
    }

    /* EOF or error: stop watching stdin but keep filtering. A filter started
     * from a shell with no controller attached should not exit just because
     * its control channel is closed. */
    if (s->stdin_src) {
        pw_loop_destroy_source(s->loop, s->stdin_src);
        s->stdin_src = NULL;
    }
}

/* ---- PipeWire ----------------------------------------------------------- */

static void on_rate_event(void *data, uint64_t count)
{
    struct pwf *s = data;
    uint32_t r = atomic_load_explicit(&s->rt_rate, memory_order_relaxed);

    (void)count;
    if (r == 0 || (double)r == s->rate) return;

    s->rate = r;
    s->core[C_RATE].value = r;
    report(&s->core[C_RATE]);
    publish(s);
}

static void on_state_changed(void *data, enum pw_filter_state old,
                             enum pw_filter_state state, const char *error)
{
    struct pwf *s = data;

    (void)old;
    printf("# state %s\n", pw_filter_state_as_string(state));
    fflush(stdout);
    if (state == PW_FILTER_STATE_ERROR) {
        err("filter: %s", error ? error : "unknown error");
        pw_main_loop_quit(s->mloop);
    }
}

static void on_process(void *data, struct spa_io_position *position)
{
    struct pwf *s = data;
    uint32_t total = position->clock.duration;
    uint32_t rate  = position->clock.rate.denom;
    uint32_t nch = s->channels, i, j, pub, done;
    float *base_in[PWF_MAX_CHANNELS], *base_out[PWF_MAX_CHANNELS];
    float *in[PWF_MAX_CHANNELS], *out[PWF_MAX_CHANNELS];
    const void *coeffs;
    int changed, meter;
    float ipk = 0.0f, opk = 0.0f;
    double tgt, inc;

    if (total == 0) return;

    /* The graph rate is only known here. Hand it to the main thread, which
     * owns coefficient design; signalling an eventfd is all this costs. */
    if (rate != 0 && rate != s->rt_rate_seen) {
        s->rt_rate_seen = rate;
        atomic_store_explicit(&s->rt_rate, rate, memory_order_relaxed);
        pw_loop_signal_event(s->loop, s->rate_ev);
    }

    for (i = 0; i < nch; i++) {
        base_in[i]  = pw_filter_get_dsp_buffer(s->in[i], total);
        base_out[i] = pw_filter_get_dsp_buffer(s->out[i], total);
    }

    /* One configuration for the whole quantum, read once. */
    pub     = atomic_load_explicit(&s->pub, memory_order_acquire);
    coeffs  = s->slots + (size_t)(pub % PWF_SLOTS) * s->desc->coeff_size;
    changed = (pub != s->rt_last_pub);
    s->rt_last_pub = pub;

    tgt = atomic_load_explicit(&s->rt_bypass, memory_order_relaxed) ? 0.0 : 1.0;
    inc = 1.0 / (double)atomic_load_explicit(&s->rt_xfade, memory_order_relaxed);
    meter = atomic_load_explicit(&s->rt_meter, memory_order_relaxed) != 0;

    /* An unconnected port is served from the shared silence and discard
     * buffers, which are only PWF_MAX_QUANTUM long, so a quantum larger than
     * that is processed in slices rather than clamped -- clamping would
     * leave the tail of a real output buffer holding the previous round's
     * samples. */
    for (done = 0; done < total; ) {
        uint32_t n = total - done;
        if (n > PWF_MAX_QUANTUM) n = PWF_MAX_QUANTUM;

        for (i = 0; i < nch; i++) {
            in[i]  = base_in[i]  ? base_in[i]  + done : s->zero;
            out[i] = base_out[i] ? base_out[i] + done : s->sink;
        }

        /* An unconnected port reads from the shared silence buffer and an
         * unconnected output is discarded into the shared scratch, so the
         * meters look at base_*[] to tell a real port from a stand-in --
         * metering the scratch would report audio nobody receives. */
        if (meter) {
            for (i = 0; i < nch; i++) {
                if (!base_in[i]) continue;
                for (j = 0; j < n; j++) {
                    float a = fabsf(in[i][j]);
                    if (a > ipk) ipk = a;
                }
            }
        }

        s->desc->process(s->desc->user, coeffs, changed, in, out, nch, n);
        changed = 0;

        /* Bypass, as a crossfade between the processed and the dry signal.
         * Every channel walks the same ramp, so they all end together. */
        if (s->rt_mix != tgt || tgt != 1.0) {
            double mix = s->rt_mix;
            for (i = 0; i < nch; i++) {
                double m = s->rt_mix;
                const float *dry = in[i];
                float *o = out[i];
                for (j = 0; j < n; j++) {
                    if      (m < tgt) { m += inc; if (m > tgt) m = tgt; }
                    else if (m > tgt) { m -= inc; if (m < tgt) m = tgt; }
                    o[j] = (float)(m * o[j] + (1.0 - m) * dry[j]);
                }
                mix = m;
            }
            s->rt_mix = mix;
        }

        /* Measured after the crossfade, so out-level is the signal that
         * actually leaves the node and bypass shows through it. */
        if (meter) {
            for (i = 0; i < nch; i++) {
                if (!base_out[i]) continue;
                for (j = 0; j < n; j++) {
                    float a = fabsf(out[i][j]);
                    if (a > opk) opk = a;
                }
            }
        }

        done += n;
    }

    if (meter) {
        double fall = (double)u2f(atomic_load_explicit(&s->rt_fall,
                                                      memory_order_relaxed))
                    * (double)total;

        s->rt_in_db  = meter_step(s->rt_in_db,  ipk, fall);
        s->rt_out_db = meter_step(s->rt_out_db, opk, fall);
        atomic_store_explicit(&s->rt_in_level,  f2u((float)s->rt_in_db),
                              memory_order_relaxed);
        atomic_store_explicit(&s->rt_out_level, f2u((float)s->rt_out_db),
                              memory_order_relaxed);
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

static void on_signal(void *data, int sig)
{
    struct pwf *s = data;
    (void)sig;
    pw_main_loop_quit(s->mloop);
}

/* ---- command line ------------------------------------------------------- */

static const char *placeholder(const pwf_param *p)
{
    if (p->unit) {
        if (!strcmp(p->unit, "Hz"))      return "HZ";
        if (!strcmp(p->unit, "dB"))      return "DB";
        if (!strcmp(p->unit, "ms"))      return "MS";
        if (!strcmp(p->unit, "samples")) return "N";
    }
    switch (p->type) {
    case PWF_INT:    return "N";
    case PWF_BOOL:   return "on|off";
    case PWF_ENUM:   return "MODE";
    case PWF_STRING: return "STR";
    default:         return "X";
    }
}

/* Multi-line text, with the prefix repeated on every line. Without this the
 * "help" command on stdin would emit lines that are neither "name value" nor
 * a #-comment, which is exactly what a consumer parses stdout for. */
static void fput_prefixed(FILE *f, const char *prefix, const char *text)
{
    const char *p = text;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        fprintf(f, "%s%.*s\n", prefix, (int)len, p);
        if (!nl) break;
        p = nl + 1;
    }
}

static void print_param_help(FILE *f, const pwf_param *orig, const char *prefix)
{
    char lhs[64], rhs[192], dflt[PWF_STRMAX];
    pwf_param copy = *orig, *p = &copy;
    int i;

    /* Show the declared default, not whatever earlier options have set. */
    p->value = p->def;
    snprintf(p->str, sizeof p->str, "%s", p->defstr ? p->defstr : "");

    if (p->flags & PWF_F_RO) {
        snprintf(lhs, sizeof lhs, "%s", p->name);
    } else if (p->shortopt) {
        snprintf(lhs, sizeof lhs, "%s, --%s %s",
                 p->shortopt, p->name, placeholder(p));
    } else {
        snprintf(lhs, sizeof lhs, "    --%s %s", p->name, placeholder(p));
    }

    fmt_value(p, dflt, sizeof dflt);
    switch (p->type) {
    case PWF_DOUBLE:
    case PWF_INT:
        snprintf(rhs, sizeof rhs, "%g..%g%s%s, default %s",
                 p->min, p->max, p->unit ? " " : "", p->unit ? p->unit : "",
                 dflt);
        break;
    case PWF_ENUM:
        rhs[0] = '\0';
        for (i = 0; p->choices[i]; i++) {
            if (i) strncat(rhs, "|", sizeof rhs - strlen(rhs) - 1);
            strncat(rhs, p->choices[i], sizeof rhs - strlen(rhs) - 1);
        }
        strncat(rhs, ", default ", sizeof rhs - strlen(rhs) - 1);
        strncat(rhs, dflt, sizeof rhs - strlen(rhs) - 1);
        break;
    case PWF_BOOL:
        snprintf(rhs, sizeof rhs, "default %s", dflt);
        break;
    case PWF_STRING:
        snprintf(rhs, sizeof rhs, "%s", dflt[0] ? dflt : "unset");
        break;
    }

    fprintf(f, "%s  %-26s ", prefix, lhs);
    if (p->help && strchr(p->help, '\n')) {
        fputc('\n', f);
        fput_prefixed(f, prefix, p->help);
    } else {
        fprintf(f, "%s\n", p->help ? p->help : "");
    }
    fprintf(f, "%s  %-26s (%s)%s\n", prefix, "", rhs,
            (p->flags & PWF_F_RO)  ? "  [read-only]" :
            (p->flags & PWF_F_CLI) ? "  [command line only]" : "");
}

static void usage(struct pwf *s, FILE *f, const char *prefix)
{
    size_t i;

    fprintf(f, "%s%s -- %s\n%s\n", prefix, s->desc->name, s->desc->summary,
            prefix);
    fprintf(f, "%susage: %s [options]\n%s\n", prefix, s->desc->name, prefix);

    fprintf(f, "%sfilter parameters (live, settable on stdin)\n", prefix);
    for (i = 0; s->fp[i].name; i++) print_param_help(f, &s->fp[i], prefix);

    fprintf(f, "%s\n%sstream and node\n", prefix, prefix);
    for (i = 0; i < C_N; i++) print_param_help(f, &s->core[i], prefix);

    fprintf(f,
"%s\n"
"%s  -h, --help                 this text\n"
"%s      --check                validate the settings, print them, and exit\n"
"%s                             without connecting to PipeWire\n"
"%s\n"
"%scontrol protocol\n"
"%s  On start every parameter is printed to stdout as one \"name value\" line.\n"
"%s  Each line read from stdin sets one parameter -- \"name value\", \"name=value\"\n"
"%s  or \"name:value\" -- and the new value is echoed back the same way. A bare\n"
"%s  parameter name queries it; \"dump\" reprints everything, \"help\" prints this,\n"
"%s  \"quit\" exits. Lines starting with # are ignored. Everything the filter\n"
"%s  writes to stdout is either a \"name value\" line or a #-comment; errors go\n"
"%s  to stderr and leave the parameter unchanged.\n",
        prefix, prefix, prefix, prefix, prefix, prefix, prefix, prefix,
        prefix, prefix, prefix, prefix, prefix);

    if (s->desc->notes && *s->desc->notes) {
        fprintf(f, "%s\n", prefix);
        fput_prefixed(f, prefix, s->desc->notes);
    }
    fflush(f);
}

static int parse_argv(struct pwf *s, int argc, char **argv, int *check)
{
    int i;

    for (i = 1; i < argc; i++) {
        char *a = argv[i], *val = NULL, name[PWF_STRMAX];
        pwf_param *p;
        double v = 0;

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(s, stdout, "");
            return 1;
        }
        if (!strcmp(a, "--check")) { *check = 1; continue; }

        if (a[0] == '-' && a[1] == '-') {
            char *eq = strchr(a + 2, '=');
            if (eq) {
                size_t len = (size_t)(eq - (a + 2));
                if (len >= sizeof name) { err("option too long: %s", a); return -1; }
                memcpy(name, a + 2, len);
                name[len] = '\0';
                val = eq + 1;
            } else {
                snprintf(name, sizeof name, "%s", a + 2);
            }
            p = find_param(s, name);
        } else if (a[0] == '-' && a[1] != '\0') {
            p = find_short(s, a);
            snprintf(name, sizeof name, "%s", a);
        } else {
            err("unexpected argument '%s'", a);
            return -1;
        }

        if (!p) { err("unknown option '%s'", a); return -1; }
        if (p->flags & PWF_F_RO) { err("%s is read-only", p->name); return -1; }

        if (!val) {
            if (p->type == PWF_BOOL && (i + 1 >= argc || argv[i + 1][0] == '-')) {
                val = "on";             /* --bypass with no value means on */
            } else if (i + 1 >= argc) {
                err("missing value for %s", a);
                return -1;
            } else {
                val = argv[++i];
            }
        }

        if (parse_value(p, val, &v) < 0) return -1;
        if (apply(s, p, v, val) < 0)     return -1;
    }
    return 0;
}

/* ---- entry point --------------------------------------------------------- */

static void set_defaults(pwf_param *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        p[i].value = p[i].def;
        if (p[i].type == PWF_STRING)
            snprintf(p[i].str, sizeof p[i].str, "%s",
                     p[i].defstr ? p[i].defstr : "");
    }
}

static const char *const chan_names[8] = {
    "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"
};

static int add_ports(struct pwf *s)
{
    uint32_t i;

    for (i = 0; i < s->channels; i++) {
        char nm[32];
        struct pw_properties *props;
        int d;

        for (d = 0; d < 2; d++) {
            snprintf(nm, sizeof nm, "%s_%u", d == 0 ? "in" : "out", i + 1);
            props = pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, nm,
                NULL);
            if (!props) return -ENOMEM;
            if (s->channels == 1)
                pw_properties_set(props, PW_KEY_AUDIO_CHANNEL, "MONO");
            else if (i < 8)
                pw_properties_set(props, PW_KEY_AUDIO_CHANNEL, chan_names[i]);

            {
                struct pwf_port *port = pw_filter_add_port(s->filter,
                        d == 0 ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                        sizeof(struct pwf_port), props, NULL, 0);
                if (!port) {
                    err("cannot add port %s", nm);
                    return -EIO;
                }
                port->s = s;
                port->idx = i;
                if (d == 0) s->in[i] = port; else s->out[i] = port;
            }
        }
    }
    return 0;
}

int pwf_run(pwf_desc *desc, int argc, char **argv)
{
    struct pwf s;
    struct pw_properties *props;
    const struct spa_pod *params[1];
    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    struct spa_process_latency_info lat;
    const char *node_name;
    int check = 0, res, rc = 0;

    /* The protocol is line-oriented and a consumer is normally on the other
     * end of a pipe, where stdio would block-buffer and hold a reply back
     * until 4 KB had piled up. Line buffering is what makes each line arrive
     * when it is written, whether or not the code remembers to flush. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&s, 0, sizeof s);
    s.desc = desc;
    s.fp   = desc->params;
    memcpy(s.core, core_template, sizeof s.core);

    set_defaults(s.core, C_N);
    set_defaults(s.fp, table_len(s.fp));

    s.rate = s.core[C_RATE].def;
    atomic_init(&s.pub, 0);
    atomic_init(&s.rt_rate, 0);
    atomic_init(&s.rt_bypass, 0);
    atomic_init(&s.rt_xfade, 1);
    s.rt_last_pub = UINT32_MAX;
    s.rt_mix = 1.0;
    s.rt_in_db = s.rt_out_db = PWF_DB_FLOOR;
    atomic_init(&s.rt_meter, 0);
    atomic_init(&s.rt_in_level,  f2u((float)PWF_DB_FLOOR));
    atomic_init(&s.rt_out_level, f2u((float)PWF_DB_FLOOR));
    atomic_init(&s.rt_fall, f2u(0.0f));

    if ((res = parse_argv(&s, argc, argv, &check)) != 0)
        return res > 0 ? 0 : 2;

    s.channels = (uint32_t)llround(s.core[C_CHANNELS].value);
    s.rate     = s.core[C_RATE].value;

    s.slots = calloc(PWF_SLOTS, desc->coeff_size);
    s.zero  = calloc(PWF_MAX_QUANTUM, sizeof *s.zero);
    s.sink  = calloc(PWF_MAX_QUANTUM, sizeof *s.sink);
    if (!s.slots || !s.zero || !s.sink) {
        err("out of memory");
        rc = 1;
        goto done;
    }

    if (desc->init &&
        desc->init(desc->user, s.channels, s.core[C_MAX_RATE].value) < 0) {
        err("cannot initialise the filter");
        rc = 1;
        goto done;
    }

    /* Slot 0 has to be valid before the data thread ever runs. */
    design_into(&s, 0);
    atomic_store_explicit(&s.pub, 0, memory_order_release);
    publish_scalars(&s);

    s.started = 1;
    report_all(&s);

    if (check) {
        printf("# ok\n");
        fflush(stdout);
        goto done;
    }

    pw_init(NULL, NULL);

    s.mloop = pw_main_loop_new(NULL);
    if (!s.mloop) {
        err("cannot create the main loop");
        rc = 1;
        goto deinit;
    }
    s.loop = pw_main_loop_get_loop(s.mloop);

    pw_loop_add_signal(s.loop, SIGINT,  on_signal, &s);
    pw_loop_add_signal(s.loop, SIGTERM, on_signal, &s);
    s.rate_ev   = pw_loop_add_event(s.loop, on_rate_event, &s);
    s.meter_timer = pw_loop_add_timer(s.loop, on_meter_timer, &s);
    arm_meter(&s);
    s.stdin_src = pw_loop_add_io(s.loop, STDIN_FILENO, SPA_IO_IN, false,
                                 on_stdin, &s);
    if (!s.stdin_src)
        printf("# warning: stdin cannot be polled, no live control\n");

    node_name = s.core[C_NODE_NAME].str[0] ? s.core[C_NODE_NAME].str
                                           : desc->name;
    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE,     "DSP",
        PW_KEY_NODE_NAME,      node_name,
        PW_KEY_NODE_DESCRIPTION, desc->summary,
        NULL);
    if (!props) {
        err("out of memory");
        rc = 1;
        goto deinit;
    }
    if (s.core[C_TARGET].str[0]) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, s.core[C_TARGET].str);
        pw_properties_set(props, PW_KEY_NODE_AUTOCONNECT, "true");
    }
    if (s.core[C_NODE_LATENCY].str[0])
        pw_properties_set(props, PW_KEY_NODE_LATENCY, s.core[C_NODE_LATENCY].str);

    s.filter = pw_filter_new_simple(s.loop, node_name, props,
                                    &filter_events, &s);
    if (!s.filter) {
        err("cannot create the filter");
        rc = 1;
        goto deinit;
    }
    if ((res = add_ports(&s)) < 0) { rc = 1; goto deinit; }

    spa_zero(lat);
    lat.rate = (int32_t)s.latency;
    lat.ns   = s.rate > 0 ? (int64_t)(s.latency * 1000000000.0 / s.rate) : 0;
    params[0] = spa_process_latency_build(&b, SPA_PARAM_ProcessLatency, &lat);

    if ((res = pw_filter_connect(s.filter, PW_FILTER_FLAG_RT_PROCESS,
                                 params, 1)) < 0) {
        err("cannot connect the filter: %s", spa_strerror(res));
        rc = 1;
        goto deinit;
    }

    pw_main_loop_run(s.mloop);

deinit:
    if (s.filter) pw_filter_destroy(s.filter);
    if (s.mloop)  pw_main_loop_destroy(s.mloop);
    pw_deinit();

done:
    if (desc->fini) desc->fini(desc->user);
    free(s.slots);
    free(s.zero);
    free(s.sink);
    return rc;
}
