/* ---- WEB IDL §3.7.10's DEFAULT ASYNCHRONOUS ITERATOR OBJECT, for an interface declaring `async_iterable<>` --
 *
 * §2.5.10 is the declaration ("Objects that implement an interface that is declared to be asynchronously
 * iterable support being iterated over asynchronously to obtain a sequence of values"); §3.7.10 is what the
 * JavaScript binding puts on the interface prototype object — `values`, and for a PAIR declaration `entries`
 * and `keys` too, with %Symbol.asyncIterator% being the SAME function object as `entries` (pair) or `values`
 * (value). §3.7.10.1 is the object those hand out and §3.7.10.2 is its prototype, whose `next` — and `return`
 * where the prose defines an asynchronous iterator return algorithm — this file implements ONCE.
 *
 * WHY IT IS SHARED, AND NOT WRITTEN INTO THE INTERFACE THAT NEEDED IT FIRST. §3.7.10.2's `next` is not two or
 * three steps: it is a brand check that REJECTS rather than throwing, an `is finished` short circuit, a
 * component algorithm returning a promise, a fulfill handler that decides between end-of-iteration and a value
 * pair, a reject handler that finishes the iterator, and — the part every hand-rolled copy gets wrong — the
 * ORDERING RULE below. `core/streams/readable_stream.c` already carries one copy of that shape for §4.2.5's
 * `async_iterable<any>` and is the conversion this file exists to make possible; a second copy written for the
 * File System Standard would have been the third statement of one algorithm.
 *
 * THE ORDERING RULE IS THE WHOLE OF STEPS 9-11, and it is what an interface must not re-derive. A `next()` made
 * while a previous one is still in flight does NOT start a second iteration step: §3.7.10.2 step 10 attaches
 * ONE function — nextSteps itself — as BOTH the fulfil and the reject handler of the outstanding `ongoing
 * promise`, and sets `ongoing promise` to the promise of THAT reaction, so the calls form a chain and each
 * one's steps begin only once its predecessor has settled either way. Step 11's "otherwise" is the only branch
 * that runs the steps here and now. `for await (const x of it)` never exercises the chain (it awaits each
 * result before asking again); two bare `it.next()` calls in one turn do, and so does anything that races a
 * `next()` against a `return()`.
 *
 * WHAT SUSPENDS. Everything in it: the component's "get the next iteration result" is a STEP (it may read the
 * page's objects, and it may have to ask the host), and so is every settle — Call(capability.[[Resolve]], …)
 * reaches 27.2.1.3.2 step 8's `then` read on whatever it is resolved with, which for a pair declaration is an
 * Array and therefore one `Object.defineProperty(Array.prototype, "then", …)` away from being the page's code.
 *
 * WHAT TIME-TRAVELS. §3.7.10.1's `ongoing promise` and `is finished` are per-ITERATOR state a flow WRITES, so
 * they live on the iterator object's own record and that record is captured into the running flow's COW delta
 * at the accessor that reaches it — one flow's iterator can be finished while its sibling's is not. The
 * component's own per-iterator state is a JS VALUE for the same reason (§State-isolation: platform data a flow
 * queues is a JS value, never malloc'd C), so its mutations are property writes the delta already captures and
 * it parks to the cold tier with the rest of the flow. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ASYNC_ITER_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ASYNC_ITER_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"

/* §2.5.10's ASYNCHRONOUS ITERATOR INITIALIZATION STEPS: "These receive the instance of the interface being
   iterated, the newly-created iterator object, and a list of IDL values representing the arguments passed, if
   any." `pstate` is the iterator's own state slot (JS_UNDEFINED on entry) — the File System Standard's "set
   iterator's past results to an empty set" is one assignment to it.
 *
 * IT IS A STEP, with the same return contract as the two algorithms below, because §3.7.10 step 3.1.6 runs it
 * INSIDE the member and what a standard writes there is not bounded by this file. Streams §4.2.5's step 1 is
 * `? AcquireReadableStreamDefaultReader(stream)`, and §4.3's acquisition SETTLES the reader's `closed` promise
 * at once on a stream that has already closed or errored — a resolving function, which reaches 27.2.1.3.2 step
 * 8's `then` read. A plain C body could only have driven that to completion.
 * >0 means it parked (the member returns that code and this is re-entered at the same point with the answer in
 * `in`), 0 means the iterator is initialised, -1 means it threw — and the member then throws rather than
 * answering with an iterator, which is what the `?` on Streams §4.2.5's step 1 means.
 * A declaration with no such prose declares NULL. */
