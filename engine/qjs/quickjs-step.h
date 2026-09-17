/* THE STEP-MACHINE DECLARATION SURFACE — public, so a HOST component can declare one.
 *
 * A continuation-holding builtin is a step machine in this engine: it declares itself at its definition
 * (JS_CFUNC_STEP_DEF) and is driven from the one convergence point every call shape reaches, so its callback's
 * body suspends and resumes at any depth. Everything needed to WRITE one lived in quickjs.c, which meant only
 * quickjs.c could write one — and a browser component under engine/host/browser was therefore forced to read
 * its Web IDL arguments with JS_GetPropertyStr from C. That is page code running with no flow base, which is
 * exactly what the twelve internal-method entries abort on: core/fetch/fetch.c hit it on
 * `fetch(new Proxy({url:"/q"}, {get(){}}))` the first time it ran, and every component after it would have.
 *
 * These three types are that surface, moved here unchanged. The types their operations name that remain
 * internal (StringBuffer, ValueSlot, JSMapRecord, REExecContext) are forward tags: a machine passes them
 * through, it never opens them. */
#ifndef QUICKJS_STEP_H
#define QUICKJS_STEP_H

#include "quickjs.h"

/* Internal to the engine, opaque to a machine that only forwards them to a visit operation.
   THESE TAGS ARE DECLARED HERE, AT FILE SCOPE, AND THAT PLACEMENT IS THE POINT. C scopes a struct tag to
   wherever it FIRST appears, so a tag whose first mention is inside a prototype is a fresh type belonging to
   that prototype — and every visitor assigning js_step_visit_*_slots would then be an incompatible-pointer
   error. gcc -w hid all three; the project's clang build does not. */
typedef struct StringBuffer StringBuffer;
struct ValueSlot;
struct JSMapRecord;
struct REExecContext;
struct JSStepHdr;

/* WHAT A FLOW-PRIVATE TREE ANSWERS, declared by the HOST because the engine owns no DOM. A tree is not a
   JSValue and not an allocation with a size, so no other operation below can name one; what the engine
   contributes is only the decision of WHICH consumer is visiting, which is the one thing a machine must never
   learn for itself.
   TWO OPERATIONS AND NOT THREE. The consumer that clones needs a third — the old-node-to-new map it re-points
   its cursors through — and needs it only for the length of one `clone` call, because a map records a copy
   that has just been made and nothing about it parks. So it lives inside that call and never crosses this
   boundary, which is also why the cursors are an argument of `clone` rather than a slot kind of their own. */
typedef struct JSStepTreeOps {
    /* Deep-copy the tree at `root` into a tree with the same owner, and re-point each of the `ncursors` cursor
       slots — every one of which names a node OF that tree — at the copy of the node it named. Fatal on
       allocation failure rather than answering NULL: a half-copied private tree has no arm it belongs to. */
    void *(*clone)(JSContext *ctx, void *root, void **cursors[], int ncursors);
    /* Destroy the tree at `root` and everything under it. */
    void  (*destroy)(JSContext *ctx, void *root);
} JSStepTreeOps;

typedef struct JSStepVisit JSStepVisit;
struct JSStepVisit {
    void (*val)(JSContext *ctx, JSValue *slot);
    /* An ACCUMULATOR the machine is building into. Not a JSValue and not shareable: two forked arms each append
       their own remaining elements, so the clone needs its own storage holding what has been accumulated so
       far. Declared as its own operation rather than left to the machine because a machine must never learn
       WHICH consumer is visiting it — that is what keeps the one declaration honest. */
    void (*strbuf)(JSContext *ctx, StringBuffer *slot);
    /* An OWN-KEY SNAPSHOT: the JSPropertyEnum array a walk took of its source, `n` entries, each holding an
       atom reference. Its own operation for the same reason the accumulator is — it is one allocation the
       byte-copy would leave two states pointing at, and its elements are atoms rather than values. */
    void (*props)(JSContext *ctx, JSPropertyEnum **slot, uint32_t n);
    /* A SLOT ARRAY a machine is sorting or gathering into: `cap` entries of storage, of which [from,to) hold
       live references. Sort's comparator is the page's code, so a fork mid-sort is ordinary, and the two arms
       merge independently from there — one array cannot serve both. */
    void (*slots)(JSContext *ctx, struct ValueSlot **slot, int64_t cap, int64_t from, int64_t to);
    /* A PLAIN BUFFER: storage holding no references, copied whole. It does not try to distinguish
       write-before-read scratch from a buffer read back across a suspension — that distinction was here and it
       is a footgun, because using the non-copying form on the second kind loses the data silently and nothing
       catches it. One operation that always copies is correct for both and cannot be got wrong. */
    void (*buf)(JSContext *ctx, void **slot, size_t bytes);
    /* An ATOM reference. Not a JSValue, and a machine that parked one mid-read owns it exactly as it owns a
       value — GetSubstitution holds the key between `$<` and `>` across the read it names. */
    void (*atom)(JSContext *ctx, JSAtom *slot);
    /* A DELEGATED MACHINE: one step machine performing an abstract operation that is itself a machine, held
       until the driver adopts it. It clones through the same tramp_step_state_clone as any other, so
       delegation composes rather than being a hole — a fork inside `str.replace`'s built-in @@replace is a
       fork inside the machine it handed its whole walk to. */
    void (*machine)(JSContext *ctx, void **slot);
    /* An ARRAY OF SUB-OBJECTS: `n` live elements of `size` bytes in a `cap`-element allocation, each visited by
       `each`. ONE operation rather than a buffer plus a loop in the machine, because the two consumers need
       OPPOSITE ORDER — the clone must copy the array before taking references into it, the teardown must
       release those references before freeing it — and getting that backwards is a use-after-free the machine
       should never be in a position to write. */
    void (*array)(JSContext *ctx, void **slot, size_t size, int n, int cap,
                  void (*each)(JSContext *ctx, void *elem, JSStepVisit *v));
    /* A REFCOUNTED READ-ONLY structure two flows may SHARE. Sharing rather than copying is not a shortcut here,
       it is the correct answer: nothing writes it after it is built, which is also what keeps pointers already
       taken INTO it valid in both arms — a copy would leave the sibling's interior pointers naming the
       original's tree. The clone takes a reference, the teardown drops one and destroys at zero. */
    void (*shared)(JSContext *ctx, void **slot, int *refs, void (*destroy)(JSContext *ctx, void *p));
    /* A COLLECTION RECORD a machine LOCKS across a callback. Map.forEach holds the record it is standing on so
       the page's callback cannot delete the cursor out from under it, and a clone standing on the same record
       needs its own lock or the first arm to finish frees what the second is still reading. */
    void (*maprec)(JSContext *ctx, struct JSMapRecord **slot);
    /* A LIVE REGEXP MATCH. The one owned thing whose two consumers are not the same walk over the same fields:
       the teardown's job IS lre_exec_end, and the fork's is to give the sibling its own backtracking stack and
       its own `reach` filter AND to re-point the two SELF-REFERENCES a byte copy leaves aimed at the original —
       the inline stack buffer, and the machine's capture array. That is why this is an operation and not a
       list: a machine must never learn which consumer is visiting it, and here the two do different work.
       `capture` is the MACHINE's block, already visited by the time this runs, so the context is re-pointed at
       whichever copy it now belongs to. Every other pointer in the context aims into the subject string or the
       compiled bytecode, and both arms hold their own reference to those, so both stay valid unchanged. */
    void (*reexec)(JSContext *ctx, struct REExecContext *ec, uint8_t **capture);
    /* A FLOW-PRIVATE DOM TREE a machine OWNS, and the CURSORS standing in it. Its own operation for the reason
       `reexec` is one: the three consumers do different work over it and none of the others can name it. A
       byte copy of the state leaves every one of those pointers aimed at the ORIGINAL arm's nodes, so a
       sibling that took one would place the same nodes into two documents and both teardowns would destroy
       them — which is exactly the corruption a fork abort exists to prevent.
       THE CURSORS TRAVEL WITH THE TREE and are not slots of their own, because a cursor is only meaningful
       against the copy the clone has just made: the clone re-points each one through its map, the teardown
       clears it (the tree owned the node, so a cursor is BORROWED and is never freed here), and the
       fingerprint folds it. A cursor slot holding NULL is a cursor the algorithm has not taken yet and is
       passed through untouched by all three.
       THE ROOT IS THE OWNED THING AND THE CURSORS ARE NOT, which is why they are two arguments and not one
       array: getting that backwards is a double free of every node in the tree. */
    void (*tree)(JSContext *ctx, void **root, void **cursors[], int ncursors, const JSStepTreeOps *ops);
};

/* A MACHINE'S DECLARATION. A host component writes one of these and hands it to JS_RegisterStepDef, which
   returns an id PAST STEPDEF_COUNT — the runtime tail beside the engine's own static table (see the
   host_step_defs note further down, and JS_NewStepClosure for the closure form a component mints its callable
   with). Everything the declaration names is in this header for that reason.

   THIS PARAGRAPH USED TO SAY THE DOOR WAS SHUT — "THE HOST-FACING DOOR IS NOT OPEN YET", that a component
   "cannot declare a step machine", and that "until then a component handles what it can read without touching
   the page's objects and DFAILs on the rest at its own name". It listed three things opening it would take,
   and all three are how the tree already stands. That is the removal-announcement shape CLAUDE.md's
   §A-SCAR-IS-NOT-A-WOUND names as the worst kind of stale claim: every other kind sends the reader to look,
   and a claim that a capability is ABSENT tells them not to. It is left here as a correction rather than
   deleted, because the reader who re-derives "only quickjs.c can write one" from the STEPDEF_* enum is the
   reader who will re-introduce the C-side JS_GetPropertyStr this door exists to end. The rule that made it
   necessary is unchanged and is why a component must reach for this: a Web IDL attribute read performed with
   JS_GetPropertyStr from C is page code running with no flow base under it, which is what the internal-method
   entries abort on. */
