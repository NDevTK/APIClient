/* ABORTCONTROLLER / ABORTSIGNAL — DOM §3.2, and the interface CLAUDE.md names when it says the concolic value
 * belongs only where the value is UNKNOWN.
 *
 * BOTH HALVES ARE HERE, AND THEY ARE DIFFERENT KINDS OF THING.
 *
 *   A CONTROLLER'S SIGNAL IS THE REAL STATE MACHINE. `new AbortController().signal.aborted` is false because
 *   this engine created the signal and knows nothing has aborted it; `controller.abort()` sets the flag, stores
 *   the reason and fires the `abort` event. There is no ignorance to model, so a concolic here would fork a
 *   branch whose sibling cannot happen.
 *
 *   A TIMEOUT SIGNAL IS UNKNOWN. Whether `AbortSignal.timeout(5000)` has fired by the time the code asks
 *   depends on wall-clock this engine does not model, and BOTH answers lead to code worth reaching — the
 *   request path and the retry/fallback path, and the fallback is routinely a different endpoint. So its
 *   `aborted` is concolic with the example a fast machine gives (false), and every branch on it forks.
 *
 * WHICH IS WHY THE C SIDE NEVER TESTS THE FLAG ITSELF. `throwIfAborted()` and the `reason` getter both have to
 * branch on a value that may be concolic, and a C `if` would silently pick one arm — the exact failure the
 * solver exists to prevent. They ask solver_decide, the same seam an OP_if asks, so the ARM is right wherever
 * the question is asked from.
 *
 * THE ARM IS NOT THE WHOLE FORK, AND WHICH OF THIS FILE'S MEMBERS CAN SUPPLY THE REST IS NOW A PER-MEMBER
 * ANSWER. A fork also needs a place for the SIBLING to come back, and an ask made from inside a plain C
 * activation the page called into has none: there is no machine state for the other arm to be snapshotted at,
 * and re-running the flow's scheduler step does not re-reach a getter's body. So the seam CRASHES at the fork
 * naming the predicate AND the ask site (solver/engine.h engine_prepare_fork), and what that names is the
 * declaration to build — the member that tests the flag becomes a step machine and asks through the PARKING
 * form, so the driver snapshots at the ask.
 *   THE TWO MEMBERS THAT TEST IT THEMSELVES ARE CONVERTED: `throwIfAborted()` and the `reason` getter are one
 * machine with two magics (sig_ask_step), and they ask abort.h's `abort_signal_aborted_step`.
 *   AND SO IS `AbortSignal.any()`, WHOSE TEST IS NOT ITS OWN FLAG BUT EVERY INPUT SIGNAL'S — §3.2 "create a
 * dependent abort signal" step 2, which is the WHOLE of that member's method steps. It asks through
 * abort.h's `abort_signal_dependent_step`, and `js_any_def` had declared that body a machine the whole time:
 * `AbortSignal.any([AbortSignal.timeout(n), c.signal])` is the spelling a bundle writes for "whichever comes
 * first", so step 2 met the one flag §3.2 models as unknown on its FIRST element and crashed.
 *   WHICH IS THE THIRD CONVERSION IN THIS ENGINE WHERE THE CRASH'S REMEDY CLAUSE WAS WRONG IN ONE DIRECTION, AND
 * THE PATTERN IS WORTH MORE THAN ANY OF THEM. The clause says "declare that builtin a step machine
 * (JS_CFUNC_STEP_DEF)", and at `fetch()`'s §5.6 step 4, at `navigator.locks.request()`'s §3.2.1 step 9 and here
 * the builtin WAS ALREADY ONE — what was missing was never the declaration, it was that the ASK could not return
 * a fork code. The remedy is right for a body that is not a machine and it is the rarer half of the population:
 * a plain C body that branches on a signal is reached from a getter or a constructor, while the asks a real
 * bundle actually drives arrive inside machines, because the members that TAKE a `signal` are the ones that also
 * take a dictionary or an iterable and therefore had to be machines already. A reader meeting this abort checks
 * whether the body is declared BEFORE reading its remedy, and that check is one grep of the `JSTrampStepDef`.
 *   THIS USED TO SAY THEY ASK "the machine's own step_fork_run", AND IT IS KEPT IN ITS OWN WORDS BECAUSE A
 * READER WHO RE-DERIVES THE REMEDY FROM "a machine forks at its own seam" WILL NAME THAT ONE AGAIN. The seam is
 * `step_tobool_run`, which keys by the OPERAND'S OWN identity — the same constraint entry a page's
 * `if (signal.aborted)` records, so the two cannot fork twice over one predicate — where the outcome seam keys
 * by (operand, operation, completion) and would both re-fork a predicate this flow may already have fixed and
 * file a domain-less shape for a parameter the page had gated. abort.h states that correction at the parking
 * form and the crash's own remedy clause carries it.
 *   WHAT IS STILL PLAIN C AND STILL CRASHES: every ask reached through `signal_is_aborted`'s non-parking form,
 * which is §3.2 signal abort's own step 1 test (signal_abort_state), the two exported helpers this file hands the
 * rest of the engine, and the NULL-`h` arm of the dependent-signal build — whose callers are the C-vector entry's,
 * each of them pairing two locals rather than converting a page's iterable. A page that branches on an
 * `AbortSignal.timeout()` flag through one of those,
 * on a decision this flow has not already taken, aborts HERE rather than stranding a prepared sibling for some
 * later fork elsewhere in the agent to trip over, which is what it did before and why it was never traced to
 * this file.
 * THAT IS THE FORKING SESSION'S HALF, AND IT IS ONLY HALF. A session may declare that it explores nothing at
 * all — a conformance run measuring this half against a spec oracle, and §@S's candidate re-fire, which is ONE
 * concrete path — and there is then no sibling to place and no crash to reach, only a question that must still
 * be answered. The seam does not pick: it asks this file, at the ask, for the arm §3.2's own model takes with
 * one world in it (signal_aborted_nonforking, below).
 *
 * THE INTERNAL SLOTS ARE AN OWN PROPERTY UNDER A PRIVATE SYMBOL, for the reason EventTarget's listener map is:
 * a write to them is an ordinary property write, so the per-flow COW delta captures it with no new delta kind.
 * An abort in one arm of a fork is invisible to the sibling for free.
 *
 * WHICH MEMBERS ARE STEP MACHINES, AND WHY THE REST ARE PROVABLY NOT. A member that can reach the page's code
 * is a machine; a member that cannot is a plain C function with an assert saying so, because a machine there
 * would be ceremony and a plain function anywhere else would be a hole in the flow machinery.
 *   - AbortSignal.timeout(ms) IS a machine: `[EnforceRange] unsigned long long` is ToNumber on whatever the
 *     page passed, so `AbortSignal.timeout({valueOf(){ for(;;){} }})` is the page's loop and has to suspend.
 *   - AbortSignal.any(signals) IS a machine, and the most of one: `sequence<AbortSignal>` is Web IDL §3.2.21.1's
 *     iterator protocol, which is the page's code at the @@iterator read, the call, every `next()` and every
 *     `done`/`value` read off its result.
 *   - AbortController.abort() is NOT, and the reason is a spec correction rather than a concession: DOM §3.1 uses
 *     `this's signal`, a SPEC-INTERNAL SLOT, not Get(this, "signal"). Reading the public property (which is what
 *     this file did) both ran a page getter from C and let a page that overrides `signal` redirect abort().
 *   - the `aborted` getter is NOT, and it is the only one of the three that was ever provably not: it touches
 *     OWN SLOTS ONLY, read with JS_GetOwnSlot, which is by definition not a lookup and cannot reach an accessor
 *     or a proxy trap — AND it hands the flag over UNREAD, so it asks the decide seam nothing and the fork a
 *     page's own `if (signal.aborted)` raises happens at the interpreter's branch hook, which holds a frame.
 *   - throwIfAborted() and the `reason` getter ARE machines, and the sentence above is why this took so long to
 *     see: they stood in that same clause, and the own-slots argument is TRUE of them and answers the wrong
 *     question. IT IS ABOUT REACHING THE PAGE'S CODE. A fork needs a RESUME POINT whether or not page code runs,
 *     and both of these ask the abortedness question, so each one forked from inside a C activation with nowhere
 *     for the sibling to come back to. THE TEST THIS PARAGRAPH OPENS WITH IS THEREFORE ONE OF TWO: a member is a
 *     machine if it can reach the page's code OR if it asks a question that can fork. See sig_ask_step. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "core/idl_iter.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/timing/timer.h"
#include "core/realm.h"
#include "core/agent_state.h"
#include "core/dom/abort.h"

/* The private key the signal's internal slots hang off — a Symbol, so a page enumerating its own objects
   cannot see it and cannot collide with it. `g_ready` rather than testing g_key, because a static JSValue is
   zero-initialised and zero is not JS_UNDEFINED. */
static JSValue g_key;
static int g_ready;
/* THE INTERFACE PROTOTYPE OBJECTS — Web IDL §3.7. Every member of these two interfaces is declared on
   `AbortSignal.prototype` / `AbortController.prototype`, not on the instance, and that is not decoration: it is
   what makes `signal instanceof AbortSignal` true, what a page's `AbortSignal.prototype.throwIfAborted.call(x)`
   reaches, and what `Object.getOwnPropertyNames(signal)` correctly reports as EMPTY. Building the members onto
   each instance instead left the interface object with no `.prototype` at all, so `instanceof` threw
   `operand 'prototype' property is not an object` — the page could not even ASK what a signal was. */
/* PER REALM — §3.7, and here it decides ANSWERS: a C member runs in the realm that DEFINED it. Held in
   quickjs's per-context class-proto slots. */
static JSClassID g_sig_class, g_ctrl_class;
/* The id JS_RegisterStepDef handed this runtime for AbortSignal.timeout's machine. One WASM instance is one
   document is one runtime, which is what the install DCHECK holds it to. */
static int g_timeout_stepid = -1;
static JSRuntime *g_abort_rt;

/* The agent's registrations — the step ids, the two class ids, and the realm-registry declaration. They belong
   to abort_init because the DECLARATION has to happen before the agent's own first realm runs the list. */
static void abort_build_agent(JSContext *ctx);