typedef int (*IdlAsyncIterInit)(JSContext *ctx, JSStepHdr *hdr, void *work,
                                JSValueConst target, JSValueConst iter,
                                int argc, JSValueConst *argv, JSValue *pstate, JSValue in,
                                JSValue **out_cb, int *out_argc);

/* §2.5.10's GET THE NEXT ITERATION RESULT — "Prose accompanying an interface with an asynchronously iterable
   declaration MUST define" it, so this is the one operation that has no default and no NULL.
 *
 * "It must return a Promise that either rejects, resolves with a special end of iteration value to signal the
 * end of the iteration, or resolves with one of the following: for value asynchronously iterable declarations:
 * a value of the type given in the declaration; for pair asynchronously iterable declarations: a tuple". The
 * end-of-iteration value is `idl_async_iter_end` below; the tuple is a two-element Array.
 *
 * IT IS A STEP, with the return contract every request in this engine has: >0 means it parked (the caller
 * returns that code and this is re-entered at the same point with the answer in `in`), 0 means `*ppromise` is
 * the promise it returns (OWNED), -1 means it threw. `work` is its own storage, `work_size` bytes, ZEROED
 * before the first entry — and a zeroed JSValue is the INTEGER 0 rather than undefined, so a state carries a
 * flag of its own for "have I started" and undefines its slots there. */
typedef int (*IdlAsyncIterNext)(JSContext *ctx, JSStepHdr *hdr, void *work,
                               JSValueConst target, JSValueConst iter, JSValue *pstate, JSValue in,
                               JSValue *ppromise, JSValue **out_cb, int *out_argc);

/* §2.5.10's ASYNCHRONOUS ITERATOR RETURN ALGORITHM — "The prose MAY also define" it, "invoked in the case of
   premature termination of the async iterator ... It must return a Promise; if that promise fulfills, its
   fulfillment value will be ignored, but if it rejects, that failure will be passed on to users of the async
   iterator API."
   A declaration whose prose defines none declares NULL, and §3.7.10.2 then gives its asynchronous iterator
   prototype object NO `return` property at all — which is not an omission but the spec's own conditional ("If
   an asynchronous iterator return algorithm is defined for the interface, then …"), and is observable: a
   `break` out of a `for await` loop runs AsyncIteratorClose, whose GetMethod finds undefined and calls
   nothing. Same step contract as the next algorithm; `value` is the argument `return(value)` was given. */
typedef int (*IdlAsyncIterReturn)(JSContext *ctx, JSStepHdr *hdr, void *work,
                                  JSValueConst target, JSValueConst iter, JSValue *pstate,
                                  JSValueConst value, JSValue in,
                                  JSValue *ppromise, JSValue **out_cb, int *out_argc);