typedef struct JSTrampStepDef {
    size_t   size;
    int     (*step)(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc);
    /* WHAT THIS MACHINE'S COMPLETION IS, and NOTHING ELSE — the teardown asks it that one question.
     *
     * It states the completion (take the result out of the state and hand it back, or hand a throw to the
     * context) and performs whatever the algorithm owes on the way out: 7.4.11's deferred IteratorClose over
     * the iterators the machine is still standing on, 25.5.2's frame unwind, a re-entrancy guard lowered.
     * It does NOT release what `visit` names and it does NOT free the state block. Both of those are the SAME
     * operation for every machine, so both live in tramp_step_state_free_1, which runs them AFTER this returns.
     * A fini that frees a declared value is therefore a double free, and the free-and-null spelling of the same
     * mistake is a leak of whatever the next field misses — which is exactly why the list is declared once.
     *
     * NULL when the machine's completion is undefined on every path (Map.prototype.forEach, a Promise resolving
     * function, 27.3.3.3's dispose run). That is not an optional hook with a fallback behind it: there is one
     * teardown, and a machine with no completion to state has nothing to say to it. */
    JSValue (*fini)(JSContext *ctx, void *st, bool take_result);
    int      arg;
    /* COERCE-THEN-COMPUTE machines only (js_primargs_step); zero for every other definition. Such a builtin's
       ONLY user code is ToPrimitive on some of its arguments, and everything after — JS_ToIndex, JS_ToNumber,
       JS_ToBigInt, JS_ToPropertyKey on a PRIMITIVE — runs none. So the builtin DECLARES which arguments it
       coerces and this one machine performs those coercions on the tramp, then calls the body with the
       primitives in place. The body is not a legacy twin: with primitive arguments it has no user code left to
       reach, which is exactly what the declaration asserts, and it is the only implementation there is. */
    JSCFunctionType body;
    uint8_t  body_proto;   /* JS_CFUNC_generic, _generic_magic or _constructor_or_func */
    int      body_magic;
    /* A VALIDATION the spec performs BEFORE the coercions — DataView's setters reject an immutable buffer
       before reading either argument, and test262 pins that ordering. It is part of the declaration because
       "everything before the coercions" is exactly what the machine has to reproduce; a builtin whose leading
       check this cannot express does not carry the declaration at all. -1 = threw.
       It is handed THE INVOCATION, not a receiver and a magic: a leading check is as likely to be about an
       ARGUMENT (FinalizationRegistry rejects a non-callable, WeakRef an unholdable target, both before the
       `prototype` read) as about the receiver, and the header is what carries all of it. */
    int      (*precheck)(JSContext *ctx, const struct JSStepHdr *h);
    /* A validation that runs between the FIRST coerced argument and the next one. DataView's setters must throw
       ToIndex's RangeError for a fractional byteOffset before the value's valueOf runs, and test262 pins that
       too. Like precheck it is part of the declaration: it names the computation the spec interleaves, and the
       body re-runs it on the way through. -1 = threw. */
    int      (*midcheck)(JSContext *ctx, JSValueConst *argp, int magic);
    /* IfAbruptCloseIterator and its kin: what the spec does when an ARGUMENT COERCION throws. The coercion
       aborts in the driver, which tears the machine down through fini, so this runs from there — and only when
       a coercion was actually in flight, because a body that threw has already run its own cleanup. */
    void     (*onerror)(JSContext *ctx, JSValueConst this_val, int magic);
    /* This machine's algorithm CATCHES an abrupt request result instead of propagating it — 27.5.1.3 step 2.g
       rejects the promise when `Get(resolution, "then")` throws, so the throw is a VALUE to it, the way
       CONT_IMPORT's is to the interpreter. Declaring it is what lets the teardown hand the exception back to
       step() (as JS_EXCEPTION, with the throw still live in the context) rather than free the chain. Zero for
       every other definition, which is the default a positional initializer leaves. */
    uint8_t  catches_abrupt;
    /* SetterThatIgnoresPrototypeProperties' `home` operand, as a class_proto id: the object the setter refuses to
       write to because the accessor LIVES there. `arg` carries the AO's `p` for the same reason — both are
       operands of the algorithm, so both are declared rather than hard-coded, which is what lets one machine
       serve %Iterator.prototype%'s two accessors and Error.prototype.stack. 0 (JS_CLASS_OBJECT) for every other
       definition; those machines never read it. */
    int      home_class;
    /* WHAT THIS MACHINE OWNS — declared ONCE and visited by everything that needs to know. This is Blink's
       Trace, and for Trace's reason: two consumers need the same list and a list written twice drifts. They are
       the TEARDOWN (release each) and the DEEP-FORK CLONE (take a second reference to each, because a concolic
       branch inside a callback forks the flow at that depth and `[1,2].forEach(e => cfg.admin ? … : …)` must
       not leave two arms sharing one cursor and one result array). Written as two lists they have already
       drifted twice in this file — a consume state co-owned `next` across two flows, and a for-await drive left
       three of its five fields borrowed — which is what ITERCONS_OWNED and AFS_OWNED were each written to stop
       locally and what this stops everywhere.
       THE TEARDOWN DISCHARGES IT ITSELF — a machine never asks for that and never can. It used to: 137 engine
       finis ended in a `tramp_step_visit_free(ctx, s)` and 46 host teardowns restated the whole list by hand,
       which is 183 copies of one question, each of them a place for the answer to go missing (querySelectorAll's
       collected matches were named here and freed by nothing, and every abandoned selector walk leaked its
       element wrappers). The same move fixed a leak nothing could see: the engine's finis each freed the state
       block and the host's never did, because a step state is plain memory that no gc walk and no refcount
       report can name. One allocation site, one free site, one discharge.
       Only the fields the machine OWNS: a BORROWED view of its own cb array or of the header's captures is not
       visited, or the clone over-counts and the teardown over-frees. Ownership that is conditional is an `if`
       around the visit, which is why this is a function and not a table of offsets — the TypedArray-filter
       writeback owns three cb slots and only while that call is in flight.
       A machine that does not declare one cannot be forked and says so at the fork rather than silently handing
       two flows one state; declaring it is part of declaring the machine. */
    void     (*visit)(JSContext *ctx, void *st, JSStepVisit *v);
    /* WHICH ALGORITHM THIS MACHINE IS, AND WHICH OF ITS STEPS EACH STAGE NAMES.
     *
     * `stage` was documented as "the machine numbers the rest however it likes", and that is the whole of the
     * problem: a stage is where a machine RESTS — where it parks, where a sibling flow overtakes it, where a
     * cold-tier resume picks it up — and a private integer says nothing about where in the algorithm that is.
     * Two spec steps bundled into one stage are two steps between which this engine cannot suspend, which is
     * exactly the skipped logic the design forbids, and nothing could see it because the machine's own numbers
     * were internally consistent either way.
     *
     * AND THE PAGE IS NOT WHAT DECIDES WHICH STEPS MAY SHARE ONE. A stage boundary is a rest point because the
     * ENGINE may need to park there — RAM pressure paging the low-value tail to the IDB cold tier, a
     * cross-session resume, a flow that outranks this one — and not one of those events consults the page. So
     * the page can neither create a rest point nor remove one, and "no page code can run between these steps"
     * justifies NOTHING: a stage that bundles a span because the page happens to be quiet across it is a
     * stretch of algorithm this engine cannot park inside, which is a cap wearing a justification. It is the
     * same claim JS_STEP_YIELD already refuses one screen down — running no user code is not what makes a C
     * body safe to leave un-parkable, being O(1) is.
     * So a stage names ONE spec step. A label may name a RANGE only when the whole range is ONE O(1) ENGINE
     * action — a computation nothing the page controls can grow — and then the label says the range in those
     * terms. A span over anything of the PAGE'S SIZE (a list, a tree, a string, a collection, a parse) is not
     * a range at all: it is a stage per step, and the stage that walks returns JS_STEP_YIELD at every turn, so
     * the scheduler is ASKED at each one and answers from the frontier rather than from the algorithm.
     * js_step_def_check holds a declaration to the half a declaration can be held to: a label may not argue
     * from the page's code or the user's code at all. The label's job is to NAME the step, and the moment it
     * argues that a span is one stage the page has become the criterion — which is the reasoning this forbids,
     * and which every bundle found so far had written into its own label before it was acted on.
     *
     * `steps[stage]` is the spec step that stage rests at, as the standard writes it ("22.2.7.2 step 2"). That
     * makes a resume point EXACT rather than approximate: the driver asserts that a machine only ever rests at
     * a step of its own algorithm, a parked machine can SAY where it is parked, and a stage number means the
     * same thing in the next session as in this one — which is what a cross-session resume of a suspended
     * machine requires and what a private counter can never give it.
     *
     * NULL-terminated, indexed by stage, so a stage past the end is a machine that invented one. BOTH ARE
     * MANDATORY: this said "a definition that has not been converted yet declares neither, and the count of
     * those is the conversion's work queue" and pointed at a build gate that reports it. The gate does not
     * exist — it named one that had already been deleted — and the queue is empty, so what the sentence
     * actually described was an exemption with nothing left to exempt and nothing watching it. Every definition
     * now declares both, asserted where a def enters the runtime (JS_RegisterStepDef for a host component's,
     * js_step_defs_check_table for the engine's own) rather than counted anywhere; the conditional that used to
     * skip step_stage_check for an undeclared def is gone with it, because a guard whose false arm checks
     * nothing is how a machine that LOSES its declaration goes on resting wherever it likes.
     *
     * THE LABEL IS THE STAGE'S IDENTITY; THE INDEX IS ONLY THIS BUILD'S NAME FOR IT — and that is why the two
     * are ONE declaration (JS_STEP_STAGES below) rather than an enum beside an array. Written apart they are
     * two statements of one fact: RegExpBuiltinExec had eight constants and eight strings, and inserting a step
     * into one and not the other renames every stage after it, so `steps[stage]` names the wrong step and the
     * numbers still agree with themselves. The old defence was to number a stage OUT of sequence rather than
     * renumber the tail, which is a workaround for a drift the single declaration makes impossible.
     * ACROSS BUILDS the index cannot be the identity at all: a flow parked at stage 5 and resumed by a build
     * whose stages moved resumes at a different step of its algorithm, silently. So a parked machine's rest
     * point is its LABEL, and a resume RESOLVES that label back to an index in the build doing the resuming —
     * which is also why two stages may not declare the same label — asserted over the WHOLE list where a
     * definition enters the runtime (js_step_labels_check), and not at a rest, because a duplicate is ambiguous
     * whether or not a flow ever parks at it and asking only at a rest leaves it silent until one does.
     * A build-version stamp is the wrong answer to the same question: it rejects every parked snapshot whenever
     * ANY machine changes, and §scheduler's frontier never drops a work item. */
    const char *algorithm;
    const char *const *steps;
    /* WHY THIS MACHINE MUST NOT BE FORKED IN THE STATE IT IS IN NOW — the reason, or NULL when it may be.
     *
     * THIS CAPABILITY EXISTED AND WAS LOST, and getting it back is what this field is. `visit` used to carry
     * it as a side effect of being optional: a machine that declared none could not be cloned, and the fork
     * said so instead of handing two flows one state. Then the teardown started reading `visit` as the
     * ownership list, which made it MANDATORY — and a machine that owns JSValues AND must not be forked in
     * some states had no way left to say the second half.
     *
     * THE ONE MACHINE THAT NEEDS IT HOLDS A LEXBOR HTML PARSER. "C state cannot be forked" is not the reason
     * and is not true — cow_capture_host_record exists precisely so a component's C record time-travels. What
     * has no halves is specifically a tokenizer standing at a position, with an open-element stack and an
     * insertion mode behind it, and lexbor exposes no copy of one.
     *
     * IT IS ALSO NOT A LICENCE, AND ITS ONLY CORRECT TRAJECTORY IS TO ZERO. There were two declarers; the
     * selector walk was the other, and its reason turned out to be false — the CSS parser it held had finished
     * compiling before the machine's first rest point, and the matching context it held cleans itself at the
     * end of every match, so neither was state and neither belonged to the machine. Both moved to
     * core/dom/selector_match.c and the declaration went with them. A reason phrased as "no page code can run
     * here, so no fork can arrive" is the tell that the same mistake is being made again: parkability is a
     * property of the ENGINE, and RAM pressure, cold-tier eviction and a cross-session resume never ask what
     * the page is doing.
     *
     * IT IS A PREDICATE AND NOT A FLAG because the answer is about the STATE, not the definition: the same
     * machine is forkable before it builds its parser and unforkable while it holds one. And it is asked at
     * the FORK, which is the only consumer the answer is about. It must NOT be asserted inside `visit` — see
     * JSStepVisit's accumulator note: a machine must never learn which consumer is visiting it, and a `visit`
     * that DCHECKs "I was forked" fires on the teardown and on the fingerprint too, reporting an ordinary
     * abandoned walk as a fork. That is not hypothetical; it is what both of these did.
     *
     * The reason is the machine's own words and it is what the fork's abort prints, so the message names the
     * object rather than the mechanism. Zero for a machine that may always be forked. */
    const char *(*unforkable)(const void *state);
} JSTrampStepDef;

/* ONE DECLARATION OF A MACHINE'S STAGES — the enum and the labels, from one list, so neither can move without
   the other. A machine writes:
       #define REX_STAGES(X) \
           X(REX_REQUIRE,  "22.2.6.2 step 2 (RequireInternalSlot)") \
           X(REX_TOSTRING, "22.2.6.2 step 3 (S is ToString(string))")
       enum { REX_STAGES(JS_STEP_STAGE_ENUM) };
       static const char *const js_regexp_exec_steps[] = { REX_STAGES(JS_STEP_STAGE_LABEL) NULL };
   and the step gate refuses a `.steps` array that is anything else — a hand-written list of strings beside a
   hand-written enum is exactly the pair that drifts. See JSTrampStepDef.steps for why the label, not the index,
   is what a parked machine is holding. */
#define JS_STEP_STAGE_ENUM(name, label)  name,
#define JS_STEP_STAGE_LABEL(name, label) label,

