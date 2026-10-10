/* The solver's typed metrics registry. A family is one declaration file under solver/metrics/, included here
 * and in metrics.c, whose rows are
 *     METRIC(id, "key", KIND, "unit", SCOPE, "owner", REPR)
 * and whose identities, IDENTITY_LE(sub, sup), are relations between its rows that hold by construction. A row's
 * value lives in one per-agent block (metrics.c). A raised family's rows are raised by their owners through the
 * macro their kind admits; a sampled family's rows are set by its composer at each composition. A family
 * becomes bytes only through metrics_family_json, which asserts the identities and the row set at composition.
 * The schema (metrics_schema_json) states every row's kind, unit, scope and owner, and JS readers take a
 * family's row set and kinds from it or from the declaration files, never from a list of their own. */
#ifndef ENGINE_HOST_SOLVER_METRICS_H
#define ENGINE_HOST_SOLVER_METRICS_H
#include "core/json_buf.h"

/* What a reader may do with a row's number: a lifetime count may be differenced across two compositions of one
   agent, a gauge is a level that may fall, a maximum is a high-water mark, a constant is written once, and a
   regime is a policy the rows beside it are read under. */
typedef enum { METRIC_LIFETIME, METRIC_GAUGE, METRIC_MAXIMUM, METRIC_CONSTANT, METRIC_REGIME } MetricKind;
/* Whose life a row's value spans. AGENT values end with the agent (a raised row is zeroed by
   metrics_agent_release, a sampled row's owner resets it in solver_agent_free); PROCESS values are never reset. */
typedef enum { METRIC_SCOPE_AGENT, METRIC_SCOPE_PROCESS } MetricScope;
/* How a row's value is written: a decimal integer, a real at one or three places, or a JSON object its
   composer wrote. */
typedef enum { METRIC_REPR_INT, METRIC_REPR_FIXED1, METRIC_REPR_FIXED3, METRIC_REPR_OBJECT } MetricRepr;
/* One member per declaration file, whose name is the member's suffix in lower case. Raised families first. */
typedef enum {
    METRIC_FAMILY_SCENSUS, METRIC_FAMILY_LAYOUT,
    METRIC_FAMILY_RUNG_ENTRY, METRIC_FAMILY_FETCH_EDGE, METRIC_FAMILY_XHR_EDGE,
    METRIC_FAMILY_WFQ, METRIC_FAMILY_WFQ_FRONTIER,
    METRIC_FAMILY_N
} MetricFamily;
#define METRIC_FAMILY_SAMPLED(f) ((f) >= METRIC_FAMILY_RUNG_ENTRY)

#define IDENTITY_LE(sub, sup)
#define METRIC(id, key, kind, unit, scope, owner, repr) METRIC_ID_##id,
typedef enum {
#include "solver/metrics/scensus.def"
#include "solver/metrics/layout.def"
#include "solver/metrics/rung_entry.def"
#include "solver/metrics/fetch_edge.def"
#include "solver/metrics/xhr_edge.def"
#include "solver/metrics/wfq.def"
#include "solver/metrics/wfq_frontier.def"
    METRIC_ROW_N
} MetricId;
#undef METRIC
/* Each row's kind, representation and family as constant expressions, so a raise or a set is checked against
   them at compile time. */
#define METRIC(id, key, kind, unit, scope, owner, repr) \
    METRIC_KIND_OF_##id = METRIC_##kind, METRIC_REPR_OF_##id = METRIC_REPR_##repr, \
    METRIC_FAMILY_OF_##id = METRICS_DEF_FAMILY,
enum {
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
};
#undef METRIC
#undef IDENTITY_LE

/* Adds n (>= 0) to a lifetime row of a raised family. Raising any other row does not compile. */
#define METRIC_ADD(id, n) do { \
    _Static_assert(METRIC_KIND_OF_##id == METRIC_LIFETIME, "METRIC_ADD raises a lifetime row; " #id " is not one"); \
    _Static_assert(!METRIC_FAMILY_SAMPLED(METRIC_FAMILY_OF_##id), \
                   "METRIC_ADD raises a raised family's row; " #id "'s family is set by its composer"); \
    metrics_add_at(METRIC_ID_##id, (n), __FILE__, __LINE__); \
  } while (0)
void metrics_add_at(MetricId id, long long n, const char *file, int line);   /* METRIC_ADD's implementation */

/* A sampled family's composer decides every row of the family before each composition: it sets the row's
   value (METRIC_SET for an integer row, METRIC_SET_REAL for a real one, METRIC_SET_OBJECT for an object, whose
   text is borrowed until that composition), or states the row absent from it (METRIC_ABSENT). Composition
   asserts every row was decided and then forgets the decisions. */
#define METRIC_SAMPLE_CHECK_(id, repr) \
    _Static_assert(METRIC_FAMILY_SAMPLED(METRIC_FAMILY_OF_##id), #id "'s family is raised, not set"); \
    _Static_assert(METRIC_REPR_OF_##id == (repr), #id " is declared with another representation")
#define METRIC_SET(id, v) do { METRIC_SAMPLE_CHECK_(id, METRIC_REPR_INT); \
    metrics_set_at(METRIC_ID_##id, (long long)(v), __FILE__, __LINE__); } while (0)
#define METRIC_SET_REAL(id, v) do { \
    _Static_assert(METRIC_FAMILY_SAMPLED(METRIC_FAMILY_OF_##id), #id "'s family is raised, not set"); \
    _Static_assert(METRIC_REPR_OF_##id == METRIC_REPR_FIXED1 || METRIC_REPR_OF_##id == METRIC_REPR_FIXED3, \
                   #id " is not declared real"); \
    metrics_set_real_at(METRIC_ID_##id, (double)(v), __FILE__, __LINE__); } while (0)
#define METRIC_SET_OBJECT(id, text) do { METRIC_SAMPLE_CHECK_(id, METRIC_REPR_OBJECT); \
    metrics_set_object_at(METRIC_ID_##id, (text), __FILE__, __LINE__); } while (0)
#define METRIC_ABSENT(id) do { \
    _Static_assert(METRIC_FAMILY_SAMPLED(METRIC_FAMILY_OF_##id), #id "'s family is raised, not set"); \
    metrics_absent_at(METRIC_ID_##id, __FILE__, __LINE__); } while (0)
void metrics_set_at(MetricId id, long long v, const char *file, int line);
void metrics_set_real_at(MetricId id, double v, const char *file, int line);
void metrics_set_object_at(MetricId id, const char *text, const char *file, int line);
void metrics_absent_at(MetricId id, const char *file, int line);

/* The agent's block. Init is called once per agent from concolic_init, the first solver bring-up every host
   runs, and asserts the block is in its pre-agent state; release is called from solver_agent_free and zeroes
   it, so a second agent in one process starts from zero. */
void metrics_agent_init(void);
void metrics_agent_release(void);

/* A family's present rows as JSON members, `"key":value` joined by commas with no braces, for a composer that
   splices them into an object of its own. Asserts the family's identities, that no lifetime or maximum row
   fell since its last composition, and, for a sampled family, that every row was decided. */
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