/* THE FIRST STAGE THAT IS THE COMPONENT'S OWN — one per hosting machine, and the same mechanism idl_args.h's
 * IDL_STEP_FIRST is, for the same reason.
 *
 * A COMPONENT'S ALGORITHM RUNS INSIDE ONE OF THIS FILE'S TWO MACHINES: the initialization steps inside §3.7.10's
 * member, and the other two inside §3.7.10.2's `next`/`return`. Those machines own the stages up to and
 * including the one that RUNS the component's algorithm, so a component that rests inside its own algorithm
 * needs stages of its own beyond them — otherwise its rest point is reported as the hosting stage, which names
 * Web IDL where the standard that is actually suspended is the component's (a flow parked in Streams §4.2.5's
 * AcquireReadableStreamDefaultReader reported "Web IDL §3.7.10.2 return step 8.4"). A private cursor byte in
 * the component's `work` is the same defect one layer down: the driver's assert cannot see it, a park cannot
 * report it and a later build cannot resolve it back to a step.
 *
 * So a component numbers its own X-list from here, exactly as a declared IDL member numbers its own from
 * IDL_STEP_FIRST:
 *
 *     enum { IDL_ASYNC_ITER_STAGE_BASE(RSI_STAGES) RSI_STAGES(JS_STEP_STAGE_ENUM) };
 *     static const char *const RSI_STEPS[] = { RSI_STAGES(JS_STEP_STAGE_LABEL) NULL };
 *
 * and hands the labels to the declaration, which JOINS them onto the hosting machine's — the one place the
 * component's declaration and the hosting definition are both in hand, so no component restates this file's
 * stages and none can index its own from the wrong base. The two numbers are checked against the two enums
 * they mirror at COMPILE time in idl_async_iter.c; they are stated here because a component cannot see those
 * enums, which are this file's own. */
#define IDL_ASYNC_ITER_STEP_FIRST      5
#define IDL_ASYNC_ITER_INIT_STEP_FIRST (IDL_STEP_FIRST + 2)

#define IDL_ASYNC_ITER_STAGE_BASE(list)      list##_base = IDL_ASYNC_ITER_STEP_FIRST - 1,
#define IDL_ASYNC_ITER_INIT_STAGE_BASE(list) list##_base = IDL_ASYNC_ITER_INIT_STEP_FIRST - 1,

typedef struct {
    /* The interface's identifier. §3.7.10.2: "The class string of an asynchronous iterator prototype object for
       a given interface is the result of concatenating the identifier of the interface and the string
       ' AsyncIterator'", and §3.7.10.1's note says the ITERATOR's class string is that same one — a default
       asynchronous iterator object has none of its own. */
    const char *iface;
    /* §2.5.10's TWO SHAPES. "If a single type parameter is given, then the interface has a value asynchronously
       iterable declaration ... If two type parameters are given, then the interface has a pair asynchronously
       iterable declaration". It decides which members §3.7.10 defines (a value declaration gets `values` and
       %Symbol.asyncIterator% only), which of them %Symbol.asyncIterator% IS, and how the fulfilment value of a
       "get the next iteration result" promise is read. It is declared here, beside the members, because it is a
       fact about the DECLARATION — the same reason IdlPairIterOps::setlike is. */
    bool pair;
    /* §3.7.10's "If jsValue does not implement definition, then throw a TypeError" — the receiver check every
       one of the three members performs before it builds anything. */
    bool (*implements)(JSValueConst v);

    IdlAsyncIterInit   init;    /* §2.5.10's initialization steps, or NULL where the prose defines none */
    IdlAsyncIterNext   next;    /* §2.5.10's get the next iteration result — required */
    IdlAsyncIterReturn ret;     /* §2.5.10's return algorithm, or NULL where the prose defines none */

    /* WHERE THE COMPONENT'S OWN ALGORITHMS REST, joined onto the two definitions that host them — see the
       bases below. `init_steps` is the initialization steps' list, numbered from IDL_ASYNC_ITER_INIT_STEP_FIRST
       and joined onto §3.7.10's member; `steps` is ONE list for the OTHER TWO, numbered from
       IDL_ASYNC_ITER_STEP_FIRST and joined onto §3.7.10.2's machine — one, because `next` and `return` share
       that machine and therefore share its step list, so their stages are numbered in one space and no two of
       them may name the same step (idl_async_iter.c asserts that at the join, and quickjs.c's step_stage_check
       asserts the round trip again at every rest).
       NULL is for an algorithm that never rests at a step of its OWN: the File System Standard's
       initialization steps are one assignment and make no request, so the hosting stage is the only rest point
       there is. An algorithm that PARKS with no stage of its own is refused where it parks — its rest point
       would be the hosting algorithm's stage, which names the wrong standard, and its re-entry would re-run
       its first steps. */
    const char *const *init_steps;
    const char *const *steps;

    /* The THREE algorithms' own step storage, and the ONE declaration of what it owns. There is no separate
       release: `visit` is the list, and the driver's teardown discharges it exactly as it discharges the
       machine's own fields — a second hand-written list is the pair this engine forbids.
       ONE declaration serves all three because only ONE of them ever runs in a given block: the initialization
       steps run inside the MEMBER's machine and the other two inside §3.7.10.2's, and each block is ZEROED
       before that algorithm's first entry. So a component's cursor byte starts at 0 in each of them, and a
       zeroed JSValue is the INTEGER 0 rather than undefined — which is why every algorithm here undefines its
       slots before the first thing that can fail. */
    size_t work_size;
    void (*work_visit)(JSContext *ctx, void *work, JSStepVisit *v);

    /* §3.7.10's "converting arguments for an asynchronous iterator method", as the argument list the
       declaration writes: `async_iterable<V>(optional T arg)`. §2.5.10 requires every one of them to be
       OPTIONAL, which is why this file calls idl_optional_from(0) for every declaration rather than letting a
       component state it — a required argument would break `for await (const x of obj)`, which calls
       %Symbol.asyncIterator% with none, and the requirement is the standard's rather than the component's.
       A declaration with no argument list leaves all four zero. */
    const IdlArgType    *arg_types;
    int                  nargs;
    const IdlDictMember *members;
    int                  nmembers;
} IdlAsyncIterOps;

