/* @S structured-source DELIVERY ENVELOPE — the ONE addressing model for a destructured attacker source.
 *
 * A scalar source ({hash}/{search}) read at a FIELD PATH is only reachable if the string was destructured
 * first (JSON.parse(s).html, s.split('|')[2], URLSearchParams.get('data')), so the breakout must ride the
 * envelope that destructuring yields AND satisfy every sibling gate the handler checks on the same object.
 * ONE descriptor (EnvKind + the sink field-path/param-key/split-index) drives ONE addressing function
 * (env_sibling_addr), so a new destructuring API is a new CASE, never a fourth bespoke reconstructor.
 *
 * NO scheduler coupling — pure construction from the sink's opaque value + the per-flow constraint set
 * (constraints.h). Isolation-testable: given a descriptor + constraints, produce the envelope string. The
 * host-edge / solver calls envelope_detect once per sink, then envelope_build per candidate. See envelope.c. */
#ifndef ENGINE_HOST_SOLVER_ENVELOPE_H
#define ENGINE_HOST_SOLVER_ENVELOPE_H
#include "quickjs.h"

/* Detect the structured-source descriptor from the sink's opaque VALUE (its JSON field-path, query-param key,
   or split delimiter/index). Sets the module descriptor + the constraints-owned sink jkey/root. Call per sink. */
void envelope_detect(JSContext *ctx, JSValueConst val);

/* Wrap `cand` in the detected envelope (JSON object / delimited parts / query string) with every sibling gate
   token placed at its address. Returns malloc'd string, or NULL when no envelope applies (caller uses raw cand). */
char *envelope_build(JSContext *ctx, const char *cand);

/* Is `tok` a sibling gate token an envelope address already places? (emit_cand's minimality skip, so a token
   the envelope structurally covers is not also redundantly prefixed onto the payload). */
int envelope_handles_token(const char *tok);

#endif
