/* The solver's typed metrics registry. A family is one declaration file under solver/metrics/, included here
 * and in metrics.c, whose rows are
 *     METRIC(id, "key", KIND, "unit", SCOPE, "owner")
 * and whose identities, IDENTITY_LE(sub, sup), are relations between its rows that hold by construction. A row's
 * value lives in one per-agent block (metrics.c) and is raised only through the macro its kind admits; a family
 * becomes bytes only through metrics_family_json, which asserts the identities at composition. The schema
 * (metrics_schema_json) states every row's kind, unit, scope and owner, and JS readers take a family's row set
 * and kinds from it rather than from a list of their own. */
#ifndef ENGINE_HOST_SOLVER_METRICS_H
#define ENGINE_HOST_SOLVER_METRICS_H
#include "core/json_buf.h"

/* What a reader may do with a row's number: a lifetime count may be differenced across two compositions of one
   agent, a gauge is a level that may fall, a maximum is a high-water mark, a constant is written once, and a
   regime is a policy the rows beside it are read under. */
typedef enum { METRIC_LIFETIME, METRIC_GAUGE, METRIC_MAXIMUM, METRIC_CONSTANT, METRIC_REGIME } MetricKind;
/* Whose life a row's value spans: AGENT rows are zeroed with the agent (metrics_agent_release). */
typedef enum { METRIC_SCOPE_AGENT } MetricScope;
/* One member per declaration file. */
typedef enum { METRIC_FAMILY_SCENSUS, METRIC_FAMILY_N } MetricFamily;

#define IDENTITY_LE(sub, sup)
#define METRIC(id, key, kind, unit, scope, owner) METRIC_ID_##id,
typedef enum {
#include "solver/metrics/scensus.def"
    METRIC_ROW_N
} MetricId;
#undef METRIC
/* Each row's kind as a constant expression, so a raise can be checked against it at compile time. */
#define METRIC(id, key, kind, unit, scope, owner) METRIC_KIND_OF_##id = METRIC_##kind,
enum {
#include "solver/metrics/scensus.def"
};
#undef METRIC
#undef IDENTITY_LE

/* Adds n (>= 0) to a lifetime row. Raising a row of another kind does not compile. */
#define METRIC_ADD(id, n) do { \
    _Static_assert(METRIC_KIND_OF_##id == METRIC_LIFETIME, "METRIC_ADD raises a lifetime row; " #id " is not one"); \
    metrics_add_at(METRIC_ID_##id, (n), __FILE__, __LINE__); \
  } while (0)
void metrics_add_at(MetricId id, long long n, const char *file, int line);   /* METRIC_ADD's implementation */

/* The agent's block. Init is called once per agent from concolic_init, the first solver bring-up every host
   runs, and asserts the block is in its pre-agent state; release is called from solver_agent_free and zeroes
   it, so a second agent in one process starts from zero. */
void metrics_agent_init(void);
void metrics_agent_release(void);

/* A family's rows as JSON members, `"key":value` joined by commas with no braces, for a composer that splices
   them into an object of its own. Asserts the family's identities and that no lifetime or maximum row fell
   since the family's last composition. */
void metrics_family_members_json(MetricFamily family, JsonBuf *b);
/* The same members as one object. */
void metrics_family_json(MetricFamily family, JsonBuf *b);
/* Every family's declaration as one object: {"families":[{"family","rows":[{"key","kind","unit","scope",
   "owner"}],"identities":[{"rel","sub","sup"}]}]}. A constant of the build. */
void metrics_schema_json(JsonBuf *b);
/* The same three as text the caller owns and frees, for a composer that splices with composef or prints. */
char *metrics_family_members_text(MetricFamily family);
char *metrics_family_text(MetricFamily family);
char *metrics_schema_text(void);

#endif
