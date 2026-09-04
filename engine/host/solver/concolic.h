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
 * NULL means this engine cannot spell the value exactly (a PAGE-CREATED object or symbol — an intrinsic is
 * named by the realm slot it occupies, see literal_ident — or a property name that would not convert). That is
 * a POSITIVE statement — never decide from it, keep both arms — and it is why absence is safe here while a
 * wrong identity is not. */
const char *concolic_ident_c(JSValueConst v);

/* THE PREDICATE A BRANCH OVER THIS VALUE IS ASKING ABOUT — a SECOND name, and a different question from the
 * identity above, which is why it is a second accessor rather than a refinement of one.
 *
 * `concolic_ident_c` names THIS VALUE and is what a DERIVATION composes from. This names the value whose truth
 * this one's truth is a function of, and is what the per-flow path constraint is keyed by. For everything that
 * is not a negation the two are the SAME string, and that is not a fallback filling a hole: a value IS the
 * predicate a branch over it tests. They part company at exactly one operator. `!p` is a new VALUE (a program
 * holds it, concatenates it, passes it — so `"" + !p` must not compose to `"" + p`'s key) and it is not a new
 * PREDICATE: `if (!p)` asks what `if (p)` asks, with the arms swapped, because §7.1.2 ToBoolean stands between
 * the value and the branch in both spellings alike. Giving the negation its own constraint key would make one
 * flow fork TWICE over one gate and then stand on two arms that contradict each other — sound, since it only
 * over-explores, and a real loss of the feasible-refinement that makes forced multi-path execution tractable.
 * So the polarity rides the ARM and never the key, and `concolic_branch_neg` is the half that carries it: 0 =
 * this value IS that predicate, 1 = its complement. `!!x`, `Boolean(x)` and `x` therefore reach ONE entry.
 * NULL/0 for an operand this engine cannot spell — no key, no polarity, both arms kept, which is the same
 * positive statement `concolic_ident_c`'s NULL makes. */
const char *concolic_branch_ident_c(JSValueConst v);
int         concolic_branch_neg(JSValueConst v);

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
   concolic_install_hooks — AND BEFORE THE AGENT'S FIRST REALM, which core/realm.c asserts. A per-realm
   intrinsic that mints through concolic_source_wrap freezes whatever answer is standing when the realm is
   built, so a host that installs this after platform_agent_init has a realm whose environment members are
   bare-concrete for the whole session and fork at none of their gates. */
void concolic_install_source_overlay(void);
/* …AND THE OTHER ANSWER, WHICH IS A STATEMENT AND NOT A SILENCE. A conformance host declares this instead, so
   that "nobody has decided yet" is a THIRD state a seam can crash on rather than a boolean's zero. Same
   position: before the agent's first realm. */
void concolic_declare_browser_only(void);
/* WHETHER EITHER OF THE TWO ABOVE HAS BEEN SAID. For the one caller that must refuse to proceed without an
   answer rather than take one — core/realm.c's realm_install_intrinsics, which is the moment a per-realm mint
   becomes permanent. */
int concolic_source_overlay_declared(void);
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

/* THE PREDICATE AN ENUMERATION OF AN UNKNOWN KEY SET FORKS ON — one question, one speller, asked by the
 * CONSUMER because this class's own internal method cannot ask it and cannot answer it either.
 *
 * WHY THE FORK IS NOT AT [[OwnPropertyKeys]], WHICH IS A FACT ABOUT THE RETURN TYPE AND NOT A GAP IN THIS
 * FILE. ECMAScript §10.1.11 [[OwnPropertyKeys]] ( ) "takes no arguments and returns a normal completion
 * containing a List of property keys", and §6.1.7 The Object Type says what may be in that List: "A property
 * key is either a String or a Symbol." §6.1.7.3 Invariants of the Essential Internal Methods states the same
 * thing as a requirement on this method's own answer — "Each element of the returned List must be a property
 * key". THE ARM IS STILL EXPRESSIBLE AND THAT IS NOT WHY THE FORK MOVED — an earlier statement of this rule
 * said a member of unknown NAME had no representation in the returned List in any arrangement of it, and that
 * was wrong: an unknown key in this engine DENOTES a real String, its own shape (concolic_key_name_hook), which
 * is what makes `o[x] = 1` then `o[x]` find one slot, so a member named that way is an ordinary String key and
 * §6.1.7.3 is satisfied outright. What the internal method genuinely cannot do is FORK: it is reached from
 * inside a C activation with no flow base under it, so there is no resume point to snapshot a sibling at, and a
 * fork with no resume point is not a fork. That is the whole of why the question is asked at the CONSUMER —
 * a step machine's request has a suspension point and this method has none.
 *
 * THE TWO ARMS, AND WHY NEITHER OF THEM IS A DEFAULT. Arm 0 is the world in which the record holds no own
 * member this run can name — the empty List, which is a real world (the payload was `{}`) and is the world a
 * program branching on `Object.keys(x).length === 0` needs to have run. Arm 1 is the world in which it holds
 * one, and it is PERFORMED by concolic_own_key_mint below rather than answered here. The empty List is a wrong
 * answer only when it is stated as a FACT; as an arm this flow decided and recorded, it is the honest half of a
 * fork.
 *
 * SO THE CONSUMER ASKS AND THIS CLASS READS THE ANSWER BACK, over ONE key. Returns a predicate value to hand
 * to the seam that has a resume point — a step machine's `step_tobool_run` (quickjs-step.h), which is the
 * BRANCH seam and therefore keys by this value's own identity, which is the key `decide_value_arm` reads back
 * here; never the OUTCOME seam, which keys by (operand, operation, completion) and would file the same
 * question under a second name. JS_UNINITIALIZED when this enumeration has no question in it — an ordinary
 * object, or a record whose EXAMPLE holds the answer, whose own key set is an observation and not an unknown.
 * It carries no
 * `src` and no `root` for concolic_new_conj's reason exactly: it is a boolean this engine COMPUTED and it pins
 * nothing. It carries no EXAMPLE either, which is the same positive statement — nothing this run observed says
 * which arm is real, so both are explored and neither is marked forced. */