void abort_init(JSContext *ctx)
{
    DCHECK(!g_ready, "abort_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "abortState", false);
    CHECK(!JS_IsException(g_key), "the AbortSignal slot key allocation failed");
    /* EVERY STATIC THIS COMPONENT HOLDS FOR THE WHOLE AGENT, DECLARED BESIDE THE LINE THAT SETS IT
       (core/agent_state.h). Until this component's release reached core/platform.c's column, not one of them
       could be declared at all — platform_check_agent_state fires on a row that declares agent state and
       carries an EMPTY release column — so the two class ids below were carried into whatever agent came
       next, and each of them doubles as this file's own declaration latch. */
    agent_state_value("abort", &g_key,
                      "§3.1's and §3.2's internal-slot record key — the private Symbol a signal's "
                      "`{aborted, reason}` and a controller's `{signal}` hang off");
    g_ready = 1;
    agent_state_flag("abort", &g_ready,
                     "the key's own latch — it is read rather than g_key because a static JSValue is "
                     "zero-initialised and zero is not JS_UNDEFINED");
    abort_build_agent(ctx);
}

/* THE AGENT'S HALF, RUN ONCE FROM core/platform.c's RELEASE COLUMN — see abort.h for why it takes the
   RUNTIME. What it frees is the Symbol above, which is an agent-lifetime value and therefore given back
   against the runtime it was minted in; the two prototypes and the two interface objects are the REALMS' and
   go with their contexts, which is why nothing else here is freed.
   THE HAND-WRITTEN RESET LINES ARE GONE RATHER THAN KEPT BESIDE THE UNDO. They put back two of this
   component's NINE slots and left the two class ids, the four step ids and the recorded runtime standing —
   the second copy of a declaration list whose drift is exactly what agent_state_undo exists to end. A
   declaration added above now owes this function nothing.
   THE UNDO IS LAST, which is agent_state.h's ordering contract: nothing above reads a slot this has nulled. */
void abort_free(JSRuntime *rt)
{
    if (!g_ready)
        return;
    JS_FreeValueRT(rt, g_key);
    agent_state_undo("abort");
}

/* The internal-slot record on `o` — `{ aborted, reason }` for a signal, `{ signal }` for a controller — or
   UNDEFINED when `o` has none. Read as an OWN SLOT,
   never a lookup: a miss on a property lookup is the solver's absent-state seam and would mint a concolic for
   an internal slot, which is right for the page's own reads and wrong here. */
static JSValue signal_slots(JSContext *ctx, JSValueConst sig)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an AbortSignal slot was asked for before the key existed");
    if (!JS_IsObject(sig))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &st, sig, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

/* IS THIS SIGNAL ABORTED, ASKED THROUGH THE SEAM RATHER THAN WITH A BARE `if`. solver_decide answers with the
   arm this flow takes for a concolic flag and prepares the other arm as its own flow; -1 means the flag is an
   ordinary boolean and the real ToBool is the answer. A bare `if` here would pick one arm of an unknown and
   delete the other's code.
   IT IS NOT "the only way a C builtin may ask", WHICH IS WHAT THIS SAID AND IS NOT TRUE OF THE FORK. See the
   file header: the arm comes back right, and the SIBLING has nowhere to resume from a PLAIN C body, so a
   first-time fork on a concolic flag crashes at the seam naming the predicate and the ask site.
   AND THAT IS NOW A STATEMENT ABOUT THIS FORM AND NOT ABOUT THIS FILE'S MEMBERS. §3.2's two members that test
   the flag themselves are step machines and ask abort.h's PARKING form instead (sig_ask_step); what still
   reaches the seam from a C activation is every caller of THIS form, which is what the ask-site parameter below
   exists to name. The clause said "until these members are declared step machines" and is kept in its own words
   because a reader who meets this crash will re-derive exactly that remedy — it is the right remedy, and the
   members it was written about have had it applied.
   THE ARM IS READ THROUGH SOLVER_ARM, and that is not decoration. The result carries SOLVER_FORKED_BIT when a
   sibling was prepared, so a first-time fork onto the true arm returns 257; this compared the raw value against
   1, took the FALSE arm, and left the flow disagreeing with its own decision vector for the rest of the run.
   The header documented the return as "the arm (0/1)", which is why the mistake was available to make. */
/* WHAT THIS QUESTION ANSWERS IN A SESSION THAT EXPLORES NOTHING — DOM §3.2 Interface AbortSignal's own model,
 * read for the one world such a session is in.
 *
 * DOM §3.2 "Interface AbortSignal" gives an AbortSignal an "abort reason", "which is initially undefined", and the `aborted` getter steps
 * "are to return true if this is aborted; otherwise false" — a REAL state machine, written only by §3.2's
 * "signal abort". This file's header says the same thing from the other side: a controller's signal is never
 * concolic, because there is no ignorance to model. The ONE flag that is concolic is `AbortSignal.timeout()`'s,
 * and what is unknown about it is not the machine, it is WHICH MOMENT this program is standing at — so the
 * value carries the answer for the moment a fast machine is standing at, as its EXAMPLE (JS_FALSE, where it is
 * minted below).
 *
 * SO THE ARM IS THE EXAMPLE'S, AND THAT IS THE SAME RULE THE OTHER TWO SEAMS FOLLOW, not a third one: the
 * interpreter's non-forking answer is the value's own ordinary truth, and a step machine's outcome 0 is its
 * ordinary completion. Taking the object's bare truthiness instead would answer ABORTED for a signal nothing
 * has aborted — `throwIfAborted()` would throw and `reason` would hand back a TimeoutError, in a session whose
 * whole purpose is to reproduce ONE path the page really takes. A fabricated abort is a different program.
 *
 * A NON-CONCOLIC FLAG DECLARES NOTHING, because the seam answers -1 for it before this value is ever read and
 * the real ToBool decides — which is DOM §3.2's answer exactly.
 *
 * AND NEITHER DOES A FLAG WHOSE EXAMPLE THIS FLOW HAS ITSELF CONTRADICTED, WHICH IS A FACT ABOUT THE FLOW AND
 * NOT ABOUT THE SLOT. `concolic_example` is a PER-FLOW accessor: §Learning-from-replies' "the forced sibling
 * drops the contradicted example" is performed AT THE READ, so a flow standing on the arm that proved the
 * modelled moment wrong is handed nothing — the value is intact, the producer supplied its boolean, and this
 * flow is simply no longer entitled to believe it. An assert stood here reading that absence as a foreign
 * producer, and it was TRUE WHEN WRITTEN and became a claim about a mechanism that did not exist yet: the two
 * states are indistinguishable through this one accessor, so the absence proves nothing about who wrote the
 * slot, and asserting over it crashed a run for taking an arm it was forced to take.
 *
 * WHAT ABSENCE ACTUALLY MEANS HERE IS SOLVER_NO_NONFORKING_ARM'S OWN SENTENCE — `this question has no answer
 * with only one world in it` — and the crash that belongs to it lives at the seam, where it can see the thing
 * this site cannot: whether the answer is NEEDED. The operand is computed at every call, including on every
 * flow the seam then answers from a REFINED or REPLAYED arm (solver/decide.h: it is consulted at exactly one
 * of the three ways a decision is reached), and the flow whose example was contradicted is by construction a
 * flow that already decided this predicate — so what fired was an assert about a value nobody was going to
 * read. Handing the absence over instead puts the failure where it is real: a NEW decision at this predicate
 * in a session that explores nothing crashes in dec_answer_here NAMING the predicate, in dev AND in release,
 * which is strictly louder than what stood here. That session never reaches it, because an example is
 * contradicted only by taking an arm against it and such a session forces no arm — which is the same reasoning
 * as core/timing/event_loop.c's el_rel_nonforking, the sibling site that already answers this shape this way. */
static int signal_aborted_nonforking(JSContext *ctx, JSValueConst flag)
{
    JSValue ex;
    int r;

    if (!concolic_is(flag))
        return SOLVER_NO_NONFORKING_ARM;
    ex = concolic_example(ctx, flag);
    if (JS_IsUndefined(ex)) {
        JS_FreeValue(ctx, ex);
        return SOLVER_NO_NONFORKING_ARM;
    }
    /* THE PRODUCER CONTRACT THAT IS STILL A CONTRACT: §3.2 declares `readonly attribute boolean aborted` over
       a flag written only by "signal abort", so an example that EXISTS states one of two moments. A third kind
       of value in it is a writer this file does not have. */
    DCHECK(JS_IsBool(ex),
           "an AbortSignal's `aborted` slot holds unknown input whose example is not a boolean — §3.2 declares "
           "`readonly attribute boolean aborted` over a flag written only by \"signal abort\", so the moment an "
           "example states is one of exactly two, and anything else was written by a producer that is not in "
           "this file");
    r = JS_ToBool(ctx, ex);
    JS_FreeValue(ctx, ex);
    return r;
}

/* `site` IS THE CALLER'S AND IS NOT COMPOSED HERE, which is the whole reason it is a parameter.
   This helper is the ONLY route in this engine by which a plain C body reaches solver_decide — `solver_decide`
   has exactly two callers in the tree and the other is the interpreter's own branch hook, which holds a frame
   and therefore never reaches the abort. So when that abort fires, the component is this one BY CONSTRUCTION
   and the only open question is WHICH ask. A site composed with SOLVER_SITE_HERE *here* would name this function
   for every one of them, which is the forwarding-function answer §AN-ASSERT-THAT-NAMES-A-REMEDY-BUT-NOT-A-SITE
   forbids — one answer for every candidate is no answer.
   THE POPULATION IS HANDED OVER AS THE DERIVATION AND NOT AS A NUMBER, because a count here moves on every
   commit that routes one of these and the only reader of it is somebody about to change that count. It read
   "26 of them: five calls below, two of which are exported and called from 24 further sites across nine files",
   and that sentence disagreed with its own list — no reading of its own two figures gives 26. The commands,
   which answer today:
     DIRECT          grep -cE 'signal_is_aborted\(ctx' engine/host/browser/core/dom/abort.c
     VIA THE PAIR    git grep -cE 'abort_signal_(aborted|reason)(_at)?[[:space:]]*\(ctx' -- '*.c'
   THE SECOND IS NOT "EXTERNAL SITES", WHICH IS WHAT IT WAS FIRST LABELLED AND IS WHY THE LABEL IS CORRECTED
   HERE RATHER THAN THE NUMBER: this file is itself one of the files that answers it, because §3.2's dependent-
   signal machinery asks through the exported macros like any other caller. A reader who takes the second figure
   as "everywhere but here" has dropped this component's own asks out of the population, which is the direction
   that makes the work look smaller — the same direction the sentence above was already wrong in.
   The two overlap BY CONSTRUCTION and are not summed: two of the DIRECT calls ARE the exported pair's bodies,
   so the second command counts the callers OF those two.
   BOTH ARE SPELLED AS THE CONSTRUCT AND NEITHER MATCHES ITS OWN TEXT, and both halves of that are deliberate.
   The CONSTRUCT, because this file's prose names all three of these helpers while arguing about them, so a count
   of the NAME scores how much documentation a component wrote rather than how many sites ask. And `\(ctx`
   rather than `(`, because that is what makes the regex unable to match the line it is printed on — the escape
   is in the text and not in what the text matches — and it drops the DEFINITIONS too, whose first parameter is
   spelled `JSContext *ctx`. The first was `grep -c 'signal_is_aborted('`, which counted this very line.
   THEY COUNT LINES AND NOT OCCURRENCES, which for C agrees: one call per statement and one statement per line.
   `grep -o … | wc -l` is the occurrence form where that stops being true. */
static int signal_is_aborted(JSContext *ctx, JSValueConst slots, const char *site)
{
    JSValue flag = JS_GetPropertyStr(ctx, slots, "aborted");
    int d = solver_decide_at(ctx, flag, signal_aborted_nonforking(ctx, flag), site);
    int r;

    if (d < 0) {
        r = JS_ToBool(ctx, flag);
    } else {
        DCHECK(SOLVER_ARM(d) == 0 || SOLVER_ARM(d) == 1,
               "a two-armed decision answered with an arm that is neither of them");
        r = SOLVER_ARM(d) == 1;
    }
    JS_FreeValue(ctx, flag);
    return r;
}

/* §3.2's "abort reason": the DOMException the spec names when the caller supplied none. Built by throwing one
   and taking it back, because that is the engine's only constructor for the interface and it runs none of the
   page's code — reading `DOMException` off the global would, since a page may replace it. */
static JSValue abort_reason_default(JSContext *ctx, const char *name, const char *msg)
{
    JS_ThrowDOMException(ctx, name, "%s", msg);
    return JS_GetException(ctx);
}

/* §3.2 "signal abort" step 2: an UNDEFINED reason becomes a new "AbortError" DOMException. It lives here, in
   the one operation, rather than at each caller counting its own arguments — `controller.abort()`, Streams
   §5.5.1's WritableStreamAbort and `AbortSignal.abort()` all reach the same step, and a caller that forgot it
   handed the page a signal whose `reason` was undefined AFTER it had aborted, which no real signal can be.
   `reason` is CONSUMED. */
static JSValue abort_reason_or_default(JSContext *ctx, JSValue reason)
{
    if (!JS_IsUndefined(reason))
        return reason;
    JS_FreeValue(ctx, reason);
    return abort_reason_default(ctx, "AbortError", "signal is aborted without reason");
}

static JSValue js_sig_get_aborted(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue slots = signal_slots(ctx, this_val), v;
    (void)argc; (void)argv;
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "aborted called on something that is not an AbortSignal");
    }
    v = JS_GetPropertyStr(ctx, slots, "aborted");
    JS_FreeValue(ctx, slots);
    return v;
}

/* §3.2's TWO MEMBERS THAT TEST THE FLAG — ONE MACHINE, TWO MAGICS, AND WHAT MAKES IT A MACHINE IS NOT THE
 * PAGE'S CODE.
 *
 * DOM §3.2 "Interface AbortSignal" states them a line apart, and they are one question with two completions:
 *   "The throwIfAborted() method steps are to throw this's abort reason, if this is aborted."
 *   "The reason getter steps are to return this's abort reason."
 * Throw it, or hand it back — which is why they share a declaration and a state rather than carrying two copies
 * of the receiver check and two copies of the ask.
 *
 * THE SECOND SENTENCE CARRIES NO TEST AND THIS ENGINE STILL NEEDS ONE, WHICH IS A MODELLING FACT AND NOT A
 * DIVERGENCE. §3.2 keeps ONE slot — "An AbortSignal object has an associated abort reason (a JavaScript value),
 * which is initially undefined" — and DERIVES the state from it: "An AbortSignal object is aborted when its
 * abort reason is not undefined." This file carries the two separately BECAUSE the abortedness is the unknown,
 * and the timeout signal is what forces that: js_timeout_step mints it with a concolic `aborted` and a REAL
 * TimeoutError already sitting in `reason`, so `signal.reason` read straight off the slot would report a timeout
 * that has not happened. The gate is what reconstructs §3.2's single-slot answer out of this engine's two, and
 * it is the whole reason `reason` branches at all.
 *
 * WHY A MACHINE, WHEN NEITHER OF THEM CAN REACH THE PAGE'S CODE. The file header's test for a machine is whether
 * a member can reach the page, and that test answers only ONE of the two things a fork needs: whether this body
 * hosts a loop of the page's. It is SILENT about whether this body can hold a SIBLING, and a fork needs a resume
 * point whether or not any page code runs. Both of these ask the abortedness question, so an
 * `AbortSignal.timeout()` a page reached through either of them forked with nowhere for the other arm to come
 * back to and crashed at solver/engine.c's seam — measured on app.gitpod.io through `AbortSignal.timeout().aborted`.
 * `aborted` IS NOT HERE AND THAT IS NOT AN OMISSION: js_sig_get_aborted hands the flag over UNREAD, so the fork
 * it leads to happens at the interpreter's own branch hook, which holds a frame and already has the resume point.
 *
 * IT ASKS THE PARKING FORM, WHICH IS THE WHOLE REPAIR. abort.h's `abort_signal_aborted_step` returns the fork
 * code this body returns unchanged, and the driver clones the state at the ask; nothing new had to be built. */
enum { SIG_ASK_THROW = 0, SIG_ASK_REASON };

