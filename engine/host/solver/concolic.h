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
#include "solver/rel_op.h"   /* the four relations a bound can be stated in — see concolic_bound */

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
   also this value's IDENTITY because nothing derived it AND its DELIVERY ROOT because a source read is where
   the bytes entered; `example` (consumed) is the concrete example or
   JS_UNDEFINED when none is known yet. Returns a new owned JSValue. A value produced BY an operation over an
   unknown is not minted here — the operator's own hook composes its identity from its operands. */
JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, JSValue example);

/* Predicates + accessors — the ONLY concolic test is this domain-carrying one (never a binary know-nothing). */
int         concolic_is(JSValueConst v);              /* 1 iff v is a concolic value */
const char *concolic_shape_c(JSValueConst v);         /* display shape (NULL if not concolic) */
const char *concolic_src_c(JSValueConst v);           /* INJECTION IDENTITY: which source (NULL if not concolic) */

/* THE DELIVERY ROOT — WHICH SOURCE'S COMPONENT PHYSICALLY CARRIED THESE BYTES IN, and the third fact this
 * record holds, because `src` was answering it and answering it WRONG.
 *
 * `src` is where an @S candidate is INJECTED, and a derivation is entitled to mint a new one: a field read of
 * an unknown object is an independently attacker-controlled datum, so concolic_exotic_get names `{state}.admin`
 * and a candidate substitutes exactly there. `root` is where the bytes ENTERED THE PROGRAM, and a derivation
 * can never change that: slicing a fragment does not stop it being the victim's own URL fragment.
 *
 * ONE FIELD ANSWERED BOTH, AND THE REPORT IT PRODUCED WAS FALSE IN THE WORST DIRECTION. A page doing
 * `eval("var t='" + location.hash.slice(1) + "';")` was fired by this engine on real Chrome at
 * `http://127.0.0.1:8781/#';X9()//` — one navigation, the one actually performed — and the popup's
 * reproduction envelope said "the engine declares no browser delivery for this source — ... there is no
 * navigation that reproduces it". `.slice` is a field read of the concolic, so concolic_exotic_get minted
 * `{location.hash}.slice`, the call minted `{location.hash}.slice()`, and the source registry — an exact
 * strcmp over the declared rows — matched neither. The consumer was right to read absence as a statement
 * (extension/lib/popup-security.js `_deliverySentence` does exactly what a consumer owes a producer); the
 * producer had lost the fact.
 * IT ALSO DECIDED WHAT THE SEARCH DELIVERS, which is the half that is not a reporting bug: concolic_deliver
 * looks the encode set up too, so a breakout injected at a DERIVED identity was handed to the page RAW —
 * exactly the false-PoC generator the delivery declaration was introduced to end (see the declaration below),
 * re-created for every `location.hash.slice(1)` in the world.
 *
 * INHERITED UNCHANGED THROUGH EVERY DERIVATION, including the two that mint a new `src`. NULL exactly when
 * `concolic_src_c` is NULL (concolic_alloc asserts the two are present together), and NULL means these bytes
 * entered through nothing this engine minted as a source — a positive statement, never a hole to fill. */
const char *concolic_root_c(JSValueConst v);

/* IDENTITY — WHICH VALUE THIS IS, and a different question from `concolic_src_c`'s WHERE IT IS INJECTED and
 * from `concolic_root_c`'s WHERE IT CAME FROM.
 *
 * Identity is COMPOSED at every derivation, because it is what the per-flow path constraint is keyed by and
 * a key must name the value a branch TESTS.
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
/* …AND THE SAME QUESTION, ASKED. A component that mints a source of its OWN — one that is not an attacker
   delivery, so concolic_source_wrap's registry and its read counter would both be a wrong answer about it —
   still owes the gate above, because a conformance host must get the spec's value and not an unknown. This is
   that gate as a predicate, so the decision lives in one place rather than being re-derived from whether some
   hook happens to be installed. */
int concolic_is_exploring(void);
/* HOW MANY ATTACKER-SOURCE VALUES THIS DOCUMENT'S RUN MINTED — the first of the facts an EMPTY @S surface
   collapses, and the one that is not about sinks at all. "No finding" has at least four readings and they take
   opposite actions: the page never read an attacker source (a driving gap — the code that reads one was never
   reached); it read one and nothing tainted reached a code-execution sink (a propagation question, or a page
   that has no such flow); something tainted reached a sink and the search was suppressed because the check on
   it was unforgeable (a POSITIVE result about the page); or no sink ran at all. Solver/solve.h counts the last
   three where they happen; this is the first. Zero here with sinks reached is a different page from zero here
   with none, and one empty array reports both. */