JSValue concolic_own_keys_pred(JSContext *ctx, JSValueConst record);

/* THE PREDICATE'S TRUE ARM, PERFORMED — JSConcolicHooks.own_key_mint, which states the contract. The record's
 * `n`-th unknown own member becomes an ORDINARY SLOT on it: the name is the shape of a derivation composed over
 * the record's own identity and `n` through concolic_new_derived — the one derivation speller, so nothing here
 * invents a name and nothing spells a second one — and the value is what concolic_exotic_get answers for that
 * name, so the member reads exactly as any other member of the record does and an @S candidate substituted for
 * it lands through the same door.
 *
 * WHY A SLOT AND NOT A KEY HANDED TO THE ENUMERATION. A slot is in the ordinary key walk, so §10.1.11's List
 * carries it with no consumer changed at all — §20.1.2.11.1 GetOwnPropertyKeys ( value, type )'s String/Symbol
 * filter classifies it correctly because it IS a String, and the enumerable-key cursor's per-key
 * [[GetOwnProperty]] re-check finds it rather than dropping it as gone between the snapshot and the read.
 * A slot is also what makes the arm PER-FLOW: an ordinary property creation is what the COW delta captures, so
 * the sibling that took the empty arm never sees the member.
 *
 * STRING AND NOT SYMBOL IS THE CHANNEL'S OWN RULE, not a coin toss over §6.1.7's two kinds: these records stand
 * for a server writing a record of fields into the page, and JS_AtomIsPublishedName states that a Symbol is not
 * on that channel.
 *
 * IDEMPOTENT IN `n`: the name is a function of the record and `n` alone, so a second enumeration in the same
 * flow replays the same answers and re-materialises the same members rather than growing the record. Returns 1
 * when the member is on the record, 0 when `record` is not one of this class's, -1 having thrown. */
int concolic_own_key_mint(JSContext *ctx, JSValueConst record, int n);

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

/* …AND WHETHER THAT SUBSTITUTION HAS ACTUALLY HAPPENED YET IN THE RUNNING FLOW. Installing a substitution says
   what a source read WILL return; this says whether one has been reached. They are different facts and only
   the second one makes the payload's bytes exist in the page's program.
   IT IS THE PRECONDITION OF EVERY OBSERVATION ABOUT A CANDIDATE'S BYTES — see the definition in concolic.c for
   the measurement it was being taken without, and §@S for why a rung whose zero and whose absence read alike
   is the defect. Per flow, carried by a fork, and RE-EARNED across a park rather than resumed, because a
   resumed candidate replays the document and reaches its own source read again.
   IT IS NOT MONOTONE, AND THIS LINE USED TO SAY IT WAS ("a payload cannot leave a program"). A payload cannot
   leave the program it entered, but a flow can be given a DIFFERENT program: decide_enter clears this
   component's per-flow state for a flow entered fresh, so a candidate RESTARTED rather than resumed reads 0
   again and is right to — its bytes are not in the replay it is now running, and a sink entry that believed
   otherwise would measure the page's own text for the second time this fact exists to prevent.
   THE MONOTONE QUANTITY IS THE OTHER ONE, AND THE TWO ARE DELIBERATELY NOT COLLAPSED: §@S's ladder records the
   same event as flow.h's FLOW_RUNG_DELIVERED, where it must NEVER be lowered, because a comparator that a
   restart demotes lets a flow lose rank for being re-entered. One event, two quantities, opposite reset rules;
   concolic.c's writer states the split at the site.
   A `0` IS A POSITIVE STATEMENT: nothing in this program is this flow's, so any string that resembles the
   candidate is the page's own text. */
int         concolic_candidate_delivered(void);

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
 * semantics rather than by a second test that would have to enumerate the string builtins. What the forgeable
 * half DOES state is its DOMAIN, and that is concolic_strpred_file below, which reaches it without enumerating
 * one either — the sentence above is about the PRINCIPAL rule and never about the report.
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
/* WHAT KIND OF PRIMITIVE A COMPARISON'S CONCRETE SIDE IS — carried BESIDE its spelling everywhere the spelling
   goes, because a spelling alone is not a value and the pin store's whole job is to hand a later read the
   VALUE back.
   THE DEFECT IT CLOSES IS NOT A PARTIAL ANSWER, IT IS A CONFIDENT WRONG ONE. §Solver-half's CONCRETIZE-ON-PIN
   argues that "once `x==='admin'` pins the value, a later READ of that source returns the pinned bytes, so a
   later branch on it is decided by RUNNING the real predicate on a real string and does not fork at all" —
   and that argument holds exactly while the pinned bytes ARE the value. A pin token is §7.1.19 ToString of the
   operand the page wrote, so `x === undefined` spelled `"undefined"` and a read that answered with those nine
   CHARACTERS handed the interpreter a truthy string where the flow had proved the value falsy: `typeof x`
   answered "string", `if (x)` took the arm the gate had just disproved, and `x + 1` composed "undefined1".
   That is worse than having no pin at all — an absent pin FORKS and explores both worlds, while a wrongly
   typed one decides one arm and is never contradicted. `x === undefined` and `x === 0` are among the most
   common predicates a minified bundle writes, so the type is not an edge of this mechanism, it is half of it.
   THE STORE HOLDS TEXT AND MUST GO ON HOLDING IT, which is why this is a tag beside the spelling rather than a
   value: a pin rides a per-flow constraint that is FROZEN into structurally-shared segments at every fork and
   parked while another flow runs, and a live JSValue crosses neither a fork's freeze, an instance, a session
   nor a park. (tag, §7.1.19 spelling) is a total encoding of every primitive this engine may name, and the
   read-back is the spec's own inverse — §7.1.4 ToNumber for a Number, run by the engine rather than by a
   hand-rolled parser. */