/* THE THIRD EXPANSION OF THE SAME ONE LIST — THE DISPATCH. A stage list that GREW while a control-flow test
 * did not is the defect this removes, and it removes it at COMPILE TIME rather than by asking a body to be
 * careful: the `case` arms are generated from the declaration, so a stage with no body does not link and a body
 * naming no stage does not compile.
 *
 * TWO INSTANCES, FOUND BY READING, NEITHER CATCHABLE BY ANY RUNTIME GATE.
 *   core/frame/history.c declared ONE stage. IDL_STEP_STAGE_BASE numbers a member's first constant at
 * IDL_STEP_FIRST, which is the stage a declared member's body is ENTERED at — so `HPR_UPDATE` was not the
 * second stage the guard `if (hdr->stage == HPR_UPDATE) goto update;` took it for, it was the first, and the
 * guard fired on the FIRST entry. §7.2.5's serialization, its URL parse and BOTH of its SecurityError refusals
 * were jumped over into a §7.4.4 work record nobody had begun. It compiled, and it would have aborted three
 * files away in a DCHECK reading "no entry — _begin builds it and must run first", with nothing pointing here.
 *   core/frame/session_history.c spelled its resume guard as a NEGATION — `if (a->stage != SH_APPLY_POPSTATE)`,
 * "anything but the last stage". Correct while §7.4.6.1 had two rest points; it gained a third and the negation
 * swallowed it, re-running ACTIVATE HISTORY ENTRY on every resume through the new stage. The symptom would have
 * been WORSE than the bug: the afterPotentialUnloads DCHECK firing on the resume, reporting "a SECOND history
 * step was applied across this one" and sending its reader to build §7.4.1.3's traversal queue for a race that
 * never happened. A correct assert pointing confidently at the wrong subsystem.
 *
 * A RESUME GUARD SPELLED AS A NEGATION IS WRONG FOR EVERY STAGE ADDED AFTER IT, and a positive one that names
 * only some of the stages is wrong for every stage added after it too — the arm it does not name falls into a
 * neighbour, and the neighbour is a REAL body running at the wrong step, which is why neither instance looked
 * like a bug at the site. So the body does not write the test at all:
 *
 *     STEP_DISPATCH(HPR_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);
 *
 *     STEP_ARM(HPR_CHECKS);
 *         ... steps 1-9 ...
 *         STEP_GOTO(hdr->stage, HPR_UPDATE, NULL);
 *         return JS_STEP_YIELD;
 *
 *     STEP_ARM(HPR_UPDATE);
 *         ... step 10 ...
 *
 * WHAT IT MAKES IMPOSSIBLE, and each of these is a compiler diagnostic and not a runtime one:
 *   - a stage added to the list with no arm — the generated `goto step_arm_<name>` names a label that does not
 *     exist, which is a hard error in every C compiler regardless of the build's warning flags (this project
 *     compiles with -Wno-unused, so an unused-label warning would have been silently discarded);
 *   - an arm whose name is not a stage of this list — STEP_ARM evaluates the enumerator, so a typo is an
 *     undeclared identifier;
 *   - two stages sharing one constant — duplicate case value.
 * AND WHAT IT MAKES UNWRITABLE IS THE PART THAT MATTERS: there is no `else`, so a stage's body cannot live in
 * the un-named span between the function's first line and the first test. That span is where history.c's steps
 * 1-9 were sitting, reachable only by falling past a guard, and it is the only place a stage's body can hide.
 * Code that legitimately runs on EVERY entry stays above the dispatch, which is a statement about it rather
 * than an accident of where the guards happened to land.
 *
 * A ONE-ENTRY STAGE LIST IS NOT REFUSED, and refusing it would be the wrong repair for that first instance. A
 * machine whose algorithm IS one spec step has one rest point, and its suspensions are its REQUESTS, which
 * resume at that same stage through their own two-phase cursor — so a second stage forced on it would be a
 * label naming no step, which js_step_def_check exists to refuse. What was wrong in history.c was not the
 * count: it was that step 10 was declared and steps 1-9 were not, while the guard read the one declared
 * constant as though it were the SECOND of two. `stage == <the first constant>` is TRUE ON ENTRY and that is
 * correct — the first stage IS the entry stage — so the question a machine can never answer from its stage is
 * "have I started?", and a machine that needs one keeps its own byte (core/file/file_picker.c's `started`).
 * The dispatch is what makes the confusion unwritable, because the arm the guard was really selecting against
 * had nowhere left to live.
 *
 * IT IS NOT A SWITCH THE BODY WRITES. A hand-written `switch (hdr->stage)` over the same constants is the same
 * shape one edit away from the same bug: it is a second statement of the stage list, and the list is what
 * drifted in both instances. Generated from the declaration there is one list.
 *
 * `algorithm` IS THE MACHINE'S OWN NAME FOR ITSELF — `hdr->def->algorithm` wherever a header is in hand, so the
 * abort names the algorithm without a second copy of the string. A WORK RECORD carrying its own stage
 * (core/frame/session_history.c's SHApply, core/events/report_exception.c's) has no header and passes its
 * algorithm's name directly; the macro takes the stage as an LVALUE EXPRESSION for the same reason STEP_GOTO
 * does — the invariant is about the cursor, not about who owns it.
 *
 * `undeclared` IS WHAT A RELEASE BUILD RETURNS, and it is a parameter because omitting it is the bug wearing
 * its own uniform: DFAIL compiles out in release, so a dispatch that merely aborted in dev would, in release,
 * fall straight out of the switch into the FIRST ARM — a machine entered at a stage it does not declare running
 * the body of a step it is not at. The macro returns instead, so the two builds disagree about the diagnostic
 * and never about the control flow.
 *
 * A MACRO RATHER THAN A FUNCTION, for STEP_GOTO's reason: DCHECK/DFAIL stamp the file and line they are WRITTEN
 * at, so a helper here would report this header at every abort and say nothing about which machine was entered
 * where. DFAIL is therefore the CALLER's — engine/host/check.h in a host component, quickjs-check.h in the
 * engine — which is also why this header must not include either. */
#define JS_STEP_STAGE_CASE(name, label) case name: goto step_arm_##name;

/* ONE STAGE'S BODY BEGINS HERE. Written as a statement (`STEP_ARM(FPK_ACCEPTS);`) rather than as a label with a
   colon, because it is both: the label the dispatch jumps to, and an evaluation of the enumerator that makes a
   name outside the declaration a compile error. Two arms may share one body by naming both in sequence, which
   is `case A: case B:` and reads as it does. FALL-THROUGH BETWEEN ARMS IS STILL THE AUTHOR'S TO REFUSE — an arm
   ends in a return, and one that sets its stage and runs on has crossed a boundary the driver never saw, so the
   label claimed a rest point that does not exist. C offers nothing that forbids that; the declaration-generated
   dispatch is what stops the OTHER half, which is a stage having no body at all. */
#define STEP_ARM(name) step_arm_##name: (void)(name)

/* THE SAME TRANSFER THE DISPATCH MAKES, MADE BY THE ALGORITHM ITSELF. An entry-stage dispatch picks one of
   several continuations (21.4.2.1 step 2's numberOfArgs picks three), and the stage list is in ALGORITHM order,
   so the arms it does not pick lie BETWEEN it and the one it does. Falling through them is wrong and a
   hand-written label beside the arm's own is a second name for one point — which is the drift STEP_DISPATCH
   exists to remove, one level down. This names the arm, so the mangling is stated once and a name that is not
   an arm of this function is an undeclared label: a hard error in every C compiler, exactly as a stage with no
   arm is.
   IT IS NOT A REST POINT AND MUST NOT BE READ AS ONE — the stage is assigned through STEP_GOTO as always, and
   the algorithm continues in the SAME entry, which is legitimate only where what it crosses is one O(1) engine
   action. A transfer over anything of the page's size is a stage that must return instead. */
#define STEP_JUMP(name) goto step_arm_##name

#define STEP_DISPATCH(list, stage, algorithm, undeclared) do {                                          \
        switch ((int)(stage)) {                                                                         \
        list(JS_STEP_STAGE_CASE)                                                                        \
        }                                                                                               \
        {                                                                                               \
            char step_dispatch_why_[400];                                                               \
                                                                                                        \
            snprintf(step_dispatch_why_, sizeof step_dispatch_why_,                                     \
                     "%s was entered at stage %u, which its stage declaration does not name — a stage " \
                     "a machine can be entered at and has no arm for is a rest point with no body, and "\
                     "the arm it would otherwise have fallen into is another step of the algorithm "    \
                     "entirely. Either the stage belongs to a different machine's declaration, or this "\
                     "one was written to a stage number it does not own",                               \
                     (algorithm), (unsigned)(stage));                                                   \
            DFAIL(step_dispatch_why_);                                                                  \
        }                                                                                               \
        return (undeclared);                                                                            \
    } while (0)

/* THE TWO QUESTIONS A STEP MACHINE'S FORK CAN BE — see JSStepHdr.fork_kind for why they are two and what is
   lost by collapsing them into one. NONE is the reset value and names no question, so a fork that reaches the
   driver carrying it is an ask that never stated which it was. */
#define JS_FORK_KIND_NONE     0
#define JS_FORK_KIND_OUTCOME  1
#define JS_FORK_KIND_TOBOOL   2