#define SIG_ASK_STAGES(X) \
    X(SIG_ASK, "DOM §3.2 Interface AbortSignal's throwIfAborted() method steps and its reason getter steps " \
               "(the one branch both are: is this signal aborted)")
enum { IDL_STEP_STAGE_BASE(SIG_ASK_STAGES) SIG_ASK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SIG_ASK_STEPS[] = { SIG_ASK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    /* HAS `flag` BEEN STATED — a flag, and not a test on the slot itself, because a step state arrives ZEROED
       and a zeroed JSValue is the INTEGER 0 rather than JS_UNINITIALIZED. Handing that integer to the parking
       form takes its already-held arm and coerces a number nobody asked about, which answers FALSE for every
       signal in the engine and answers it in silence. abort.h's contract says the emptiness of the slot is a
       thing the caller STATES; this byte is how this caller can tell whether it has stated it yet. */
    uint8_t started;
    /* abort_signal_aborted_step's BORROWED OPERAND, held where the sibling's snapshot carries it: a deep fork
       byte-copies this state and re-takes only what `visit` names, so a flag kept in a C local is gone in the
       arm that resumes and one in a field the visit does not name is freed by both. */
    JSValue flag;
} SigAskState;

static void sig_ask_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SigAskState *s = st;

    /* GUARDED, for the reason `started` exists at all: before the first entry has STATED the slot it holds the
       integer 0, which is not a value anything may take a second reference to. */
    if (s->started) v->val(ctx, &s->flag);
}

static int sig_ask_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SigAskState *s = st;
    int magic = idl_step_magic(hdr);
    JSValue slots;
    int aborted = 0, r;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    DCHECK(hdr->stage == SIG_ASK, "an AbortSignal flag test resumed into a stage §3.2 does not have");
    DCHECK(magic == SIG_ASK_THROW || magic == SIG_ASK_REASON,
           "§3.2's flag test ran under a magic neither of its two members was declared with");
    if (!s->started) {
        s->flag = JS_UNINITIALIZED;   /* STATED, never read off the slot — see the field */
        s->started = 1;
    }
    /* WEB IDL §3.7.6's AND §3.7.7's IMPLEMENTATION CHECK, RE-ASKED ON EVERY RE-ENTRY. That costs nothing and is
       what makes a resume identical to a first entry: it is an own-slot read of a private Symbol, so it runs no
       code of the page's and cannot answer differently for the same object between two scheduler turns. */
    slots = signal_slots(ctx, hdr->this_val);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "%s called on something that is not an AbortSignal",
                          magic == SIG_ASK_REASON ? "reason" : "throwIfAborted");
        return JS_STEP_ABRUPT;
    }
    r = abort_signal_aborted_step(ctx, hdr, hdr->this_val, &s->flag, &aborted);
    if (r) {
        /* PARKED. The fork code goes back UNCHANGED and the operand stays held, because the sibling resumes AT
           it; `cb_result` is NOT freed, because the driver re-enters this body with the same one and the stage
           is unchanged, so the receiver read above is the same read with the same answer. */
        JS_FreeValue(ctx, slots);
        return r;
    }
    JS_FreeValue(ctx, cb_result);
    if (!aborted) {
        /* §3.2: throwIfAborted "does nothing"; `reason` is undefined, which is what the single-slot model reads
           for a signal whose abort reason was never set. */
        JS_FreeValue(ctx, slots);
        *presult = JS_UNDEFINED;
        return JS_STEP_DONE;
    }
    {
        JSValue reason = JS_GetPropertyStr(ctx, slots, "reason");

        JS_FreeValue(ctx, slots);
        if (magic == SIG_ASK_REASON) {
            *presult = reason;
            return JS_STEP_DONE;
        }
        JS_Throw(ctx, reason);
        return JS_STEP_ABRUPT;
    }
}

/* `unforkable` IS ABSENT, AND THAT ABSENCE IS THE DECLARATION core/idl_args.h ASKS FOR. The state is one byte
   and one JSValue the `visit` names, so a deep fork's byte copy plus that one re-take is the whole of it: there
   is no heap pointer and no value the declaration cannot reach, which is that header's own condition for a
   machine that may ALWAYS be forked. It is also the one thing this conversion exists for — a machine that
   refused the fork would MOVE the crash rather than close it. */
static const IdlStepDecl SIG_ASK_DECL = { sig_ask_step, sizeof(SigAskState), sig_ask_visit, NULL,
                                          "DOM §3.2 AbortSignal.throwIfAborted() / AbortSignal.reason",
                                          SIG_ASK_STEPS };
static int g_sig_throw_stepid = -1, g_sig_reason_stepid = -1;

/* Create a signal. `aborted` and `reason` are CONSUMED. */
static JSValue signal_new(JSContext *ctx, JSValue aborted, JSValue reason)
{
    JSValue sig, st;
    JSAtom k;

    {
        /* abort_signal_proto ASSERTS that this realm ran its install — the members live on that prototype, so
           a signal minted before it exists would have none of them.
           IT WEARS THE CLASS, and that is what makes `AbortSignal` a DECLARABLE type. This was a plain
           JS_NewObjectProto, so the only brand an AbortSignal had was its private slot record — which a body
           can test and a DECLARATION cannot, since Web IDL's §3.2.15 conversion in core/idl_args.c compares
           class ids. HTML §7.2.6.10.1's `required AbortSignal signal` is a declared dictionary member, so
           without this the type would have had to be re-stated as a hand-written check in NavigateEvent's
           constructor — the exact duplication a declared type exists to remove. The class already existed for
           its per-realm prototype slot; giving it to the instances too is what every other interface in this
           engine does (core/frame/navigation_history_entry.c mints through the same pair). */
        JSValue sp = abort_signal_proto(ctx);
        sig = JS_NewObjectProtoClass(ctx, sp, g_sig_class);
        JS_FreeValue(ctx, sp);
    }
    CHECK(!JS_IsException(sig), "the AbortSignal allocation failed");
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "the AbortSignal slot record allocation failed");
    JS_SetPropertyStr(ctx, st, "aborted", aborted);
    JS_SetPropertyStr(ctx, st, "reason", reason);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the AbortSignal slot key could not be interned");
    JS_SetProperty(ctx, sig, k, st);
    JS_FreeAtom(ctx, k);
    /* THE SLOT RECORD IS ALL AN INSTANCE CARRIES. Every member is on the prototype above. */
    return sig;
}

/* ---- §3.2's DEPENDENT SIGNALS ------------------------------------------------------------------------------
 *
 * Three more items on a signal's slot record: `dependent` (a boolean), `sources` and `deps` (§3.2's SOURCE
 * SIGNALS and DEPENDENT SIGNALS). They are JS Arrays on the record for the reason the algorithm list is one —
 * a write to them is an ordinary property write the per-flow COW delta captures, so one arm's dependent
 * subscription is invisible to its sibling and both park to the cold tier for free.
 *
 * THE SPEC CALLS BOTH SETS WEAK AND THESE ARE STRONG, which is over-RETENTION and never a wrong answer: the
 * sets are only ever read to propagate an abort, and quickjs collects the source↔dependent cycle the moment
 * both ends are unreachable. §3.2.1's GC requirement is the OPPOSITE direction — a dependent must not be
 * collected while its sources live — and a strong edge from source to dependent satisfies it outright. */

static JSValue signal_list(JSContext *ctx, JSValueConst sig, const char *name, int create)
{
    JSValue slots = signal_slots(ctx, sig), arr;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, slots, name);
    if (!JS_IsArray(arr) && create) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        CHECK(!JS_IsException(arr), "a signal's dependency list could not be allocated");
        JS_SetPropertyStr(ctx, slots, name, JS_DupValue(ctx, arr));
    }
    JS_FreeValue(ctx, slots);
    return arr;
}

static uint32_t array_len(JSContext *ctx, JSValueConst arr);

static void signal_list_append(JSContext *ctx, JSValueConst sig, const char *name, JSValueConst v)
{
    JSValue arr = signal_list(ctx, sig, name, 1);

    /* The list is created on demand and the allocation is a CHECK, so the only way this is not an array is a
       caller that handed a non-signal — which is a bug in the caller and not a case to skip quietly. */
    DCHECK(JS_IsArray(arr), "a signal list was appended to on something that is not an AbortSignal");
    JS_SetPropertyUint32(ctx, arr, array_len(ctx, arr), JS_DupValue(ctx, v));
    JS_FreeValue(ctx, arr);
}

/* §3.2's `dependent` boolean. */
static bool signal_dependent(JSContext *ctx, JSValueConst sig)
{
    JSValue slots = signal_slots(ctx, sig), v;
    bool b;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return false; }
    v = JS_GetPropertyStr(ctx, slots, "dependent");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, slots);
    return b;
}

/* §3.2 step 4.1.1 of signal abort, and step 2 of "create a dependent abort signal": take a reason that has
   ALREADY been defaulted, with none of "signal abort"'s other steps. `reason` is CONSUMED. */
static void signal_adopt_reason(JSContext *ctx, JSValueConst sig, JSValue reason)
{
    JSValue slots = signal_slots(ctx, sig);

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); JS_FreeValue(ctx, reason); return; }
    JS_SetPropertyStr(ctx, slots, "aborted", JS_TRUE);
    JS_SetPropertyStr(ctx, slots, "reason", reason);
    JS_FreeValue(ctx, slots);
}

/* §3.2 "create a dependent abort signal", over a list held AS A JS ARRAY.
 *
 * THE LIST IS A JS VALUE AND NOT A C VECTOR, and that is the algorithm's shape rather than a convenience. Its
 * one script-visible caller is `AbortSignal.any(sequence<AbortSignal>)`, whose Web IDL conversion SUSPENDS at
 * every element — each `next()` and each `value` read is the page's code — so the half-built list has to be
 * something the flow's snapshot carries and its per-flow COW delta captures. That is exactly the "platform data
 * a flow queues is a JS value, never malloc'd C" rule: an Array's growth is a property write the delta already
 * captures, and a malloc'd vector parked across a suspension is a leak no GC walk can see.
 *
 * THE REALM is `ctx`'s — the algorithm's third parameter — because signal_new mints on that realm's
 * AbortSignal.prototype, which for a step machine is the realm that DEFINED the member.
 *
 * AND STEP 2'S TEST CAN FORK, WHICH IS WHY THIS TAKES A STEP HEADER AND WHY ITS THREE SLOTS ARE THE CALLER'S.
 * "If signal is aborted" is a test of each INPUT signal's flag, and the one flag §3.2 models as unknown is
 * `AbortSignal.timeout()`'s — so `AbortSignal.any([AbortSignal.timeout(n), c.signal])`, which is the spelling a
 * bundle writes for "whichever comes first", asks an unknown on its FIRST element. Asked through the
 * non-parking form that is a fork with no resume point and it ABORTS at solver/engine.c's seam.
 *   `h` IS A DECLARATION BY THE ONE CALLER THAT CAN MAKE IT AND NOT A ROUTING TABLE. There is nothing to look
 * up: non-NULL means `a clone is coming, this ask may rest`, NULL means `there is no activation here` — the
 * same shape engine.c's own `g_fork_snapshot_owed` is, moved from a global to an argument because here the fact
 * is per-CALL rather than per-session. It selects no second implementation of the algorithm: every line below
 * is shared, and the arm it picks is which of abort.h's TWO ASK ENTRIES answers one test. The NULL arm is not a
 * softer path either — it reaches the identical seam and CRASHES where it cannot carry the sibling, naming the
 * site it was handed, which is the forcing function intact.
 *   `at` IS LOAD-BEARING AND NOT AN OPTIMISATION. The seam BORROWS the flag of the element it is asking about
 * and holds it across the park, so a resume that restarted step 2 at element 0 would be asked about element 0's
 * flag while the seam held element `at`'s — "the operand the resuming arm was about to be asked about", which is
 * what every consumer of that seam asserts. It is a plain integer, so a deep fork's byte-copy carries it and the
 * caller's `visit` must NOT name it.
 *   `result` IS MINTED ONCE AND NOT ONCE PER ENTRY, for the reason abort.h gives for `held`: a fork re-enters the
 * asking arm at its top twice, so a step-1 mint reached on a resume would leak the parent's signal and hand the
 * sibling a DIFFERENT object than the arm it is replaying was about. */