typedef enum {
    CONCOLIC_LIT_NONE = 0,   /* an Object or a Symbol: no spelling this engine may take — see literal_tok */
    CONCOLIC_LIT_STRING,
    CONCOLIC_LIT_NUMBER,
    CONCOLIC_LIT_BOOL,
    CONCOLIC_LIT_NULL,
    CONCOLIC_LIT_UNDEFINED,
    CONCOLIC_LIT_BIGINT
} ConcolicLit;
/* A comparison-result bool for a component whose IDL member IS a comparison. `kind` is what the member
   compares against and is STATED rather than assumed: it composes the predicate's key beside `tok`, so a
   component that compared against a number while this entry spelled a string would compose the key of a
   comparison against the string `"5"` — a different predicate, decided independently of the one the page's
   own `x === 5` decides, and pinning the source to two characters. */
JSValue     concolic_new_cmp(JSContext *ctx, const char *src, int op, ConcolicLit kind, const char *tok);
/* …AND ITS TWIN FOR A RELATION OVER TWO LIVE VALUES, either of which may be unknown — for a browser component
   whose own algorithm compares two operands (HTML §8.7's timer task source orders one expiry against another,
   and the expiry of a timer set with unknown external input is unknown). `op` NAMES THE SPEC RELATION being
   performed, because the interpreter's entry (concolic_rel_hook) takes quickjs's own opcode for the operator
   and quickjs exports no such number — a component that invented one would be spelling a second opcode
   namespace. The identity is composed HERE from that name and BOTH operands, so decide.c stays the one speller
   of a constraint key. The result is a predicate value to hand to solver_decide; it pins nothing (an ordering
   determines no value, and an equality against an unknown has none to pin to). Operands are BORROWED. */
JSValue     concolic_new_rel(JSContext *ctx, const char *op, JSValueConst a, JSValueConst b);
/* …AND THE MINT OVER TWO PREDICATES, for a component whose own algorithm answers ONE boolean whose truth is
 * the conjunction of two undecided comparisons over DIFFERENT operand pairs — CSS Typed OM 1 §4.3.1 Common
 * Numeric Operations, and the CSSNumericValue Superclass's `equals`, over two items whose `value` internal
 * slots are two different unknowns. Binary; a fold over the pair gives the N-ary case, and this mint's set
 * semantics are what make the fold's shape not matter.
 *
 * WHY THE COMPONENT CANNOT ANSWER THIS ITSELF. It must hand ONE value back for the page's own `if` to fork
 * on. Deciding either comparison inside the member would fork from a plain C activation, which has no flow
 * base under it for a sibling to be snapshotted at, and would fork over a value the page may never branch on
 * at all. Deriving the answer from ONE operand through the builtin seam is worse: it would file the pair's
 * question under one conjunct's key, so `p ∧ q` and `p ∧ r` would be one constraint entry and the flow's
 * record of either would decide the other — the collapse §Solver-half's "keyed by the PREDICATE's own
 * identity … so the flow's record of one predicate decides that predicate and never its neighbour" forbids.
 *
 * THE IDENTITY IS THE SET OF CONJUNCTS, CANONICALLY ORDERED AND FLATTENED — the same rule
 * concolic_source_wrap_joint states for a joint provenance, and for the same reason. `∧` is commutative and
 * associative over booleans, so `p ∧ q`, `q ∧ p`, `(p ∧ q) ∧ r` and `p ∧ (q ∧ r)` name at most two
 * propositions between them and must name at most two keys. Without that, ONE flow could take the true arm
 * of `p ∧ q` and the false arm of `q ∧ p` — one proposition, two keys, a world holding both answers, every
 * arm in range and every assert on the path satisfied. The order is `strcmp` over the members' identities,
 * which are TEXT composed from program facts, so it is the same order on the flow that minted it, on the
 * flow that resumes it from the cold tier, and in the next session.
 *
 * A DUPLICATE IS DEDUPLICATED HERE WHERE A JOINT PROVENANCE'S IS A CRASH, AND THE DIFFERENCE IS WHO COULD
 * HAVE KNOWN. A joint domain's members are a list its caller ASSEMBLED, so a repeat is that caller having
 * lost track of its own inventory. These two are VALUES, and whether they are the same proposition is a
 * question about their identities that only this mint holds both of — pushing it back would put the
 * comparison in every caller, which is the one-fact-answered-from-many-places defect, and a fold over N
 * items is exactly the caller that cannot answer it. `p ∧ p` IS `p`, so a set that collapses to one member
 * comes back as that predicate rather than as a second key naming it.
 *
 * IT DECOMPOSES NOTHING, WHICH IS THE SOUND DIRECTION AND NOT A LIMITATION. The true arm of `p ∧ q` proves
 * both conjuncts and the false arm proves neither, and this engine files ONE entry for the conjunction and
 * reasons inside none of them: propagating the true arm into `p`'s own entry would need the SAT layer
 * §Re-execution forbids, and `p ∧ ¬p` therefore forks over a question that is unsatisfiable — which errs
 * toward MORE exploration, exactly the direction §Headless requires.
 *
 * IT CARRIES NO `src` AND NO `root`, AND THAT IS A POSITIVE STATEMENT RATHER THAN A DROPPED FIELD. Both are
 * facts about where BYTES entered and where an @S candidate substitutes; a conjunction is a boolean this
 * engine COMPUTED from two predicates that may be about two different holes, so there is no single answer
 * and naming one operand's would state half a provenance as the whole of it. The other predicate mints carry
 * them only because a PIN needs them, and a conjunction pins nothing.
 *
 * Both operands are BORROWED and must be unknown (an already-decided boolean is one the caller folds away by
 * handing back the other). The example is the real `&&` run on the two examples where both have one. */
JSValue     concolic_new_conj(JSContext *ctx, JSValueConst a, JSValueConst b);
/* THE PIN A TAKEN ARM CAN PERFORM, and nothing else — OPCMP_EQ/NE with the source and the concrete token, or
   OPCMP_NONE. An ordering answers NONE because it pins nothing (`x > 5` narrows a domain and determines no
   value; §Solver-half keeps that a domain-annotated shape rather than inventing a 6), and so does an equality
   whose OTHER side is also unknown, because there is no concrete value to pin to. The PREDICATE itself is not
   read through here: it lives in the value's identity, composed from the operator and both operands, which is
   what decide.c keys the constraint by.
   `pkind` IS THE TOKEN'S OTHER HALF and is answered wherever `ptok` is: a caller that took the spelling and
   not the kind is about to pin nine characters where the page proved `undefined`. It answers
   CONCOLIC_LIT_NONE exactly when the result is OPCMP_NONE. */