typedef struct JSStepHdr {
    const JSTrampStepDef *def;
    /* THE RUNTIME'S CENSUS OF LIVE MACHINES — the same accounting gc_obj_list gives every GC object, given to
       the one allocation class that escapes it. A step state is plain js_malloc'd memory holding references, so
       a machine the driver drops is invisible to JS_FreeRuntime's object walk AND to its refcount report: the
       objects it held are reported with nothing naming their owner, and the state itself is reported by nothing
       at all. Two pointers rather than a list_head because this header is public and list.h is not; the list is
       quickjs.c's (js_step_census_*), and its invariants are asserted there — a machine is on it from
       tramp_step_state_new (and from the CLONE, whose memcpy copies the ORIGINAL's links and must relink)
       until tramp_step_state_free_1 takes it off, immediately before the definition's fini frees the block. */
    struct JSStepHdr *census_prev, *census_next;
    int orig_cfirst, orig_cargc;
    uint8_t orig_is_tail;
    /* A step machine whose CALLBACK is itself a step machine — `arr.map(String)` is the ordinary case. The inner
       one is driven by the same do_step_tramp, and its result is delivered to the outer step instead of being
       pushed. Without this the inner reached js_call_c_function's DFAIL: the CALL arm invoked a C callback in
       place, which is right for a callback with no body and wrong for one that IS a machine. Third place this
       outer-continuation shape has been needed (construct, ToPrimitive, and here), which is what makes it the
       primitive rather than three special cases. */
    void *outer;
    uint8_t outer_kind;
    /* A CONSTRUCT request's NEW TARGET (owned), UNINITIALIZED when it is the constructor itself — which is what
       every builtin's Construct performs and what the request meant before Reflect.construct needed to say
       otherwise. It lives here rather than in an interpreter register because a machine cannot reach one, and
       the request can suspend between the step that sets it and the dispatch that reads it. */
    JSValue ctor_ntgt;
    /* A CAPABILITY request's result: NewPromiseCapability's [promise, resolve, reject], OWNED. It lives here for
       the reason ctor_ntgt does — a machine cannot reach an interpreter register, and the request SUSPENDS
       between the step that asks and the delivery that answers, because the subclass constructor is user code.
       Three values rather than one because that is what the record is; packing them into the step's single
       result would make every consumer unpack a tuple the engine had just built. */
    JSValue cap_promise, cap_funcs[2];
    /* A DEFINE request's descriptor SHAPE, for a machine that needs one other than CreateDataProperty's. The
       request expressed only that one shape, hardcoded at three sites, and Object.defineProperty needs an
       arbitrary descriptor — so the shape became operands. They ride the HEADER rather than the cb array for the
       same reason get_atom does: a request's operands that have no stack slot live here, and every existing
       DEFINE keeps working by leaving them alone. desc_flags == 0 means CreateDataProperty's shape.
       READ AND RESET by the driver like every other request input, so a shape cannot leak into the next one. */
    JSValueConst desc_get, desc_set;   /* BORROWED: the machine holds them across the request */
    int desc_flags;
    /* A GETOWNPROP request's ANSWER SHAPE, for a machine that wants the descriptor RECORD instead of the
       descriptor OBJECT — the invariants' JSDescFacts, reached from a step machine rather than from a
       continuation. It rides the header for the same reason the shape above does: a request's operand with no
       stack slot lives here, and the driver READS AND RESETS it like every other request input, so an answer
       shape cannot leak into the next request. NULL = the object, which is what every other consumer takes. */
    struct JSDescFacts *desc_facts;
    /* A DELEGATE request's inner machine, owned until the driver adopts it. Delegation is how a step builtin
       performs an ABSTRACT OPERATION that is itself a machine — .finally's PromiseResolve(C, v), which 27.5.5.3
       step 4 states as the operation and not as C.resolve, so it cannot be reached as an ordinary call.
       FORWARDING the inner's step from the outer's (what str.replace's @@replace delegation does) works only
       while the inner's requests carry everything in cb_result: a CAPABILITY or CONSTRUCT records the state the
       DRIVER is stepping, which under forwarding is the OUTER, so the answer lands on the wrong header. Handing
       the inner to the driver instead makes it a real machine on the chain, and the CONT_STEP outer it already
       carries is what delivers its result back. */
    void *delegate;
    /* CONT_FOROF_NEXT carries no STATE — its enum_rec offset is the whole of what the continuation needs — so a
       machine reached as a for-of .next() (Iterator.concat's) has an outer_kind with a NULL outer, and this is
       what its delivery reads. Without it the DONE arm saw no outer at all and pushed the result as a bare
       operand, which the loop then read as its enum_rec. */
    int outer_forof;
    /* THE INVOCATION. Captured (owned) by the driver before the first step, because a prologue that suspends
       resumes into a step whose C locals are gone and whose operand stack the machine must not depend on. It is
       also what let every machine drop its private copy of the receiver and the argument vector: those were the
       same three lines of duplication in twelve inits, and each one was a separate chance to forget a dup. */
    uint16_t stage;      /* 0 = the prologue has not run; the machine numbers the rest however it likes */
    int argc;
    int arg;             /* def->arg — the mode that used to be an init parameter */
    JSValue this_val;    /* the receiver; new_target for a constructor step */
    JSValue func_obj;    /* the callee — a closure step machine reads its func_data record through this */
    JSValue *argv;       /* argc owned values, in the tail of the state's own allocation */
    /* A prologue's in-flight sub-sequence (today: LengthOfArrayLike). `coerce` is the value it is holding across
       a suspension — the machine's C locals are gone when it resumes — and cb_coerce is the request buffer, a
       borrowed [value] for a TOPRIMITIVE, [obj] for a GETPROP, or [ctor, arg] for a CONSTRUCT. `len_phase` is the
       sub-sequence's own cursor, kept here so the machine spends ONE of its stages on the whole read instead of
       one per suspension point. Every coercing prologue needs exactly this, so it lives in the header rather
       than being re-declared (and mis-declared) in each state. */
    JSValue coerce;
    /* THE OPERAND WHOSE COERCION IS OVER UNKNOWN EXTERNAL INPUT — see JS_STEP_UNKNOWN. OWNED, and it lives for
       exactly as long as the return that names it: the machine parks the operand here, returns
       JS_STEP_UNKNOWN, and the driver takes it at the one place a machine's completion is placed, with no
       opcode, no request and no suspension in between. So it is NOT in any machine's `visit` and must not be —
       nothing can fork or park while it is set, which tramp_step_state_clone and tramp_step_state_free_1 both
       ASSERT rather than trust. JS_UNINITIALIZED = this machine is not standing on one. */
    JSValue unknown_operand;
    /* THE UNKNOWN-LENGTH CHAIN's cursor and its operation string — see step_length_unknown. A length read that
       produced unknown external input is answered by a chain of per-position outcome forks, and the machine
       SUSPENDS at every one of them (the sibling's snapshot is taken at the ask), so neither the position nor
       the string the fork is keyed by may live in a C local. The string is the header's own storage rather than
       a shared scratch buffer because two machines can be in flight at once — an outer and its delegate — and
       one formatting over the other's would key two different collections' positions to one predicate. */
    int len_probe;
    char len_op[32];
    JSValue cb_coerce[3];   /* three, because the BARE [[Set]] request carries [obj, receiver, value] — the
                               receiver is a separate operand in the spec and a builtin's Set(O,P,V,true) is the
                               one form that can leave it implicit. */
    uint8_t len_phase, spc_phase, num_phase, str_phase, get_phase, cs_phase, exec_phase, prog_phase;
    /* the [[OwnPropertyKeys]] sub-sequence's own cursor. Its OWN byte rather than get_phase's, because the one
       algorithm that needs it — Web IDL's record<> conversion — takes the key list and then reads each key,
       so the two sub-sequences are in flight in the same stage and sharing a phase would answer one at the
       other's call site. */
    uint8_t keys_phase;
    /* THE OWN-KEY-SET PREDICATE, held across the fork that asks it — see step_ownkeys_run. An enumeration of a
       record standing for unknown external input that carries no example forks over whether the record holds an
       own member at all, and step_tobool_run's operand must live somewhere the SIBLING'S SNAPSHOT CARRIES: the
       ask suspends, and the C frame that minted the value is gone when the answer lands. It is on the header
       rather than in a machine's own state because the ask is the WRAPPER's, shared by every consumer of the
       request, and a per-machine field would be the same declaration repeated once per consumer with one more
       chance to forget the dup. OWNED, so tramp_step_state_clone dups it and the teardown releases it;
       JS_UNINITIALIZED = this enumeration carries no question (an ordinary object, or a record whose example
       holds the answer). It IS carried across a fork, unlike `unknown_operand`, and that is the whole point. */
    JSValue keys_pred;
    /* THE UNKNOWN-MEMBER CHAIN's cursor and its operation string — see step_ownkeys_run. Once the predicate
       above answers TRUE the record holds a member, and how MANY it holds is a second unknown: every count is a
       world the program can be in, so the enumeration asks a chain of per-position questions ("beyond the n
       already named, is there another?") exactly as step_length_unknown asks one per index. The machine
       SUSPENDS at every one of them, so neither the cursor nor the string the question is keyed by may live in
       a C local. Its OWN buffer and not len_op's: that chain and this one cannot be in flight on one header
       today, and sharing the storage would make a future overlap key two different questions to one string
       rather than fail. KEYS_PROBE_UNASKED = no chain is running, which is also the state the wrapper restores
       before it issues the request, so the cursor never spans an outstanding delivery. */
    int keys_probe;
    char keys_op[40];
/* NO CHAIN IS RUNNING. It is -1 and not 0 because 0 is the first POSITION the chain stands at, and a cursor
   whose "not started" reads as its own first value is one that re-names the member it has already named every
   time the wrapper is re-entered. Beside the field it is the sentinel for, so the two cannot drift. */
#define KEYS_PROBE_UNASKED (-1)
    /* the [[GetOwnProperty]] sub-sequence's own cursor, for the same reason keys_phase has one: Web IDL's
       record<> conversion asks for a key's DESCRIPTOR and then READS that key, so the two are in flight in the
       same stage and a shared phase would answer one at the other's call site. */
    uint8_t desc_phase;
    /* an ARGUMENT COERCION is outstanding, so an abandon here is the spec's abrupt-completion case (take/drop's
       IfAbruptCloseIterator). It lives on the header because the teardown is what has to act on it, and the
       teardown releases this_val — the receiver the handler needs — before the machine's own fini can see it. */
    uint8_t coercing;
    /* An OPCODE-driven machine whose operation yields NOTHING — an object spread mutates its target in place, the
       way a property write does. The DONE path otherwise always leaves exactly one value on the operand stack, and
       an opcode's COMPILED stack has no slot for it (unlike a call site, whose callee slot is the result slot). */
    uint8_t discard_result;
    /* AN OUTCOME FORK — see step_fork_run. `fork_over`, `fork_op`, `fork_n` and `fork_real` are the request's
       operands, and the first two are BORROWED exactly as desc_get/desc_set are: the machine holds the operand
       (it is one of its own arguments or a value on its state) and the driver READS AND RESETS them before it
       does anything else, so a request cannot leak into the next one and a SNAPSHOT taken at the fork carries
       neither.
       `fork_real` IS RESET TO JS_OUTCOME_REAL_UNSTATED AND NOT TO ZERO, which is the whole reason the sentinel
       has a name: zero is a legal completion, so a stale zero left behind by a finished request would read as
       the positive claim "a real session takes outcome 0" at the next ask that forgot to state one.
       `fork_arm` is the answer and `fork_phase` says WHICH HALF OF THE ASK/ANSWER PAIR this entry is — 0 is
       "ask", and it is 0 in the sibling's snapshot ON PURPOSE: the sibling re-enters at the same point, ASKS
       AGAIN, and its own decision vector replays the arm it was forked for. That is what makes a parked and
       resumed sibling take the same arm in the next session as in this one; an arm baked into the clone would
       be a second, weaker answer to a question the vector already answers.
       IT DOES NOT SAY WHETHER AN ASK IS OUTSTANDING, AND THIS SENTENCE SAID IT DID — a claim its own next
       clause contradicts, since the sibling carries 0 and IS outstanding. A machine re-entered at a stage that
       both issues a REQUEST and forks needs that second fact to know whether `cb_result` is its request's
       answer or the driver's filler, and reading `fork_phase != FORK_PH_ANSWERED` for it is a two-way test
       over three routes: it groups the SIBLING with the request. Every prototype-chain walk in the engine was
       written from this sentence and took the filler as its next chain link — one aborting loudly and three
       answering wrongly in silence. `step_fork_pending` is the fact; `fork_ask_key` below is what it reads. */
    JSValueConst fork_over;
    const char  *fork_op;
    int          fork_n;
    int          fork_real;
    int          fork_arm;
    uint8_t      fork_phase;
    /* WHICH QUESTION THIS FORK IS — the machine's ALGORITHM asking which of its own completions it reaches
       (JS_FORK_KIND_OUTCOME, step_fork_run), or §7.1.2 ToBoolean asking whether one VALUE is truthy
       (JS_FORK_KIND_TOBOOL, step_tobool_run). The two share every line of the fork's bookkeeping and the
       driver's whole snapshot, and they differ in exactly one thing: WHICH SEAM ANSWERS.
       THAT IS NOT A DETAIL OF PLUMBING, IT IS THE CONSTRAINT KEY. A ToBoolean is the SAME PREDICATE the
       interpreter's own `if` asks about — `if (p)`, `if (!p)`, `Boolean(p)` and a callback returning `p` into
       `filter` are ONE gate, keyed by the value's own branch identity — so routing it to the outcome seam
       would file a SECOND, independent entry over one predicate, and a flow that had already fixed `p` would
       fork again and then stand on two arms that contradict each other. It would also record NONE of what a
       branch records: no CONCRETIZE-ON-PIN, no excluded token, no `{int>5}` bound and no `{startsWith:/api}`
       call predicate, so a report would print an unconstrained parameter where the page had gated one.
       It is a ROUTING question and never a fallback selector: delete either seam and the other still has to be
       asked, because they are two questions and not two implementations of one.
       JS_FORK_KIND_NONE is what the driver RESETS it to, for `fork_real`'s reason exactly — a stale kind would
       read as a positive claim at the next ask that forgot to state one, and both asks state it always. */
    uint8_t      fork_kind;
    /* WHICH QUESTION THE OUTSTANDING ANSWER BELONGS TO — the identity of the ask, so that the answer cannot be
       consumed by a DIFFERENT call site. `fork_arm` alone says nothing about which fork it answers, and every
       machine declares n == 2, so an answer delivered to the wrong ask passes every check there was: it is a
       real arm, in range, recorded by the flow's decision vector under the ASKING operation's key and then read
       as the answer to another operation's. That is silent, and it is the exact failure a delegating algorithm
       produces when its phase space overlaps the one it delegates into.
       IT IS A CONTENT HASH AND NOT THE POINTER, for the same reason the driver resets `fork_op` before it
       snapshots: the op string commonly lives on the MACHINE'S OWN state (a per-position buffer that carries an
       index), so the clone's copy is at a different address and a flow rebuilt from the cold tier has no
       address at all. The bytes are the only thing that is the same question in every tier. Zero is "nothing
       asked", which a js_mallocz'd state already reads as. */
    uint32_t     fork_ask_key;
    /* WHICH STAGE ISSUED THE FORK THAT IS OUTSTANDING — req_stage's argument, made of the one request kind that
       did not have it, and made because the key above CANNOT ANSWER THIS QUESTION AND WAS BEING ASKED IT.
       An ask key names the QUESTION and a resume needs the SITE, which are two questions over one string —
       and they agree at every machine until one algorithm asks ONE predicate at TWO of its own call sites. Then
       the stricter question decides the string and the looser one loses in silence, which is exactly what an
       elimination chain does: its key is composed from the PREDICATE and the member's own name, deliberately
       carrying neither the algorithm nor the operand (so that two members reached over one unknown agree), so
       an algorithm running the chain TWICE over TWO operands spells the SAME BYTES at the same position in
       both. DOM §4.10 Interface CharacterData's `substringData(offset, count)` is that algorithm: with both
       operands unknown it draws Web IDL §3.2.4.6's chain over `offset` and then again over `count`, and at the
       position where the two chains stand on the same number the ask key hashes identically — so the re-entry
       that follows the `count` chain's park re-asks the ANSWERED `offset` chain, the key check agrees, and one
       question's world is filed as another's with every arm real and in range.
       THE STAGE IS WHAT TELLS THEM APART, for the reason req_stage gives one paragraph down: a machine that has
       not moved is at the site that parked, and a stage is the resume point the driver already checks and a
       parked flow already says out loud. It is WRITTEN ON EVERY ASK and cleared with `fork_ask_key`, so the two
       halves of one outstanding question are set and reset together and a stale stage cannot outlive the key
       that names its question.
       NAMED RESIDUAL — TWO ASKS AT ONE STAGE OVER TWO OPERANDS SPELLING ONE OPERATION STRING ARE STILL ONE
       QUESTION TO BOTH HALVES OF THIS CHECK, and that is narrower than the invariant rather than wrong: an
       algorithm whose two sub-sequences can be in flight across a re-entry is one whose resume point is a
       STAGE, which is what the check above requires of it, and no algorithm in this engine asks that shape at
       one stage today (§5.1's two asks off one phase byte spell two operation strings over ONE operand, so the
       key tells them apart; §4.10's two chains spell one string over two operands and are now two stages).
       WHAT THE NEXT DIFF BUILDS: an OPERAND-IDENTITY accessor on JSConcolicHooks — the table carries `is`,
       `example` and `lead` and NOTHING that names a value, which is why step_fork_key can only reach the
       operation string — folded into that hash beside the op, so an ask is keyed by the predicate's own
       identity (operator AND operand) exactly as the constraint the seam files under it already is.
       HOW ITS ABSENCE SHOWS: an algorithm that grows a second sub-sequence over a second unknown at ONE stage
       consumes the first's arm at the second's call site with every assert on the path satisfied — the arm is
       real, in range, and recorded under the other operand's key — and the symptom is a report in which two
       unknowns of one call carry one unknown's narrowing. */
    uint16_t     fork_stage;
    /* the key a GETPROP sub-sequence is holding across its suspension. It is OWNED here because the driver
       BORROWS the atom the request carries, and released by the shared teardown so an abandon mid-read cannot
       leak it. JS_ATOM_NULL is zero, so a js_mallocz'd state already reads as "no read in flight". */
    JSAtom get_atom;
    /* WHICH STAGE ISSUED THE KEYED REQUEST THAT IS IN FLIGHT — the half of the two-phase contract that was
       missing, and the half that makes a keyed resume EXACT rather than merely plausible.
       A keyed sub-sequence parks on its first call and answers on its second, and the answering branch TAKES
       the value — so the second call has to be the same call site. That was checked by the KEY, and a key names
       the PROPERTY rather than the site: two reads of the same key at two stages are indistinguishable by it
       (STEP_GOTO below says so in as many words: "only when the two call sites happen to name DIFFERENT keys;
       when they name the same one it says nothing"), the INDEX forms pass JS_ATOM_NULL and were checked by
       nothing at all, and the own-keys and own-descriptor requests carried no site check of any kind.
       The STAGE is what the algorithm already declares (JSTrampStepDef.steps) and it is exactly what tells two
       sites apart, because a machine that has not moved is at the site that parked. A request answered at a
       stage other than the one that asked is a resume continuing in a DIFFERENT STEP of the algorithm with
       another step's value in hand — the loss §Time-travel calls a cap, and the one the shutdown in Streams
       §4.9.1 "Working with readable streams" shipped: the pipe fulfilled with its destination still locked
       and nothing said so.
       ONE STAMP FOR THE THREE KEYED CURSORS rather than one each. They are in flight together only WITHIN one
       stage — Web IDL's record<> takes the key list and then reads each key — so an overlapping request issued
       at a DIFFERENT stage is refused where it is ISSUED, which names the machine that walked away from its
       own request instead of leaving a later end to read a stamp the newer request had overwritten. */
    uint16_t req_stage;
    /* WHAT THE DRIVER OWES THIS MACHINE WHILE IT IS PARKED — the value it will be re-entered with, and the
     * completion that was live in the context when it parked. Both JS_UNINITIALIZED exactly when the machine is
     * NOT sitting in a CONT_STEP_YIELD anchor, which is the invariant the park and the resume each assert.
     *
     * A STEP MACHINE'S REST POINT IS NOT ONLY ITS OWN YIELD. It rests every time it hands the driver a request
     * and the driver hands an answer back, and for a machine walking a structure of the PAGE'S SIZE by keyed
     * requests — Array.prototype.concat's HasProperty/Get/CreateDataProperty per element, sort's gather and its
     * write-back — that round trip IS the iteration. The answer to a plain data slot is computed by the driver
     * in place, so no page code runs, nothing yields, and the walk used to reach the machine's next step with
     * the scheduler never once consulted: an un-parkable span the length of the receiver, which is the cap
     * §Time-travel forbids however carefully the frames under it were flattened.
     *
     * THE ANSWER IS WHY THIS HAS TO BE ON THE HEAP. It lives in an interpreter local on its way from the driver
     * to the machine, and a local is exactly what a snapshot cannot reach — the same reason the machine itself
     * is put in an anchor frame rather than left in `cont_st`. So the park MOVES it here and the resume takes it
     * back, and the resume is then byte-identical: the machine re-enters at the call site that parked on its
     * request, holding the value that request produced, with nothing replayed and nothing recomputed.
     *
     * `park_exc` IS MOVED, NEVER DUPLICATED, and it is a separate field rather than a flag on `park_in` because
     * the two are different facts: `park_in` may be JS_EXCEPTION (the delivery is abrupt) while a machine's
     * FIRST entry may equally carry a live throw with an ordinary delivery — step_request_check says so in as
     * many words. rt->current_exception is per-RUNTIME, so a flow that parks with one standing would hand its
     * completion to whichever flow ran next; the exception rides the flow instead, exactly as the anchor's
     * close_saved_exc rides an unwinding close. */
    JSValue park_in;
    JSValue park_exc;
} JSStepHdr;


