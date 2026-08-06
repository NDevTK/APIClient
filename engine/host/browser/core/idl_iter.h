/* DRIVING AN ES ITERABLE AS A SEQUENCE OF REQUESTS — Web IDL §3.2.20's `sequence<T>` conversion. See idl_iter.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ITER_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ITER_H
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

#endif