int         concolic_cmp(JSValueConst v, const char **psrc, ConcolicLit *pkind, const char **ptok);
/* …AND WHICH EQUALITY THE PAGE WROTE FOR IT, WHICH IS A FIFTH FACT ABOUT THE SAME OBSERVATION AND NOT A
   SPELLING OF THE OPERATOR. The four above say an arm determines something, what it determines, what that
   spells and which hole it is about; this says under WHICH ALGORITHM, and the answer changes what an arm is
   entitled to conclude. Asked only of a value `concolic_cmp` has just answered OPCMP_EQ or OPCMP_NE for — a
   predicate that is not an equality has no algorithm and this aborts rather than inventing one, so "not an
   equality" is unrepresentable in the return type instead of being encoded as a member of it.
   IT IS NOT READ OFF THE IDENTITY. `cmp_op_ident` composes `==`/`===` into the predicate's key, so the
   algorithm is recoverable from that string — by MATCHING it, which is what §RUN-DON'T-MATCH forbids and what
   a key-format change would silently break. The fact is recorded as a fact. */
JSConcolicEqOp concolic_cmp_algo(JSValueConst v);
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
/* JSConcolicHooks.to_bool — §7.1.2 ToBoolean ( arg ) over unknown input, and §13.5.7's Logical NOT with
   it (`negate`). Same rule as .arith and .to_str: the operator answers because the conversion boundary owes C
   a real boolean, and for a concolic that boundary answers from the REPRESENTATION — an ordinary object, so
   `true`. The result carries the operand's own predicate as its BRANCH KEY with the polarity beside it, which
   is what makes `if (p)` and `if (!p)` one constraint entry; see concolic_branch_ident_c. */
JSValue     concolic_tobool_hook(JSContext *ctx, JSValueConst v, int negate);
/* JSConcolicHooks.key_read — `obj[x]` with an unknown key reads an unknown property. */
JSValue     concolic_key_read_hook(JSContext *ctx, JSValueConst obj, JSValueConst key);
/* JSConcolicHooks.key_name — the real string an unknown key denotes (its shape), stable per source. */
JSValue     concolic_key_name_hook(JSContext *ctx, JSValueConst key);
/* JSConcolicHooks.key_value — that trade UNDONE, where a property NAME is handed back to the program as a
 * VALUE. `key_name` spends an unknown's provenance and domain to buy an atom, which is the honest price of
 * ECMAScript §6.1.7 The Object Type's "A property key is either a String or a Symbol." — an atom cannot carry
 * an unknown. It is the right price at every site that wanted BYTES (a lookup, a `@WHY`'s display form,
 * concolic_name_cstr's selector), and a LOSS at an ENUMERATION, which is the one site that wanted a VALUE:
 * `for (k in o)` and `Object.keys(o)` hand the composed bytes to the page, so `k === "admin"` is decided by
 * comparing two concrete strings and PRUNES an arm nothing contradicts, and `fetch("/api/" + k)` emits a
 * concrete path segment no run computed — §@H's wrong report rather than a partial one, one step worse than
 * the case it names, because nothing marks these bytes as a shape at all. §solver bans this shape by name in
 * the JSON.stringify case: taint is preserved, never a de-tainting placeholder.
 * Answers JS_UNINITIALIZED for every string this file did not mint for an unknown key, which is every atom the
 * parser, the bytecode and the engine's own vocabulary produce.
 *
 * NAMED RESIDUAL — NOT COVERED: four other conversions of a property name to a value the program observes.
 * §10.5.11 [[OwnPropertyKeys]] ( )'s CreateArrayFromList over a validated `ownKeys` trap result; the same
 * List built for a non-Proxy (`Object.getOwnPropertyNames`, `Reflect.ownKeys`); the `prop` ARGUMENT a Proxy
 * handler is called with; and §25.5.4 JSON.stringify ( value [ , replacer [ , space ] ] )'s key argument to a
 * replacer. Each still takes JS_AtomToValue, so each delivers the composed bytes. This entry is CORRECT for
 * what it covers and narrower than the rule, because the three §7.3.23 EnumerableOwnProperties ( obj, kind )
 * and §14.7.5.10.2.1 %ForInIteratorPrototype%.next ( ) seams hand their result straight to the page while the
 * ownKeys List re-enters §10.5.11 steps 7-22's own invariant checks, which read it back as keys — so routing it is a
 * question about that machine and not one line.
 * WHAT THE NEXT DIFF BUILDS: those four sites take quickjs.c's `js_enum_key_value` — the one speller the three
 * covered seams already route through — with §10.5.11's List asserted to survive its own re-read as the same
 * atom, since a restored unknown must spell the name it was minted from or the invariant check is comparing
 * two vocabularies.
 * HOW ITS ABSENCE SHOWS: `o[location.hash] = 1; Object.getOwnPropertyNames(o).forEach(k => fetch("/a/" + k))`
 * emits a concrete `@H` path segment carrying the shape text, where the same program written with
 * `Object.keys` emits a domain-annotated one. */
