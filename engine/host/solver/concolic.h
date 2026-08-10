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

/* INSTALL THE WHOLE HOOK SET. It exists because the set was written out as a struct literal at each entry —
   main.c and test_forced.c — and the two DRIFTED: the fixture harness installed three of the ten, so every
   targeted test in this repo ran against a weaker engine than the one that ships, and a relational compare on a
   concolic reached the ToNumber boundary's DCHECK instead of the .rel hook that answers it. A capability the
   solver owns is declared once BY the solver; an entry asks for it, it does not enumerate it. */
/* THE VALUE SEMANTICS of a concolic — how it adds, compares, coerces and reports its type. Install this in ANY
   host where a concolic can be reached: without it every operator falls through to the ordinary-object path
   and the first coercion throws "toPrimitive" out of an expression the page never wrote. */
void concolic_install_hooks(void);
/* IS THIS HOST EXPLORING? Where concolic values COME FROM — a different decision from what they DO. A global
   that was never set becomes unknown server-injected input rather than a ReferenceError, and a browser value
   an attacker controls becomes a source rather than the plain value the spec computes. A conformance host
   wants the spec's answers and declines this; it still gets the value semantics above. Install after
   concolic_install_hooks. */
void concolic_install_source_overlay(void);

/* THE ONE SEAM a browser component hands a computed value through to become an attacker SOURCE. Returns
   `computed` unchanged where no source overlay is installed, and a concolic carrying it as the EXAMPLE where
   one is. `computed` is consumed either way. */
JSValue concolic_source_wrap(JSContext *ctx, const char *shape, const char *src, JSValue computed);

/* Propagation through `+` — install with JS_SetConcolicAddHook. */
int         concolic_add_hook(JSContext *ctx, JSValue *sp);

/* @S candidate injection: during a verification re-run, the source identified by `src` (a field path like
   "{state}.code") returns the concrete `payload` instead of a concolic. NULL/NULL clears it. */
void        concolic_set_candidate(const char *src, const char *payload);

/* A SOURCE'S BROWSER DELIVERY — what the browser does to the attacker's bytes before the page ever reads them,
   declared by the component that owns the source.
   This was MISSING, and its absence is a false-PoC generator rather than a gap. The solver invents a breakout
   and hands it to the source RAW, so a candidate containing `<` looked like it broke out of an HTML sink fed
   from `location.hash` — when a real browser percent-encodes `<` in a fragment, so the page would have read
   `%3C` and nothing would have happened. CLAUDE.md states the consequence directly: a raw-hash HTML breakout is
   a FALSE PoC unless the app decodes it, while a JS-context one (`';X9();//`) is REAL, because the fragment set
   encodes backtick and NOT the apostrophe. Those two outcomes are the same candidate through the same source;
   only the delivery tells them apart.
   `encode` lists the bytes this component percent-encodes (C0 controls and DEL are always encoded, so they are
   not listed); `prefix` is the character the component's value carries — `#` for a fragment, `?` for a query,
   0 for none. Data rather than code, because that is what differs between sources. */
void        concolic_declare_source(const char *src, const char *encode, char prefix);
/* The bytes this source's component percent-encodes, or NULL if the source declared no delivery (it is handed
   over as-is). A PARKED @S search reports it because it is the constraint every candidate had to survive, and
   it is a FACT read from the declaration rather than a guess at why nothing fired. */
const char *concolic_source_encodes(const char *src);

/* Comparison constraint domain: `x === 'admin'` on a concolic must FORK (not collapse to concrete false), and
   the taken arm pins/negates. */
enum { OPCMP_NONE = 0, OPCMP_EQ = 1, OPCMP_NE = 2 };
JSValue     concolic_new_cmp(JSContext *ctx, const char *src, int op, const char *tok);  /* a comparison-result bool */
int         concolic_cmp(JSValueConst v, const char **psrc, const char **ptok);          /* OPCMP_* of a cmp result */
int         concolic_cmp_hook(JSContext *ctx, JSValue *sp, int is_neq);
/* JSConcolicHooks.rel for < <= > >= — the ordering twin of cmp. */
int         concolic_rel_hook(JSContext *ctx, JSValue *sp, int op);
/* JSConcolicHooks.type_of — `typeof` an unknown is an unknown string, so the comparison forks. */
JSValue     concolic_typeof_hook(JSContext *ctx, JSValueConst v);   /* JS_UNINITIALIZED = not concolic, run the real typeof */
/* JSConcolicHooks.arith / .to_str — ToNumber and ToString over unknown input, answered by the OPERATOR (the
   conversion boundary owes C a real primitive). The result keeps the source; the example is the REAL op run on
   the operands' examples, or absent when there is nothing concrete to run. */
int         concolic_arith_hook(JSContext *ctx, JSValue *sp, int op, int nops);
JSValue     concolic_tostr_hook(JSContext *ctx, JSValueConst v);
/* JSConcolicHooks.key_read — `obj[x]` with an unknown key reads an unknown property. */
JSValue     concolic_key_read_hook(JSContext *ctx, JSValueConst obj, JSValueConst key);
/* JSConcolicHooks.key_name — the real string an unknown key denotes (its shape), stable per source. */
JSValue     concolic_key_name_hook(JSContext *ctx, JSValueConst key);
JSValue     concolic_builtin_hook(JSContext *ctx, JSValueConst v, const char *op, JSValue example);
/* THE BYTES A DOM MEMBER NEEDS FROM AN ARGUMENT THAT MAY BE UNKNOWN — a selector, an attribute name, a class
   token, an id. An unknown denotes its SHAPE (a real string, stable per source, the key_name rule); anything
   else converts normally. OWNED either way: free with JS_FreeCString. */
const char *concolic_name_cstr(JSContext *ctx, JSValueConst v);
void        concolic_pin(const char *src, const char *val);   /* EQ true-arm: this source now reads `val` (real @H value) */
/* THE OTHER HALF OF THE PATH CONSTRAINT. A predicate that pins nothing still narrows: taking the true arm of
   `if (cfg.admin)` says the value is truthy FOR THIS FLOW, and a bundle tests the same flag over and over. The
   branch records its outcome under `key` (the source path for a bare truthiness test, the source plus its
   operator and token for a comparison, so two different predicates over one source stay independent) and a
   later branch on the SAME key is DECIDED rather than forked. That is feasible-refinement — sound-only, since
   it prunes an arm the flow's own constraint already contradicts, never one whose domain permits both — and it
   is what keeps N tests of one flag from costing 2^N flows. -1 = not yet decided in this flow. */
void        concolic_constrain_branch(const char *key, int truth);
int         concolic_branch_decided(const char *key);
void        concolic_clear_pins(void);                        /* per-flow: clear the whole constraint */
/* Swap the per-flow pin map when the scheduler interleaves flows: suspend snapshots it, resume restores it. */
void       *concolic_pins_suspend(void);
void        concolic_pins_resume(void *blob);
void        concolic_pins_blob_free(void *blob);
/* WHAT THE FROZEN CONSTRAINT CHAIN IS HOLDING — the third of the three chains built on cow.c's refcounted
   immutable segment, reported beside the other two for the reason they are reported beside each other: a
   per-flow allocation nobody released looks identical from any one of them, and only the number that CLIMBS
   names which. A blob is one pointer at a segment, so a parked flow's own constraint cost is here and not in
   its per-flow rows. */
void        concolic_chain_stats(long *segs, long *entries, long *bytes);

#endif