/* DECLARE one interface's asynchronous iteration: the iterator class, the `next`/`return` machines and the
   three methods. Once per AGENT, from the component's `_init`, and BEFORE any realm is built — the pool is
   sealed by the first install, because a declaration arriving afterwards would be a class no existing realm has
   a prototype for. Returns a handle. */
int idl_async_iter_declare(JSContext *ctx, const IdlAsyncIterOps *ops);

/* INSTALL, on ONE realm: §3.7.10.2's asynchronous iterator prototype object for this interface (whose
   [[Prototype]] is %AsyncIteratorPrototype%, which is where `@@asyncIterator` returning `this` and the
   async-iterator helpers come from), and §3.7.10's `values` / `entries` / `keys` / %Symbol.asyncIterator% on
   the interface prototype object.
   BOTH HALVES HERE, rather than the prototype in a realm intrinsic of its own: the iterator prototype is only
   reachable through these members, so building it wherever they are built is what keeps the two in step — a
   realm that does not get the members (§3.3.13 removes every [SecureContext] member of a non-secure one) must
   not get a prototype object nothing can reach either. */
void idl_async_iter_install(JSContext *ctx, JSValueConst proto, int handle);

/* §2.5.10's END OF ITERATION — "a special end of iteration value to signal the end of the iteration".
 *
 * It is a value the standard puts OUTSIDE the JavaScript value space, and this engine gives it an object of an
 * agent-private class: unforgeable (a page has no door onto the class), distinguishable from every value a
 * declaration's type admits (a value declaration of type `any` may legitimately yield `undefined`, so
 * `undefined` cannot be the marker even though the File System Standard's prose says "resolve promise with
 * undefined"), and carrying no state to get wrong. OWNED. */
JSValue idl_async_iter_end(JSContext *ctx);

/* RELEASE what the DECLARATIONS allocated — the joined stage-label arrays, one pair per declared interface,
   held for the agent's life because the definitions BORROW them (JS_RegisterStepDef's contract) and freed with
   it, exactly as idl_args_free releases the pool's own join. The declarations themselves are the AGENT's, so
   this puts the table back where it was before the first one: a next agent in this process declares again, and
   §2.5.10's end-of-iteration class is re-declared with its runtime rather than kept as an id no live runtime
   has a class for. */
void idl_async_iter_free(void);

#endif
