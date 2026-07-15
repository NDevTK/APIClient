/* The CONCOLIC VALUE — the atom of the forced-execution solver (rebuilt clean on upstream quickjs).
 *
 * A concolic value is NOT a binary "opaque" sentinel. It is a triple:
 *   - SOURCE identity   : where the value came from (an attacker source "{hash}", a reply field, injected state)
 *   - CONSTRAINT domain : what the value can be, narrowed by every predicate the flow took (accumulated elsewhere)
 *   - EXAMPLE           : a concrete value when the code pins/computes/learns one (else absent)
 *
 * It is a real JSObject of a host-registered class (JS_NewClassID/JS_NewClass — upstream public API, so the qjs
 * fork carries NO value-type delta). The interpreter learns to BRANCH and PROPAGATE on it through a minimal set
 * of hooks, keeping the fork a thin edge. Every semantic — provenance, example propagation, display shape —
 * lives here in the host: the SOLVER owns its value type as a C component, not a fork mutation. */
#ifndef ENGINE_HOST_SOLVER_CONCOLIC_H
#define ENGINE_HOST_SOLVER_CONCOLIC_H

#include "quickjs.h"

/* Register the Concolic class in ctx's runtime (once, at engine init). */
void concolic_init(JSContext *ctx);
void concolic_free(JSContext *ctx);

/* Mint a concolic value. `shape` is the @H/@S display form ("{hash}", "/api/{region}"); `src` is the source
   identity used to correlate constraints across a flow (may equal shape); `example` (consumed) is the concrete
   example or JS_UNDEFINED when none is known yet. Returns a new owned JSValue. */
JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, JSValue example);

/* Predicates + accessors — the ONLY concolic test is this domain-carrying one (never a binary know-nothing). */
int         concolic_is(JSValueConst v);              /* 1 iff v is a concolic value */
const char *concolic_shape_c(JSValueConst v);         /* display shape (NULL if not concolic) */
const char *concolic_src_c(JSValueConst v);           /* source identity (NULL if not concolic) */
JSValue     concolic_example(JSContext *ctx, JSValueConst v);   /* the concrete example (dup'd) or JS_UNDEFINED */
void        concolic_set_example(JSContext *ctx, JSValueConst v, JSValue example);   /* attach/replace (consumes example) */

/* Propagation through `+` — install with JS_SetConcolicAddHook. */
int         concolic_add_hook(JSContext *ctx, JSValue *sp);

/* @S candidate injection: during a verification re-run, the source identified by `src` (a field path like
   "{state}.code") returns the concrete `payload` instead of a concolic. NULL/NULL clears it. */
void        concolic_set_candidate(const char *src, const char *payload);

/* Comparison constraint domain: `x === 'admin'` on a concolic must FORK (not collapse to concrete false), and
   the taken arm pins/negates. */
enum { OPCMP_NONE = 0, OPCMP_EQ = 1, OPCMP_NE = 2 };
JSValue     concolic_new_cmp(JSContext *ctx, const char *src, int op, const char *tok);  /* a comparison-result bool */
int         concolic_cmp(JSValueConst v, const char **psrc, const char **ptok);          /* OPCMP_* of a cmp result */
int         concolic_cmp_hook(JSContext *ctx, JSValue *sp, int is_neq);                   /* == / === propagation (JSConcolicCmpHook) */
void        concolic_pin(const char *src, const char *val);   /* EQ true-arm: this source now reads `val` (real @H value) */
void        concolic_clear_pins(void);                        /* per-flow: clear all pins */

#endif
