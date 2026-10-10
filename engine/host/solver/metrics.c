/* The solver's typed metrics registry (see metrics.h). */
#include "solver/metrics.h"
#include "check.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char  *key;
    MetricKind   kind;
    const char  *unit;
    MetricScope  scope;
    const char  *owner;    /* the component whose code raises the row */
    MetricRepr   repr;
    MetricFamily family;
} MetricRow;

/* The family a declaration file's rows belong to is the file: each include below names it once. */
#define IDENTITY_LE(sub, sup)
#define METRIC(id, key, kind, unit, scope, owner, repr) \
    [METRIC_ID_##id] = { key, METRIC_##kind, unit, METRIC_SCOPE_##scope, owner, METRIC_REPR_##repr, \
                         METRICS_DEF_FAMILY },
static const MetricRow ROWS[METRIC_ROW_N] = {
#define METRICS_DEF_FAMILY METRIC_FAMILY_SCENSUS
#include "solver/metrics/scensus.def"
#undef METRICS_DEF_FAMILY
#define METRICS_DEF_FAMILY METRIC_FAMILY_LAYOUT
#include "solver/metrics/layout.def"
#undef METRICS_DEF_FAMILY
#define METRICS_DEF_FAMILY METRIC_FAMILY_RUNG_ENTRY
#include "solver/metrics/rung_entry.def"
#undef METRICS_DEF_FAMILY
#define METRICS_DEF_FAMILY METRIC_FAMILY_FETCH_EDGE
#include "solver/metrics/fetch_edge.def"
#undef METRICS_DEF_FAMILY
#define METRICS_DEF_FAMILY METRIC_FAMILY_XHR_EDGE
#include "solver/metrics/xhr_edge.def"
#undef METRICS_DEF_FAMILY
#define METRICS_DEF_FAMILY METRIC_FAMILY_WFQ
#include "solver/metrics/wfq.def"
#undef METRICS_DEF_FAMILY
#define METRICS_DEF_FAMILY METRIC_FAMILY_WFQ_FRONTIER
#include "solver/metrics/wfq_frontier.def"
#undef METRICS_DEF_FAMILY
#define METRICS_DEF_FAMILY METRIC_FAMILY_COLD
#include "solver/metrics/cold.def"
#undef METRICS_DEF_FAMILY
};
#undef METRIC
#undef IDENTITY_LE

typedef struct { MetricId sub, sup; } MetricIdentity;   /* value(sub) <= value(sup) */
#define METRIC(id, key, kind, unit, scope, owner, repr)
#define IDENTITY_LE(sub, sup) { METRIC_ID_##sub, METRIC_ID_##sup },
static const MetricIdentity IDENTITIES[] = {
#include "solver/metrics/scensus.def"
#include "solver/metrics/layout.def"
#include "solver/metrics/rung_entry.def"
#include "solver/metrics/fetch_edge.def"
#include "solver/metrics/xhr_edge.def"
#include "solver/metrics/wfq.def"
#include "solver/metrics/wfq_frontier.def"
#include "solver/metrics/cold.def"
};
#undef IDENTITY_LE
#undef METRIC
enum { METRIC_IDENTITY_N = (int)(sizeof IDENTITIES / sizeof IDENTITIES[0]) };

/* What a sampled family's composer decided for a row since the family's last composition. */
enum { METRIC_UNDECIDED = 0, METRIC_DECIDED_SET, METRIC_DECIDED_ABSENT };

/* The agent's block. `value` holds integer rows, `real` real rows and `object` the borrowed text of object
   rows; `composed` and `composed_real` are each row as its family's last composition read it, which is what a
   lifetime or maximum row is checked against; `decided` is a sampled row's decision for the next composition;
   `live` is set between init and release. All zero is the pre-agent state. */
typedef struct {
    long long     value[METRIC_ROW_N];
    double        real[METRIC_ROW_N];
    const char   *object[METRIC_ROW_N];
    long long     composed[METRIC_ROW_N];
    double        composed_real[METRIC_ROW_N];
    unsigned char decided[METRIC_ROW_N];
    int           live;
} MetricsAgent;
static MetricsAgent g_agent;