long concolic_source_reads(void);

/* ANSWER [[GetOwnProperty]] FROM REAL SLOTS ONLY, for the span between these two calls.
 *
 * A concolic answers ECMAScript §10.1.5 [[GetOwnProperty]] ( propertyKey ) for every member its EXAMPLE holds,
 * because that is the record's own surface and §7.3.23 EnumerableOwnProperties ( obj, kind ) gates every key
 * on it. That answer is SYNTHESISED — the descriptor is composed at the query and the value is a derivation
 * minted afresh — and a synthesised descriptor is not a SLOT.
 *
 * THE COW DELTA READS SLOTS, AND THE DIFFERENCE IS THE WHOLE OF WHY THIS EXISTS. A capture records what
 * storage held before a flow wrote it and an unapply puts that back; handed a synthesised descriptor it
 * records `existed = 1` for a member that occupies no storage, and the unapply then WRITES it — materialising
 * a real own slot, holding one flow's derivation, on an object that never had one, where it shadows this
 * class's own [[Get]] for every sibling flow from then on. What is lost is exactly the two per-flow facts that
 * live in that [[Get]]: an @S candidate's substitution at that member, and CONCRETIZE-ON-PIN.
 *
 * QUICKJS ALREADY DRAWS THIS LINE ONE INTERNAL METHOD OVER and draws it in the engine: delete_property takes
 * an `as_slot` flag and refuses to consult a class's [[Delete]] under it, saying that "an exotic
 * named-property hook is a VIEW over storage that is NOT this object's shape … so the storage a flow can
 * mutate through the view is captured on the object that OWNS it and never as a slot on the viewer". Every
 * word of that is true of [[GetOwnProperty]], and JS_GetOwnSlotDesc passes the same `as_slot` and then
 * consults the hook anyway. Until the two agree there, the host says it here: the delta enters this mode
 * around its baseline read and its read-back, and this class answers from the ordinary layer.
 *
 * It is entered around ONE lookup that runs no page code and therefore cannot suspend; nesting is a DCHECK. */
void concolic_slots_only_begin(void);
void concolic_slots_only_end(void);

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

/* Propagation through a CONCATENATION — installed as JSConcolicHooks.add (JS_SetConcolicHooks). `op` names
   which spec algorithm the caller is performing, because 13.15.3's `+` has a numeric arm and 22.1.3.5's
   String.prototype.concat does not; see JSConcolicAddOp in quickjs.h. */