static int dependent_signal_build(JSContext *ctx, JSStepHdr *h, JSValueConst signals,
                                  JSValue *result, JSValue *held, uint32_t *at, const char *site)
{
    uint32_t i, n = array_len(ctx, signals);

    /* Step 1. WHAT THIS RELIES ON, NAMED BECAUSE IT IS THE LOAD-BEARING AND NON-OBVIOUS HALF: step 1 runs BEFORE
       step 2's fork, so both arms hold a reference to ONE result object and then WRITE to it — the aborted arm
       sets `aborted` and `reason`, the other sets `dependent`. Those are own-property writes under this file's
       private symbol, which is the thing this file's header says it chose that representation FOR: "an abort in
       one arm of a fork is invisible to the sibling for free", because the per-flow COW delta captures an
       ordinary property write with no new delta kind.
       IT IS NOT A NEW DEPENDENCY. `signals` is in the IDENTICAL position and has been since this machine was
       written: it is minted at ANY_START, it is APPENDED TO after a fork inside the page's iterator can have
       happened, and this machine's own `visit` comment states the consequence as the point — "two arms of a
       branch inside the page's iterator hand `any()` two different sequences". A result object shared across
       step 2's fork rides exactly that mechanism, so if one works the other does.
       HOW ITS ABSENCE WOULD SHOW: two arms of one `AbortSignal.any()` answering the SAME `aborted`, which is one
       timeline's write landing in both. test_forced.c's `anyiter` row is where the two are exercised in one
       machine — the iterator park and step 2's fork, in that order. */
    if (JS_IsUninitialized(*result)) {
        *result = signal_new(ctx, JS_FALSE, JS_UNDEFINED);
        CHECK(!JS_IsException(*result), "a dependent AbortSignal could not be allocated");
    }
    /* Step 2: an already-aborted input decides the answer OUTRIGHT — the result is born aborted with that
       signal's reason and registers no dependency at all, which is why the operators' "if internal options's
       signal is aborted, reject and return" test answers correctly on the very first line.
       THE CURSOR ADVANCES ONLY ONCE THE ASK HAS ANSWERED, which is what makes a resume re-ask the same element
       rather than the next one. */
    while (*at < n) {
        JSValue sig = JS_GetPropertyUint32(ctx, signals, *at);
        int aborted;

        if (h) {
            int r = abort_signal_aborted_step(ctx, h, sig, held, &aborted);

            if (r) {              /* PARKED or FORKED — `*at` names the element the seam holds the flag of */
                JS_FreeValue(ctx, sig);
                return r;
            }
        } else {
            aborted = abort_signal_aborted_at(ctx, sig, site) ? 1 : 0;
        }
        if (aborted) {
            /* THE REASON REPLAYS AND DOES NOT FORK A SECOND TIME. `abort_signal_reason_at` asks the
               NON-parking test over the SAME `aborted` flag, and `decide_key` composes a branch identity out of
               THE VALUE ALONE, so both asks are one key; this arm has just recorded `aborted = 1` under it, so
               the read is answered as a feasible refinement and never reaches `engine_prepare_fork`. */
            signal_adopt_reason(ctx, *result, abort_signal_reason_at(ctx, sig, site));
            JS_FreeValue(ctx, sig);
            return 0;
        }
        JS_FreeValue(ctx, sig);
        (*at)++;
    }
    {   /* Step 3 */
        JSValue slots = signal_slots(ctx, *result);
        DCHECK(JS_IsObject(slots), "a signal this component just minted has no slot record");
        JS_SetPropertyStr(ctx, slots, "dependent", JS_TRUE);
        JS_FreeValue(ctx, slots);
    }
    /* STEPS 3 AND 4 RUN NONE OF THE PAGE'S CODE AND ASK THE SOLVER NOTHING, so there is no rest point below
       this line and `at` is spent at `n` by the time it is reached. Every read is an own slot the engine wrote
       and every append is the engine's own list. */
    for (i = 0; i < n; i++) {                                   /* Step 4 */
        JSValue sig = JS_GetPropertyUint32(ctx, signals, i);
        if (!signal_dependent(ctx, sig)) {
            signal_list_append(ctx, *result, "sources", sig);
            signal_list_append(ctx, sig, "deps", *result);
        } else {
            /* Step 4.2: FLATTEN — a dependent input contributes its own SOURCES, never itself, so the graph
               this builds is always exactly one hop deep. */
            JSValue src = signal_list(ctx, sig, "sources", 0);
            uint32_t k, m = JS_IsArray(src) ? array_len(ctx, src) : 0;
            for (k = 0; k < m; k++) {
                JSValue s = JS_GetPropertyUint32(ctx, src, k);
                /* §3.2 step 4.2.1's assert, NARROWED TO THE HALF THAT IS THIS ENGINE'S OWN FACT. It read
                   `!abort_signal_aborted(ctx, s) && !signal_dependent(ctx, s)`, and the first operand ASKS THE
                   SOLVER — which forks — so a `DCHECK` whose condition "MUST be side-effect-free" was minting a
                   flow, in DEV ONLY, which is the arm-divergence §Offensive-programming forbids: release
                   compiles the ask out entirely, so the two regimes explored different worlds from inside an
                   assertion. It is not reachable through step 2's answers either — step 2 asks about the INPUT
                   signals and this asks about the SOURCE signals of a dependent input, which are different
                   objects — so it was a FIRST-TIME fork and the abort it reached named this assert's own line.
                   The dropped half is also the half an assert may not stand on: §3.2 models a
                   `AbortSignal.timeout()` flag as UNKNOWN, so "this source is not aborted" is not an invariant
                   this codebase computed and can therefore not be violated. WHAT SURVIVES IS THE FLATTENING
                   ITSELF — `dependent` is a boolean step 3 above writes and nothing else does, so it is exactly
                   the fact this algorithm guarantees and the one a reader needs. */
                DCHECK(!signal_dependent(ctx, s),
                       "§3.2 step 4.2.1: a source signal of a dependent signal was ITSELF dependent — the "
                       "flattening this algorithm performs is what makes that impossible, and `dependent` is a "
                       "flag step 3 of this same algorithm is the only writer of");
                signal_list_append(ctx, *result, "sources", s);
                signal_list_append(ctx, s, "deps", *result);
                JS_FreeValue(ctx, s);
            }
            JS_FreeValue(ctx, src);
        }
        JS_FreeValue(ctx, sig);
    }
    return 0;                                                   /* Step 5 */
}

/* THE PARKING ENTRY — abort.h states its contract and its three slots. */
int abort_signal_dependent_step(JSContext *ctx, JSStepHdr *h, JSValueConst signals,
                                JSValue *result, JSValue *held, uint32_t *at)
{
    DCHECK(h != NULL, "the parking form of §3.2's create-a-dependent-abort-signal was asked without a step "
                      "header — the header is where the driver reads the outstanding ask and writes its answer, "
                      "so a NULL one is a plain C body that has not been declared a machine and must use "
                      "abort_signal_dependent_new");
    DCHECK(result != NULL && held != NULL && at != NULL,
           "§3.2's create-a-dependent-abort-signal was asked to park with one of its three slots missing — the "
           "signal it is building, the flag the seam borrows and the element the ask is about all have to live "
           "where the SIBLING'S SNAPSHOT CARRIES them, so a caller with nowhere to keep one of them would hand "
           "the resuming arm a freed value or the wrong element's question");
    /* THE SITE IS THE PARKING FORM'S OWN AND NAMES NOTHING, deliberately: the ask below cannot reach
       engine_prepare_fork's abort at all, because the seam it asks returns the fork code instead. */
    return dependent_signal_build(ctx, h, signals, result, held, at,
                                  "core/dom/abort.c (the parking form asks no non-parking test)");
}

/* THE NON-PARKING ENTRY, for the one shape that has no resume point: a C caller whose list is two locals.
   It is `_at`-shaped and the macro expands at each of ITS callers, for abort.h's reason — the ask lives in the
   shared body above, so a site composed here would name this function for every caller at once.

   NAMED RESIDUAL — WHAT IS NOT COVERED. Step 2's test asked through THIS entry still reaches
   solver/engine.c's `engine_prepare_fork` and aborts, because this entry's caller has no step header to park on.
   It is stated as a PROPERTY and not as a list, for the reason the sibling residual at
   core/events/event_target.c gives: a named caller goes stale the day it is converted. THE PROPERTY IS — a
   caller that reaches this entry while holding no `JSStepHdr`, with at least one input signal whose `aborted`
   flag is unknown and no arm yet recorded for it. The second half is a fact about the FLOW and not about the
   caller, which is why this cannot be answered from here and why the crash's `site` is what answers it.
   THE DERIVATION, because a count here rots on the next conversion:
     git grep -cE 'abort_signal_dependent_new(_at)?[[:space:]]*\(ctx' -- '*.c'
   SPELLED AS THE CONSTRUCT AND UNABLE TO MATCH ITS OWN LINE, for the two reasons the pair's derivation above
   states; it counts this file's own definitions out because theirs spell `JSContext *ctx`.
   WHAT THE NEXT DIFF BUILDS — AND ITS HALF NAMING §5.4'S REQUEST CONSTRUCTOR IS MET AND IS KEPT IN ITS OWN
   WORDS BELOW, BECAUSE A READER WHO RE-DERIVES THE SPLIT FROM `a caller has no header` WILL WRITE THE SAME PAIR
   AGAIN. It read: each such caller routed to `abort_signal_dependent_step` with the three slots on its OWN
   state — and the ordered subproblem is not uniform across them, because a caller that holds no header AT ALL
   needs one first; §11's promise-returning operators and §5.4's request constructor are two different answers
   to that, and a diff that treats them as one would add a call where there is no header to pass.
   THE ROUTING IS RIGHT AND THE PAIR NAMED THE WRONG TWO SITES, WHICH IS WHY THE SHAPE IS WORTH KEEPING AND THE
   EXAMPLES ARE NOT. §5.4's CONSTRUCTOR held a header all along — `js_request_ctor_decl` has declared that body a
   machine since it was written — so it was the ROUTING kind and not the needs-a-machine kind, and it is now
   routed with its three slots on `JSRequestCtorState` and a stage of its own for the ask. §11's operators hold
   one too: `JSObsState`'s FIRST field is a `JSStepHdr`. So the split the clause describes is real and neither of
   its examples was on the far side of it. The caller in this tree that genuinely holds NO header is
   core/fetch/request.c's `js_request_clone`, a plain `JS_CFUNC_DEF`, and its own site carries the three-clause
   residual for it. WHETHER THE PAIR WAS WRONG AT BIRTH IS NOT ESTABLISHABLE FROM THIS CHECKOUT — this
   repository answers `git rev-parse --is-shallow-repository` with true, so a pickaxe here returns the graft
   commit and not an origin, and the honest finding is that the clause disagrees with the tree TODAY.
   WHAT IS OUTSTANDING IS THEREFORE TWO DIFFERENT DIFFS AND THE ORDER IS THE CLAUSE'S OWN: `js_request_clone`
   needs the MACHINE first and the routing second, and §11's two sites need only the routing — with their slots
   on `JSObsState` rather than on the per-subscription record, because the record is a JS object the COW delta
   captures and step 2's cursor is a plain integer a deep fork's byte-copy carries.
   HOW ITS ABSENCE WOULD SHOW: solver/engine.c's `engine_prepare_fork` abort naming a caller of this entry as its
   ask site, with its question reading as a derivation of an `AbortSignal.timeout()` flag. That is the one
   observation that cannot be confused with the converted path's, because the parking arm cannot reach that seam.
   RETIREMENT: this record goes when this entry has no callers left outside this file, after which the NULL-`h`
   arm of the build above is the thing to delete and the `if (h)` with it. */
static JSValue dependent_signal_new_at(JSContext *ctx, JSValueConst signals, const char *site)
{
    JSValue result = JS_UNINITIALIZED, held = JS_UNINITIALIZED;
    uint32_t at = 0;
    int r = dependent_signal_build(ctx, NULL, signals, &result, &held, &at, site);

    DCHECK(r == 0, "§3.2's create-a-dependent-abort-signal parked with no step header to park on — the NULL-`h` "
                   "arm asks the non-parking test, which answers 0 or 1 and cannot return a fork code");
    DCHECK(JS_IsUninitialized(held),
           "the non-parking arm of §3.2's create-a-dependent-abort-signal left the seam's borrow filled — only "
           "abort_signal_aborted_step fills that slot, and the NULL-`h` arm never calls it");
    (void)r;
    return result;
}

/* THE SAME ALGORITHM REACHED FROM C, for a caller whose list is two locals rather than a converted sequence —
   §11's promise-returning operators, which pair their own controller's signal with the caller's. It builds the
   list and delegates; there is ONE implementation of the algorithm and this is not a second one.
   IT FORWARDS THE SITE IT WAS HANDED AND NEVER COMPOSES ONE, which is abort.h's rule for an intermediate. Step
   2's ask is the one that can reach solver/engine.c's abort from these callers, and a site composed here would
   name THIS function for every one of them — one answer for every candidate, which is no answer. */
JSValue abort_signal_dependent_new_at(JSContext *ctx, JSValueConst *signals, int n, const char *site)
{
    JSValue list = JS_NewArray(ctx), result;
    int i;

    CHECK(!JS_IsException(list), "the source list of a dependent AbortSignal could not be allocated");
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_DupValue(ctx, signals[i]));
    result = dependent_signal_new_at(ctx, list, site);
    JS_FreeValue(ctx, list);
    return result;
}

/* §3.2 "signal abort", STEP 2 AND STEP 3: set the flag and the reason. Already-aborted answers false, which is
   what keeps a double abort() from running anything twice. `reason` is CONSUMED. */
static bool signal_abort_state(JSContext *ctx, JSValueConst sig, JSValue reason)
{
    JSValue slots = signal_slots(ctx, sig);

    if (!JS_IsObject(slots) || signal_is_aborted(ctx, slots, SOLVER_SITE_HERE)) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, reason);
        return false;
    }
    JS_SetPropertyStr(ctx, slots, "aborted", JS_TRUE);
    JS_SetPropertyStr(ctx, slots, "reason", abort_reason_or_default(ctx, reason));
    JS_FreeValue(ctx, slots);
    return true;
}