static int row_present(MetricId id)
{
    return !METRIC_FAMILY_SAMPLED(ROWS[id].family) || g_agent.decided[id] == METRIC_DECIDED_SET;
}

static void put_row(JsonBuf *b, MetricId id)
{
    char t[64];
    int n = 0;

    switch (ROWS[id].repr) {
    case METRIC_REPR_INT:    n = snprintf(t, sizeof t, "%lld", g_agent.value[id]); break;
    case METRIC_REPR_FIXED1: n = snprintf(t, sizeof t, "%.1f", g_agent.real[id]); break;
    case METRIC_REPR_FIXED3: n = snprintf(t, sizeof t, "%.3f", g_agent.real[id]); break;
    case METRIC_REPR_OBJECT: json_buf_raw(b, g_agent.object[id]); return;
    }
    DCHECKF(n > 0 && (size_t)n < sizeof t, "the value of `%s` did not fit its conversion buffer", ROWS[id].key);
    json_buf_raw(b, t);
}

/* One writer per family, expanded from its declaration so every key reaches json_buf_key as a literal. */
#define IDENTITY_LE(sub, sup)
#define METRIC(id, key, kind, unit, scope, owner, repr) \
    if (row_present(METRIC_ID_##id)) { \
        json_buf_raw(b, sep); json_buf_key(b, key); put_row(b, METRIC_ID_##id); sep = ","; \
    }
static void scensus_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/scensus.def"
}
static void layout_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/layout.def"
}
static void rung_entry_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/rung_entry.def"
}
static void fetch_edge_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/fetch_edge.def"
}
static void xhr_edge_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/xhr_edge.def"
}
static void wfq_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/wfq.def"
}
static void wfq_frontier_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/wfq_frontier.def"
}
static void cold_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/cold.def"
}
#undef METRIC
#undef IDENTITY_LE

/* `census` is the census object a family's rows are composed into, NULL for the result document's own top
   level, where extension/bridge.js asserts every schema row present and numeric. */
static const struct {
    const char *name;
    void (*members)(JsonBuf *b);
    const char *census;
} FAMILIES[METRIC_FAMILY_N] = {
    [METRIC_FAMILY_SCENSUS]      = { "scensus",      scensus_members,      NULL },
    [METRIC_FAMILY_LAYOUT]       = { "layout",       layout_members,       NULL },
    [METRIC_FAMILY_RUNG_ENTRY]   = { "rung_entry",   rung_entry_members,   "_cold" },
    [METRIC_FAMILY_FETCH_EDGE]   = { "fetch_edge",   fetch_edge_members,   "_cold" },
    [METRIC_FAMILY_XHR_EDGE]     = { "xhr_edge",     xhr_edge_members,     "_cold" },
    [METRIC_FAMILY_WFQ]          = { "wfq",          wfq_members,          "_wfq" },
    [METRIC_FAMILY_WFQ_FRONTIER] = { "wfq_frontier", wfq_frontier_members, "_wfq" },
    [METRIC_FAMILY_COLD]         = { "cold",         cold_members,         "_cold" },
};

static const char *kind_name(MetricKind k)
{
    switch (k) {
    case METRIC_LIFETIME: return "lifetime";
    case METRIC_GAUGE:    return "gauge";
    case METRIC_MAXIMUM:  return "maximum";
    case METRIC_CONSTANT: return "constant";
    case METRIC_REGIME:   return "regime";
    }
    DFAIL("a metric row carries a kind outside MetricKind — the table is built from the declaration files' KIND "
          "tokens, each of which names an enumerator, so this value was written past the table");
    return "?";
}

static const char *scope_name(MetricScope s)
{
    switch (s) {
    case METRIC_SCOPE_AGENT:   return "agent";
    case METRIC_SCOPE_PROCESS: return "process";
    }
    DFAIL("a metric row carries a scope outside MetricScope — the table is built from the declaration files' "
          "SCOPE tokens, each of which names an enumerator, so this value was written past the table");
    return "?";
}

