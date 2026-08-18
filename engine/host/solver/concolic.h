/* The CONCOLIC VALUE — the atom of the forced-execution solver (rebuilt clean on upstream quickjs).
 *
 * A concolic value is NOT a binary "opaque" sentinel. It is a triple:
 *   - SOURCE identity   : where the value came from (an attacker source "location.hash", a reply field, state)
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
/* THE AGENT'S HALF, UNDONE — solver/engine.h's release column. What a C static holds here for the whole agent:
   the SOURCE REGISTRY's array (whose rows are the declaring components' claims, given back at their own
   releases, which this asserts and which it NAMES the delinquent component of) and the installed @S
   substitution. The value CLASS is deliberately not among them and the release says why at the line that
   checks it. */
void concolic_free(void);

/* Mint a concolic value AT A SOURCE — the root of a derivation, where an unknown enters the program. `shape`
   is the @H/@S display form ("{location.hash}", "/api/{region}"); a DECLARED source's shape is its provenance
   in braces and concolic_source_wrap asserts that, so a component spells the pair from ONE token of its own
   (core/frame/location.h) and never two literals; `src` is the PROVENANCE (may equal shape), which is
   also this value's IDENTITY because nothing derived it; `example` (consumed) is the concrete example or
   JS_UNDEFINED when none is known yet. Returns a new owned JSValue. A value produced BY an operation over an
   unknown is not minted here — the operator's own hook composes its identity from its operands. */
JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, JSValue example);

/* Predicates + accessors — the ONLY concolic test is this domain-carrying one (never a binary know-nothing). */
int         concolic_is(JSValueConst v);              /* 1 iff v is a concolic value */
const char *concolic_shape_c(JSValueConst v);         /* display shape (NULL if not concolic) */
const char *concolic_src_c(JSValueConst v);           /* PROVENANCE: which source (NULL if not concolic) */

/* IDENTITY — WHICH VALUE THIS IS, and a different question from `concolic_src_c`'s WHERE IT CAME FROM.
 *
 * Provenance is INHERITED through a derivation (`location.hash.slice(1)` is still the fragment, for injection,
 * for the declared percent-encode set, for what a report names). Identity is COMPOSED at every derivation,
 * because it is what the per-flow path constraint is keyed by and a key must name the value a branch TESTS.
 * One field answered both for a long time, and the consequence was silent and in the one direction §Solver-half
 * forbids: `x`, `x*2`, `x < 700` and `x < 300` shared a provenance, so a flow's record of any of them DECIDED
 * the rest and feasible-refinement pruned arms nothing contradicts. A pruned arm emits nothing, so no gate
 * could ever see it.
 * NULL means this engine cannot spell the value exactly (an operand that is a plain object or a symbol, a
 * property name that would not convert). That is a POSITIVE statement — never decide from it, keep both arms —
 * and it is why absence is safe here while a wrong identity is not. */
const char *concolic_ident_c(JSValueConst v);

/* THE ONE ENCODING every identity and every constraint key is written in — a TAG plus FIELDS, each written as
   `<byte length>:<bytes>`. Two different (tag, fields) sequences can never write the same string, so a
   collision is impossible BY CONSTRUCTION rather than detected: no field's contents can spell another field's
   boundary, and the length is measured and written by the same two lines so no buffer can truncate. Returns a
   heap string the caller frees, or NULL when any field is absent (the composition is then absent too).
   decide.c builds its constraint keys with this, which is why the solver has ONE speller and not three. */
char       *concolic_ident_compose(const char *tag, const char *const *fields, int n);
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

/* A VALUE THAT IS A JOINT FUNCTION OF SEVERAL SOURCES — ONE concolic, ONE identity, a domain over the SET.
 *
 * WHAT REACHES THIS. A `width: auto` box with a real border has a used content width of `containing block −
 * margins − paddings − snapped borders`, and css-values §6 snaps that border to a whole number of DEVICE
 * PIXELS: the number is a joint function of the initial containing block and of the device pixel ratio, and a
 * page comparing it against 700 is branching on the pair. `100vmin` is the same shape in one token (both
 * viewport axes are operands of §6.1.2.2's smaller-of), and three facts already meet where the two combine.
 * Neither operand's identity can answer for the result: keeping the first reports a narrowing over the ICB the
 * page never made, and keeping the largest, or the one that won a `max`, reports a dependence that reverses at
 * another viewport.
 *
 * THE IDENTITY IS THE MEMBERS' OWN, CANONICALLY ORDERED — sorted here, so the key is a property of the SET and
 * not of the arithmetic that assembled it. `cb - margins - borders` and `cb - borders - margins` are one
 * dependence, and two spellings of one identity would fork the same predicate twice; a caller-chosen order
 * would put that canonicalization in every caller, which is the one-fact-answered-from-many-places defect
 * CLAUDE.md §per-realm names. A duplicate member is a CRASH rather than a dedup: a set holds each member once,
 * and a key that counted how many times the arithmetic touched a fact would depend on the arithmetic again.
 *
 * IT IS A SET AND NOT AN EXPRESSION over the sources. An expression is the recorded transform §Re-execution
 * forbids — it cannot see interprocedural or shared-mutable state, and building one beside the sound
 * re-execution search is the cardinal misread of §Solver-half. What propagates is PROVENANCE (which inputs),
 * never the operation; the EXAMPLE is right because the engine ran the real arithmetic.
 *
 * SO A NARROWING OF THE JOINT NARROWS NOTHING ELSE, and that is the sound direction rather than a limitation.
 * Taking the true arm of `usedWidth < 700` records a fact about THIS relation; it does not pin the ICB (many
 * pairs give one width, and inverting the derivation is the banned expression again), and a flow that has
 * already decided `innerWidth < 768` does not decide this one. Both keep forking, which errs toward MORE
 * exploration exactly as §Headless requires.
 *
 * `shapes[i]` is member i's display form and `srcs[i]` its source identity, `n >= 1` of them; the degenerate
 * n == 1 composes to the member itself, so there is one path and no second spelling for a single source.
 * `computed` is CONSUMED and becomes the joint value's example. */