int         concolic_add_hook(JSContext *ctx, JSValue *sp, JSConcolicAddOp op);

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
   FIVE MECHANISMS, not one row per source: what differs between two sources carried in the victim's own
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
    /* THE ATTACKER POSTS IT — HTML §9.3.3 "Posting messages", the window post message steps. It is its own
       mechanism and not a plant, because the two differ in exactly the way §S(b) cares about: a plant is
       written BEFORE the victim's load and read back on it, while this is delivered to a document that is
       ALREADY RUNNING, by a document the attacker holds open beside it (the victim in the attacker's iframe,
       or opened as a popup). The reproduction is therefore neither a URL nor a two-stage load: it is a second
       document, live at the same time, and the envelope has to say so.
       IT CARRIES NO PERCENT-ENCODE SET, and that is a measured property of the mechanism rather than an
       unfilled column: §9.3.3 step 7 hands the message to StructuredSerializeWithTransfer and step 8.4
       deserializes it, and neither transforms a string — so the bytes the attacker writes are the bytes the
       handler reads. That is precisely why postMessage breakouts reproduce where a raw-fragment one does not. */
    SRC_DELIVER_CROSS_DOCUMENT_MESSAGE,
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
/* A SOURCE WHOSE VALUE IS THE ATTACKER'S OWN PRINCIPAL — the second half of a row, declared by the component
 * that owns it, and the reason `event.origin` is a different KIND of attacker input from `event.data`.
 *
 * WHAT IT MEANS. Every other declared source is a value the attacker WRITES: any byte string the search solves
 * for is one the attacker can put there, so an equality gate on it (`hash === "admin"`) is SOLVED and the pin
 * is the answer. A principal is a value the attacker NAMES BY OWNING: HTML §9.3.2.2 "User agents" states the
 * whole basis of this API — "the integrity of this API is based on the inability for scripts of one origin to
 * post arbitrary events … to objects in other origins" — so the browser stamps `event.origin` and the attacker
 * chooses it only by registering the domain it names. §9.3.2.1 "Authors" is the other side of the same fact:
 * checking `origin` is what the standard tells a page to do, and it works precisely because the value cannot
 * be written.
 *
 * WHAT IT DECIDES. An equality against a concrete token PINS the source (decide.c's concretize-on-pin), and a
 * pin on a principal is a demand the attacker cannot meet from a document of their own — so a flow that
 * reached a sink through one is a REAL code path that no cross-document attacker can drive, and an @S finding
 * taken off it would be a PoC that does not reproduce. §Attacker-sources states the pair exactly: a forgeable
 * check (endsWith/includes/startsWith) is SOLVED, an unforgeable one (=== exact) is unsatisfiable cross-origin
 * and SUPPRESSES the finding. The forgeable half needs nothing here, and that is the point of splitting them:
 * a prefix or substring predicate PINS NOTHING (concolic_cmp answers OPCMP_NONE for it), so it never reaches
 * this rule and the search proceeds — which is the correct verdict for it and is reached by the pin's own
 * semantics rather than by a second test that would have to enumerate the string builtins.
 *
 * IT IS A PROPERTY OF THE SOURCE AND NOT OF THE SINK, which is why it is declared beside the delivery and not
 * asked at the emission: the same fact decides every finding rooted there, and a consumer that re-derived it
 * from the source's NAME would be the `{hash}|{search}|{pm}` taxonomy solver/concolic.h already records as a
 * table matching shapes this engine never emitted.
 * The component must already own `src`; a principal declared for a row nobody holds is a claim with no
 * claimant, which is the one thing the registry's give-back cannot express. */
void        concolic_declare_source_principal(const char *component, const char *src);
/* HAS THIS FLOW PINNED A PRINCIPAL SOURCE? — asked where an attacker value ARRIVES AT A SINK, because that is
   the last point at which the flow that reached it still exists. It reads the running flow's own path
   constraint, so it parks and forks with the flow exactly as every other narrowing does, and it answers 0 for
   a flow that merely tested a principal without pinning it (a prefix check, or the FALSE arm of an equality).
   A DERIVED spelling counts: `event.origin.toLowerCase() === X` pins the derived value and not the source, and
   the demand on the attacker's principal is the same either way — which is why the pin is recorded under the
   pinned value's ROOT as well as its own identity (concolic_pin), and why this question is asked of the root. */
int         concolic_principal_pinned(void);
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
/* BOTH OF THESE ARE ASKED ABOUT A `root`, NEVER ABOUT A `src`, AND THE PARAMETER IS NAMED FOR IT. A DERIVED
   identity matches no declared row and the answer comes back as the silence that means "this source declared
   none" — which is a different fact wearing the same shape. That is how a fire-verified fragment XSS was
   reported as having no navigation that reproduces it. Ask concolic_root_c, whose whole reason for existing is
   to be the argument here.
   AND A ROOT MAY NAME A SET, so neither is a strcmp over the whole of it any more. A joint provenance is its
   members joined (concolic_source_wrap_joint), and matching the joined string against the rows found nothing
   and answered that same silence — the defaulted-field defect standing exactly where a wrong answer becomes a
   wrong PoC. They now walk the members: a joint whose only DECLARING member is one source answers that
   member's declaration, which is the ordinary case because most joints in this engine are over environment
   facts that declare nothing. A root with TWO declaring members has no single honest answer — each carries its
   own percent-encode set and its own address component — so it CRASHES rather than presenting one source's
   constraint as the whole of one; the mechanism to build is one candidate per declaring member, each with its
   own envelope, and the one that fires is the one emitted. */
int         concolic_source_delivery(const char *root, const char **kind, char *prefix);
/* The bytes this source's component percent-encodes, or NULL if the source declared no delivery (it is handed
   over as-is). A PARKED @S search reports it because it is the constraint every candidate had to survive, and
   it is a FACT read from the declaration rather than a guess at why nothing fired. */
const char *concolic_source_encodes(const char *root);

/* Comparison constraint domain: `x === 'admin'` on a concolic must FORK (not collapse to concrete false), and
   the taken arm pins/negates. */