/* AN OWNERSHIP DECLARATION, AS A TYPE — JSTrampStepDef::visit's signature, and IdlStepDecl::visit's, and the
   signature of every helper record's own (a request cursor, a work block, a queue). Named because the three
   consumers below take one, and a type written out at each of them is a type that can drift at one of them. */
typedef void (*JSStepVisitFn)(JSContext *ctx, void *state, JSStepVisit *v);

/* THE FREEING CONSUMER OF A DECLARATION — release everything `visit` names, once.
 *
 * A state's `visit` is the ONE list of what it owns. Until this was exported, only quickjs.c could read it that
 * way, so every host step machine wrote a SECOND list in its teardown: the same JSValues, by hand, in another
 * function. That is precisely the pair this engine forbids — a field added to a state creates an obligation at
 * the clone and at the teardown, and nothing catches the one that is missed. It had already been missed, in
 * querySelectorAll: `visit` named the collected-matches array, the teardown named nothing, and every abandoned
 * selector walk leaked its element wrappers.
 *
 * A MACHINE'S OWN STATE NO LONGER COMES HERE FROM ITS TEARDOWN, and no machine may bring it: the driver
 * discharges JSTrampStepDef::visit itself once `fini` has stated the completion. What is left for this is the
 * EMBEDDED RECORD — a work block, a request cursor, a queue that a machine owns a field of and whose visit its
 * own visit forwards to; that record's release is driven by the machine, so it needs the consumer by name.
 *
 * The visitor is IDEMPOTENT — a value slot is left JS_UNDEFINED, a pointer slot NULL, an atom JS_ATOM_NULL — so
 * a state torn down through a nested declaration that has already been discharged frees nothing twice.
 * JS_StepFreeVisitor is the same consumer for a helper record whose visit takes its own typed pointer rather
 * than a `void *`; there is one implementation, spelled two ways because C cannot cast the function type
 * without lying about it. */
JS_EXTERN void JS_StepVisitFree(JSContext *ctx, JSStepVisitFn visit, void *state);
JS_EXTERN JSStepVisit *JS_StepFreeVisitor(void);

/* WHAT THE OTHER HALF OF A TEARDOWN MAY NOT TOUCH, AS A NUMBER.
 *
 * A host machine's teardown has two halves and the split between them is the whole point: the DECLARATION owns
 * every reference (discharged by JS_StepVisitFree), and the component's own `release` owns what the declaration
 * does not name — a lexbor handle, a foreign C allocation, a global flag the algorithm took and must give back
 * (§4.13.4 step 14's "regardless of whether the above steps threw", HTML §4.10.22.3 step 8's). `release` runs
 * FIRST and may READ an owned value, because those flags live on one.
 *
 * A `release` that FREES one is the second list growing back, and it is invisible either way it is written:
 * free-and-null leaves the visit's free a no-op, free-without-null makes the visit's free a double free. So the
 * teardown folds the declaration into this number before `release` and again after, and asserts they match.
 *
 * WHAT THE NUMBER IS MADE OF IS SLOT IDENTITY, NEVER A REFERENCE COUNT, and a caller has to know that to read
 * the assert it fires. The property is "the declaration still names what it named": free-and-null, replace and
 * hand-over all move it. Free-WITHOUT-null does not — it reaches the discharge as the second free and the
 * allocator answers there. A reference count would appear to close that gap and cannot: a count says how many
 * holders an object has and never WHICH, so a `release` giving back somebody else's reference to a declared
 * object moves it exactly as a `release` discharging the declaration does, and where the slot is the sole
 * holder the count can only be read out of a block the offending `release` has already returned. So this
 * bracket does NOT forbid a `release` from moving reference counts elsewhere in the agent's object graph; it
 * forbids it from touching the slots the declaration names.
 *
 * EVERY operation is folded, not only the reference-holding ones. What `release` may own is what the
 * declaration does NOT name, and this walk reaches only what it DOES — so a `buf` named by the visit is the
 * declaration's allocation exactly as a `val` is its reference: the fork copies it with js_malloc and the
 * discharge frees it with js_free. Exempting those is how nine such buffers came to be freed by seven
 * `release` hooks and grown with the C library's realloc, which a fork would then have handed to the wrong
 * allocator. Pointers are folded, never dereferenced. Cheap, allocation-free, and dev-only at its call sites. */
JS_EXTERN uint64_t JS_StepVisitOwnedFingerprint(JSContext *ctx, JSStepVisitFn visit, void *state);

/* Register a host component's step machine; the return value is the id JS_CFUNC_STEP_DEF names it by. The
   definition is BORROWED and must outlive the runtime — static data, as the engine's own are. A machine with no
   `visit` is refused: it could not be forked, and a concolic branch inside its callback would hand two flows
   one state. */
JS_EXTERN int JS_RegisterStepDef(JSRuntime *rt, const JSTrampStepDef *def);

/* A STEP MACHINE THAT CARRIES CAPTURED VALUES — the step analogue of JS_NewCFunctionData, and the shape a
   PROMISE REACTION has to be. A reaction knows which object it belongs to only by capture, and the work it does
   (calling the page's algorithm again, rejecting parked requests) is work only a machine may do; without this
   the two were mutually exclusive from outside quickjs.c. `stepid` is what JS_RegisterStepDef handed out; the
   machine reads what it captured with JS_StepClosureData, off the callee its header already carries. */
JS_EXTERN JSValue JS_NewStepClosure(JSContext *ctx, int stepid, int length, int data_len, JSValueConst *data);
/* THE SAME MINT CARRYING §3.7.6's NAME. A closure whose name a page can READ — an IDL accessor is the case that
   forced this — states it here; every internal reaction passes NULL through the entry above and is unchanged. */
JS_EXTERN JSValue JS_NewStepClosure2(JSContext *ctx, int stepid, const char *name, int length, int data_len,
                                     JSValueConst *data);
JS_EXTERN JSValueConst JS_StepClosureData(const JSStepHdr *h, int i);

/* WHAT step() RETURNS. A machine reports one of these at every stage, and a host machine reports them for the
   same reasons the engine's own do. */
#define JS_STEP_DONE     0   /* the machine is finished; fini yields its result */
#define JS_STEP_ABRUPT (-1)  /* it threw; the completion value is live in the context */
#define JS_STEP_REQUEST  6   /* it parked on a sub-sequence's request and will be re-entered with the answer */
#define JS_STEP_CALL     3   /* it parked on a CALL of the page's code (step_call_run); same re-entry contract */
/* IT PARKED ON A [[Construct]] OF THE PAGE'S CODE (step_construct_run); same re-entry contract as a CALL.
   The code existed and was spelled `4` at every site, which is a request nobody outside quickjs.c could name
   and therefore one no HOST machine could make — and the first host algorithm that needs one is not exotic:
   DOM §4.9's "create an element" step 5.1.4.1 CONSTRUCTS the page's custom element class, synchronously,
   inside document.createElement. A number with no name is also how a driver's dispatch and a machine's return
   drift apart, which is the same reason the stage labels are a declaration rather than an integer. */
#define JS_STEP_CONSTRUCT 4
/* "I HAVE MORE WORK; RE-ENTER ME." IT ASKS NOTHING, AND THAT IS THE POINT. This headline said `PREEMPT ME IF
   YOU WANT` and stated a channel the code does not have: the driver's arm reads nothing off the machine, sets
   the delivery to JS_UNDEFINED and jumps to do_step_step, and the offer there is made on EVERY re-entry of
   every machine with the kind fixed at that label — so a machine that returns this expresses no preference and
   one that returns a request expresses none either. It USED to ask, at the driver's own arm, and that made the
   only C span this driver could be preempted inside the one a machine had explicitly declared, which left a
   walk built out of keyed requests — most of them — with no rest point at all. The paragraph directly below
   has said the corrected thing throughout; where two declarations of one contract disagree, the CONSUMER is
   what settles it — and the driver's arm, plus the two sites that gloss a `return JS_STEP_YIELD` at all,
   already spell it "I have more work; re-enter me".
   WHAT THE CODE STILL DOES IS PRODUCE A RE-ENTRY, which is where the offer already is. The bytecode half of
   that is a loop back-edge; a machine that walks a structure of the PAGE'S SIZE — a DOM subtree, a token list,
   a document to serialise, a parse — needs the same, because otherwise it runs to completion inside one
   `def->step()` call however carefully the frames beneath it were flattened. Running no user code is NOT what
   makes a C body safe to leave un-parkable; being O(1) is, and almost nothing that walks a page is.
   Return it with no request pending; the machine is re-entered with JS_UNDEFINED, and when nobody is waiting it
   is re-entered immediately, which costs one predicted call per iteration. That is cheap enough to ask at every
   step of a walk, which is where it belongs. */
#define JS_STEP_YIELD   22
/* "MY COMPLETION IS ONE OF N FEASIBLE OUTCOMES — FORK HERE." Returned by step_fork_run; the driver decides the
   arm and snapshots the flow for the others. Never returned by a machine directly — see step_fork_run. */