JSValue     concolic_key_value_hook(JSContext *ctx, JSValueConst name);
JSValue     concolic_builtin_hook(JSContext *ctx, JSValueConst v, const char *op, JSValue example);
/* …AND THE SAME DERIVATION OVER SEVERAL OPERANDS AT ONCE — the VALUE twin of concolic_new_rel, for a component
 * whose own algorithm COMPUTES a result from more than one operand and cannot say which of them decided it.
 *
 * IT EXISTS BECAUSE ONE-OPERAND DERIVATION IS THE WRONG ANSWER AND WAS WRITTEN DOWN AS SUCH. concolic_builtin_hook
 * composes `(operand, operation)`, so a component with two unknown operands had to pick one and name it, and two
 * flows differing only in the operand it did NOT pick then share a derivation identity — one constraint entry
 * for two propositions, so the second flow's gate is decided by the first's record. That is not a hypothesis:
 * core/html/html_progress.c and core/html/html_meter.c each carried a named residual asking for exactly this
 * entry, and CSS Typed OM 1 §4.3.1's arithmetic reaches it on its first line (`CSS.px(x).add(CSS.px(y))` is
 * defined as "the sum of the value internal slots", over two slots either of which may be unknown).
 *
 * THE IDENTITY IS THE OPERANDS **IN ORDER**, WHICH IS WHERE THIS PARTS COMPANY WITH concolic_new_conj. That mint
 * composes a flattened, sorted, deduplicated SET, and it is right to, because `∧` is commutative and
 * associative so `p ∧ q` and `q ∧ p` name ONE proposition. An arithmetic or spec-algorithm derivation is not:
 * `a - b` and `b - a` are two values, `min(a, b)` and `min(b, a)` are one value only for operations the caller
 * happens to know are commutative, and a mint that sorted would answer BOTH subtractions from one key — the
 * two-arms-of-one-proposition defect with the halves swapped. So the order the caller passes is the order the
 * key states, and a caller whose operation IS commutative gains nothing by sorting: two orderings of one
 * commutative operation are two keys for one value, which costs an extra frontier entry and decides nothing
 * wrongly, where one key for two values decides a gate the flow never asked about.
 *
 * IT IS ONE SPELLER WITH concolic_builtin_hook AND NOT A SECOND ONE. That entry is this composition at n == 1 —
 * `(ident, op)` under the same tag — and it now calls this, so a derivation named through either spelling
 * composes the same bytes and no existing constraint key moves. n MUST BE AT LEAST 1 and the DISPLAY shape is
 * the only thing that branches on it: `{x}.op()` reads as a method on the operand and is what every existing
 * one-operand derivation already renders as, while `op({x}, {y})` is the only honest rendering of a result
 * neither operand is the subject of.
 *
 * `op` NAMES THE SPEC ALGORITHM, not the C operator, for concolic_new_rel's reason: the name IS half the key,
 * so two different operations over one operand list must not compose to one identity.
 *
 * `src` AND `root` COME FROM THE FIRST UNKNOWN OPERAND, which is the rule concolic_arith_hook already applies to
 * the interpreter's own `x + y` (it reads both from `a` when `a` is unknown and from `b` otherwise). Matching it
 * is the point: a component performing an addition and a page performing the same addition must state the same
 * provenance, or one @H record splits into two that do not compare.
 *
 * The operands are BORROWED; `example` is CONSUMED and is the REAL operation run by the CALLER on the operands'
 * own examples (concolic_example) — never predicted here, and JS_UNDEFINED where any operand has none, because
 * @H never invents. Answers JS_UNINITIALIZED when NO operand is unknown, exactly as concolic_builtin_hook does
 * for a known operand: the caller already computed the answer and there is nothing to derive. */
JSValue     concolic_new_derived(JSContext *ctx, const char *op, const JSValueConst *operands, int n,
                                 JSValue example);
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
   `message.origin` would make a later read of `event.origin` itself answer the lowercased token.
   `kind` SAYS WHAT `val` SPELLS, and the two are one argument in two halves — see ConcolicLit. The store may
   only hold what the read-back can mint back as the REAL value, so this refuses a kind it cannot reproduce
   rather than recording a spelling for it: a source that is not pinned simply forks again at its next gate,
   which is sound and is what the engine did before any pin existed.

   `algo` NAMES WHICH EQUALITY HELD, AND IT DECIDES ONLY ONE OF THE TWO WRITES ABOVE. They are one ACT and
   they are not one DECISION: the value pin is a DETERMINATION, and the root mark is a DEMAND.
   UNDER `===` THE HOLDING ARM DETERMINES THE VALUE. §7.2.14 IsStrictlyEqual ( x, y ) step 1 is "If
   SameType(x, y) is false, return false", so the arm is reachable only where the operand IS the token.
   UNDER `==` IT DETERMINES NOTHING, FOR EVERY KIND THIS STORE CAN SPELL, and that is a reading of §7.2.13
   IsLooselyEqual ( x, y )'s fourteen steps rather than a caution. Step 2 is "If x is null and y is undefined,
   return true" and step 3 its converse, so `x == undefined` holds for BOTH of two values. Steps 5 and 6 admit
   a String against a Number token and the reverse; steps 9 and 10 send a Boolean through ToNumber; steps 7 and
   13 admit a BigInt. And step 12 — "If x is an Object and y is either a String, a Number, a BigInt, or a
   Symbol, return ! IsLooselyEqual(? ToPrimitive(x), y)" — admits an OBJECT against every token kind there is,
   by running the PAGE's own valueOf. So the holding arm leaves a SET in every case, and pinning one member of
   it picks a WITNESS: §@H's line is whether a VALUE was determined, never whether a constraint was, and
   choosing `undefined` out of {undefined, null} is the same fabrication as inventing 6 for `x > 5`.
   IT IS WORSE THAN A WRONG REPORT BECAUSE OF CONCRETIZE-ON-PIN. A pinned source reads back as the pinned
   value, so every LATER branch over it is decided by running the real predicate instead of forking — the
   witness does not merely get emitted, it deletes the sibling world the run never contradicted. An absent pin
   forks and explores both; a wrong one decides an arm nothing downstream can contradict.
   THE DEMAND IS MADE UNDER BOTH, AND REFUSING IT WOULD MANUFACTURE A FALSE PoC. `if (e.origin == TRUSTED)` is
   a spelling real handlers write, and a principal is a String while the token is a String — so §7.2.13 step 1
   ("If SameType(x, y) is true, then … Return IsStrictlyEqual(x, y)") hands that comparison straight to
   §7.2.14, and the check is exactly as unforgeable cross-origin as `===`. `concolic_principal_pinned` reads
   this mark and solve.c suppresses on it, so gating the mark on the algorithm too would un-suppress every
   loose origin check in every bundle — §Attacker-sources' "never a false PoC" failing in the one direction
   that emits one. The demand is that the flow required a particular value OF the principal; that a loose
   equality bounds rather than determines it does not make it less of a demand.
   AN UNSTATED ALGORITHM TAKES THE REFUSED ARM. JS_CONCOLIC_EQ_LOOSE is quickjs.h's zero, so a caller that
   forgets refuses the value and keeps the demand — forgetting is not a way to be exempted.
   THE NARROWING THE LOOSE ARM DID OBSERVE IS FILED ELSEWHERE, and this function is not where a reader should
   look for it. Refusing the VALUE is the whole of what this line does; the SET §7.2.13 leaves is a real fact
   the flow observed and `concolic_looseeq` below records it, keyed by the HOLE the report prints rather than
   by the `src` a read concretizes through — which is the reason it is a separate recorder and not a third
   state here. So the two arms of one loose gate now state one thing each: the failing arm an exclusion, the
   holding arm the predicate that held.
   NAMED RESIDUAL — A BIGINT IS NOT PINNED. What is not covered: `x === 5n` on its true arm records nothing, so
   a bundle gating on a BigInt literal re-forks every later branch over that source instead of deciding it.
   What the next diff builds: a quickjs export that mints a BigInt from its §6.1.6.2.21
   BigInt::toString ( x, radix ) spelling — §7.1.16 StringToBigInt ( string )'s own inverse. The widest thing
   quickjs.h exports toward a BigInt is JS_NewBigInt64, so an arbitrary-precision literal has no route back
   into a value at all and the int64-only half-route would make the pin depend on the literal's MAGNITUDE,
   which is a value-dependent behaviour split rather than a narrower mechanism. How its absence would show: two
   sibling flows for every repeated `===` against one BigInt-valued source, where a string-valued source
   produces one; the fork census names the predicate. */