enum { OPCMP_NONE = 0, OPCMP_EQ = 1, OPCMP_NE = 2 };
JSValue     concolic_new_cmp(JSContext *ctx, const char *src, int op, const char *tok);  /* a comparison-result bool */
/* …AND ITS TWIN FOR A RELATION OVER TWO LIVE VALUES, either of which may be unknown — for a browser component
   whose own algorithm compares two operands (HTML §8.7's timer task source orders one expiry against another,
   and the expiry of a timer set with unknown external input is unknown). `op` NAMES THE SPEC RELATION being
   performed, because the interpreter's entry (concolic_rel_hook) takes quickjs's own opcode for the operator
   and quickjs exports no such number — a component that invented one would be spelling a second opcode
   namespace. The identity is composed HERE from that name and BOTH operands, so decide.c stays the one speller
   of a constraint key. The result is a predicate value to hand to solver_decide; it pins nothing (an ordering
   determines no value, and an equality against an unknown has none to pin to). Operands are BORROWED. */
JSValue     concolic_new_rel(JSContext *ctx, const char *op, JSValueConst a, JSValueConst b);
/* THE PIN A TAKEN ARM CAN PERFORM, and nothing else — OPCMP_EQ/NE with the source and the concrete token, or
   OPCMP_NONE. An ordering answers NONE because it pins nothing (`x > 5` narrows a domain and determines no
   value; §Solver-half keeps that a domain-annotated shape rather than inventing a 6), and so does an equality
   whose OTHER side is also unknown, because there is no concrete value to pin to. The PREDICATE itself is not
   read through here: it lives in the value's identity, composed from the operator and both operands, which is
   what decide.c keys the constraint by. */
int         concolic_cmp(JSValueConst v, const char **psrc, const char **ptok);
/* JSConcolicHooks.cmp — `op` names WHICH equality the program wrote (quickjs.h's JSConcolicEqOp), because
   §7.2.13 IsLooselyEqual and §7.2.14 IsStrictlyEqual disagree and this hook does two things that need the
   answer: it composes the operator into the predicate's IDENTITY (so `x == '1'` and `x === '1'` are two
   constraint entries rather than one deciding the other), and it RUNS the comparison on the operands' concrete
   examples to give the result its own. */
int         concolic_cmp_hook(JSContext *ctx, JSValue *sp, int is_neq, JSConcolicEqOp op);
/* JSConcolicHooks.rel for < <= > >= — the ordering twin of cmp, and it composes its operator and BOTH operands
   into the result's identity exactly as the equality hook does. It composed neither, which made every ordering
   over one source ONE predicate: `parseInt(gCS(a).width) < 700` decided `parseInt(gCS(b).width) < 300`. */
int         concolic_rel_hook(JSContext *ctx, JSValue *sp, int op);
/* JSConcolicHooks.type_of — `typeof` an unknown is an unknown string, so the comparison forks. */
JSValue     concolic_typeof_hook(JSContext *ctx, JSValueConst v);   /* JS_UNINITIALIZED = not concolic, run the real typeof */
/* JSConcolicHooks.arith / .to_str — §7.1.4 ToNumber ( arg ) and §7.1.19 ToString ( arg ) over unknown input,
   answered by the OPERATOR (the conversion boundary owes C a real primitive). The result keeps the source; the
   example is the REAL op run on the operands' examples, or absent when there is nothing concrete to run.
   `.to_str` IS 22.1.1.1 String ( value ) AND NOT ToString EVERYWHERE, and the distinction is what keeps this
   from becoming a second name for concolic_builtin_hook: `String(x)` is the algorithm whose result IS the
   coercion, while every builtin that merely CONSUMES the string derives ITS OWN result from the operand, named
   by its own operation. The engine reaches that second derivation from the one sub-sequence all of them coerce
   through (step_tostring_run's JS_STEP_UNKNOWN), so there is one derivation per question rather than one hook
   per call site. */
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
/* EQ true-arm: `src` now reads `val` (a real @H value) FOR THIS FLOW.
   `root` IS RECORDED BESIDE IT AND IS A DIFFERENT FACT — where the pinned bytes ENTERED the program, which a
   derivation never changes (solver/concolic.h's `concolic_root_c`). The pin itself is keyed by the value's own
   identity, because that is what a later read of THAT value concretizes through; the root is written as a
   separate mark under the root's own key so a question about the SOURCE — has this flow demanded a particular
   value of it — is answerable without walking the chain and without a second reading of the pin. The two must
   not be one entry: `event.origin.toLowerCase() === X` pins the DERIVED value, and writing `X` under
   `message.origin` would make a later read of `event.origin` itself answer the lowercased token. */