#define JS_STEP_FORK    23
/* "MY COMPLETION IS A VALUE THIS ENGINE WAS NEVER GIVEN." Returned by the COERCION SUB-SEQUENCES — ToString
 * (step_tostring_run) and the numeric family (step_toint64_run, step_todouble_run, step_tolength_run,
 * step_toint32_run, step_toint32sat_run, step_tofloat64_run) — never by a machine directly, and it is the
 * answer to ECMAScript §7.1.19 ToString ( arg ) and §7.1.4 ToNumber ( arg ) over UNKNOWN EXTERNAL INPUT.
 *
 * §7.1.19 step 9 asserts the remaining case is an Object and step 10 is `ToPrimitive(arg, string)`; §7.1.4
 * steps 7-8 are the same pair with hint `number`, which is why one mechanism answers both. §7.1.1
 * ToPrimitive ( input [ , preferredType ] ) over a concolic returns it unchanged — a concolic stands for a
 * value that IS primitive in the page and wears an Object only because the solver needs a carrier with an
 * exotic [[Get]] — so step 12's recursive ToString is a ToString over an unknown String, whose result is an
 * unknown String. The conversion BOUNDARY cannot answer that (JS_ToStringInternal owes C a real JSString and
 * says so), so the answer is given where the spec performs the coercion, which for every builtin that can run
 * the page's `toString` is this one shared sub-sequence.
 *
 * WHAT THE DRIVER DOES WITH IT: the machine's whole completion becomes a concolic DERIVED from the operand,
 * named by the algorithm and the spec step it was coercing at. That is the generalisation of a rule this file
 * already wrote by hand at Array.prototype.join — "an unknown element makes the whole join unknown", the
 * concrete prefix subsumed rather than kept, because a partial string is a value the page never computes and
 * the source identity is what a later sink solves for. Every value-producing algorithm has that shape: a
 * result computed out of a string nobody has is a result nobody has, and it keeps `src` and `root` so a branch
 * over it still forks and an @S candidate still injects at the source that fed it.
 *
 * A MACHINE WITH A MORE PRECISE ANSWER STATES IT AT ITS OWN SITE, and several already do — String.prototype.
 * concat folds an unknown piece through the `+` hook so the known literals survive, the Error constructor
 * defines the derived unknown as `message` and CARRIES ON building the error object, parseInt/parseFloat and
 * RegExp name their own operation. Those are not a fallback this selects against: they are the algorithm's own
 * semantics, and they run first because they are IN the algorithm. This is what an algorithm with nothing
 * better to say completes as, and there is nothing left for it to fall back TO.
 *
 * THAT ANSWER IS GIVEN IN ONE OF TWO PLACES, AND WHICH ONE IS DECIDED BY THE VALUE, NOT BY TASTE. The four
 * above ask BEFORE the coercion, because the operand they are looking at is the one the page handed them. A
 * machine whose operand may become unknown ONLY PART-WAY THROUGH the coercion cannot ask there: `cmp()` may
 * return an ordinary object whose `valueOf` answers with unknown external input, and §7.1.1 ToPrimitive
 * ( input [ , preferredType ] ) is what discovers that — one hop after the pre-check would have run. Such a
 * machine answers WHEN THE SUB-SEQUENCE REPORTS, by consuming JS_STEP_UNKNOWN instead of returning it, which
 * is the ONE site both ways in converge on. It then owns the parked operand, and STEP_UNKNOWN_ANSWERED is how
 * it gives it back. A pre-check ALONE is not a narrower version of this — it is a hole with a crash in it. */
#define JS_STEP_UNKNOWN 24

/* THIS SITE ANSWERED THE COERCION ITSELF — release the operand JS_STEP_UNKNOWN parked, because the driver
 * never sees the code and so never takes it. Use it ONLY where the machine's own answer does not need the
 * unknown: §23.1.3.30.2 CompareArrayElements ( x, y, comparator )'s result is an ORDERING and not a value the
 * sort returns, so an unknown one carries nothing forward and the elements keep their own provenance. A
 * machine whose answer must PRESERVE the taint does not use this — it takes the operand the way
 * js_str_replace's forwarding site does, or lets the driver derive the completion.
 *
 * A MACRO RATHER THAN A FUNCTION so the DCHECK stamps the site that gave the answer. Its remedy — go and look
 * at what handed this step code out, because a coercion sub-sequence did not — names an action with no object
 * the moment every caller reports one shared line. (No quotation marks anywhere in this block: the audit reads
 * a quoted run under a citation as a claim about the SECTION, and this tree's own prose in quotes lands in the
 * same bucket a fabricated sentence does, with nothing mechanical to separate them.) */
#define STEP_UNKNOWN_ANSWERED(ctx_, h_) do {                                                            \
        JSStepHdr *step_unk_h_ = (h_);                                                                  \
        DCHECK(!JS_IsUninitialized(step_unk_h_->unknown_operand),                                        \
               "this site answered a coercion's JS_STEP_UNKNOWN with a value of its own while the "     \
               "machine is standing on no unknown operand — the operand is parked by the coercion "     \
               "sub-sequence and by nothing else, so the step code being answered here came from "      \
               "somewhere that never asked the question");                                              \
        JS_FreeValue(ctx_, step_unk_h_->unknown_operand);                                                \
        step_unk_h_->unknown_operand = JS_UNINITIALIZED;                                                 \
    } while (0)

/* The machine's own arguments, borrowed. Out-of-range reads undefined, which is what the IDL's optional
   arguments mean at this level. */
static inline JSValueConst step_arg(const JSStepHdr *h, int i)
{
    return i < h->argc ? h->argv[i] : JS_UNDEFINED;
}

/* [[Get]] AS A REQUEST — the operation a browser component needs and the reason this header exists. Reading a
   Web IDL attribute off an object the page supplied is one accessor or Proxy trap away from being the page's
   code, so it cannot be a JS_GetPropertyStr from C; this parks the machine on the read and answers at the SAME
   call site when it is re-entered. Returns JS_STEP_REQUEST (the caller returns it), 0 once *pout is set, or -1.
   `atom` is BORROWED.

   -1 IS ALSO HOW AN ABRUPT COMPLETION ARRIVES — for this request and for every KEYED one below it (the write,
   the define, the delete, the membership test, the own-keys and own-descriptor reads). The request ENDS at the
   call site that parked on it, exactly as a normal completion ends it — the cursor rewinds, the key is
   released — and returns -1 with the completion live in the context. A machine that propagates writes what it
   already writes (`if (r < 0) return JS_STEP_ABRUPT;`); a machine whose definition declares `catches_abrupt`
   branches there instead and says what its algorithm does with the throw. The CALL and CONSTRUCT requests
   still report theirs as a JS_EXCEPTION in `*pout` — the encoding JS_Call itself uses — so the two halves of
   the request layer do not yet answer the same way; moving them onto -1 is a change at ninety-nine call sites
   and belongs in its own diff, not smuggled into this one. Neither encoding can go unnoticed: a machine that
   ignores an abrupt delivery and asks for anything with that throw still live aborts at the driver's one
   convergence point, naming its algorithm, its stage and the step code it asked for. */
JS_EXTERN int step_getprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValue in,
                               JSValue *pout, JSValue **out_cb, int *out_argc);

/* A KEYED WRITE AS A REQUEST, in 7.3.4 Set ( O, P, V, Throw )'s Throw-FALSE form: `O.[[Set]](key, value, O)`
   with the boolean it answers DISCARDED. The read above is one half of what a browser component needs of a
   property; this is the other, and it runs the page's code for the same reasons — the target may be an
   accessor whose setter loops, or a Proxy whose `set` trap does.
   THE THROWING FORM IS DELIBERATELY NOT HERE. Web IDL § 3.7.6 Attributes' create-an-attribute-setter step
   4.5.8.4 — the [PutForwards] forwarding, which is the operation a host had no way to perform at all — is
   stated with Throw false, and the difference is reachable from script wherever the forwarded-through
   attribute is not [LegacyUnforgeable]: the throwing form invents a TypeError where the spec's silently does
   nothing. A host needing Set(O,P,V,true) exports that one when it has a member that means it, rather than
   taking this one and being wrong in the direction nothing tests.
   `atom` is BORROWED. Returns 14 (the caller returns it), 0 once the write is done, or -1 — the same abrupt
   encoding as every other keyed request above. */
JS_EXTERN int step_setprop_bare_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom,
                                    JSValueConst value, JSValue in, JSValue **out_cb, int *out_argc);

/* A NUMERIC COERCION AS A REQUEST — the other half of what a browser component needs, and the half that was
   missing. Web IDL's integer types (`[EnforceRange] unsigned long long milliseconds`, `long`, `unsigned long`)
   are ToNumber on whatever the page passed, so `AbortSignal.timeout({valueOf(){ for(;;){} }})` is the page's
   code and cannot be a JS_ToInt64Sat from C. With only the property read exported, a component's only honest
   options were to abort on an object argument or to run the coercion off the chain; this makes it a request
   like any other. Returns JS_STEP_REQUEST (the caller returns it), 0 once *pres is set, or -1. */
JS_EXTERN int step_toint64_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, int64_t *pres,
                               JSValue **out_cb, int *out_argc);

/* THE SAME REQUEST STOPPING AT THE NUMBER. Web IDL's integer conversions are NOT ToIntegerOrInfinity: `long` is
   ToNumber then a modulo 2^32 and `[Clamp] long long` is ToNumber then a round-half-to-EVEN, and a saturating
   int64 has thrown away what each of those needs before it is one. The coercion is the part that runs the page's
   code; the arithmetic after it is the caller's type. */
JS_EXTERN int step_todouble_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, double *pres,
                                JSValue **out_cb, int *out_argc);

/* A CALL AS A REQUEST — see the definition in quickjs.c. A browser component that must RUN the page's code and
   then continue (dispatchEvent walking a listener list, §2.9's synchronous dispatch) cannot JS_Call from C:
   that is the drive-to-completion the engine aborts on. `phase` and `cb` are the MACHINE's own — a host machine
   holds a call across several of its stages, and the buffer must be in its `visit` for a fork to copy it.
   `cb` is 2 + argc slots, [this, func, args...]; this dups in and releases out, so the machine owns nothing it
   has to remember. Returns 3 (the caller returns it), or 0 once *pout is the call's result. */
JS_EXTERN int step_call_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst func,
                            JSValueConst this_val, int argc, JSValueConst *argv, JSValue in, JSValue *pout,
                            JSValue **out_cb, int *out_argc);

/* A [[Construct]] AS A REQUEST — step_call_run's sibling, and the operation a browser component needs to run a
   page's CLASS. `document.createElement('x-app')` performs DOM §4.9 step 5.1.4.1's "constructing C with no
   arguments" SYNCHRONOUSLY, and that constructor is the page's code with loops, awaits and DOM mutations in
   it; JS_CallConstructor from C is the drive-to-completion the engine aborts on, and the class's own `super()`
   would reach a C entry with no flow base under it.
   Same contract as the call request in every respect but the buffer SHAPE, which is what the driver's
   JS_STEP_CONSTRUCT arm reads: `cb` is 1 + argc slots, [ctor, args...] — there is no receiver, because a
   construct makes one. NEW TARGET is the constructor itself, which is what every algorithm that states
   "Construct(C)" means; a machine needing a distinct one sets `h->ctor_ntgt` (Reflect.construct's operand).
     0 = *pout is the constructed object, JS_STEP_CONSTRUCT = the caller must return that step code, and a
   throw ABANDONS the machine unless its definition declares that it catches an abrupt request result. */
JS_EXTERN int step_construct_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst ctor,
                                 int argc, JSValueConst *argv, JSValue in, JSValue *pout,
                                 JSValue **out_cb, int *out_argc);

/* PASS THE BUFFER THROUGH THIS, and pass it the ARRAY. The capacity is then the array's own size rather than a
   number written beside it that a later argument can outgrow — which is exactly how a two-argument call into a
   three-slot buffer scribbled over the field next door. A machine that FORWARDS someone else's buffer takes
   `(JSValue *cb, int cb_cap)` and forwards both; handing this macro that pointer yields a capacity of zero, so
   the mistake is a DCHECK on the first call rather than a corrupted neighbour. */
#define STEP_CB_SLOTS(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))
#define STEP_CB(arr) (arr), STEP_CB_SLOTS(arr)

/* THE SAME DERIVATION, FOR THE THREE LOOPS EVERY OWNER OF A REQUEST BUFFER WRITES — the `init` that undefines
   its slots, the `visit` that walks them for a fork, and the `fini` that releases them. Each of those was a
   literal count beside an array declared with a literal size, so a buffer that grew had FOUR places to grow in
   and the ones that were missed did not fail where they were: a visit one slot short leaves a live value the
   fork never dups, and a fini one slot short leaks it. Derive the count from the array and there is one. */
#define STEP_CB_FOREACH(arr, i) for ((i) = 0; (i) < STEP_CB_SLOTS(arr); (i)++)

/* LEAVING A STAGE WITH A REQUEST STILL IN FLIGHT IS THE BUG THIS EXISTS FOR — every stage transition in a
 * machine that owns a request buffer goes through here.
 *
 * A request is TWO-PHASE: the stage that issues it is re-entered with its cursor at 1 and MUST reach the SAME
 * step_call_run / step_construct_run / step_getprop_run to collect the answer, because the cursor is what tells
 * that call site "you are the resume, take the value" rather than "ask". A stage that re-evaluates its decision
 * on the way back in can decide differently — the shutdown in Streams §4.9.1 "Working with readable streams"
 * did exactly that, since ISSUING the close is what made WritableStreamCloseQueuedOrInFlight true, so the
 * resume took the "already closing" branch
 * and walked away from its own call. The cursor stayed at 1, the NEXT stage's request read it as a resume and
 * took the abandoned call's result as its own, and `writer.releaseLock()` was never called at all: the pipe
 * fulfilled with the destination still locked. Nothing said so, because the value a stage received was a REAL
 * value — the other request's. quickjs.c's step_getprop_done DFAILs on the same event one step LATER, and only
 * when the two call sites happen to name DIFFERENT keys; when they name the same one it says nothing.
 *
 * The rule is structural, which is why it is a primitive and not a defence one component wrote for itself: a
 * stage's DECISION is made once, with every cursor at rest, and a stage that holds a request does nothing but
 * hold it. Aborting AT the transition names the stage that walked away, rather than the innocent stage three
 * transitions later whose request silently answered without asking.
 *
 * THE CURSOR LIST IS NULL-TERMINATED AND NAMES EVERY SUB-SEQUENCE THIS MACHINE CAN HAVE IN FLIGHT — its own
 * call/construct phase bytes, a sub-record's (`&s->w.phase`), and, for a machine whose requests are the keyed
 * ones, the header's own `&h->get_phase`. Listing them is the machine's statement of what it owns, in the same
 * spirit as `visit`: a machine that holds no request buffer at all lists none and assigns its stage directly,
 * because there is no cursor to leave behind and a fabricated one would assert nothing. The list is where the
 * cursor goes the day such a machine grows one.
 *
 * `dst` IS THE STAGE LVALUE, NOT A HEADER, because the invariant is about the cursor and not about who owns the
 * stage: a WORK RECORD a machine suspends inside (AbortSignalWork's §3.2 walk, report_exception's) carries a
 * stage and a phase of its own and needs exactly this over them.
 *
 * A MACRO RATHER THAN AN INLINE FUNCTION, for the reason the whole assert exists: DCHECK stamps the file and
 * line it is WRITTEN at, so a function here would report this header at every abort and say nothing about which
 * transition walked away. Expanding at the call site is what makes the message the answer. DCHECK itself is
 * therefore the caller's — engine/host/check.h in a host component, quickjs-check.h in the engine — which is
 * also why this header must not include either of them. */