void        concolic_pin(const char *src, const char *root, ConcolicLit kind, const char *val,
                         JSConcolicEqOp algo);
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
/* THE EQUALITY'S THIRD ARM — WHAT A **LOOSE** EQUALITY'S HOLDING ARM ESTABLISHES, which `concolic_pin` refuses
 * to record and `concolic_exclude` is the wrong polarity for.
 *
 * §7.2.14 IsStrictlyEqual ( x, y ) step 1 is "If SameType(x, y) is false, return false", so a `===` that HELD
 * determined the operand and the pin IS the record of it. §7.2.13 IsLooselyEqual ( x, y ) coerces instead, so
 * its holding arm leaves a SET — and `concolic_pin` therefore writes no value for it, which is right and
 * leaves the fact unfiled. That silence is the defect: §@H's shape states PROVENANCE and DOMAIN, and "the
 * failure is asymmetric — a shape carrying provenance alone renders an UNCONSTRAINED parameter and a
 * range-gated one with identical bytes, so its silence about the gate is read as the positive statement
 * 'anything goes'". The true arm of `x == 0` emitted its parameter with the same bytes as a parameter nothing
 * ever tested, while its own SIBLING carried an exclusion — two arms of one observation disagreeing about
 * whether a gate was seen at all.
 *
 * IT INVENTS NOTHING, WHICH IS THE WHOLE OF WHY IT IS NOT THE PIN UNDER ANOTHER NAME. §@H's line is "whether a
 * VALUE was determined, never whether a CONSTRAINT was": the operand is the concrete side the PAGE wrote, the
 * arm is the arm this flow took, and no member of the set is chosen. A pin says "the value IS this"; this says
 * "the page's own `==` against this answered true here", which is a fact about the PREDICATE and not about the
 * value.
 *
 * WHAT THE HOLDING SET *IS* DEPENDS ON THE TOKEN'S KIND, AND THAT IS EXACTLY WHY THIS RECORD DOES NOT STATE
 * IT. Under §7.2.13 an `undefined` token admits {undefined, null} (steps 2 and 3, plus step 4's [[IsHTMLDDA]]
 * object in a web host); a Number token admits every String whose §7.1.4 ToNumber is that number (step 6),
 * `false`/`true` through steps 9 and 10, a BigInt through step 13, and — step 12 — every Object whose
 * ToPrimitive lands there, which runs the PAGE'S OWN valueOf; a String token admits Numbers and BigInts that
 * spell it (steps 5, 7 and 8) and objects again. Six kinds, six different readings, and the Object arm of every
 * one of them is decided by code that is not running by the time a report is composed. So a consumer that
 * rendered the SET would be re-implementing §7.2.13's fourteen steps beside the engine that already runs
 * them — a second copy of the spec's own table, and the one thing §RUN-DON'T-MATCH forbids most plainly. What
 * is carried is the PREDICATE the page wrote, and the reader reads it as JavaScript: `== 0` says what `== 0`
 * says. ONE recorder therefore serves every kind, and the kind is a FIELD OF THE ROW rather than a choice of
 * recorder.
 *
 * `kind` IS NOT DECORATION ON `tok` — IT IS THE HALF THAT MAKES THE ROW A DIFFERENT DEMAND. `x == undefined`
 * demands a value that is null or undefined, which for a query parameter is the demand that it be ABSENT;
 * `x == "undefined"` demands the nine characters. §7.1.19 ToString flattens the two onto one spelling, exactly
 * as it does for the pin, so a row that carried the spelling alone would state one of them and mean the other.
 * It is the same pair `concolic_pin` takes and it is asserted the same way.
 *
 * IT IS NOT `concolic_strpred_file`, AND THE THREE REASONS ARE WHY THIS IS A THIRD RECORDER RATHER THAN A
 * FOURTH KIND OF ROW IN THAT ONE. (1) `ConcolicCallPred.method` is "the property NAME the page read off the
 * unknown — an atom, never a guess", and `==` is an OPERATOR: nothing was read off the value, so a row spelling
 * `==` there states of the program a thing the program does not contain. (2) The two classes' ARMS are filed
 * differently — a call predicate files BOTH arms into its own set, while a loose equality's failing arm is
 * already `concolic_exclude`'s, so a `holds:false` equality row is a value no producer can ever emit and would
 * be a dead branch in every consumer of it. (3) This row must carry its operand's KIND, which is meaningless
 * for a call and would put a field on every call-predicate row that answers a question that row is never
 * asked — §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS, manufactured rather than met.
 *
 * IT IS ALSO NOT `concolic_bound`, WHICH IS THE NEAR MISS. An interval is a claim about a NUMBER, and
 * `x == 0` holding is not one: it admits `""`, `false` and `[]`, so writing lo=hi=0 would state that the value
 * IS the number zero — the WITNESS this whole rule exists to refuse, arriving through a different recorder.
 * And `x == undefined` has no double to file at all, which `concolic_bound`'s own `txt && *txt` assert says.
 *
 * A BIGINT TOKEN IS RECORDED HERE THOUGH `concolic_pin` REFUSES ONE, and the two are consistent rather than in
 * tension: the pin's read-back must MINT the value again (`pin_mint`, which quickjs.h exports no route to for
 * an arbitrary-precision literal), while this must only PRINT it. A row states a fact; a pin answers a read.
 *
 * Keyed by the HOLE KEY for `concolic_cmp_subject`'s reason. Within one flow the observations CONJOIN and
 * accumulate (`if (x == 0 && x == "")` is two facts about one parameter); an exact repeat — same kind, same
 * spelling — is idempotent. Across sightings of one endpoint the merge is the OPPOSITE and lives in
 * endpoint.c, for the reason stated there.
 * IT DOES NOT TRAVEL THROUGH A DERIVATION, for the reason `concolic_exclude` and `concolic_bound` give:
 * `x.length == 0` is a fact about the LENGTH, and carrying it onto `x` would be the recorded
 * transform-expression §Re-execution forbids. */
