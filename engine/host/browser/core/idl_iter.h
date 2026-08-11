/* DRIVING AN ES ITERABLE AS A SEQUENCE OF REQUESTS — Web IDL §3.2.20's `sequence<T>` conversion. See idl_iter.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ITER_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ITER_H
#include <stdbool.h>
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

void iter_cursor_init(IterCursor *c);
void iter_cursor_visit(JSContext *ctx, IterCursor *c, JSStepVisit *v);
void iter_cursor_release(JSContext *ctx, IterCursor *c);
/* ONE VALUE per successful return: `c->done` says the iteration ended, otherwise `c->value` holds it (owned by
   the cursor). Call it again for the next. Returns >0 (the caller returns it), 0, or -1 with a throw live. */
int  iter_cursor_run(JSContext *ctx, JSStepHdr *h, IterCursor *c, JSValueConst src,
                     JSValue in, JSValue **out_cb, int *out_argc);

/* WEB IDL §3.2.21's `record<K, V>` CONVERSION, as the same shape of cursor. It is [[OwnPropertyKeys]] followed
   by a [[GetOwnProperty]] and a [[Get]] per key — on a Proxy those are the page's `ownKeys`, `getOwnPropertyDescriptor`
   and `get` traps, so every one of them is a request. Shared for the same reason the iterable cursor is:
   `new Headers({a:1})` and `new URLSearchParams({a:1})` are one algorithm.
   `key_ok` runs on the key BEFORE the value's [[Get]] is issued, because §3.2.21 step 5.2 converts the key
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
void record_cursor_release(JSContext *ctx, RecordCursor *c);
/* ONE PAIR per successful return: `c->done` says the keys are exhausted, otherwise `c->name` and `c->value`
   hold it (owned by the cursor). Returns >0 (the caller returns it), 0, or -1 with a throw live. */
int  record_cursor_run(JSContext *ctx, JSStepHdr *h, RecordCursor *c, JSValueConst src, JSValue in,
                       int (*key_ok)(JSContext *ctx, JSValueConst key, void *user), void *user,
                       JSValue **out_cb, int *out_argc);

/* ---- §3.7.10's DEFAULT ITERATOR OBJECT, for an interface declaring `iterable<K, V>` ----------------------
 *
 * `keys()`, `values()`, `entries()`, `forEach()`, `@@iterator` and the iterator object they hand out are the
 * same six things for every such interface — Headers and URLSearchParams differ only in WHAT the pairs are.
 * Shared for the reason the two conversion cursors are: the iterator prototype object's [[Prototype]] is
 * %IteratorPrototype%, its `next` is writable+enumerable+configurable and its @@toStringTag is the interface
 * name plus " Iterator", and getting any of that wrong once per interface is how a surface drifts.
 *
 * `pair` yields the i-th pair AS OF NOW: §3.7.10's "value pairs to iterate over" is recomputed at every step,
 * so a callback that appends during forEach is seen by the steps after it, and `count` is asked again each
 * time. `count` returns -1 when `target` is not an instance of the interface, which is the receiver check. */
typedef struct {
    int  (*count)(JSContext *ctx, JSValueConst target);
    /* Fills *key and *value (owned by the caller). Only called with 0 <= i < count. */
    void (*pair)(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value);
    const char *iface;   /* the interface's identifier, for the @@toStringTag and the class name */
    /* IS THIS A `setlike<V>` RATHER THAN AN `iterable<K, V>`? §3.7.10 makes @@iterator the same function object
       as `entries` for the second and as `values` for the first, so `for (const s of set)` yields the VALUES
       and not « v, v » pairs. It is declared here, beside the members, because it is a fact about the
       DECLARATION — a caller passing a flag to the install would be the same fact asked at the wrong place,
       and the interface that forgot to pass it would silently iterate pairs. */
    bool setlike;
} IdlPairIterOps;

/* DECLARE the iterator class, its prototype and the forEach machine for one interface. Returns a handle. */
int  idl_pair_iter_declare(JSContext *ctx, const IdlPairIterOps *ops);
/* INSTALL keys/values/entries/forEach/@@iterator on the interface's prototype. §3.7.10 makes @@iterator the
   SAME function object as `entries`, which this does. */
void idl_pair_iter_install(JSContext *ctx, JSValueConst proto, int handle);
/* Release the iterator prototype the declaration minted. */
/* §3.7.10's iterator prototype objects for ONE realm, declared into core/realm.h's list by the first
   idl_pair_iter_declare — one install builds every declared interface's. */
void idl_pair_iter_install_protos(JSContext *ctx);

#endif