void metrics_agent_init(void)
{
    static const MetricsAgent zero;
    DCHECK(!memcmp(&g_agent, &zero, sizeof zero),
           "the metrics block was not in its pre-agent state at an agent's bring-up — either a second "
           "metrics_agent_init ran within one agent, or the previous agent's solver_agent_free did not reach "
           "metrics_agent_release, and this agent's census would start from the previous one's totals");
    for (int i = 0; i < METRIC_ROW_N; i++)
        DCHECKF(ROWS[i].key && ROWS[i].unit && *ROWS[i].unit && ROWS[i].owner && *ROWS[i].owner &&
                    ROWS[i].family >= 0 && ROWS[i].family < METRIC_FAMILY_N,
                "metric row %d is incomplete — every METRIC(...) declares its key, unit and owner, and its "
                "declaration file names its family where metrics.c includes it", i);
    for (int i = 0; i < METRIC_IDENTITY_N; i++)
        DCHECKF(ROWS[IDENTITIES[i].sub].family == ROWS[IDENTITIES[i].sup].family &&
                    ROWS[IDENTITIES[i].sub].repr == METRIC_REPR_INT &&
                    ROWS[IDENTITIES[i].sup].repr == METRIC_REPR_INT,
                "the identity %s <= %s relates rows of two families or a row that is not an integer — it is "
                "asserted over integer values when one family is composed",
                ROWS[IDENTITIES[i].sub].key, ROWS[IDENTITIES[i].sup].key);
    g_agent.live = 1;
}

void metrics_agent_release(void)
{
    DCHECK(g_agent.live, "the metrics block was released without an agent's bring-up — concolic_init calls "
                         "metrics_agent_init on every host that runs solver_agent_free");
    memset(&g_agent, 0, sizeof g_agent);
}

void metrics_add_at(MetricId id, long long n, const char *file, int line)
{
    DCHECKF(id >= 0 && id < METRIC_ROW_N, "%s:%d raised metric id %d, which no declaration file declares",
            file, line, (int)id);
    DCHECKF(g_agent.live, "%s:%d raised `%s` outside an agent's life — before concolic_init or after "
            "solver_agent_free — so the count belongs to no agent's census. An agent built without concolic_init "
            "still mints attacker sources while the process-wide source overlay stands; the route is an "
            "overlay declared per agent, so such an agent declares itself browser-only", file, line,
            ROWS[id].key);
    DCHECKF(ROWS[id].kind == METRIC_LIFETIME && !METRIC_FAMILY_SAMPLED(ROWS[id].family),
            "%s:%d added to `%s`, which is declared %s in a %s family; only a raised family's lifetime row "
            "accumulates", file, line, ROWS[id].key, kind_name(ROWS[id].kind),
            METRIC_FAMILY_SAMPLED(ROWS[id].family) ? "sampled" : "raised");
    DCHECKF(n >= 0 && g_agent.value[id] <= LLONG_MAX - n, "%s:%d added %lld to `%s` at %lld — a lifetime row "
            "is raised by a non-negative count and cannot wrap", file, line, n, ROWS[id].key,
            g_agent.value[id]);
    g_agent.value[id] += n;
}

/* A sampled row's decision, checked against its declaration: the row is a sampled family's, it is written in
   the representation it was declared with, and it was not already decided for this composition. */
static void decide(MetricId id, MetricRepr repr, int absent, const char *file, int line)
{
    DCHECKF(id >= 0 && id < METRIC_ROW_N, "%s:%d set metric id %d, which no declaration file declares",
            file, line, (int)id);
    DCHECKF(METRIC_FAMILY_SAMPLED(ROWS[id].family) && (absent || ROWS[id].repr == repr),
            "%s:%d set `%s`, which its declaration file does not declare as a sampled %s row", file, line,
            ROWS[id].key, absent ? "" : (repr == METRIC_REPR_OBJECT ? "object" : "numeric"));
    DCHECKF(g_agent.decided[id] == METRIC_UNDECIDED, "%s:%d decided `%s` a second time before its family's "
            "composition — one composition reads one value per row, so the first decision would be lost",
            file, line, ROWS[id].key);
    g_agent.decided[id] = absent ? METRIC_DECIDED_ABSENT : METRIC_DECIDED_SET;
}