typedef struct { char *tok; signed char kind; } ConcolicLooseEq;
void        concolic_looseeq(const char *hole, ConcolicLit kind, const char *tok);
/* WHAT THIS FLOW'S OWN LOOSE EQUALITIES HELD OF `hole` — borrowed, valid until the running flow's constraint
   next grows, `*n` 0 with a NULL return when the flow proved nothing. That zero is a POSITIVE statement (no
   loose equality over this value held on the path that built this request), never a hole a caller may fill. */
const ConcolicLooseEq *concolic_looseeq_read(const char *hole, int *n);
/* IS `p` THE SAME OBSERVATION AS (kind, tok)? ONE SPELLER, for `concolic_pred_same`'s reason exactly: it is
   asked at both ends of the pipeline — this file dedups a repeat WITHIN a flow, endpoint.c intersects sets
   ACROSS sightings — and an intersection using a stricter rule than the dedup drops from the record a claim
   every observed path obeyed. The KIND is half the comparison: `x == 0` and `x == "0"` are two gates. */
int         concolic_looseeq_same(const ConcolicLooseEq *p, ConcolicLit kind, const char *tok);
/* WHAT ONE ROW OWNS, COPIED AND RELEASED BY ONE PAIR OF LINES EACH — `concolic_pred_copy`'s rule over this
   row's own fields, and here for the same reason: three holders keep a set of these (the flow's constraint
   head, its copy-up from an inherited segment, and endpoint.c's per-request row and merged param), so a field
   added to the struct must have nowhere else it could be written. */
void        concolic_looseeq_copy(ConcolicLooseEq *dst, const ConcolicLooseEq *src);
void        concolic_looseeq_release(ConcolicLooseEq *p);
/* THE KIND AS A REPORT NAMES IT — "string", "number", "boolean", "null", "undefined", "bigint".
   IT IS NOT `lit_tag`, AND THE TWO ARE NOT A DUPLICATED TABLE: that one is the kind's one-letter field in a
   composed constraint IDENTITY, where the only property that matters is that two kinds never write one byte,
   and this is the word a reviewer reads. One fact, two questions asked of it. Both switch over the whole enum
   with no default, so a kind added to `ConcolicLit` reddens both rather than silently taking one's fallthrough.
   Aborts on CONCOLIC_LIT_NONE: a row is written only where the operand HAS a spelling, so a kindless one
   reaching an emission is a record whose own producer could not say what type the page compared against. */
const char *concolic_lit_report_name(ConcolicLit k);
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