JSValue concolic_source_wrap_joint(JSContext *ctx, const char *const *shapes, const char *const *srcs,
                                   int n, JSValue computed);

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
   a FALSE PoC unless the app decodes it, while a JS-context one (`';X9()//`) is REAL, because the fragment set
   encodes backtick and NOT the apostrophe. Those two outcomes are the same candidate through the same source;
   only the delivery tells them apart.
   `encode` lists the bytes this component percent-encodes (C0 controls and DEL are always encoded, so they are
   not listed); `prefix` is the character the component's value carries — `#` for a fragment, `?` for a query,
   0 for none. Data rather than code, because that is what differs between sources. */
/* AND `deliver` IS THE OTHER HALF OF THE SAME DECLARATION — the half a REPRODUCTION needs where `encode` and
   `prefix` are the half a BREAKOUT needs. §S(d) requires every emitted PoC to carry its reproduction envelope,
   and the envelope's two hardest facts — is this a TWO-STAGE plant-then-load PoC (§S(b)), and can a delivery
   layer perform the delivery at all — are facts about the SOURCE, so they are stated by the component that
   OWNS the source and never re-derived from its name by whoever renders the report. That re-derivation is not
   hypothetical: the offscreen matched `/\{(hash|search|pm|reply)\}/` against a display shape the engine has
   never emitted, so live verify could not build a PoC for any finding this engine produces, and the taxonomy
   it invented had a `pm` entry for a source no component declares.
   FOUR MECHANISMS, not one row per source: what differs between two sources carried in the victim's own
   address is only the component `prefix` already names. */
/* The two address mechanisms are named as the PAIR they are — whose address carries the payload — because that
   is the whole difference between them and because `navigation` is already a word the FIRING vocabulary uses
   for something else (`firesOn`), and one word meaning two things across two vocabularies on one record is how
   the last set of names went wrong. */
typedef enum {
    SRC_DELIVER_ADDRESS = 0,          /* the payload rides the VICTIM'S OWN address, at `prefix`'s component */
    SRC_DELIVER_PLANT,                /* placed BEFORE the victim's load, read back on it — §S(b) two-stage */
    SRC_DELIVER_REFERRING_ADDRESS,    /* the payload rides the address the victim ARRIVES FROM */
    SRC_DELIVER_USER_FILE,            /* the user hands the document a file whose bytes the attacker chose */
} SourceDeliverKind;
/* AND EVERY ROW NAMES ITS CLAIMANT — the declare, the give-back and the ask-first below are one mechanism
   around that one field. A declaration here is core/platform.h's FOURTH paragraph exactly: the STORAGE is this component's array and
   the CLAIM is the declaring component's agent state, so the claimant gives it back at its own release and the
   holder asserts at its own that nothing is left pointing into it. `component` is that claimant — the same
   name core/platform.c's row and core/agent_state.h's slots use, and stored as the pointer for the reason that
   file stores it that way: a component name is a static string that outlives the agent it names.
   IT IS A FIELD RATHER THAN A CONVENTION BECAUSE THE GIVE-BACK IS KEYED BY IT. The alternative — a give-back
   naming each SOURCE — reads well for the two components whose rows are fixed and fails for the one whose rows
   are not: core/file/file_system.c declares `file:NAME` per file the device holds, so a by-name release forces
   that component to keep its own parallel list of what it declared, which is a SECOND copy of this array that
   must agree with it. One list, here, with the owner on the row: nothing to drift, and the holder's assert can
   NAME the component that did not finish instead of reciting a list of files that goes stale. */
void        concolic_declare_source(const char *component, const char *src, const char *encode, char prefix,
                                    SourceDeliverKind deliver);