/* ---- §3.2's ABORT ALGORITHMS ------------------------------------------------------------------------------ */

/* The signal's algorithm list, an Array on its slot record — created on demand, so a signal nobody registers
   against carries nothing. An ALGORITHM IS NOT A LISTENER: it runs before the `abort` event, the page cannot
   see it or remove it, and the whole list is dropped once it has run. */
static JSValue signal_algos(JSContext *ctx, JSValueConst sig, int create)
{
    JSValue slots = signal_slots(ctx, sig), arr;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, slots, "algorithms");
    if (!JS_IsArray(arr) && create) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        CHECK(!JS_IsException(arr), "a signal's abort-algorithm list could not be allocated");
        JS_SetPropertyStr(ctx, slots, "algorithms", JS_DupValue(ctx, arr));
    }
    JS_FreeValue(ctx, slots);
    return arr;
}

static uint32_t array_len(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = 0;
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

void abort_signal_add_algorithm(JSContext *ctx, JSValueConst sig, JSValueConst fn)
{
    JSValue arr;

    DCHECK(JS_IsFunction(ctx, fn), "an abort algorithm that is not callable was registered on a signal");
    /* DOM §3.2: "if signal is aborted, then return". A signal that has already fired has already emptied its list,
       so registering into it would leave a value nothing will ever run or free. */
    if (abort_signal_aborted(ctx, sig))
        return;
    arr = signal_algos(ctx, sig, 1);
    DCHECK(JS_IsArray(arr), "an abort algorithm was registered on something that is not an AbortSignal — every "
                            "caller brands its signal first, so this is a lost registration and not a no-op");
    JS_SetPropertyUint32(ctx, arr, array_len(ctx, arr), JS_DupValue(ctx, fn));
    JS_FreeValue(ctx, arr);
}

void abort_signal_remove_algorithm(JSContext *ctx, JSValueConst sig, JSValueConst fn)
{
    JSValue arr = signal_algos(ctx, sig, 0);
    uint32_t i, n, k = 0;

    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return; }
    n = array_len(ctx, arr);
    /* COMPACT IN PLACE rather than splice: `splice` is a page-visible method on Array.prototype and this list
       is the engine's, so it is walked with own indices like everything else here. */
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_VALUE_GET_TAG(v) == JS_VALUE_GET_TAG(fn) && JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(fn)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        JS_SetPropertyUint32(ctx, arr, k++, v);
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, k));
    JS_FreeValue(ctx, arr);
}

bool abort_signal_is(JSContext *ctx, JSValueConst v)
{
    /* THE SLOT RECORD IS THE BRAND. A signal is the only thing this component gives one of these to, and it is
       under a private Symbol the page cannot mint — so this is a real brand test and not a shape test. */
    JSValue slots = signal_slots(ctx, v), flag;
    bool ok;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return false; }
    /* A CONTROLLER has one of these too, holding `signal`; only a SIGNAL's has `aborted`. */
    flag = JS_GetPropertyStr(ctx, slots, "aborted");
    ok = !JS_IsUndefined(flag);
    JS_FreeValue(ctx, flag);
    JS_FreeValue(ctx, slots);
    return ok;
}

JSClassID abort_signal_class(void)
{
    DCHECK(g_sig_class != 0, "AbortSignal's class was asked for before abort_init declared it — a DECLARATION "
                             "that brands against it (HTML §7.2.6.10.1's NavigateEventInit) is made at agent "
                             "init, so core/platform.c's order is what puts this component ahead of it");
    return g_sig_class;
}

bool abort_signal_aborted_at(JSContext *ctx, JSValueConst sig, const char *site)
{
    JSValue slots = signal_slots(ctx, sig);
    bool b = JS_IsObject(slots) && signal_is_aborted(ctx, slots, site);
    JS_FreeValue(ctx, slots);
    return b;
}

/* THE PARKING FORM OF "IS THIS SIGNAL ABORTED" — see abort.h for the contract and for why it is the BRANCH
   seam. It takes no `site`, and that is not an omission: a site exists so an abort can name what to convert,
   and this form is what a converted caller uses — reaching it means the question has a resume point and there
   is no abort to name anything at. */
int abort_signal_aborted_step(JSContext *ctx, JSStepHdr *h, JSValueConst sig, JSValue *held, int *out)
{
    int r;

    DCHECK(h != NULL, "the parking form of §3.2's aborted test was asked without a step header — the header is "
                      "where the driver reads the outstanding ask and writes its answer, so a NULL one is a "
                      "plain C body that has not been declared a machine and must use the non-parking form");
    DCHECK(held != NULL, "the parking form of §3.2's aborted test was asked with no slot to hold its operand — "
                         "the seam borrows the flag for the length of the request, so a caller with nowhere to "
                         "keep it would hand the resuming sibling a freed value");
    if (JS_IsUninitialized(*held)) {
        JSValue slots = signal_slots(ctx, sig);

        /* NOT AN AbortSignal, OR ONE WITH NO SLOT RECORD: §3.2's own answer is that it is not aborted, and
           there is no unknown to fork over. The non-parking form answers the same way for the same state. */
        if (!JS_IsObject(slots)) {
            JS_FreeValue(ctx, slots);
            *out = 0;
            return 0;
        }
        *held = JS_GetPropertyStr(ctx, slots, "aborted");
        JS_FreeValue(ctx, slots);
    }
    r = step_tobool_run(ctx, h, *held, "DOM §3.2 Interface AbortSignal `aborted`", out);
    if (r) return r;            /* JS_STEP_FORK — the operand stays held, because the sibling resumes AT it */
    DCHECK(*out == 0 || *out == 1,
           "§3.2's aborted test came back from the branch seam with neither truth value — the seam answers a "
           "two-armed question, so a third value is an arm this file never declared");
    JS_FreeValue(ctx, *held);
    *held = JS_UNINITIALIZED;
    return 0;
}

JSValue abort_signal_reason_at(JSContext *ctx, JSValueConst sig, const char *site)
{
    JSValue slots = signal_slots(ctx, sig), v;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    v = signal_is_aborted(ctx, slots, site) ? JS_GetPropertyStr(ctx, slots, "reason") : JS_UNDEFINED;
    JS_FreeValue(ctx, slots);
    return v;
}

/* ---- §3.2 "signal abort", THE WHOLE OPERATION ------------------------------------------------------------- */

enum { SA_START = 0, SA_TAKE, SA_ALGOS, SA_FIRE, SA_DONE };

void abort_signal_work_start(AbortSignalWork *w)
{
    int k;
    /* A step state arrives ZEROED, and a zeroed JSValue is the INTEGER 0 rather than undefined. */
    w->stage = SA_START;
    w->phase = 0;
    w->i = w->j = 0;
    w->targets = w->algos = w->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(w->cb, k) w->cb[k] = JS_UNDEFINED;
}

void abort_signal_work_visit(JSContext *ctx, AbortSignalWork *w, JSStepVisit *v)
{
    int k;
    v->val(ctx, &w->targets);
    v->val(ctx, &w->algos);
    v->val(ctx, &w->ev);
    STEP_CB_FOREACH(w->cb, k) v->val(ctx, &w->cb[k]);
}

void abort_signal_work_release(JSContext *ctx, AbortSignalWork *w)
{
    int k;
    JS_FreeValue(ctx, w->targets);
    JS_FreeValue(ctx, w->algos);
    JS_FreeValue(ctx, w->ev);
    w->targets = w->algos = w->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(w->cb, k) { JS_FreeValue(ctx, w->cb[k]); w->cb[k] = JS_UNDEFINED; }
}

int abort_signal_run(JSContext *ctx, AbortSignalWork *w, JSValueConst sig, JSValueConst reason,
                     JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (w->stage == SA_START) {
        if (!signal_abort_state(ctx, sig, JS_DupValue(ctx, reason))) {
            /* Already aborted: §3.2 step 1 returns, so nothing runs and nothing fires. */
            JS_FreeValue(ctx, in);
            STEP_GOTO(w->stage, SA_DONE, &w->phase, NULL);
            return 0;
        }
        /* STEPS 3-4, ENTIRELY BEFORE ANY OF THE PAGE'S CODE RUNS. Every non-aborted dependent takes THIS
           signal's (already-defaulted) reason now, so the source's own algorithms and `abort` listeners run in
           a world where each dependent already reads `aborted === true`. Deferring a dependent's state to its
           own turn of the walk below would be one turn late and page-visible. */
        w->targets = JS_NewArray(ctx);
        CHECK(!JS_IsException(w->targets), "signal abort: OOM collecting the signals whose abort steps run");
        JS_SetPropertyUint32(ctx, w->targets, 0, JS_DupValue(ctx, sig));
        {
            JSValue deps = signal_list(ctx, sig, "deps", 0);
            uint32_t k, n = JS_IsArray(deps) ? array_len(ctx, deps) : 0;
            for (k = 0; k < n; k++) {
                JSValue d = JS_GetPropertyUint32(ctx, deps, k);
                if (!abort_signal_aborted(ctx, d)) {
                    signal_adopt_reason(ctx, d, abort_signal_reason(ctx, sig));
                    JS_SetPropertyUint32(ctx, w->targets, array_len(ctx, w->targets), JS_DupValue(ctx, d));
                }
                JS_FreeValue(ctx, d);
            }
            JS_FreeValue(ctx, deps);
        }
        w->j = 0;
        STEP_GOTO(w->stage, SA_TAKE, &w->phase, NULL);
    }

    /* STEPS 5-6: "run the abort steps" for the signal, then for each dependent that took its reason. One walk,
       because the two steps ARE the same three sub-steps applied to different signals. */
    for (;;) {
        JSValue cur;

        if (w->stage == SA_DONE)
            break;
        DCHECK(JS_IsArray(w->targets), "a signal-abort request resumed with no target list");
        if (w->j >= array_len(ctx, w->targets)) { STEP_GOTO(w->stage, SA_DONE, &w->phase, NULL); break; }
        cur = JS_GetPropertyUint32(ctx, w->targets, w->j);

        if (w->stage == SA_TAKE) {
            /* THE LIST IS SNAPSHOT AND EMPTIED BEFORE THE FIRST ALGORITHM RUNS. DOM §3.2 empties it as its own
               step, and an algorithm is free to abort another signal that shares this one's list-manipulating
               code; a walk over the live array would then run an entry that was added after the operation
               began, which the spec's "for each algorithm of signal's abort algorithms" over an emptied list
               cannot. */
            JS_FreeValue(ctx, w->algos);
            w->algos = signal_algos(ctx, cur, 0);
            {
                JSValue slots = signal_slots(ctx, cur);
                if (JS_IsObject(slots))
                    JS_SetPropertyStr(ctx, slots, "algorithms", JS_UNDEFINED);
                JS_FreeValue(ctx, slots);
            }
            w->i = 0;
            STEP_GOTO(w->stage, SA_ALGOS, &w->phase, NULL);
        }

        while (w->stage == SA_ALGOS) {
            JSValue out;
            if (!JS_IsArray(w->algos) || w->i >= array_len(ctx, w->algos)) {
                STEP_GOTO(w->stage, SA_FIRE, &w->phase, NULL);
                break;
            }
            {
                JSValue fn = JS_GetPropertyUint32(ctx, w->algos, w->i);
                r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), fn, JS_UNDEFINED, 0, NULL, in, &out,
                                  out_cb, out_argc);
                JS_FreeValue(ctx, fn);
            }
            if (r > 0) { JS_FreeValue(ctx, cur); return r; }
            in = JS_UNDEFINED;
            if (JS_IsException(out)) { JS_FreeValue(ctx, cur); return -1; }
            JS_FreeValue(ctx, out);
            w->i++;
        }

        if (w->stage == SA_FIRE) {
            /* DOM §3.2 "fire an event named abort at signal" — the ONE §2.9 dispatch, as a REQUEST, so the
               listeners run as ordinary preemptible page code and the caller resumes after every one of them
               has returned. */
            if (JS_IsUndefined(w->ev))
                w->ev = event_new(ctx, "abort", /*bubbles*/ false, /*cancelable*/ false);
            r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), cur, w->ev, JS_UNDEFINED, in, NULL, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r > 0) { JS_FreeValue(ctx, cur); return r; }
            if (r < 0) { JS_FreeValue(ctx, cur); return -1; }
            /* The event belongs to the signal it was dispatched at: §2.9 leaves `target` and the dispatch
               flag on it, so the next target gets its own. */
            JS_FreeValue(ctx, w->ev);
            w->ev = JS_UNDEFINED;
            w->j++;
            STEP_GOTO(w->stage, SA_TAKE, &w->phase, NULL);
        }
        JS_FreeValue(ctx, cur);
    }
    JS_FreeValue(ctx, in);
    DCHECK(w->stage == SA_DONE, "a signal-abort request resumed in a stage it never parks in");
    return 0;
}

/* §3.2 abort() — A MACHINE, because the spec makes the dispatch SYNCHRONOUS: a page that calls ac.abort() and
   then reads a flag its listener set must see it already set, and a queued fire answers after abort() returned.
   Every step before the fire runs none of the page's code (the slot reads are own slots, the default reason is
   an engine-built DOMException), so the machine has exactly one suspension point: the listeners. */