void metrics_set_at(MetricId id, long long v, const char *file, int line)
{
    decide(id, METRIC_REPR_INT, 0, file, line);
    g_agent.value[id] = v;
}

void metrics_set_real_at(MetricId id, double v, const char *file, int line)
{
    decide(id, ROWS[id].repr == METRIC_REPR_FIXED3 ? METRIC_REPR_FIXED3 : METRIC_REPR_FIXED1, 0, file, line);
    g_agent.real[id] = v;
}

void metrics_set_object_at(MetricId id, const char *text, const char *file, int line)
{
    decide(id, METRIC_REPR_OBJECT, 0, file, line);
    DCHECKF(text && text[0] == '{', "%s:%d set the object row `%s` to text that does not open an object",
            file, line, ROWS[id].key);
    g_agent.object[id] = text;
}

void metrics_absent_at(MetricId id, const char *file, int line)
{
    decide(id, ROWS[id].repr, 1, file, line);
}

/* The composition-time half of the contract: a sampled family's every row was decided, every identity of the
   family holds over its present rows, and no present lifetime or maximum row fell since the family was last
   composed. Then the family's present rows are recorded as composed. */
static void compose_check(MetricFamily family)
{
    DCHECKF(family >= 0 && family < METRIC_FAMILY_N, "a metrics family %d was composed that metrics.h does "
            "not declare", (int)family);
    DCHECK(g_agent.live, "a metrics family was composed outside an agent's life — its rows were zeroed at "
                         "solver_agent_free or never brought up, so the document would report another agent");
    for (int i = 0; i < METRIC_ROW_N; i++)
        DCHECKF(ROWS[i].family != family || !METRIC_FAMILY_SAMPLED(family) ||
                    g_agent.decided[i] != METRIC_UNDECIDED,
                "the %s census was composed with `%s` neither set nor stated absent — its composer decides every "
                "row its declaration file declares, so a declared row it no longer emits is a row nobody reads",
                FAMILIES[family].name, ROWS[i].key);
    for (int i = 0; i < METRIC_IDENTITY_N; i++) {
        const MetricIdentity *e = &IDENTITIES[i];
        if (ROWS[e->sub].family != family || !row_present(e->sub) || !row_present(e->sup)) continue;
        DCHECKF(g_agent.value[e->sub] <= g_agent.value[e->sup],
                "the %s census reports %s = %lld above %s = %lld — the declaration states the first counts a "
                "subset of what the second counts, so a raise of one went missing at its owner (%s / %s)",
                FAMILIES[family].name, ROWS[e->sub].key, g_agent.value[e->sub], ROWS[e->sup].key,
                g_agent.value[e->sup], ROWS[e->sub].owner, ROWS[e->sup].owner);
    }
    for (int i = 0; i < METRIC_ROW_N; i++) {
        int monotone;
        if (ROWS[i].family != family || !row_present(i)) continue;
        monotone = ROWS[i].kind == METRIC_LIFETIME || ROWS[i].kind == METRIC_MAXIMUM;
        if (ROWS[i].repr == METRIC_REPR_INT) {
            DCHECKF(!monotone || g_agent.value[i] >= g_agent.composed[i],
                    "the %s row `%s` fell from %lld to %lld between two compositions of one agent — a %s row "
                    "is only raised, and only its scope's end lowers it", FAMILIES[family].name, ROWS[i].key,
                    g_agent.composed[i], g_agent.value[i], kind_name(ROWS[i].kind));
            g_agent.composed[i] = g_agent.value[i];
        } else if (ROWS[i].repr != METRIC_REPR_OBJECT) {
            DCHECKF(!monotone || g_agent.real[i] >= g_agent.composed_real[i],
                    "the %s row `%s` fell from %f to %f between two compositions of one agent — a %s row is "
                    "only raised, and only its scope's end lowers it", FAMILIES[family].name, ROWS[i].key,
                    g_agent.composed_real[i], g_agent.real[i], kind_name(ROWS[i].kind));
            g_agent.composed_real[i] = g_agent.real[i];
        }
    }
}