/* THE CLAIM, GIVEN BACK — called from the claimant's own `_free`, once, whatever number of rows it declared,
   and concolic_free asserts the registry is empty afterwards. It removes THIS component's rows and no others,
   which is what keeps that assert falsifiable: a claimant that forgot leaves its rows behind and is named. A
   call that emptied the registry outright would be a release undoing a declaration it did not make — the shape
   core/platform.c's own table check forbids one column up — and "everyone gave their rows back" and "one
   claimant forgot and the wipe covered it" would then be the same state. */
void        concolic_undeclare_sources(const char *component);
/* DOES THIS COMPONENT ALREADY OWN A ROW FOR THIS SOURCE? The question a claimant whose rows are DYNAMIC has,
   asked of the one registry rather than of a private copy of it — `concolic_source_encodes(src)` is the wrong
   question for it, because that reads the whole registry and answers "yes" for a row another component
   declared. Aborts if the source is declared by somebody else: one source is owned by one component. */
int         concolic_source_declared_by(const char *component, const char *src);
/* THE DECLARED DELIVERY as a report states it: the mechanism's TOKEN and the address component the payload
   rides (0 for every mechanism that is not an address). Returns 0 when this source declared none — which is a
   FACT, not a default: server-injected page state (`window.__FLAGS`) is written by the attacker directly and
   no component transforms or carries it, so there is nothing to declare and a consumer must say exactly that
   rather than guess a vector. */
int         concolic_source_delivery(const char *src, const char **kind, char *prefix);
/* The bytes this source's component percent-encodes, or NULL if the source declared no delivery (it is handed
   over as-is). A PARKED @S search reports it because it is the constraint every candidate had to survive, and
   it is a FACT read from the declaration rather than a guess at why nothing fired. */
const char *concolic_source_encodes(const char *src);

/* Comparison constraint domain: `x === 'admin'` on a concolic must FORK (not collapse to concrete false), and
   the taken arm pins/negates. */
enum { OPCMP_NONE = 0, OPCMP_EQ = 1, OPCMP_NE = 2 };
JSValue     concolic_new_cmp(JSContext *ctx, const char *src, int op, const char *tok);  /* a comparison-result bool */
/* THE PIN A TAKEN ARM CAN PERFORM, and nothing else — OPCMP_EQ/NE with the source and the concrete token, or
   OPCMP_NONE. An ordering answers NONE because it pins nothing (`x > 5` narrows a domain and determines no
   value; §Solver-half keeps that a domain-annotated shape rather than inventing a 6), and so does an equality
   whose OTHER side is also unknown, because there is no concrete value to pin to. The PREDICATE itself is not
   read through here: it lives in the value's identity, composed from the operator and both operands, which is
   what decide.c keys the constraint by. */
int         concolic_cmp(JSValueConst v, const char **psrc, const char **ptok);
int         concolic_cmp_hook(JSContext *ctx, JSValue *sp, int is_neq);
/* JSConcolicHooks.rel for < <= > >= — the ordering twin of cmp, and it composes its operator and BOTH operands
   into the result's identity exactly as the equality hook does. It composed neither, which made every ordering
   over one source ONE predicate: `parseInt(gCS(a).width) < 700` decided `parseInt(gCS(b).width) < 300`. */
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
   branch records its outcome under `key` — which decide.c composes from the IDENTITY of the value the branch
   tests, and a comparison result's identity already carries its operator and BOTH its operands — and a later
   branch on the SAME key is DECIDED rather than forked. That is feasible-refinement, and it is sound ONLY
   while the key names one predicate: it may prune an arm the flow's own constraint contradicts and never one
   whose domain still permits both. It is what keeps N tests of one flag from costing 2^N flows. -1 = not yet
   decided in this flow, which is also what an unspellable value answers for ever. */
void        concolic_constrain_branch(const char *key, int truth);
int         concolic_branch_decided(const char *key);
void        concolic_clear_pins(void);                        /* per-flow: clear the whole constraint */
/* Swap the per-flow pin map when the scheduler interleaves flows: suspend snapshots it, resume restores it. */
void       *concolic_pins_suspend(void);
void        concolic_pins_resume(void *blob);
void        concolic_pins_blob_free(void *blob);
/* The empty one, for a flow the COLD TIER resumed: it stands on a recorded decision chain (so the scheduler
   resumes rather than enters it) and has learned nothing yet in this session — it re-derives every pin and every
   decided predicate as it replays the gates that produced them. See the definition. */
void       *concolic_pins_blob_empty(void);
/* WHAT THE FROZEN CONSTRAINT CHAIN IS HOLDING — the third of the three chains built on cow.c's refcounted
   immutable segment, reported beside the other two for the reason they are reported beside each other: a
   per-flow allocation nobody released looks identical from any one of them, and only the number that CLIMBS
   names which. A blob is one pointer at a segment, so a parked flow's own constraint cost is here and not in
   its per-flow rows. */
void        concolic_chain_stats(long *segs, long *entries, long *bytes);

#endif
