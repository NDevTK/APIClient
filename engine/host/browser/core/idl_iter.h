/* DRIVING AN ES ITERABLE AS A SEQUENCE OF REQUESTS — Web IDL §3.2.21's `sequence<T>` conversion. See idl_iter.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ITER_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ITER_H
#include <stdbool.h>
#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"

/* DRIVING A JS ITERABLE, one value per call, as a sub-sequence. Every step of the iterator protocol is the
   page's code — the @@iterator read, its call, the `next` read, each `next()` call, and the `done`/`value`
   reads off each result — so none of them can be a C loop.
   It lives here rather than in whichever component needed it first because it is WEB IDL'S conversion and not
   that component's: `new Headers([[k,v]])` and `new URLSearchParams([[k,v]])` are the same algorithm over the
   same protocol, and Web IDL NESTS it (a `sequence<sequence<T>>` runs the protocol over the outer value and
   again over each inner pair), which is why this is a cursor a caller instantiates twice rather than a loop
   written once. */
typedef struct {
    uint8_t phase;
    uint8_t cphase;                    /* the call request's own phase — a cursor holds a call across stages */
    JSValue iterfn, iter, next_fn, res, value;   /* owned */
    JSValue cb[2];                     /* 2 + 0: the protocol's calls take no arguments */
    int     done;
} IterCursor;

/* ECMAScript 7.3.10 GetMethod MINUS its step 1, the `? GetV(value, propertyKey)`, which is the ONE step of it
   that cannot live in a helper: the read is a request in this engine, so it is issued by the caller's own step
   machine and what is shared is the DECISION made on its answer. The other three steps ARE that decision, and
   they are not the question "is this a function":
     - step 2, "If func is either undefined or null, return undefined" — there is NO method, which is not an
       error and not a second arm;
     - step 3, "If IsCallable(func) is false, throw a TypeError exception" — NOT a quiet fall-through.
   Web IDL §3.2.25's sequence arm is `Let method be ? GetMethod(V, %Symbol.iterator%)` and a union chooses that
   arm by it, so a union spelling the test as `Get(V, @@iterator) is not undefined` took its OTHER arm for a
   value the standard rejects: `new Headers({[Symbol.iterator]: 1})` walked the record arm and produced an empty
   header list where a browser throws. ECMAScript's GetIterator adds a step of its own (an ABSENT method is also
   a TypeError there), which is why a caller that iterates unconditionally reads a `0` as its own failure.
   `method` is the value the [[Get]] answered and stays the CALLER'S to release, on every one of the three
   outcomes — the sequence arm hands the same value on to iter_cursor_init_from_method, which consumes it.
   `what` NAMES THE READ, not the property — GetMethod is defined over any P, and its callers here read
   @@iterator, so the subject is spelled out by the one that made the read ("a Headers init's @@iterator") and
   the message is about the value ("... is not callable").
   Returns 1 (callable: the method), 0 (there is no method), or -1 with step 3's TypeError live. */
int idl_get_method(JSContext *ctx, JSValueConst method, const char *what);

/* PLANT A CURSOR ON A CLEAR SLOT — the memset behind `iter_cursor_init`. Call it through the macro, which is
   what states the invariant at the SITE that plants. */
void iter_cursor_plant(IterCursor *c);
/* DOES THIS CURSOR HOLD ANYTHING — side-effect-free, so it is a DCHECK condition. A never-used cursor inside a
   zeroed state block holds the non-refcounted integer 0 in every slot and a released one holds JS_UNDEFINED, so
   "empty" is "no slot carries a reference" rather than any one spelling of nothing. */
bool iter_cursor_empty(const IterCursor *c);

/* A CURSOR IS PLANTED ON A CLEAR SLOT, AND THE MEMSET IS WHY THAT IS AN ASSERTION AND NOT A CONVENTION.
   `iter_cursor_plant` MEMSETS, so planting over a cursor that still holds its iterator, its `next` and its last
   result DROPS all three — a leak the runtime's GC teardown reports as a whole realm surviving and that no call
   site names, because the site that lost the reference is not the site that notices. That is the shape it took:
   an argument-position `sequence<T>` left its cursor loaded when it finished, the DICTIONARY at the next
   declared position started its walk on the same cursor, and `Object x N` / `Function x2 refcount N..N` /
   `Array Iterator x N` came out of the census for N conversions of one document.
   IT IS A MACRO SO THE ABORT NAMES THE PLANTER. This invariant is over a TRANSITION and it is reachable from
   every call site of the name below; checked inside the function it would stamp idl_iter.c for all of them and
   the reader would be handed a remedy with no object. */