typedef struct JSAbortState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   sig;      /* the signal being aborted (owned) */
    AbortSignalWork w;  /* the shared "signal abort" request's own record */
} JSAbortState;

static void js_abort_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSAbortState *s = st;
    v->val(ctx, &s->sig);
    abort_signal_work_visit(ctx, &s->w, v);
}

/* WHERE THIS MACHINE RESTS. DOM §3.1 "Interface AbortController"'s abort() is one step — "signal abort on
   this with reason if it is given" — and DOM §3.1 states that controller-level operation as "signal abort on
   controller's signal with reason if it is given", so what runs the signal's abort algorithms and fires
   `abort` at it is that second operation, which is the page's code. So the operand is one stage and the
   operation is the other; `started` was a private flag standing in for exactly that split, with no way to
   say which of the two a parked flow was at. */
#define ABORT_STAGES(X) \
    X(ABORT_SIGNAL_SLOT, "DOM §3.1 AbortController.abort() (this's signal, the operand step 1 hands on)") \
    X(ABORT_SIGNAL_RUN,  "DOM §3.1 AbortController.abort() step 1 (signal abort on it: the abort algorithms, " \
                         "then the `abort` event)")
enum { ABORT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ABORT_STEPS[] = { ABORT_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_abort_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSAbortState *s = st;
    int r;

    if (s->hdr.stage == ABORT_SIGNAL_SLOT) {
        /* §3.2 step 1 is `this.[[Signal]]` — an INTERNAL SLOT. Read as an own slot, so no accessor and no proxy
           trap can sit on it: a page that assigns over the public `signal` property does not redirect abort(). */
        JSValue slots = signal_slots(ctx, s->hdr.this_val);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->sig = JS_UNDEFINED;
        abort_signal_work_start(&s->w);
        s->hdr.stage = ABORT_SIGNAL_RUN;
        if (JS_IsObject(slots))
            s->sig = JS_GetPropertyStr(ctx, slots, "signal");
        JS_FreeValue(ctx, slots);
        if (!JS_IsObject(s->sig)) {
            JS_ThrowTypeError(ctx, "abort called on something that is not an AbortController");
            return JS_STEP_ABRUPT;
        }
    }
    /* THE WHOLE OF "signal abort", not a piece of it: the reason travels verbatim (the operation is what turns
       an undefined one into an AbortError), the signal's abort algorithms run, and then the `abort` event is
       fired at it. abort() has one thing to do and this is it. */
    DCHECK(s->hdr.stage == ABORT_SIGNAL_RUN, "abort() resumed into a stage §3.2 does not have");
    r = abort_signal_run(ctx, &s->w, s->sig, step_arg(&s->hdr, 0), cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_abort_def = {
    sizeof(JSAbortState), js_abort_step, NULL, 0,   /* §3.1 abort() returns undefined whatever the listeners did */
    .visit = js_abort_visit,
    .algorithm = "DOM §3.1 AbortController.abort(reason)", .steps = ABORT_STEPS
};
static int g_abort_stepid = -1;

JSValue abort_signal_new(JSContext *ctx)
{
    return signal_new(ctx, JS_FALSE, JS_UNDEFINED);
}

/* §3.2's `[SameObject] readonly attribute AbortSignal signal` — an ACCESSOR on the prototype reading the
   [[Signal]] slot, which is where the IDL puts it. It was an own DATA property, and the difference is not
   cosmetic: a page could assign over `controller.signal` and hand the next reader a different object while
   abort() went on aborting the real one. [SameObject] is satisfied because the slot holds one signal for the
   controller's whole life. */
static JSValue js_ctrl_get_signal(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = signal_slots(ctx, this_val), sig;
    (void)magic;
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "not an AbortController");
    }
    sig = JS_GetPropertyStr(ctx, slots, "signal");
    JS_FreeValue(ctx, slots);
    return sig;
}

static JSValue js_abort_controller_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValue obj, sig;
    (void)argc; (void)argv;

    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor AbortController requires 'new'");
    {
        JSValue cp = JS_GetClassProto(ctx, g_ctrl_class);
        DCHECK(!JS_IsNull(cp), "an AbortController was built in a realm with no AbortController.prototype");
        obj = JS_NewObjectProto(ctx, cp);
        JS_FreeValue(ctx, cp);
    }
    if (JS_IsException(obj))
        return obj;
    sig = signal_new(ctx, JS_FALSE, JS_UNDEFINED);
    /* [[Signal]] — the ONE place the signal lives. Both abort() and the `signal` getter read it. */
    {
        JSValue st = idl_slots_new(ctx);
        JSAtom k = JS_ValueToAtom(ctx, g_key);
        CHECK(!JS_IsException(st) && k != JS_ATOM_NULL, "the AbortController slot record allocation failed");
        JS_SetPropertyStr(ctx, st, "signal", sig);
        JS_SetProperty(ctx, obj, k, st);
        JS_FreeAtom(ctx, k);
    }
    return obj;
}

/* AbortSignal.abort(reason) — §3.2: a signal that is already aborted. */
static JSValue js_sig_static_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue reason;
    (void)this_val;
    reason = abort_reason_or_default(ctx, (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED);
    return signal_new(ctx, JS_TRUE, reason);
}

/* AbortSignal.timeout(ms) — the UNKNOWN one, and the one member of this interface that reaches the page's code.
   The flag is concolic because whether the deadline has passed when the code asks depends on wall-clock this
   engine does not model, and both answers lead to code worth reaching. The MACHINE is because
   `[EnforceRange] unsigned long long milliseconds` is ToNumber on whatever was passed, so
   `AbortSignal.timeout({ valueOf() { for(;;){} } })` is the page's loop: it has to suspend and resume at the
   exact stage, which is what step_toint64_run parks on. */
/* WHERE THIS MACHINE RESTS. The coercion is not part of §3.2's four steps — it is Web IDL's
   `[EnforceRange] unsigned long long milliseconds`, which precedes step 1 and is the page's `valueOf` — so it
   is its own stage. It was folded into the same stage as the build, which is a rest point inside the page's
   code sharing a number with one after it. */
#define TIMEOUT_STAGES(X) \
    X(TIMEOUT_COERCE, "Web IDL §3.2.4.8 [EnforceRange] unsigned long long (converting `milliseconds`, which runs " \
                      "the page's valueOf)") \
    X(TIMEOUT_BUILD,  "DOM §3.2 AbortSignal.timeout steps 1-4 (a new AbortSignal, its aborted state and its " \
                      "TimeoutError reason)")