void        concolic_pin(const char *src, const char *root, const char *val);
/* THE HOLE A REPORT PRINTS, SPELLED ONCE — the joint between a constraint recorded at a BRANCH and a domain
   emitted at a REQUEST, which are the two ends §Solver-half's two-facts rule has to connect.
   A shape is what the @H surface renders where the code computed no value, and the BRACES DO NOT SIT IN THE
   SAME PLACE in the two spellings that reach it: a source read displays `{location.hash}`, while a member read
   composes `"%s.%s"` over its parent's, so `state.id` displays as `{state}.id` with the member path OUTSIDE
   the braces, and endpoint.c's path scan re-spells that same segment again as `{state.id}`. Three spellings,
   one hole. The key is the shape with EVERY brace removed, which makes all three one name — and it is the only
   thing left of the value by the time the emission runs, because a request's address is a STRING and the
   concolic that carried each hole is long gone.
   Returns malloc'd, or NULL when `shape` names no hole at all (a concrete value has no domain to look up and
   must not borrow one) and for the unnameable `{}` (endpoint.c mints no param for it either). */
char       *concolic_hole_key(const char *shape);
/* THE SUBJECT OF A COMPARISON RESULT — the hole key of the unknown operand the predicate is ABOUT, borrowed.
   `src` is NOT it and substituting one for the other is the failure this exists to prevent: a member read's
   `src` IS its braced shape, so a domain recorded under `src` at the branch would be looked up under the
   brace-stripped name at the emission and never found — a constraint observed, stored, and unreadable.
   Present exactly when the pin is (concolic_cmp answers something other than OPCMP_NONE): the operator, the
   token and the subject are ONE observation, asserted together at the mint. */
const char *concolic_cmp_subject(JSValueConst v);
/* …AND THE SAME OPERAND BY ITS OWN IDENTITY, which is a different name for a different consumer and not a
   spelling variant of the hole above. The HOLE is what a report prints and is derived from the display shape
   by stripping braces, so it is lossy and not total — `{}` has none, and two shapes can strip to one string —
   which is right for a name the emission has to RECONSTRUCT from printed text and wrong for a key an
   execution fact is filed under. This is the operand VALUE's composed identity, and it is what
   concolic_contradict_example is keyed by: on the arm that contradicts the predicate's own example, the
   operand's example is contradicted with it, exactly as the pin and the exclusion are two arms of one
   observation. NULL when both sides are unknown (the contradicting arm proves one example wrong and never
   which), and NULL for an operand this engine cannot spell. Borrowed. */
const char *concolic_cmp_subject_ident(JSValueConst v);
/* THE NEGATIVE HALF OF THE EQUALITY OBSERVATION — the arm concolic_pin does NOT cover, and it is an
   OBSERVATION rather than an absence.
   Forced multi-path execution runs BOTH arms of every equality gate, so each `x === "admin"` in a bundle
   produces exactly one pinned flow and exactly one flow that has PROVEN `x != "admin"`. The pin rides the
   value as its example; the negation had nowhere to go, so the arm the tool exists to explore — the one that
   is not the value the code happened to test for — reported its parameter with the SAME BYTES as a parameter
   nothing constrained at all. §Solver-half calls that a wrong report and not a partial one, because the
   report's silence about the gate is read as the positive statement "anything goes".
   It is keyed by the HOLE KEY of the value, not by its `src`, for the reason concolic_cmp_subject states.
   `tok` is the concrete side of the equality — a value the code WROTE, never one this engine chose, so
   recording it invents nothing. Repeats are idempotent.
   IT DOES NOT TRAVEL THROUGH A DERIVATION, AND THAT IS THE ANSWER RATHER THAN THE GAP. A gate over `x` is a
   fact about `x`; the request may carry `encodeURIComponent(x)`, which is a different hole with a different
   shape and therefore a different key, so nothing is filed under it. Propagating the parent's exclusion onto
   the child would be a claim the run never made — `x.length !== 3` says nothing whatever about `x` — and it
   is exactly the recorded transform-expression §Re-execution forbids, arriving as a convenience. The
   constraint on the transformed value is observed the only way it can be: by the flow branching on IT. */