#define iter_cursor_init(cur_)                                                                      \
    do {                                                                                            \
        IterCursor *iter_cursor_init_c_ = (cur_);                                                   \
        DCHECK(iter_cursor_empty(iter_cursor_init_c_),                                              \
               "a `sequence<T>` cursor was planted over one that still held its iterator — planting "\
               "MEMSETS, so this line is dropping the iterator, its `next`, the last iterator result "\
               "and the last element pulled, with nothing left that can free them. A cursor that "   \
               "answered `done` releases itself (Web IDL §3.2.21.1 Creating a sequence from an "     \
               "iterable step 3.2 returns and never looks at the record again), so one arriving here "\
               "loaded was ABANDONED mid-walk: release it at the site that abandoned it");           \
        iter_cursor_plant(iter_cursor_init_c_);                                                      \
    } while (0)

void iter_cursor_visit(JSContext *ctx, IterCursor *c, JSStepVisit *v);
void iter_cursor_release(JSContext *ctx, IterCursor *c);
/* ONE VALUE per successful return: `c->done` says the iteration ended, otherwise `c->value` holds it (owned by
   the cursor). Call it again for the next. Returns >0 (the caller returns it), 0, or -1 with a throw live.
   A CURSOR THAT HAS ANSWERED `done` HOLDS NOTHING AND IS NOT RE-RUN. Web IDL §3.2.21.1 Creating a sequence from
   an iterable step 3.2 — "If next is done, then return an IDL sequence value of type sequence<T> of length i"
   (its tail names the collected elements) — RETURNS, so the iterator record is dead from that step on
   and every value the cursor is holding at it is unreachable by the algorithm. Releasing there rather than
   leaving the owner to do it is what makes the state impossible instead of conventional: the owner's `visit`
   still covers a cursor abandoned MID-walk (a throw at an element conversion, a machine torn down at a park),
   which is the only way one can still be loaded, and that is exactly the case the plant above asserts on. */
int  iter_cursor_run(JSContext *ctx, JSStepHdr *h, IterCursor *c, JSValueConst src,
                     JSValue in, JSValue **out_cb, int *out_argc);

/* ECMAScript's GetIteratorFromMethod — the entry Web IDL §3.2.25 step 11.2 reaches this protocol through, and
   a SECOND entry rather than a flag on the first, because the two differ in who performed the @@iterator read.
   A union whose member types include a sequence chooses its arm with ? GetMethod(V, %Symbol.iterator%) and
   then hands §3.2.21.1's "create a sequence from an iterable" THAT method. Reading @@iterator again here would be
   a second [[Get]] the page can SEE — a Proxy `get` trap counts them, and an accessor may answer a different
   function the second time, so the sequence would be built by an iterator the union never inspected. The cursor
   therefore starts at the CALL of the method it is planted with.
   `method` is CONSUMED, and it must be callable: that is GetMethod's own answer (undefined and null mean there
   is no method, anything else non-callable is a TypeError), so the caller has already decided it and this
   asserts the decision rather than repeating it. It plants through `iter_cursor_init`, so the empty-slot
   invariant above is this entry's too and is stated in exactly one place. */
void iter_cursor_init_from_method(JSContext *ctx, IterCursor *c, JSValue method);

/* WEB IDL §3.2.23's `record<K, V>` CONVERSION, as the same shape of cursor. It is [[OwnPropertyKeys]] followed
   by a [[GetOwnProperty]] and a [[Get]] per key — on a Proxy those are the page's `ownKeys`, `getOwnPropertyDescriptor`
   and `get` traps, so every one of them is a request. Shared for the same reason the iterable cursor is:
   `new Headers({a:1})` and `new URLSearchParams({a:1})` are one algorithm.
   `key_ok` runs on the key BEFORE the value's [[Get]] is issued, because §3.2.23 step 4.2.1 converts the key
   first and a record of {a:"b", "\uFFFF":"d"} must therefore perform five operations and not six. It runs none
   of the page's code (the key is already a String or a Symbol); return -1 with a throw live to stop. */
typedef struct {
    uint8_t phase;
    int     i, n;               /* the cursor into the key list, and how many keys there are */
    JSValue keys, name, value;  /* owned */
    int     done;
} RecordCursor;