enum { TIMEOUT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TIMEOUT_STEPS[] = { TIMEOUT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSTimeoutState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   result;   /* the signal, once built (owned) */
} JSTimeoutState;

/* WHAT THIS MACHINE OWNS: its answer. The coercion's own in-flight value lives on the header, which the shared
   teardown releases. */
static void js_timeout_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTimeoutState *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_timeout_fini(JSContext *ctx, void *st, bool take_result)
{
    JSTimeoutState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    (void)ctx;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* DOM §3.2 STEP 3's COMPLETION STEPS — "queue a global task on the timer task source given global to signal abort
 * given signal and a new TimeoutError DOMException". A machine, because signalling abort RUNS THE PAGE'S CODE:
 * the signal's abort algorithms and then its `abort` listeners, with every dependent signal taking the reason
 * first. HTML §8.7 Timers's timer_after performs it at the expiry, on the same task source the page's own timers are
 * on, so a timeout signal is ordered against them the way a browser orders it.
 *
 * THE SIGNAL IS CAPTURED, NOT PASSED. §8.7 Timers performs the completion steps with no arguments — it has none to
 * give — so the one thing this needs travels as closure data, which is what JS_NewStepClosure is for. */
#define TIMEOUT_FIRE_STAGES(X) \
    X(TIMEOUT_FIRE_RUN, "DOM §3.2 AbortSignal.timeout step 3's queued task (signal abort on the timeout " \
                        "signal with a new TimeoutError DOMException: its abort algorithms, then `abort`)")
enum { TIMEOUT_FIRE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TIMEOUT_FIRE_STEPS[] = { TIMEOUT_FIRE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr      hdr;
    AbortSignalWork w;   /* the shared "signal abort" operation's record — one implementation, two callers */
    /* HAS THE WORK RECORD BEEN STARTED — a flag rather than a test on one of its fields, because a step state
       arrives ZEROED and a zeroed JSValue is the INTEGER 0 rather than undefined (abort_signal_work_start says
       so where it sets them). Zero is "not yet", which is the one thing readable before anything has written. */
    uint8_t        started;
} JSTimeoutFireState;

static void js_timeout_fire_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTimeoutFireState *s = st;
    if (s->started) abort_signal_work_visit(ctx, &s->w, v);
}

static int js_timeout_fire_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTimeoutFireState *s = st;
    JSValueConst sig = JS_StepClosureData(&s->hdr, 0);
    JSValue reason;
    int r;

    DCHECK(s->hdr.stage == TIMEOUT_FIRE_RUN,
           "the timeout signal's queued task resumed into a stage §3.2 does not have");
    if (!s->started) {
        abort_signal_work_start(&s->w);
        s->started = 1;
    }
    /* A NEW DOMException EACH TIME, which is the step's own wording: the signal was CREATED holding a default
       reason so `signal.reason` reads sensibly before the deadline, and the abort carries a fresh one. */
    reason = abort_reason_default(ctx, "TimeoutError", "signal timed out");
    r = abort_signal_run(ctx, &s->w, sig, reason, cb_result, out_cb, out_argc);
    JS_FreeValue(ctx, reason);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_timeout_fire_def = {
    sizeof(JSTimeoutFireState), js_timeout_fire_step, NULL, 0,
    .visit = js_timeout_fire_visit,
    .algorithm = "DOM §3.2 AbortSignal.timeout step 3's completion steps",
    .steps = TIMEOUT_FIRE_STEPS
};
static int g_timeout_fire_stepid = -1;

static int js_timeout_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTimeoutState *s = st;
    int64_t ms = 0;
    JSValue flag;
    int r;

    if (s->hdr.stage == TIMEOUT_COERCE) {
        s->result = JS_UNDEFINED;
        /* The coercion runs whatever the page put in `valueOf`, and its SIDE EFFECTS are observable — so it
           runs even though the duration itself does not change what this engine models. Dropping it would be a
           quieter spec bug than getting the number wrong. */
        r = step_toint64_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &ms, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = TIMEOUT_BUILD;
    }
    DCHECK(s->hdr.stage == TIMEOUT_BUILD, "AbortSignal.timeout resumed into a stage §3.2 does not have");

    /* The SHAPE carries its provenance in braces and the source identity is it bare — concolic_new's rule.
       A timeout signal's flag is the one §3.2 value a page branches on (`if (signal.aborted)` picks the
       fallback path and its endpoints), so a shape naming no hole meant that gate recorded nothing. */
    flag = concolic_new(ctx, "{AbortSignal.timeout().aborted}", "AbortSignal.timeout().aborted", JS_FALSE);
    /* THE EXAMPLE IS FALSE AND TWO SEAMS DEPEND ON IT AGREEING. A session that explores nothing answers this
       predicate two different ways depending on which form asked: the plain one takes
       signal_aborted_nonforking, which is ToBoolean of THIS example, and the parking one takes the step
       driver's numbering rule, which for a two-armed truth is arm 0 — false. They agree only while the example
       below is false, and a producer that minted a true one would make a conformance run's two forms disagree
       about one signal with nothing to say so. Asserted HERE, at the mint, because this is the only line that
       decides it. */
#if APICLIENT_DEV
    /* THE READ IS INSIDE THE DEV GUARD BECAUSE concolic_example DUPS, and a DCHECK's condition must be
       side-effect-free: the allocation happens here, the assert reads two locals, and release does neither. */
    {
        JSValue ex = concolic_example(ctx, flag);
        DCHECK(JS_IsBool(ex) && !JS_ToBool(ctx, ex),
               "§3.2's timeout signal was minted with an `aborted` example that is not FALSE — abort.h's "
               "parking form documents that the plain and parking seams give a non-forking session the same "
               "answer, and that equality is this example being false: the plain form answers ToBoolean of it "
               "and the parking form answers the step driver's arm 0");
        JS_FreeValue(ctx, ex);
    }
#endif
    CHECK(!JS_IsException(flag), "minting the timeout signal's aborted flag failed");
    s->result = signal_new(ctx, flag, abort_reason_default(ctx, "TimeoutError", "signal timed out"));
    /* DOM §3.2 STEP 3: "run steps after a timeout given global, \"AbortSignal-timeout\", milliseconds, and the
       following step: queue a global task on the timer task source given global to signal abort given signal
       and a new TimeoutError DOMException."
       THE CONCOLIC FLAG ABOVE IS A DIFFERENT QUESTION and both are needed. It answers `has the deadline passed
       by the time the code asks`, which is unknown and forks; this schedules the abort that actually happens,
       which RUNS THE PAGE'S CODE — the signal's abort algorithms and its `abort` listeners, with every
       dependent signal taking the reason first. Without it `AbortSignal.timeout(0).addEventListener('abort',f)`
       never ran f, a fetch's abort algorithm never shut the request down, and `AbortSignal.any([c.signal,
       timeout])` had one arm that could not fire.
       THE SIGNAL TRAVELS AS CLOSURE DATA because §8.7 Timers performs the completion steps with no arguments. */
    if (g_timeout_fire_stepid < 0) {
        g_timeout_fire_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_timeout_fire_def);
        /* DECLARED HERE AND NOT IN abort_build_agent, BECAUSE HERE IS WHERE IT IS SET. It is the one slot of
           this component's that is minted LAZILY — an agent whose pages never call `AbortSignal.timeout()`
           leaves it at -1 and owes the undo nothing — so a declaration written beside the eager three would
           be a claim about a registration that may never happen. The guard is what makes this run once:
           core/agent_state.h aborts on one address declared twice. */
        agent_state_id("abort", &g_timeout_fire_stepid,
                       "§3.2 step 3's completion steps — the machine `run steps after a timeout` parks on, "
                       "registered at the first timeout signal rather than at declaration");
    }
    {
        JSValueConst data = s->result;
        JSValue steps = JS_NewStepClosure(ctx, g_timeout_fire_stepid, 0, 1, &data);

        CHECK(!JS_IsException(steps),
              "AbortSignal.timeout: the completion steps' callee could not be allocated — a timeout signal "
              "whose abort was never scheduled reads as one that simply never fires");
        timer_after(ctx, (double)ms, steps);
        JS_FreeValue(ctx, steps);
    }
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_timeout_def = {
    sizeof(JSTimeoutState), js_timeout_step, js_timeout_fini, 0, .visit = js_timeout_visit,
    .algorithm = "DOM §3.2 AbortSignal.timeout(milliseconds)", .steps = TIMEOUT_STEPS
};

/* ---- DOM §3.2's AbortSignal.any(signals) ------------------------------------------------------------------------
 *
 * `[NewObject] static AbortSignal any(sequence<AbortSignal> signals)`, and DOM states its steps as exactly one:
 * "return the result of creating a dependent abort signal from signals using AbortSignal and the current realm".
 * So the member IS the dependent-signal machinery above, which is why that had to exist first — an `any` written
 * as `add an algorithm to each input that aborts the result` would be a different algorithm wearing this one's
 * name, one turn late and page-visible (see abort.h).
 *
 * EVERYTHING BEFORE THAT ONE STEP IS WEB IDL §3.2.21's CONVERSION, AND IT IS THE PAGE'S CODE FROM END TO END —
 * the @@iterator read, the call that returns the iterator, the `next` read, every `next()` call and every
 * `done`/`value` read off its result. A C loop over it is the drive-to-completion this engine aborts on, so the
 * member is a machine driving core/idl_iter.h's shared cursor, exactly as URLSearchParams' sequence arm does:
 * `AbortSignal.any({ *[Symbol.iterator]() { while (true) yield c.signal; } })` suspends and resumes at whichever
 * `next()` it was inside, and a sibling flow runs meanwhile.
 *
 * THE ELEMENT TYPE IS CONVERTED AS EACH ELEMENT ARRIVES, NEVER AFTER THE WALK. §3.2.21.1 step 3.3 converts S_i
 * the moment the iterator yielded it, so `AbortSignal.any([sig, 0, sig2])` throws its TypeError with the third
 * element never asked for — collecting first and brand-checking after is one observable operation too many, and
 * the operation count is what a test pins. */
#define ANY_STAGES(X) \
    X(ANY_START,   "Web IDL §3.6 step 5 and §3.2.21 step 1 (`signals` is required, so a call with no argument " \
                   "empties the effective overload set; a non-Object is a TypeError before @@iterator is read)") \
    X(ANY_ELEMENT, "Web IDL §3.2.21.1 step 3 (the next value of `sequence<AbortSignal> signals`, then step 3.3's " \
                   "§3.2.15 interface-type conversion of what the iterator yielded)") \
    X(ANY_CREATE,  "DOM §3.2 AbortSignal.any step 1 (create a dependent abort signal from signals using " \
                   "AbortSignal and the current realm)")
enum { ANY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ANY_STEPS[] = { ANY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSAnyState {
    JSStepHdr  hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    IterCursor cur;      /* §3.2.21.1's protocol over the argument, one value per turn */
    JSValue    signals;  /* the sequence converted so far, an Array (owned) */
    JSValue    result;   /* the dependent signal, once created (owned) */
    /* §3.2 step 2's TWO SLOTS, which are this machine's because the driver clones THIS state at the ask — see
       abort.h at abort_signal_dependent_step. `dep_flag` is the seam's borrowed `aborted` flag, held across the
       park, and `dep_at` is the element the outstanding ask is about. A plain integer is byte-copied by the
       clone, which is why only the first of the two is visited. */
    JSValue    dep_flag;
    uint32_t   dep_at;
} JSAnyState;

/* WHAT THIS MACHINE OWNS: the cursor's five in-flight values and its call buffer (the cursor declares its own),
   the list being built, and the answer. A fork mid-conversion gives each arm its own list — two arms of a
   branch inside the page's iterator hand `any()` two different sequences, which is the whole point. */
static void js_any_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSAnyState *s = st;
    iter_cursor_visit(ctx, &s->cur, v);
    v->val(ctx, &s->signals);
    v->val(ctx, &s->result);
    v->val(ctx, &s->dep_flag);   /* §3.2 step 2's borrowed `aborted` flag, held across its fork */
}

static JSValue js_any_fini(JSContext *ctx, void *st, bool take_result)
{
    JSAnyState *s = st;
    JSValue r;

    (void)ctx;
    /* THE ANSWER IS NOT MINTED UNTIL ANY_CREATE RUNS, so a taken result that is still EMPTY is a completion
       claimed for a stage that never answered — the two throws at ANY_START and the two at ANY_ELEMENT all
       return JS_STEP_ABRUPT, which is the arm that takes nothing. This is the slot's own emptiness sentinel and
       not JS_UNDEFINED, because UNDEFINED is a value a finished algorithm could legally hold. */
    DCHECK(!take_result || !JS_IsUninitialized(s->result),
           "AbortSignal.any's result was taken before §3.2 step 1 had minted it — only the ANY_CREATE stage "
           "answers, and every abrupt arm of this machine returns JS_STEP_ABRUPT instead");
    r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static int js_any_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSAnyState *s = st;
    JSValue in = cb_result;
    int r;

    if (s->hdr.stage == ANY_START) {
        /* §3.6 step 5: `signals` is not optional, so a call with no argument removes the member's only entry
           from the effective overload set and throws — which a page must be able to tell apart from
           `AbortSignal.any([])`, a VALID call yielding a signal that nothing can ever abort.
           THE STATE IS COMPLETE BEFORE EITHER THROW, because the teardown runs through fini either way. */
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        iter_cursor_init(&s->cur);
        s->signals = JS_UNDEFINED;
        /* §3.2 STEP 1'S ANSWER AND STEP 2'S TWO SLOTS, EMPTY AS A THING THIS STAGE STATES. A zeroed step state's
           JSValue is the INTEGER 0 rather than JS_UNDEFINED, so neither `result`'s "not minted yet" nor
           `dep_flag`'s "nothing borrowed" may be read off the slot — and `result` is JS_UNINITIALIZED rather
           than JS_UNDEFINED for the same reason, because UNDEFINED is what a completed algorithm could legally
           hold and the build below mints on emptiness.
           THE INIT IS IN A STAGE THE ASKING STAGE CANNOT RE-ENTER, which is why abort.h's `step_fork_pending`
           exit is not needed here and the assert at the ask states the fact instead. `stage` is assigned only
           FORWARD in this machine (START → ELEMENT → CREATE) and a clone carries it, so a fork at step 2's ask
           resumes at ANY_CREATE and never reaches this line twice. fetch.c needs that exit because its init and
           its ask share one stage; this machine's do not, and the stage test is the stronger guard of the two. */
        s->result = s->dep_flag = JS_UNINITIALIZED;
        s->dep_at = 0;
        if (s->hdr.argc < 1) {
            JS_ThrowTypeError(ctx, "AbortSignal.any requires 1 argument, but only 0 were passed");
            return JS_STEP_ABRUPT;
        }
        /* §3.2.21 step 1: a non-Object is a TypeError HERE, before @@iterator is asked for. It is not a
           formality — `AbortSignal.any("")` must throw without reading String.prototype[@@iterator], and a
           conversion that starts at the read would iterate a string's characters and fail one step later with
           the wrong error after one operation too many. */
        if (!JS_IsObject(step_arg(&s->hdr, 0))) {
            JS_ThrowTypeError(ctx, "AbortSignal.any: the argument is not an object");
            return JS_STEP_ABRUPT;
        }
        s->signals = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->signals), "AbortSignal.any: the sequence being converted could not be allocated");
        s->hdr.stage = ANY_ELEMENT;
    }

    while (s->hdr.stage == ANY_ELEMENT) {
        r = iter_cursor_run(ctx, &s->hdr, &s->cur, step_arg(&s->hdr, 0), in, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        in = JS_UNDEFINED;
        if (s->cur.done) { s->hdr.stage = ANY_CREATE; break; }
        /* §3.2.15's interface-type conversion: a platform object implementing AbortSignal crosses as itself and
           anything else is a TypeError. The brand is the private slot record, which the page cannot forge. */
        if (!abort_signal_is(ctx, s->cur.value)) {
            JS_ThrowTypeError(ctx, "AbortSignal.any: an element of the sequence is not an AbortSignal");
            return JS_STEP_ABRUPT;
        }
        JS_SetPropertyUint32(ctx, s->signals, array_len(ctx, s->signals), JS_DupValue(ctx, s->cur.value));
    }

    /* Step 1, THROUGH THE PARKING FORM, because step 2 CAN FORK and this machine CAN carry the sibling.
       THIS USED TO SAY "it runs none of the page's code … so it is one stage and the machine has nothing left to
       rest on", AND IT IS KEPT IN ITS OWN WORDS BECAUSE EVERY CLAUSE OF IT IS TRUE AND THE CONCLUSION IS NOT.
       None of the page's code does run here; what runs is a SOLVER ASK, once per input signal, over the one flag
       §3.2 models as unknown — and an ask that forks needs a rest point exactly as a callback does. So the
       sentence was right about page code, right about own slots, and wrong about the only thing it was being
       read for. A reader who re-derives `no page code, therefore no rest point` will write it again.
       WHAT IT COST: `AbortSignal.any([AbortSignal.timeout(n), c.signal])` — the spelling a bundle writes for
       "whichever comes first" — asked step 2's test through the non-parking form from inside this plain C
       helper, reached solver/engine.c's `engine_prepare_fork` with nowhere for the sibling to resume, and
       ABORTED. `js_any_def` has declared this body a step machine the whole time, so nothing had to be built:
       the driver holds the resume point it clones at and the whole repair is asking through the seam that can
       return the fork code. The crash's remedy clause says "declare that builtin a step machine", which is the
       right remedy for a body that is not one and is not what this population needed.
       THE STAGE IS UNCHANGED ACROSS THE PARK, deliberately: the sibling re-enters AT the ask and `dep_at` is
       what tells it WHICH element the seam is holding the flag of. */
    DCHECK(s->hdr.stage == ANY_CREATE, "AbortSignal.any resumed into a stage §3.2 does not have");
    /* NOTHING IS DELIVERED INTO THIS STAGE, WHICH IS WHY NOTHING FREES `in` ON THIS PATH. The loop above sets it
       to JS_UNDEFINED before it breaks, and the only way to re-enter here is step 2's fork, whose two entries
       quickjs re-enters with JS_UNDEFINED rather than with the same delivery (quickjs.c states it at
       `step_fork_pending`). A real value arriving here would be a reference this stage drops on the floor, so the
       fact is asserted rather than papered over with a free that would ALSO drop it. */
    DCHECK(JS_IsUndefined(in),
           "AbortSignal.any's step-1 stage was entered carrying a delivery — this stage asks only the branch "
           "seam, whose fork entries arrive with JS_UNDEFINED, so a value here is one nothing in this machine "
           "will release");
    /* THE SEAM'S BORROW AND THIS MACHINE'S OUTSTANDING FORK AGREE, asserted at the one point they can disagree
       — the same one fact fetch.c's §5.6 step 4 states, and the only thing about this slot a reader can check.
       A held slot with no fork outstanding is a reference nothing will free; an empty one with a fork
       outstanding is the operand the resuming arm was about to be asked about. */
    DCHECK(step_fork_pending(&s->hdr) ? !JS_IsUninitialized(s->dep_flag)
                                      : JS_IsUninitialized(s->dep_flag),
           "AbortSignal.any reached §3.2 step 2's ask with the seam's borrow and this machine's outstanding fork "
           "disagreeing — abort_signal_dependent_step fills `dep_flag` at the ask, HOLDS it across JS_STEP_FORK "
           "because the sibling resumes AT the ask, and clears it when the ask answers");
    r = abort_signal_dependent_step(ctx, &s->hdr, s->signals, &s->result, &s->dep_flag, &s->dep_at);
    if (r)
        return r;   /* PARKED. Nothing is outstanding to discharge: `in` was consumed at ANY_START or above. */
    DCHECK(!JS_IsUninitialized(s->result),
           "§3.2's create-a-dependent-abort-signal answered without minting step 1's signal — it answers 0 only "
           "with the algorithm complete, and step 1 is its first statement");
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_any_def = {
    sizeof(JSAnyState), js_any_step, js_any_fini, 0, .visit = js_any_visit,
    .algorithm = "DOM §3.2 AbortSignal.any(signals)", .steps = ANY_STEPS
};
static int g_any_stepid = -1;

/* §3.2's IDL declares no constructor, so `new AbortSignal()` is a TypeError — and so is calling it. The
   interface object exists only to carry the statics and to be the thing `instanceof` names. */
static JSValue js_abort_signal_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* §3.2's STEP IDS AND CLASSES ARE THE AGENT'S; the PROTOTYPES are each realm's — see abort_install_protos.
   A step id is a runtime registration and a class id is one too, so both are minted once for the whole agent;
   the two prototype OBJECTS are Web IDL §3.7's per-realm ones, and the realm registry builds them. */
static void abort_build_agent(JSContext *ctx)
{
    JSClassDef sd = { "AbortSignal" }, cd = { "AbortController" };

    if (g_timeout_stepid < 0) {
        g_abort_rt = JS_GetRuntime(ctx);
        agent_state_ptr("abort", &g_abort_rt, "the runtime §3.2's three step machines were registered in");
        g_timeout_stepid = JS_RegisterStepDef(g_abort_rt, &js_timeout_def);
        agent_state_id("abort", &g_timeout_stepid,
                       "§3.2's `AbortSignal.timeout()` machine — `[EnforceRange] unsigned long long` is "
                       "ToNumber on whatever the page passed, so the coercion is the page's own code");
        g_abort_stepid = JS_RegisterStepDef(g_abort_rt, &js_abort_def);
        agent_state_id("abort", &g_abort_stepid, "§3.2's `AbortSignal.abort()` machine");
        g_any_stepid = JS_RegisterStepDef(g_abort_rt, &js_any_def);
        agent_state_id("abort", &g_any_stepid,
                       "§3.2's `AbortSignal.any()` machine — `sequence<AbortSignal>` is Web IDL "
                       "§3.2.21.1's iterator protocol, which is the page's code at every step of it");
        /* §3.2's TWO FLAG-TESTING MEMBERS — ONE DECLARATION AND TWO MAGICS, because they are one branch with two
           completions (sig_ask_step). DECLARED THROUGH THE IDL POOL rather than with JS_RegisterStepDef beside
           the three above, and the reason is the ATTRIBUTE: core/idl_args.h's idl_install_accessor_step is this
           engine's one form for a machine behind an accessor, and it mints from the pool — which is also what
           states §3.7.6 Attributes' accessor name and §3.7.7 Operations' length instead of this file spelling
           either. The pool is sealed after agent init, so a declaration made here is made once per AGENT and
           idl_declared_before_seal is what catches a per-realm one. */
        g_sig_throw_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &SIG_ASK_DECL, SIG_ASK_THROW);
        agent_state_id("abort", &g_sig_throw_stepid,
                       "§3.2's `throwIfAborted()` machine — it asks whether the signal is aborted, so the fork "
                       "that ask raises needs a state for the sibling to resume at");
        g_sig_reason_stepid = idl_getter_id_step(ctx, &SIG_ASK_DECL, SIG_ASK_REASON);
        agent_state_id("abort", &g_sig_reason_stepid,
                       "§3.2's `reason` getter machine — the same ask and the other completion");
    }
    if (g_sig_class) return;
    JS_NewClassID(JS_GetRuntime(ctx), &g_sig_class);
    JS_NewClass(JS_GetRuntime(ctx), g_sig_class, &sd);
    /* THE SHARP ONE. This release used to leave the id SET, and an id carried into a second agent names a
       class in a runtime that is gone while the `if (g_sig_class) return;` above reads it as already
       declared — so that agent's abort_build_agent returns before re-registering and every AbortSignal it
       mints is branded with a number the live runtime never issued. It is also the class a DECLARED
       interface-typed position brands against (abort_signal_class below), so a zero there is a declaration
       that brands nothing. */
    agent_state_class("abort", &g_sig_class,
                      "§3.2's AbortSignal class — the brand abort_signal_class hands a declaration, and "
                      "this file's declaration latch");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ctrl_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ctrl_class, &cd);
    agent_state_class("abort", &g_ctrl_class, "§3.1's AbortController class");
    realm_declare_intrinsic(abort_install_protos);
}