/* IT ASKS ABOUT A TRANSITION, AND A WRITE THAT CHANGES NOTHING IS NOT ONE. The invariant is that the NEXT
 * stage's request site must not collect THIS one's answer; when the stage does not move there is no next site,
 * the same site resumes and collects its own. Restricting the question that way is not a softening — it is the
 * invariant stated exactly — and the case it admits is the ordinary shape of a machine that loops over a pair:
 * readable_stream.c's tee derives its index FROM the stage (`i = s->hdr.stage == TS_B1`) and re-enters the loop
 * body on every resume, so its first statement rewrote the stage it already held while `tee_ctrl_run`'s cursor
 * was legitimately mid-flight. That fired on the smoke fixture, and the machine was right: nothing had moved.
 * Written as a comparison against `to` rather than as a "resuming?" flag, because a flag would be the machine
 * telling the assert when not to run, which is the recognizer shape this file bans everywhere else. */
/* AND THE SCAN IS BOUNDED BY THE ARRAY'S OWN SIZE, NEVER BY THE TERMINATOR IT IS LOOKING FOR — because the
 * terminator is a thing a CALL SITE supplies, and a scan that trusts it to be there reads past the end of the
 * array the moment one site forgets. That is not a diagnostic that degrades: reading `rest_[n]` off an
 * n-element automatic array is undefined behaviour, and an optimising compiler is entitled to conclude the loop
 * never exits and to emit the infinite jump that says so. It did — one `[PutForwards]` transition, written with
 * its cursor and no terminator, compiled at -O1 into a two-byte `jmp .` INSIDE the step body, so every
 * assignment through that binding (`el.style = …`, `rule.media = …`, `window.location = …`) burned the whole
 * CPU budget in ONE C activation with the interpreter never getting control back — the drive-to-completion this
 * file's every other rule exists to prevent, arrived through a missing comma-NULL rather than through a loop
 * anybody wrote. THE SIZE IS KNOWN AT THE EXPANSION, so the bound is the size; the terminator stays the
 * convention every site already writes, and its ABSENCE is now a named abort at the site that forgot it instead
 * of a program the compiler is free to reshape. Asked before the transition test, because a malformed list is
 * malformed whether or not the stage moves. */
#define STEP_GOTO(dst, to, ...) do {                                                                    \
        const uint8_t *const step_goto_rest_[] = { __VA_ARGS__ };                                       \
        const size_t step_goto_n_ = sizeof step_goto_rest_ / sizeof step_goto_rest_[0];                 \
        size_t step_goto_i_;                                                                            \
        DCHECK(step_goto_rest_[step_goto_n_ - 1] == NULL,                                               \
               "a STEP_GOTO's cursor list does not end in its NULL terminator — the list is what the "  \
               "transition asserts over, and one that does not say where it stops is a scan off the "   \
               "end of it. Append NULL to the cursors this transition names");                          \
        if ((dst) != (to))                                                                              \
            for (step_goto_i_ = 0; step_goto_i_ + 1 < step_goto_n_; step_goto_i_++)                     \
                DCHECK(*step_goto_rest_[step_goto_i_] == 0,                                             \
                       "a stage was left with a sub-sequence's request still in flight — the next "     \
                       "stage's request will collect this one's answer");                               \
        (dst) = (to);                                                                                   \
    } while (0)

/* A WELL-KNOWN SYMBOL'S ATOM. A host machine can read any NAMED property (step_getprop_run takes an atom, and
   JS_NewAtom makes one from a string), but a symbol-keyed one it could not name at all — and @@iterator is the
   discriminator Web IDL uses for every `sequence or record` union. Headers' fill is the first to need it: the
   spec picks the sequence arm when the init is ITERABLE, and without this the closest available test was
   JS_IsArray, which is narrower — `new Headers(new Map(...))` is iterable, is not an array, and would have
   taken the record arm and produced an EMPTY header list. These are permanent atoms, so the answer needs
   neither a dup nor a free.
   @@unscopables is here for the same reason one step further out: it is a property key a host DEFINES rather
   than reads, and Web IDL §3.7.3 Interface prototype object makes every interface that declares a
   §3.3.14 [Unscopable] member define one on its prototype ("Perform ! DefinePropertyOrThrow(interfaceProtoObj,
   %Symbol.unscopables%, desc)"). The engine already implements the READ side — 9.1.1.2.1 HasBinding's
   `Get(bindingObject, @@unscopables)` is what makes a `with` statement skip the name — so without this the
   host could build the object and had no way to name the key it goes under. */
typedef enum {
    JS_WKS_ITERATOR = 0,
    JS_WKS_ASYNC_ITERATOR,
    JS_WKS_TO_STRING_TAG,
    JS_WKS_UNSCOPABLES,
} JSWellKnownSymbol;
JS_EXTERN JSAtom JS_WellKnownSymbolAtom(JSWellKnownSymbol which);

/* WEB IDL §3.2.12 USVString's `USVString`: a DOMString whose UNPAIRED SURROGATES have each been replaced by U+FFFD.
   That replacement is the whole of what makes the type different from a DOMString, and it cannot be done from
   outside the engine — a host sees only the UTF-8 the C-string conversion produces, where an unpaired
   surrogate has already been written out in whatever form that conversion chose. Every URL member takes
   USVStrings, and wpt asserts the replacement directly. `str` is OWNED; a string with no unpaired surrogate is
   returned unchanged, so the common case allocates nothing. */
JS_EXTERN JSValue JS_ToScalarValueString(JSContext *ctx, JSValue str);

/* A DIAGNOSTIC string for a value, built WITHOUT invoking the page's `toString`. JS_ToCString does invoke it,
   and at a C entry there is no flow to run it on — which since the coercion methods became step machines is
   the state the engine's own backstop names ("route this site to the ToPrimitive trampoline"). That backstop
   is right, and this is the answer for the one shape of caller that cannot take its advice: a HOST reporting
   what went wrong must not depend on the code that went wrong. Every C embedder reaching it derives the same
   fallback, so the engine owns it once instead: an object is its `name`/`message`, falling back to its
   CONSTRUCTOR's name — test262's Test262Error carries only `message` and a custom `toString`, which is exactly
   the page code this must not run — and anything else is its class via JS_ToObjectString.
   The result is malloc'd when *powned is set and a JS C-string otherwise, which is the only reason the caller
   has to know which; JS_DiagFreeCString takes both and releases the right one. */
JS_EXTERN const char *JS_DiagCString(JSContext *ctx, JSValueConst v, char **powned);
JS_EXTERN void JS_DiagFreeCString(JSContext *ctx, const char *s, char *owned);

/* WEB IDL'S `BufferSource` AS ONE READ. The type is `(ArrayBuffer or ArrayBufferView)` and ArrayBufferView is
   `(Int8Array or ... or DataView)` — so a host component reading a BodyInit has to accept a DataView, which
   JS_GetTypedArrayBuffer refuses because it is not a typed array even though it carries the same
   JSTypedArray. The alternative from outside the engine is reading `buffer`/`byteOffset`/`byteLength` as
   PROPERTIES, which are accessors — page-visible, patchable, and a C-driven getter call the flow design has no
   driver for. Returns the underlying ArrayBuffer (owned) with the view's window, or an exception.
   The bytes come from JS_GetArrayBuffer on the result.
   THE WINDOW IT ANSWERS WITH IS THE SPEC'S, NOT THE [[ByteLength]] SLOT'S. A length-tracking view — one built
   over a resizable ArrayBuffer with no explicit length — has [[ByteLength]] = auto, so ECMAScript
   § 10.4.5.12 TypedArrayByteLength and § 25.3.1.3 GetViewByteLength derive its length from the buffer at every
   read. This returns that derived length (JS_GetTypedArrayBuffer does too); it once returned the slot, which
   for such a view records the buffer's size at CONSTRUCTION and is stale the moment the buffer is resized. */
JS_EXTERN JSValue JS_GetArrayBufferView(JSContext *ctx, JSValueConst obj, size_t *pbyte_offset,
                                        size_t *pbyte_length);

/* WEB IDL § 3.2.26 Buffer source types' TWO REFUSALS, each asked of the buffer under a buffer source — of V
   itself for an ArrayBuffer, of V.[[ViewedArrayBuffer]] for a DataView or a typed array, which is § 3.2.26's
   own "underlying buffer of a buffer source type instance". One call answers every arm of that conversion, so
   a binding layer performs no walk of its own and cannot get the two arms' wording apart.
   JS_IsFixedLengthBufferSource is ECMAScript § 25.1.3.9 IsFixedLengthArrayBuffer over that buffer, and it is
   what § 3.2.26's last conversion step tests: a type not carrying § 3.3.1's [AllowResizable] extended
   attribute must throw a TypeError when this is false. JS_IsSharedBufferSource is IsSharedArrayBuffer over the
   same buffer, for the step just before it, which refuses a shared buffer to a type not carrying [AllowShared].
   NEITHER TAKES A CONTEXT AND NEITHER THROWS, which is what lets a caller ask them in § 3.2.26's own order:
   that algorithm has no detach and no bounds test, so a detached or out-of-bounds view must still be told
   whether its buffer is resizable rather than met with an exception. Both REQUIRE a buffer source — the brand
   test comes first, and anything else is fatal at the call. */
JS_EXTERN bool JS_IsFixedLengthBufferSource(JSValueConst obj);
JS_EXTERN bool JS_IsSharedBufferSource(JSValueConst obj);
/* AND THE THIRD QUESTION § 3.2.26 ASKS OF THAT BUFFER, which is not a refusal but a value: step 5 of "get a
   copy of the bytes held by the buffer source" is "If IsDetachedBuffer(jsArrayBuffer) is true, then return the
   empty byte sequence". A caller must be able to ask it BEFORE it reads the view's window, because the only
   route from a view to its buffer (JS_GetArrayBufferView, above) refuses an out-of-bounds view and a detached
   buffer makes every view over it out of bounds — so without this the algorithm met an exception at the step
   before the one that defines its answer. Same contract as the pair above: no context, no throw, and the
   buffer-source brand test comes first. */
JS_EXTERN bool JS_IsDetachedBufferSource(JSValueConst obj);

/* %IteratorPrototype%. Web IDL §3.7.9.2 Iterator prototype object states that the ITERATOR PROTOTYPE OBJECT of
   an `iterable<>` interface has it as its [[Prototype]] ("The [[Prototype]] internal slot of an iterator
   prototype object must be %Iterator.prototype%" — the spec spells the intrinsic the ES2025 way; the engine's
   own name for it is this function's). NOT §3.7.10, which is "Asynchronous iterable declarations" and whose
   step 2 asserts a definition reaching it "does not have an indexed property getter or an iterable
   declaration" — that number belongs to the pair below, at §3.7.10.2, and had been written here instead.
   That inheritance is what gives `headers.keys()` the whole
   iterator-helper surface (`@@iterator` returning this, `take`, `drop`, `map`) without the component defining
   one member of it. A host component cannot reach the intrinsic from outside: it is neither a global nor
   reachable by name, and the only script-level route is
   `Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()))` — which is exactly the walk the WPT
   assertion performs, so deriving it that way from C would be checking the engine against itself. Dup'd. */
JS_EXTERN JSValue JS_GetIteratorPrototype(JSContext *ctx);

/* %AsyncIteratorPrototype%, for the same reason and with less of a workaround available: Web IDL §3.7.10.2
   Asynchronous iterator prototype object makes it the [[Prototype]] of every `async iterable<>` interface's
   asynchronous iterator prototype object ("The [[Prototype]] internal slot of an asynchronous iterator
   prototype object must be %AsyncIteratorPrototype%"). This read §3.7.11 until it was checked against the
   fetched text: §3.7.11 is "Maplike declarations" and says nothing about any of this. And unlike
   %IteratorPrototype% there is no script-level walk to it at all from a host that has no async generator to
   hand. Dup'd. */
JS_EXTERN JSValue JS_GetAsyncIteratorPrototype(JSContext *ctx);