/* THE THIRD PREDICATE CLASS — A METHOD CALL OVER AN UNKNOWN WHOSE RESULT A BRANCH TESTS, AND THE HEADLINE
 * SHAPE §@H NAMES (`{startsWith:/api}`).
 *
 * An equality determines a VALUE on one arm; an ordering determines a BOUND on both; a call determines
 * NEITHER and still narrows — `if (!path.startsWith("/api")) return;` proves, of the arm that reaches the
 * request, that the page's own `startsWith("/api")` answered true of it, and proves the negation of exactly
 * that on the sibling. Both are facts the run OBSERVED, in the same sense a pin is: the method name is the
 * atom the page read, the arguments are the literals the page passed, and the arm is the arm this flow took.
 * §@H's line is whether a VALUE was determined, never whether a constraint was, so nothing here is invented
 * and nothing is a member of anything.
 *
 * IT ENUMERATES NO BUILTIN, WHICH IS WHAT KEEPS IT ON THE RIGHT SIDE OF §RUN-DON'T-MATCH. The record does not
 * ask what `startsWith` MEANS and no consumer of it re-implements one: what is carried is the page's own
 * predicate, spelled, plus the answer the run got. A test that had to recognise the string builtins is what
 * the principal rule at the top of this header declines to build, for the same reason, and this is the other
 * half of that sentence — the forgeable half needs no recogniser HERE either.
 *
 * IT DOES NOT TRAVEL THROUGH A DERIVATION, exactly as concolic_exclude and concolic_bound do not, and here
 * that is the whole distance between this and the recorded transform-expression §Re-execution forbids.
 * `x.slice(1).startsWith("/api")` is a fact about `x.slice(1)` — a different hole with a different key — and
 * carrying it up onto `x` would be an INVERSION of `slice`, which is the banned thing under its own name.
 * Nothing composes, nothing is inverted, nothing derives a value: one predicate is observed, filed under the
 * receiver it was called on, and read once at the emission.
 *
 * A PINNED RECEIVER NEVER REACHES HERE, which is CONCRETIZE-ON-PIN doing its own work rather than a case this
 * has to handle: concolic_exotic_get answers a pinned source with the real bytes, so `x.startsWith("/api")`
 * on a pinned `x` runs the REAL §22.1.3.23 builtin over a real String and the branch does not fork at all.
 *
 * A NEGATED GATE RECORDS THE SAME FACT AS ITS POSITIVE SPELLING, AND THE MECHANISM THAT MAKES IT SO IS NOT
 * HERE. `if (!x.startsWith("/api"))` is the shape a real bundle writes most often, and it used to record
 * nothing at all — not because this side was wrong, but because §7.1.2 ToBoolean answered the branch before
 * the branch was asked: `!` compiles to OP_lnot, whose interpreter body coerces its operand, and the coercion
 * of a concolic is a coercion of its REPRESENTATION, which is an ordinary object and therefore `true`. So
 * `!unknown` was the concrete `false` and the fork never happened. The defect shape is worth keeping because
 * it recurs wherever a value class rides an engine's own tag: a coercion that answers from the representation
 * is not a coarse answer, it is a DECIDED one, and it deletes an arm with nothing to say so.
 * The operator answers now (quickjs.h's JSConcolicHooks.to_bool, concolic_tobool_hook), and the arm — never
 * the constraint key — carries the polarity, so ONE entry serves both spellings. What this file therefore
 * does NOT do, and must not be made to do, is negate the observation: an equality's OPCMP_EQ/NE, an ordering's
 * relation and a call's `holds` would each need their own inversion, and the third has none — §RUN-DON'T-MATCH
 * forbids deciding what `startsWith` MEANS, so there is no paired method to flip to. The arm is one XOR and is
 * the same statement for all three. See concolic_branch_ident_c for the key, and pred_carry_through_not for
 * why the record travels verbatim. */
typedef struct {
    const char *method;         /* the property NAME the page read off the unknown — an atom, never a guess */
    const char *const *args;    /* every argument, each as the page's own §7.1.19 ToString of it */
    int nargs;                  /* …and how many; 0 is a real answer (`x.trim()` tested as a condition) */
    const char *subject;        /* the HOLE KEY of the RECEIVER — what the emission looks a domain up by */
} ConcolicCallPred;
/* What a CALL RESULT records about the predicate it IS — the twin of concolic_rel, read at the branch.
   Returns 1 and fills `*out` when this value is a call over an unknown whose method, receiver-hole and EVERY
   argument this engine could spell; 0 with `*out` cleared otherwise, which is a POSITIVE statement: an
   argument that is a plain object or a function has no spelling, so the predicate is unnameable and the
   branch forks with no domain rather than filing a claim missing one of its operands. All of it is ONE
   observation, written together at the mint and asserted together there. Borrowed from the value. */
int         concolic_strpred(JSValueConst v, ConcolicCallPred *out);
/* ONE SUCH PREDICATE, AS THE FLOW'S CONSTRAINT HOLDS IT. `holds` is the arm — 1 where the page's own call
   answered true on this path, 0 where it answered false — and the false one is not an absence but the half
   forced multi-path produces at exactly the same rate as the true one. */
typedef struct { char *method; char **args; int nargs; signed char holds; } ConcolicPred;
/* IS `p` THE SAME OBSERVATION AS (method, args, holds)? Method, arm and EVERY argument — `x.startsWith("/api")`
   beside `x.startsWith("/admin")` is two gates and not one repeat, and the true and false arms of one gate are
   two facts and not one. ONE SPELLER, because it is asked at both ends of the pipeline and they would
   otherwise be free to disagree about which two predicates are one: this file dedups a repeat WITHIN a flow,
   endpoint.c intersects sets ACROSS sightings, and an intersection using a stricter rule than the dedup drops
   from the record a claim every observed path obeyed. */
int         concolic_pred_same(const ConcolicPred *p, const char *method, const char *const *args, int nargs,
                               int holds);
/* WHAT ONE ROW OWNS, COPIED AND RELEASED BY ONE PAIR OF LINES EACH. Three holders keep a set of these — the
   flow's constraint head, its copy-up from an inherited segment, and endpoint.c's per-request row and merged
   param — so a field added to the struct creates an obligation at every one of them, and the only way to make
   that impossible to miss is for there to be nowhere else it could be written. `concolic_pred_copy` deep-copies
   INTO an uninitialised `*dst`; `concolic_pred_release` frees what a row holds and leaves the row itself to
   whoever owns the array it sits in — which is what a mid-array `free()` would get wrong. */
void        concolic_pred_copy(ConcolicPred *dst, const ConcolicPred *src);
void        concolic_pred_release(ConcolicPred *p);
/* FILE ONE — the twin of concolic_bound, keyed by the HOLE KEY for concolic_cmp_subject's reason. Within one
   flow the observations CONJOIN and accumulate (`x.startsWith("/api") && !x.includes("..")` is two facts
   about one parameter, and a record holding one of them is a wrong report by this header's own rule); an
   exact repeat is idempotent. Across sightings of one endpoint the merge is the OPPOSITE and lives in
   endpoint.c, for the reason stated there. */
void        concolic_strpred_file(const char *hole, const char *method, const char *const *args, int nargs,
                                  int holds);
/* WHAT THIS FLOW'S OWN CALLS PROVED ABOUT `hole` — borrowed, valid until the running flow's constraint next
   grows, `*n` 0 with a NULL return when the flow proved nothing. That zero is a POSITIVE statement (no call
   over this value was branched on along the path that built this request), never a hole a caller may fill. */
const ConcolicPred *concolic_strpred_read(const char *hole, int *n);
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