/* DOM §3.1 "Interface AbortController"'s AND §3.2 "Interface AbortSignal"'s TWO INTERFACE PROTOTYPE OBJECTS
   *AND* THEIR TWO INTERFACE OBJECTS, FOR ONE REALM.
   BOTH INTERFACES ARE `[Exposed=*]`, AND WEB IDL §3.8 "Platform objects implementing interfaces" IS GIVEN A
   REALM. Its `define the global property references` is "To define the global property references on target,
   given realm realm" and its step 1 is "Let interfaces be a list that contains every interface that is exposed
   in realm" — a REALM, with no Document anywhere in the algorithm. The two interface objects used to be placed
   from core/platform.c's per-DOCUMENT column through an `abort_install`, so a realm that reaches no
   platform_document_install got neither name: a worker realm always, and a Window realm until a Document was
   installed over it. They are minted here instead, beside the two prototypes this function has already built,
   which is also what removes the two prototype re-reads that entry made — a JS_GetClassProto of g_ctrl_class
   and an abort_signal_proto, each a second answer to a question settled a few lines up.
   A NAMED RESIDUAL, AND THE ENGINE IS RIGHT FOR EVERY REALM IT BUILDS RATHER THAN UNFINISHED. §3.2 declares
   `timeout` `[Exposed=(Window,Worker)]` — a MEMBER exposure NARROWER than its own interface's `*` — and the
   three statics below are placed unconditionally. WHAT IS NOT COVERED: a member whose own §3.3.7 exposure set
   is narrower than its interface's, on an interface that is not a [Global] one. WHAT THE NEXT DIFF BUILDS: a
   member-exposure table keyed by (interface, member) rather than by member name alone — core/idl_args.c's
   idl_member_exposed_in_realm says at its own banner that IDL_MEMBER_EXPOSURE holds only the members of
   [Global] interfaces, so a non-global interface's member looked up there is answered out of an unrelated
   construct's set and the question cannot be asked today. HOW ITS ABSENCE WOULD SHOW: a WORKLET realm
   answering `typeof AbortSignal.timeout === "function"`. No such realm exists — every realm_install_intrinsics
   in this tree is reached with "Window" or "DedicatedWorkerGlobalScope", whose global names are exactly the
   two `timeout` names — so this diff widens the interface object's reach without widening that member's. */
void abort_install_protos(JSContext *ctx)
{
    JSValue sig_p, ctrl_p, prev, ctrl, sigctor, global;

    DCHECK(g_sig_class != 0, "a realm asked for AbortSignal.prototype before the interfaces were declared");
    DCHECK(g_ready, "a realm built §3.1's and §3.2's interfaces before abort_init");
    /* THE STEP IDS BELOW ARE THE AGENT'S, so the runtime that minted them is the runtime whose realms may
       carry functions holding them. This moved here WITH the three statics and the controller's `abort`: it is
       an invariant about the objects this function now mints, and it stood in abort_install because that is
       where they used to be minted. */
    DCHECK(g_abort_rt == NULL || g_abort_rt == JS_GetRuntime(ctx),
           "§3.1's and §3.2's interfaces were built in a second runtime — their step ids belong to the first, "
           "and a runtime is an AGENT");
    prev = JS_GetClassProto(ctx, g_sig_class);
    DCHECK(JS_IsNull(prev), "abort_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* AbortSignal.prototype FIRST: the controller's prototype does not need it, but a signal minted by
       anything at all does, and `abort_signal_new` is reachable from §5.4 the moment this returns. */
    /* §3.2: AbortSignal INHERITS EventTarget, so `addEventListener` and the `onabort` handler attribute are
       reached through the chain rather than copied onto each signal. */
    sig_p = event_target_derived_proto(ctx);   /* §3.2: `AbortSignal : EventTarget` */
    /* Web IDL §3.7.3: the interface prototype object carries the interface's identifier as its @@toStringTag,
       which is what makes `Object.prototype.toString.call(controller.signal)` answer "[object AbortSignal]" —
       the brand check a page performs without `instanceof`, and the one wpt asserts about every interface it
       touches. These two were the last interface prototypes in the engine without it. */
    idl_interface_tag(ctx, sig_p, "AbortSignal");
    event_target_install_handlers(ctx, sig_p, EH_SIGNAL);
    {
        JSAtom a = JS_NewAtom(ctx, "aborted");
        JS_DefinePropertyGetSet(ctx, sig_p, a,
                                JS_NewCFunction(ctx, js_sig_get_aborted, "get aborted", 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    /* §3.2's `reason` AND `throwIfAborted()` ARE MACHINES, SO THEY GO THROUGH THE INSTALLERS — see sig_ask_step
       for why the fork needs them to be. `reason` is an ACCESSOR whose getter is one, which is exactly what
       core/idl_args.h's idl_install_accessor_step is for, and the mint behind it composes §3.7.6 Attributes'
       "get reason" instead of this site hand-spelling it — which is what the raw define `aborted` still uses is
       deciding for itself, along with §3.7.6's [[Enumerable]]/[[Configurable]] pair.
       THE CONSTRUCT IS NOT SPELLED IN THIS COMMENT ON PURPOSE. core/idl_args.h's raw-site residual derives its
       population by grepping for that function's NAME over this tree's `.c` files, so a comment arguing ABOUT it
       is counted as one of the sites it is arguing about — and this file has already been the one that made that
       census wrong once. */
    DCHECK(g_sig_reason_stepid >= 0 && g_sig_throw_stepid >= 0,
           "§3.2's two flag-testing members were installed on a realm's prototype before abort_build_agent "
           "declared them");
    idl_install_accessor_step(ctx, sig_p, "reason", g_sig_reason_stepid, -1);
    idl_install_method(ctx, sig_p, "throwIfAborted", g_sig_throw_stepid);

    /* DOM §3.2's Web IDL §3.7.1 "Interface object", WITH ITS THREE STATICS. They are members OF this object, so they are minted
       with it and not after: an `AbortSignal` whose `abort`, `timeout` and `any` are missing is one a page can
       feature-detect and not call. */
    sigctor = JS_NewCFunction2(ctx, js_abort_signal_ctor, "AbortSignal", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(sigctor), "the AbortSignal interface object allocation failed");
    JS_SetPropertyStr(ctx, sigctor, "abort",
                      JS_NewCFunction(ctx, js_sig_static_abort, "abort", 0));
    JS_SetPropertyStr(ctx, sigctor, "timeout",
                      JS_NewCFunction2(ctx, NULL, "timeout", 1, JS_CFUNC_step, g_timeout_stepid));
    /* §3.2's third static. Its ONE required argument is what `length` states, and the sequence conversion
       behind it is the page's code, which is why it is a machine and `abort` beside it is not. */
    JS_SetPropertyStr(ctx, sigctor, "any",
                      JS_NewCFunction2(ctx, NULL, "any", 1, JS_CFUNC_step, g_any_stepid));
    /* THE HANDOVER IS LAST: JS_SetClassProto TAKES the reference, so `sig_p` is this function's until the
       realm owns it, and Web IDL §3.7.1 "Interface object"'s pairing above reads a LOCAL rather than a class
       slot it has already given away. */
    JS_SetConstructor(ctx, sigctor, sig_p);
    JS_SetClassProto(ctx, g_sig_class, sig_p);

    ctrl_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(ctrl_p), "the AbortController.prototype allocation failed");
    idl_interface_tag(ctx, ctrl_p, "AbortController");
    {
        JSAtom a = JS_NewAtom(ctx, "signal");
        JS_DefinePropertyGetSet(ctx, ctrl_p, a,
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_ctrl_get_signal, "get signal", 0,
                                                     JS_CFUNC_getter_magic, 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    JS_SetPropertyStr(ctx, ctrl_p, "abort",
                      JS_NewCFunction2(ctx, NULL, "abort", 0, JS_CFUNC_step, g_abort_stepid));

    /* DOM §3.1's Web IDL §3.7.1 "Interface object". */
    ctrl = JS_NewCFunction2(ctx, js_abort_controller_ctor, "AbortController", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctrl), "the AbortController constructor allocation failed");
    JS_SetConstructor(ctx, ctrl, ctrl_p);   /* .prototype and .constructor, both directions, off the LOCAL */
    JS_SetClassProto(ctx, g_ctrl_class, ctrl_p);

    /* WEB IDL §3.8's TWO PROPERTY REFERENCES, IN THE ORDER THE DELETED PER-DOCUMENT ENTRY PLACED THEM. */
    global = JS_GetGlobalObject(ctx);
    DCHECK(JS_IsObject(global), "a realm's global object is not an object");
    idl_define_global_property_reference(ctx, global, "AbortController", ctrl);
    idl_define_global_property_reference(ctx, global, "AbortSignal", sigctor);
    JS_FreeValue(ctx, global);
}

JSValue abort_signal_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_sig_class);
    DCHECK(!JS_IsNull(proto), "AbortSignal.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