/* ECMAScript §27.1.5.1 "CreateAsyncFromSyncIterator ( syncIteratorRecord )" — the ONE step of
   GetIterator(obj, ASYNC) a host cannot perform itself. §27.1.4.1 stood here and is
   "%AsyncIteratorPrototype% [ %Symbol.asyncDispose% ] ( )", a different operation entirely. The DEFINITION
   of the function declared below carried the right number the whole time, so one operation's declaration
   and definition disagreed inside one repository — a pair no instrument compares, because each half is
   correct-looking alone and nothing makes a reader open the other.
   The @@asyncIterator read, the fallback @@iterator read and the method call are all requests this header
   already exports; the wrapper is an intrinsic whose `next` awaits the sync result's VALUE, which is what makes
   an iterable of promises yield what they resolve to. `sync_iter` and `next_method` are CONSUMED; *pnext is the
   wrapper's `next`, called exactly as the sync one would be. */
JS_EXTERN JSValue JS_NewAsyncFromSyncIterator(JSContext *ctx, JSValue sync_iter, JSValue next_method,
                                              JSValue *pnext);

/* [[OwnPropertyKeys]] AS A REQUEST — what a Web IDL `record<K, V>` argument is made of, and the last operation a
   browser component needed that it could not perform. `fetch(u, {headers: {...}})` converts that bag by taking
   its own keys and then reading each one, and on a Proxy the key list IS the page's `ownKeys` trap — so taking
   it with JS_GetOwnPropertyNames from C is the drive-to-completion every other C-side key walk was converted
   away from, and adding a third one would move the build's own C-enum-only-walk ratchet backwards.
   The answer is the ARRAY the spec's CreateArrayFromList builds, of Strings and Symbols 10.5.11's invariant has
   already validated — so the caller may walk it with plain index reads. Returns 11 (the caller returns it), 0
   once *pout is the array, or -1. */
JS_EXTERN int step_ownkeys_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSValue in, JSValue *pout,
                               JSValue **out_cb, int *out_argc);

/* [[GetOwnProperty]] AS A REQUEST — the other half of a `record<K, V>` conversion, and the half that decides
   whether a key counts at all. Web IDL §3.2.23 "Records — record<K, V>"'s *convert a JavaScript value to
   record* step 4.1 asks for each key's DESCRIPTOR and step 4.2 keeps the key only if it is present and
   ENUMERABLE; on a Proxy that is the page's `getOwnPropertyDescriptor` trap. That section holds TWO
   algorithms and the standard NAMES them, so a bare §3.2.23 step number picks neither; the host tree's
   sibling citations of this same list write the name out, and this one had drifted from them unnoticed
   because it cited an ANCHOR instead of a number and so shared no digits with any of them to compare.
   THE NUMBERS `5.1` AND `5.2` STOOD HERE, and wpt's headers-record is why they survived: its own comments
   number the descriptor read `5.1` and the [[Get]] `5.2`, so a reader checking the first digit against the
   corroborator the sentence below names would have CONFIRMED it. The second was never even that — it stood
   over the ENUMERABLE test, which that numbering gives no number of its own. And neither reading matches an
   edition anyone can still fetch: in the one this tree's corpus records and in one from 2019 alike the list
   has FIVE top-level steps with the loop at step 4. A stale number in a test corpus is not a second
   opinion; it is the same recollection the citation was already making.
   Skipping it does not merely lose the trap — it silently includes non-enumerable properties, and wpt's
   headers-record pins the operation SEQUENCE (get @@iterator, ownKeys, then getOwnPropertyDescriptor and get per
   key), so the omission is observable as a count.
   The answer is the descriptor OBJECT the engine builds, or undefined when the property is gone between the
   key snapshot and here — reading `enumerable` off it runs none of the page's code. `atom` is BORROWED.
   Returns 12 (the caller returns it), 0 once *pout is set, or -1. */
JS_EXTERN int step_getownprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValue in,
                                  JSValue *pout, JSValue **out_cb, int *out_argc);

/* AN OUTCOME FORK AS A REQUEST — the primitive that lets a step machine fork, and the one operation for which
   there was previously no answer but an abort.
 *
 * A bytecode branch forks because the interpreter has everything a fork needs at an OP_if: a condition, a pc
 * to resume the sibling at, a frame to snapshot. A C builtin whose result DEPENDS ON UNKNOWN INPUT has the
 * same decision and none of those things — `JSON.parse(text)` over unknown text completes with a value or with
 * a SyntaxError, both feasible, and a builtin that picks one has DELETED the other arm, which is exactly the
 * arm a `catch` reaches and a sink lives behind. So the machine declares the fork instead, and the driver
 * snapshots the whole flow AT the machine (the machine's own state included, cloned through its `visit`) so
 * the sibling RESUMES here with the other outcome. It is a snapshot, never a replay.
 *
 * `over` is the unknown operand the outcome depends on and `op` names the operation; together they are the
 * constraint key, which the SOLVER builds — the engine states the question, never the policy. `n` is how many
 * completions the machine declares feasible; which one means "threw" is a fact about the algorithm and stays
 * the machine's own, with ONE rule on the numbering: OUTCOME 0 IS THE ONE A RUN WITH NO FORKING POLICY TAKES.
 * A machine therefore numbers its ordinary completion first, because the @S candidate re-fire runs ONE
 * concrete path and must not be diverted down an exceptional arm on its way to the sink.
 *
 * `real` IS THE MACHINE'S SECOND DECLARATION AND IT IS A DIFFERENT QUESTION FROM THE NUMBERING ABOVE: not
 * "which completion does a run with no forking policy take" but "which completion does THIS operation reach
 * when run on the operand's EXAMPLE" — the concrete value the run already carries. A branch gets that answer
 * from the condition's own example; an outcome's arms are not booleans, so the seam that holds the operand and
 * the operation's NAME cannot compute it and must not guess it from the name. The machine can, because it owns
 * the semantics, and it says so HERE rather than anywhere else so that the two declarations sit together and
 * neither can be added without the other being visible. JS_OUTCOME_REAL_UNSTATED (quickjs.h) is what a machine
 * passes when it cannot say — an operand carrying no example, an operation whose completion over one is not
 * computable here — and it is a positive statement: the fork still happens, both arms still run, and neither
 * is marked forced. Stating it is what makes the ordinary completion the PRIMARY arm and a forced one as
 * visible in a request's provenance as a forced branch already is.
 *
 * Returns JS_STEP_FORK (the caller returns it — the state must be complete-or-empty at that point, since the
 * SIBLING'S SNAPSHOT IS TAKEN THERE), or 0 once *parm is this flow's outcome. Both a park and a cross-session
 * resume land back on the ask, which re-derives the same arm from the flow's decision vector. */
JS_EXTERN int step_fork_run(JSContext *ctx, JSStepHdr *h, JSValueConst over, const char *op, int n, int real,
                            int *parm);

/* IS ONE OF THIS MACHINE'S FORKS OUTSTANDING? — true across BOTH entries a fork produces (the parent's
 * delivery and the sibling's resume) and false on a request's answer, which is the one distinction a machine
 * re-entered at a stage that both REQUESTS and FORKS has to make: on the two fork entries `cb_result` is the
 * driver's filler, and taking it as a value overwrites the very operand the fork was asked about.
 * DECLARED HERE AND NOT LEFT TO EACH MACHINE because the header used to state the hazard and offer no exit —
 * "it is 0 in the sibling's snapshot ON PURPOSE" is a warning a caller could not act on, and the predicate
 * every caller then reached for instead (`fork_phase != FORK_PH_ANSWERED`) is the one that fails on it. A host
 * step machine meets the same three entries, so it gets the same one answer rather than deriving a fifth. */
JS_EXTERN bool step_fork_pending(const JSStepHdr *h);

/* §7.1.2 ToBoolean ( arg ) AS A STEP MACHINE'S OWN, which is what a C builtin that branches on what the
 * page's callback returned is performing. `arr.filter(x => x.isAdmin)` over a collection whose fields are
 * unknown external input has TWO feasible answers per element, and §7.1.2 decides neither of them: its steps
 * answer from the value's TYPE, an unknown rides an ordinary Object, and step 4 returns true for every one. So
 * the coercion did not answer the question coarsely, it DECIDED it — every element kept, no fork, nothing to
 * say so, and the rule that both arms run wherever the domain permits both of them false of the commonest
 * predicate a real bundle writes. It is the identical defect the `!` operator had, one layer out: a coercion
 * that answers from the REPRESENTATION deletes an arm silently, wherever it is performed.
 *
 * THE ANSWER IS THE BRANCH SEAM AND NOT THE OUTCOME SEAM, and that is the whole of the design — see
 * JSStepHdr.fork_kind. A ToBoolean is the same predicate `if (p)` asks about, so it is keyed by the VALUE'S OWN
 * branch identity and it records what a branch records (the pin, the excluded token, the bound, the call
 * predicate). The outcome seam keys by (operand, OPERATION, completion), which is right for a machine asking
 * which of ITS completions it reaches and wrong here twice over: it would fork a second time over a predicate
 * the flow may already have fixed, and it would file a domain-less shape for a parameter the page had gated.
 *
 * IT IS NOT A SECOND SPELLING OF JSConcolicHooks.to_bool, WHICH MINTS A VALUE. `to_bool` exists because a
 * program HOLDS the boolean (`var q = !p`), so `!p` needs an identity of its own or `"" + !p` and `"" + p`
 * compose to one derivation. A machine that only needs a C `int` to branch on holds no value and mints none —
 * exactly as OP_if_false hands its operand straight to the branch hook while OP_lnot is the one that mints.
 *
 * `op` NAMES THE ASK ON THIS MACHINE AND IS NOT THE CONSTRAINT KEY. The key is the operand's, above; this is
 * what `fork_ask_key` hashes, so a machine with two ToBoolean asks cannot consume one's answer at the other's
 * call site. A machine with one ask names its coercion and is done.
 *
 * `v` is BORROWED for the length of the request, exactly as step_fork_run's `over` is, so the caller must hold
 * it somewhere THE SNAPSHOT CARRIES — its own state, reached by its `visit` — and not in a C local. A value
 * that is not unknown input is answered HERE with the ordinary §7.1.2 and no fork, which is why this has no
 * predicate at its call sites: there is no second path for one to select.
 *
 * Returns JS_STEP_FORK (the caller returns it; the state must be complete-or-empty, since the sibling's
 * snapshot is taken there), or 0 once *pres is 0 or 1.
 *
 * RESIDUAL — WHAT IS NOT COVERED. The array-callback walk (every/some/filter and their %TypedArray% twins) is
 * the only consumer. Every other C body that coerces a value the PAGE produced still answers from §7.1.2's own
 * steps and therefore takes `true` for unknown input: the find family (Array find/findIndex/findLast/
 * findLastIndex and their four %TypedArray% twins — EIGHT builtins sharing one machine, which is the number
 * that matters, since routing the machine covers all of them), the Iterator helper `filter` predicate, the
 * eager iterator terminals (`some`/`every`/`find` over an iterator), and the internal-method booleans a Proxy
 * trap returns ([[Has]], [[Set]], [[DefineOwnProperty]], [[PreventExtensions]], [[SetPrototypeOf]],
 * [[IsExtensible]]).
 *
 * WHAT THE NEXT DIFF BUILDS. The find machine alone is routable as this one was: its state begins with a
 * JSStepHdr, so it needs a held-operand field of its own (JSArrayEvery.test's shape and sentinel) and its
 * predicate-test branch calling this. The two ITERATOR families are NOT — their records carry no JSStepHdr at
 * all, so they have no fork seam to reach and the diff that covers them is the one that gives them one; naming
 * them here without that distinction would send the next reader to add a call where there is no header to pass.
 *
 * HOW ITS ABSENCE WOULD SHOW. `[{}].find(x => x.isAdmin)` over unknown input answers with the element and
 * forks nothing, where the same predicate written as `for (…) if (x.isAdmin)` forks two worlds — one builtin
 * and its hand-written equivalent disagreeing about the same program, which is exactly the divergence the
 * solver differential is built to fail on. */
JS_EXTERN int step_tobool_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, const char *op, int *pres);

/* ToString AS A REQUEST — the coercion nearly every Web IDL argument actually is. `DOMString type`,
   `DOMString name`, `DOMString selector`: each is ToString on whatever the page passed, so
   `el.addEventListener({toString(){ for(;;){} }}, f)` is the page's loop. Without this a component's only
   honest move was JS_ToCString from C, which in this engine does not quietly misbehave — it reaches
   JS_ToPrimitiveFree's DFAIL and aborts, naming the site — but aborting is where a capability is missing, not
   where it is built. Returns JS_STEP_REQUEST (the caller returns it), 0 once *pout is set, or -1.
   …OR JS_STEP_UNKNOWN, when the value being coerced is UNKNOWN EXTERNAL INPUT and §7.1.19 ToString ( arg )
   therefore has no String to produce. Nothing is written to *pout on that path; the caller returns the code
   like any other positive one — which every call site already does, because that is what parking on a request
   has always meant — and the driver completes the machine with the derived unknown. See JS_STEP_UNKNOWN. */
JS_EXTERN int step_tostring_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, JSValue *pout,
                                JSValue **out_cb, int *out_argc);

#endif
