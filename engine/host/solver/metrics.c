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
    MetricFamily family;
} MetricRow;

/* The family a declaration file's rows belong to is the file: each include below names it once. */
#define IDENTITY_LE(sub, sup)
#define METRIC(id, key, kind, unit, scope, owner) \
    [METRIC_ID_##id] = { key, METRIC_##kind, unit, METRIC_SCOPE_##scope, owner, METRICS_DEF_FAMILY },
static const MetricRow ROWS[METRIC_ROW_N] = {
#define METRICS_DEF_FAMILY METRIC_FAMILY_SCENSUS
#include "solver/metrics/scensus.def"
#undef METRICS_DEF_FAMILY
};
#undef METRIC
#undef IDENTITY_LE

typedef struct { MetricId sub, sup; } MetricIdentity;   /* value(sub) <= value(sup) */
#define METRIC(id, key, kind, unit, scope, owner)
#define IDENTITY_LE(sub, sup) { METRIC_ID_##sub, METRIC_ID_##sup },
static const MetricIdentity IDENTITIES[] = {
#include "solver/metrics/scensus.def"
};
#undef IDENTITY_LE
#undef METRIC
enum { METRIC_IDENTITY_N = (int)(sizeof IDENTITIES / sizeof IDENTITIES[0]) };

/* The agent's block. `composed` is each row as its family's last composition read it, which is what a lifetime
   row is checked against; `live` is set between init and release. All zero is the pre-agent state. */
typedef struct {
    long long value[METRIC_ROW_N];
    long long composed[METRIC_ROW_N];
    int       live;
} MetricsAgent;
static MetricsAgent g_agent;

static void put_value(JsonBuf *b, long long v)
{
    char t[24];
    snprintf(t, sizeof t, "%lld", v);
    json_buf_raw(b, t);
}

/* One writer per family, expanded from its declaration so every key reaches json_buf_key as a literal. */
#define IDENTITY_LE(sub, sup)
#define METRIC(id, key, kind, unit, scope, owner) \
    json_buf_raw(b, sep); json_buf_key(b, key); put_value(b, g_agent.value[METRIC_ID_##id]); sep = ",";
static void scensus_members(JsonBuf *b)
{
    const char *sep = "";
#include "solver/metrics/scensus.def"
}
#undef METRIC
#undef IDENTITY_LE

static const struct {
    const char *name;
    void (*members)(JsonBuf *b);
} FAMILIES[METRIC_FAMILY_N] = {
    [METRIC_FAMILY_SCENSUS] = { "scensus", scensus_members },
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
    case METRIC_SCOPE_AGENT: return "agent";
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
        DCHECKF(ROWS[IDENTITIES[i].sub].family == ROWS[IDENTITIES[i].sup].family,
                "the identity %s <= %s relates rows of two families — it is asserted when one family is "
                "composed, and the other operand may be read at a different instant",
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
    DCHECKF(ROWS[id].kind == METRIC_LIFETIME, "%s:%d added to `%s`, which is declared %s; only a lifetime row "
            "accumulates", file, line, ROWS[id].key, kind_name(ROWS[id].kind));
    DCHECKF(n >= 0 && g_agent.value[id] <= LLONG_MAX - n, "%s:%d added %lld to `%s` at %lld — a lifetime row "
            "is raised by a non-negative count and cannot wrap", file, line, n, ROWS[id].key,
            g_agent.value[id]);
    g_agent.value[id] += n;
}

/* The composition-time half of the contract: every identity of the family holds and no lifetime or maximum row
   fell since the family was last composed. Then the family's rows are recorded as composed. */
static void compose_check(MetricFamily family)
{
    DCHECKF(family >= 0 && family < METRIC_FAMILY_N, "a metrics family %d was composed that metrics.h does "
            "not declare", (int)family);
    DCHECK(g_agent.live, "a metrics family was composed outside an agent's life — its rows were zeroed at "
                         "solver_agent_free or never brought up, so the document would report another agent");
    for (int i = 0; i < METRIC_IDENTITY_N; i++) {
        const MetricIdentity *e = &IDENTITIES[i];
        if (ROWS[e->sub].family != family) continue;
        DCHECKF(g_agent.value[e->sub] <= g_agent.value[e->sup],
                "the %s census reports %s = %lld above %s = %lld — the declaration states the first counts a "
                "subset of what the second counts, so a raise of one went missing at its owner (%s / %s)",
                FAMILIES[family].name, ROWS[e->sub].key, g_agent.value[e->sub], ROWS[e->sup].key,
                g_agent.value[e->sup], ROWS[e->sub].owner, ROWS[e->sup].owner);
    }
    for (int i = 0; i < METRIC_ROW_N; i++) {
        if (ROWS[i].family != family) continue;
        DCHECKF(!(ROWS[i].kind == METRIC_LIFETIME || ROWS[i].kind == METRIC_MAXIMUM) ||
                    g_agent.value[i] >= g_agent.composed[i],
                "the %s row `%s` fell from %lld to %lld between two compositions of one agent — a %s row is "
                "only raised, and only metrics_agent_release lowers it", FAMILIES[family].name, ROWS[i].key,
                g_agent.composed[i], g_agent.value[i], kind_name(ROWS[i].kind));
        g_agent.composed[i] = g_agent.value[i];
    }
}

void metrics_family_members_json(MetricFamily family, JsonBuf *b)
{
    compose_check(family);
    FAMILIES[family].members(b);
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
    for (int f = 0; f < METRIC_FAMILY_N; f++) {
        const char *rsep = "", *isep = "";
        if (f) json_buf_raw(b, ",");
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