void record_cursor_init(RecordCursor *c);
void record_cursor_visit(JSContext *ctx, RecordCursor *c, JSStepVisit *v);
/* ONE PAIR per successful return: `c->done` says the keys are exhausted, otherwise `c->name` and `c->value`
   hold it (owned by the cursor). Returns >0 (the caller returns it), 0, or -1 with a throw live. */
int  record_cursor_run(JSContext *ctx, JSStepHdr *h, RecordCursor *c, JSValueConst src, JSValue in,
                       int (*key_ok)(JSContext *ctx, JSValueConst key, void *user), void *user,
                       JSValue **out_cb, int *out_argc);

/* ---- §3.7.9.1's DEFAULT ITERATOR OBJECT, for an interface declaring `iterable<K, V>` ----------------------
 *
 * WHICH NUMBERS THESE ARE, STATED ONCE. Web IDL carries the name "Iterable declarations" TWICE and the title
 * does not tell the two apart: §2.5.9 is the `iterable<>` DECLARATION and the terms it mints ("value pairs to
 * iterate over", "pair iterator", "value iterator"), and §3.7.9 is the JavaScript binding — the "define the
 * iteration methods" algorithm that puts members on a prototype, with §3.7.9.1 Default iterator objects and
 * §3.7.9.2 Iterator prototype object beneath it. Citations here name the ALGORITHM beside the number for that
 * reason. §3.7.10 is "Asynchronous iterable declarations" and owns none of this: its own step 2 asserts that
 * a definition reaching it "does not have an indexed property getter or an iterable declaration".
 * SUB-NUMBERS ARE TOP-LEVEL `<li>`s WITH LIST DEPTH TRACKED, counted off the fetched text, and every step
 * cited below holds exactly one list.
 *
 * `keys()`, `values()`, `entries()`, `forEach()`, `@@iterator` and the iterator object they hand out are the
 * same six things for every such interface — Headers and URLSearchParams differ only in WHAT the pairs are.
 * Shared for the reason the two conversion cursors are: the iterator prototype object's [[Prototype]] is
 * %IteratorPrototype%, its `next` is writable+enumerable+configurable and its @@toStringTag is the interface
 * name plus " Iterator", and getting any of that wrong once per interface is how a surface drifts.
 *
 * `pair` yields the i-th pair AS OF NOW: the "value pairs to iterate over" §2.5.9 mints are re-read at every
 * step by §3.7.9's forEach step 8.3 ("Set pairs to idlObject's current list of value pairs to iterate over"),
 * so a callback that appends during forEach is seen by the steps after it, and `count` is asked again each
 * time. `count` returns -1 when `target` is not an instance of the interface, which is the receiver check. */
typedef struct {
    int  (*count)(JSContext *ctx, JSValueConst target);
    /* Fills *key and *value (owned by the caller). Only called with 0 <= i < count. */
    void (*pair)(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value);
    const char *iface;   /* the interface's identifier, for the @@toStringTag and the class name */
    /* IS THIS A `setlike<V>` RATHER THAN AN `iterable<K, V>`? TWO SECTIONS ANSWER, one per declaration, and
       neither answers for the other: §3.7.9's define-the-iteration-methods step 2.1 builds ONE function object
       F and makes it both @@iterator and `entries` for an `iterable<K, V>`, while §3.7.12.2 %Symbol.iterator%
       gives a `setlike<V>` a @@iterator "whose value is the function object that is the value of the values
       property". So `for (const s of set)` yields the VALUES
       and not « v, v » pairs. It is declared here, beside the members, because it is a fact about the
       DECLARATION — a caller passing a flag to the install would be the same fact asked at the wrong place,
       and the interface that forgot to pass it would silently iterate pairs. */
    bool setlike;
} IdlPairIterOps;

/* DECLARE the iterator class, its prototype and the forEach machine for one interface. Returns a handle. */
int  idl_pair_iter_declare(JSContext *ctx, const IdlPairIterOps *ops);
/* INSTALL keys/values/entries/forEach/@@iterator on the interface's prototype. §3.7.9 step 2.1 makes @@iterator
   the SAME function object as `entries` (one `F`, defined twice), which this does — and §3.7.12.2 makes it the
   `values` object for a `setlike<V>`. */
void idl_pair_iter_install(JSContext *ctx, JSValueConst proto, int handle);
/* Release the iterator prototype the declaration minted. */
/* §3.7.9.2's iterator prototype objects for ONE realm, declared into core/realm.h's list by the first
   idl_pair_iter_declare — one install builds every declared interface's. */
void idl_pair_iter_install_protos(JSContext *ctx);

#endif