void        concolic_exclude(const char *hole, const char *tok);
/* THIS FLOW TOOK AN ARM THE VALUE'S OWN EXAMPLE CONTRADICTS, so the value answers with no example on this
   path — §Learning-from-replies' "the forced sibling drops the contradicted example, so only gate-DEPENDENT
   values degrade to a shape while gate-independent values stay concrete". `ident` is the VALUE's identity
   (concolic_ident_c), never its source: a derived value carries its OPERAND's `src`, so keying by source would
   silence the operand too, which is the chain-inversion §Re-execution forbids arrived at by accident. The fact
   is per-flow and rides the same constraint chain as a pin — inherited by a fork, carried across a freeze, and
   re-derived by a replay, because a resumed flow re-reaches the branch that proved it.
   IT DOES NOT REACH BACK. A value computed BEFORE the branch baked its example in on a prefix both arms share
   and keeps it; only reads TAKEN AFTER the gate degrade, which is what makes the sentence above true of
   gate-dependent values and false of gate-independent ones. */
void        concolic_contradict_example(const char *ident);
/* What this flow has proven `hole` is NOT — borrowed, valid until the running flow's constraint next grows,
   and `*n` is 0 with a NULL return when the flow proved nothing about it. That is a POSITIVE statement (no
   equality gate over this value took its false arm on the path that built this request), never a hole. */
const char *const *concolic_excluded(const char *hole, int *n);
/* THE ORDERING'S HALF OF THE SAME RULE — the exclusion's twin over an ORDERED domain, and the second-largest
   gate class a real minified bundle contains.
   An equality determines a VALUE on one arm and an exclusion on the other. An ordering determines NO value on
   either arm and a BOUND on both, which is why it needs its own recorder rather than a third state on the pin:
   §@H forbids inventing `6` for `x > 5` and requires stating `{int>5}`, and those two sentences are the same
   sentence — the line between a pin and a domain is whether a VALUE was determined, never whether a constraint
   was. `rel` is normalised SUBJECT-ON-THE-LEFT by the hook, `num` is the literal the page wrote, and `txt` is
   that literal's own §6.1.6.1.20 Number::toString, kept so no consumer re-spells a double into a number the
   source file does not contain.
   WITHIN ONE FLOW REPEATED OBSERVATIONS CONJOIN. `if (x > 5 && x < 100)` is two facts about one parameter and
   a record holding only one of them is a wrong report by this rule's own terms, so the two sides are stored
   apart and each keeps the TIGHTER of what it held and what arrived (an exclusive bound beating an inclusive
   one at the same number). Across flows and across sightings of one endpoint the merge is the OPPOSITE and
   lives in endpoint.c, for the reason stated there.
   IT DOES NOT TRAVEL THROUGH A DERIVATION, for the reason concolic_exclude gives: `x.length < 3` is a fact
   about the LENGTH, and carrying it onto `x` would be the recorded transform-expression §Re-execution
   forbids. */
void        concolic_bound(const char *hole, RelOp rel, double num, const char *txt);
/* THE INTERVAL THIS FLOW NARROWED `hole` TO. Returns 1 and fills `*out` when some side was observed, 0 with
   every field of `*out` cleared when none was — and that zero is a POSITIVE statement (no ordering gate over
   this value ran on the path that built this request), never a hole a caller may fill with a guess.
   `lo_txt`/`hi_txt` are BORROWED and valid until the running flow's constraint next grows. */
typedef struct {
    int has_lo, has_hi;         /* whether each side was observed at all */
    double lo, hi;              /* the numbers, for ordering two bounds against each other */
    int lo_incl, hi_incl;       /* 1 = `>=` / `<=` (inclusive), 0 = `>` / `<` (exclusive) */
    const char *lo_txt, *hi_txt;/* …and the page's own spelling of each, which is what a report prints */
} ConcolicBound;
int         concolic_bound_read(const char *hole, ConcolicBound *out);
/* THE ORDERING A COMPARISON RESULT CARRIES — the relation subject-left, its concrete side both spelled and as
   a double, and the hole key the bound is a fact ABOUT. REL_NONE when this result is not an ordering over a
   concrete finite Number, which is a positive statement: the predicate still forks, and only the bound is
   absent (see concolic_rel_hook for the two ways that happens and why each is the honest answer).
   All four are ONE observation, written together at the mint and asserted together here. */
RelOp       concolic_rel(JSValueConst v, const char **ptok, double *pnum, const char **psubj);
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