/* A sampled family's decisions last one composition; its object texts were borrowed for it. */
static void compose_done(MetricFamily family)
{
    if (!METRIC_FAMILY_SAMPLED(family)) return;
    for (int i = 0; i < METRIC_ROW_N; i++) {
        if (ROWS[i].family != family) continue;
        g_agent.decided[i] = METRIC_UNDECIDED;
        g_agent.object[i] = NULL;
    }
}

void metrics_family_members_json(MetricFamily family, JsonBuf *b)
{
    compose_check(family);
    FAMILIES[family].members(b);
    compose_done(family);
}

void metrics_family_json(MetricFamily family, JsonBuf *b)
{
    json_buf_raw(b, "{");
    metrics_family_members_json(family, b);
    json_buf_raw(b, "}");
}

void metrics_schema_json(JsonBuf *b)
{
    json_buf_raw(b, "{");
    json_buf_key(b, "families");
    json_buf_raw(b, "[");
    const char *fsep = "";
    for (int f = 0; f < METRIC_FAMILY_N; f++) {
        const char *rsep = "", *isep = "";
        if (FAMILIES[f].census) continue;
        json_buf_raw(b, fsep);
        fsep = ",";
        json_buf_raw(b, "{");
        json_buf_key(b, "family");
        json_buf_str(b, FAMILIES[f].name);
        json_buf_raw(b, ",");
        json_buf_key(b, "rows");
        json_buf_raw(b, "[");
        for (int i = 0; i < METRIC_ROW_N; i++) {
            if (ROWS[i].family != (MetricFamily)f) continue;
            json_buf_raw(b, rsep);
            rsep = ",";
            json_buf_raw(b, "{");
            json_buf_key(b, "key");
            json_buf_str(b, ROWS[i].key);
            json_buf_raw(b, ",");
            json_buf_key(b, "kind");
            json_buf_str(b, kind_name(ROWS[i].kind));
            json_buf_raw(b, ",");
            json_buf_key(b, "unit");
            json_buf_str(b, ROWS[i].unit);
            json_buf_raw(b, ",");
            json_buf_key(b, "scope");
            json_buf_str(b, scope_name(ROWS[i].scope));
            json_buf_raw(b, ",");
            json_buf_key(b, "owner");
            json_buf_str(b, ROWS[i].owner);
            json_buf_raw(b, "}");
        }
        json_buf_raw(b, "],");
        json_buf_key(b, "identities");
        json_buf_raw(b, "[");
        for (int i = 0; i < METRIC_IDENTITY_N; i++) {
            if (ROWS[IDENTITIES[i].sub].family != (MetricFamily)f) continue;
            json_buf_raw(b, isep);
            isep = ",";
            json_buf_raw(b, "{");
            json_buf_key(b, "rel");
            json_buf_str(b, "le");
            json_buf_raw(b, ",");
            json_buf_key(b, "sub");
            json_buf_str(b, ROWS[IDENTITIES[i].sub].key);
            json_buf_raw(b, ",");
            json_buf_key(b, "sup");
            json_buf_str(b, ROWS[IDENTITIES[i].sup].key);
            json_buf_raw(b, "}");
        }
        json_buf_raw(b, "]}");
    }
    json_buf_raw(b, "]}");
}

char *metrics_family_members_text(MetricFamily family)
{
    JsonBuf b = { 0 };
    metrics_family_members_json(family, &b);
    return json_buf_take(&b);
}

char *metrics_family_text(MetricFamily family)
{
    JsonBuf b = { 0 };
    metrics_family_json(family, &b);
    return json_buf_take(&b);
}

char *metrics_schema_text(void)
{
    JsonBuf b = { 0 };
    metrics_schema_json(&b);
    return json_buf_take(&b);
}
