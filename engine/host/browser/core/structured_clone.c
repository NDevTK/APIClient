/* STRUCTURED SERIALIZATION — HTML §2.7.
 *
 * WHAT IT IS FOR. Three separate places in this engine need "a copy of this value in this realm, deep, cyclic,
 * and refusing what cannot be copied", and each of them is currently either missing or wrong:
 *   - HTML 9.4.2's MessagePort.postMessage, which delivers a SERIALIZED message — a port that handed the same
 *     object reference to both sides would not be a port, it would be a shared variable.
 *   - HTML 9.4.4's window.postMessage, the same.
 *   - Streams §4.9.7's ReadableStreamTee with cloneForBranch2, which is how Fetch's `response.clone()` gives
 *     the second branch its own chunk. Fourteen of response-clone's subtests assert exactly that the two
 *     branches deliver DIFFERENT objects with equal contents.
 * It is therefore a subproblem of all three and is built before any of them.
 *
 * IT IS THE ENGINE'S OWN SERIALIZER, NOT A SECOND ONE. quickjs writes and reads an object graph already —
 * JS_WriteObject/JS_ReadObject, with JS_WRITE_OBJ_REFERENCE for the arbitrary graph that §2.7's `memory` map
 * exists to allow. Hand-rolling a traversal here would be a second implementation of the same thing, and the
 * first would keep being the one the engine actually uses everywhere else. What this file owns is the WEB's
 * half: which values are refused, and with which exception.
 *
 * BYTECODE IS NOT ALLOWED THROUGH, and that is the point rather than a limitation. §2.7 throws a
 * "DataCloneError" DOMException for a callable, and JS_WRITE_OBJ_BYTECODE is exactly the flag that would let a
 * function across; leaving it off makes the engine's writer fail on the same values the standard refuses. The
 * refusal is then RE-REPORTED as the DOMException the standard names, because a page catches it by `.name`.
 *
 * TRANSFERRING IS HERE TOO, AND IT IS THE SAME WRITER AGAIN. §2.7.7's ArrayBuffer arm has to move
 * [[ArrayBufferData]], [[ArrayBufferByteLength]] and — for a resizable one — [[ArrayBufferMaxByteLength]] into
 * a realm-independent holder, and the engine's own writer already encodes exactly those three (BC_TAG_ARRAY_BUFFER
 * carries byte_length AND max_byte_length, and the reader hands them both back to the constructor). So the
 * ArrayBuffer transfer steps are `serialize, then JS_DetachArrayBuffer`, and the receiving steps are
 * `deserialize` — no second encoding of a buffer, and the resizable arm is right without this file knowing what
 * resizable means. A DETACHED buffer is refused by the writer, which IS §2.7.7 step 4's IsDetachedBuffer check,
 * re-reported as the DataCloneError the standard names. A SharedArrayBuffer is a different class, so it is not
 * transferable here at all — also a DataCloneError, which is what §2.7.7's IsSharedArrayBuffer test gives it. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/structured_clone.h"

int structured_serialize(JSContext *ctx, JSValueConst v, StructuredData *out)
{
    out->buf = NULL;
    out->len = 0;
    /* §2.7.1 STEP 1: a Symbol is a "DataCloneError" DOMException, and it is checked HERE because the engine's
       writer has no arm that refuses one — it encodes the symbol and the clone silently succeeded, which is a
       value the standard says cannot cross a port. A primitive the writer handles correctly needs no such
       check; this is the one it does not. */
    if (JS_IsSymbol(v)) {
        JS_ThrowDOMException(ctx, "DataCloneError", "a Symbol cannot be cloned");
        return -1;
    }
    /* §2.7.1 StructuredSerialize, then §2.7.2 StructuredDeserialize into THIS realm — which is the whole of
       §2.7.3's structuredClone(), and the whole of what a same-agent port delivery needs.
       JS_WRITE_OBJ_REFERENCE is `memory`: a value reached twice comes back as the SAME object on the other
       side, and a cycle terminates instead of recursing. Without it `const a = {}; a.self = a` is a hang. */
    out->buf = JS_WriteObject(ctx, &out->len, v, JS_WRITE_OBJ_REFERENCE);
    if (!out->buf) {
        /* THE ENGINE'S REFUSAL IS THE STANDARD'S, RE-REPORTED. The writer throws its own error for a value it
           cannot encode — a function, a Proxy, a Promise — and every one of those is a value §2.7 refuses with
           a "DataCloneError" DOMException. A page catches this by `.name`, so reporting the writer's own
           TypeError instead would be caught by nothing the standard describes. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowDOMException(ctx, "DataCloneError", "the value could not be cloned");
        return -1;
    }
    return 0;
}

void structured_data_free(JSContext *ctx, StructuredData *d)
{
    js_free(ctx, d->buf);
    d->buf = NULL;
    d->len = 0;
}

JSValue structured_deserialize(JSContext *ctx, const StructuredData *in)
{
    JSValue out;
    DCHECK(in->buf != NULL, "a serialized record was deserialized after it had been freed");
    out = JS_ReadObject(ctx, in->buf, in->len, JS_READ_OBJ_REFERENCE);
    if (JS_IsException(out)) {
        /* A GRAPH THE WRITER PRODUCED AND THE READER REFUSED is not a page error — it is these two halves
           disagreeing, which is a should-never-happen and is worth a crash rather than a DOMException that
           sends the next reader looking at the page's value. */
        DFAIL("the engine serialized a value it could not then deserialize — the writer and the reader "
              "disagree about the format, which no page input can cause");
    }
    return out;
}

JSValue structured_clone(JSContext *ctx, JSValueConst v)
{
    StructuredData d;
    JSValue out;

    if (structured_serialize(ctx, v, &d) < 0)
        return JS_EXCEPTION;
    out = structured_deserialize(ctx, &d);
    structured_data_free(ctx, &d);
    return out;
}

/* ---- §2.7.2's transferables ---------------------------------------------------------------------------------
 *
 * A DATA HOLDER IS A TWO-ELEMENT ARRAY: §2.7.7's [[Type]] and the registering component's own record. The type
 * is what makes a SECOND transferable possible at all — §2.7.8's first act is "let interfaceName be
 * transferDataHolder.[[Type]]", and a holder that named nothing forced the deserializer to guess, which is why
 * there could only ever have been one. */

enum { TH_TYPE = 0, TH_DATA, TH_N };

/* THE JS-SPEC ARM, FIRST, because §2.7.7 asks it first: an [[ArrayBufferData]] slot decides the ArrayBuffer
   branch and everything else is "a platform object that is a transferable object". SharedArrayBuffer is a
   different class here, so JS_IsArrayBuffer is false for one and it falls out of the registry as
   non-transferable — the DataCloneError §2.7.7's IsSharedArrayBuffer test also gives it. */
static bool ab_is(JSValueConst v)
{
    return JS_IsArrayBuffer(v);
}

static JSValue ab_out(JSContext *ctx, JSValueConst v)
{
    StructuredData d;
    JSValue h;

    /* §2.7.7 step 4.1: a DETACHED buffer is a "DataCloneError" DOMException — and the writer refusing one IS
       that check, re-reported by structured_serialize as the exception the standard names. */
    if (structured_serialize(ctx, v, &d) < 0)
        return JS_EXCEPTION;
    h = JS_NewArrayBufferCopy(ctx, d.buf, d.len);
    structured_data_free(ctx, &d);
    if (JS_IsException(h))
        return h;
    /* §2.7.7 step 4.4's DetachArrayBuffer, AFTER the holder exists: a page that catches an allocation failure
       here still has its buffer. */
    JS_DetachArrayBuffer(ctx, v);
    return h;
}

static JSValue ab_in(JSContext *ctx, JSValueConst holder)
{
    StructuredData d;
    size_t n = 0;

    d.buf = JS_GetArrayBuffer(ctx, &n, holder);
    d.len = n;
    DCHECK(d.buf != NULL, "an ArrayBuffer transfer holder was not the byte record its transfer steps wrote");
    return structured_deserialize(ctx, &d);
}

static const StructuredTransferable AB_TRANSFERABLE = { "ArrayBuffer", ab_is, ab_out, ab_in };

/* The platform's transferable INTERFACES, beside the JS-spec arm above. An array rather than one slot because
   MessagePort, ImageBitmap and ReadableStream are all transferable and each registers its own; the count is
   small and the lookup is a walk, which is what a registry of a handful of kinds should be. */
#define SC_MAX_TRANSFERABLE 8
static const StructuredTransferable *g_transferable[SC_MAX_TRANSFERABLE];
static int g_transferable_n;

void structured_register_transferable(const StructuredTransferable *t)
{
    int i;
    DCHECK(t != NULL && t->type && t->is && t->out && t->in,
           "a transferable was registered without its [[Type]] and all three of its steps");
    for (i = 0; i < g_transferable_n; i++) {
        if (g_transferable[i] == t) return;   /* one document, one registration */
        DCHECK(strcmp(g_transferable[i]->type, t->type) != 0,
               "two transferable interfaces registered under one [[Type]] — §2.7.8 chooses the receiving steps "
               "by that name, so a holder would be received by whichever of them the walk reached first");
    }
    CHECK(g_transferable_n < SC_MAX_TRANSFERABLE, "more transferable interfaces than this registry holds");
    g_transferable[g_transferable_n++] = t;
}

static const StructuredTransferable *transferable_of(JSValueConst v)
{
    int i;
    if (AB_TRANSFERABLE.is(v)) return &AB_TRANSFERABLE;
    for (i = 0; i < g_transferable_n; i++)
        if (g_transferable[i]->is(v)) return g_transferable[i];
    return NULL;
}

/* §2.7.8 step 2.4: the interface named by a holder's [[Type]], or NULL — which the standard turns into a
   "DataCloneError" for an interface that is not exposed in the target realm. Every realm of this agent runs the
   same intrinsic list, so registry membership IS that question here. */
static const StructuredTransferable *transferable_named(const char *type)
{
    int i;
    if (!strcmp(AB_TRANSFERABLE.type, type)) return &AB_TRANSFERABLE;
    for (i = 0; i < g_transferable_n; i++)
        if (!strcmp(g_transferable[i]->type, type)) return g_transferable[i];
    return NULL;
}

uint32_t structured_transfer_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len;
    uint32_t n = 0;
    int r;

    if (JS_IsUndefined(arr)) return 0;
    DCHECK(JS_IsArray(arr), "a transfer list or holder list was read that this file did not build — everything "
                            "past structured_transfer_list is the engine's own array, and a page's object here "
                            "would run its getters where no stage names the read");
    len = JS_GetPropertyStr(ctx, arr, "length");
    DCHECK(!JS_IsException(len), "reading the length of an engine-built array threw");
    r = JS_ToUint32(ctx, &n, len);
    DCHECK(r >= 0, "an engine-built array's length was not a number");
    (void)r;
    JS_FreeValue(ctx, len);
    return n;
}

int structured_transfer_list(JSContext *ctx, JSValueConst list, JSValue *out)
{
    uint32_t n = 0, i;
    JSValue arr;

    *out = JS_UNDEFINED;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) return -1;
    if (JS_IsUndefined(list) || JS_IsNull(list)) { *out = arr; return 0; }
    if (!JS_IsObject(list)) {
        JS_FreeValue(ctx, arr);
        JS_ThrowTypeError(ctx, "the transfer list is not a sequence");
        return -1;
    }
    {
        JSValue len = JS_GetPropertyStr(ctx, list, "length");
        if (JS_IsException(len)) { JS_FreeValue(ctx, arr); return -1; }
        if (JS_ToUint32(ctx, &n, len) < 0) { JS_FreeValue(ctx, len); JS_FreeValue(ctx, arr); return -1; }
        JS_FreeValue(ctx, len);
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        if (JS_IsException(e)) { JS_FreeValue(ctx, arr); return -1; }
        /* §3.2.16: the element type is `object`, so a non-object is the conversion's TypeError and never a
           DataCloneError — a page passing `[1]` is a type error about its argument, not a clone failure. */
        if (!JS_IsObject(e)) {
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, arr);
            JS_ThrowTypeError(ctx, "a transfer list entry is not an object");
            return -1;
        }
        JS_SetPropertyUint32(ctx, arr, i, e);
    }
    *out = arr;
    return 0;
}

int structured_serialize_transfer(JSContext *ctx, JSValueConst v, JSValueConst transfer,
                                  StructuredWithTransfer *out)
{
    uint32_t n, i, j;

    out->data.buf = NULL;
    out->data.len = 0;
    out->holders = JS_UNDEFINED;
    DCHECK(JS_IsArray(transfer), "StructuredSerializeWithTransfer was handed a transfer list that is not the "
                                 "materialized sequence — structured_transfer_list is what converts one, and "
                                 "reading the page's object again here would run its getters a second time");
    n = structured_transfer_len(ctx, transfer);

    /* §2.7.7 STEP 2, BEFORE THE MESSAGE IS SERIALIZED: every entry must be transferable and must appear once.
       The standard's own reason is stated in its note — "transferable is not transferred yet as transferring
       has side effects and StructuredSerializeInternal needs to be able to throw first" — so a page that
       catches either of these errors still holds everything it named. */
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, transfer, i);
        if (!transferable_of(e)) {
            JS_FreeValue(ctx, e);
            JS_ThrowDOMException(ctx, "DataCloneError", "the transfer list names a value that is not "
                                                        "transferable");
            return -1;
        }
        for (j = 0; j < i; j++) {
            JSValue o = JS_GetPropertyUint32(ctx, transfer, j);
            bool same = JS_VALUE_GET_PTR(o) == JS_VALUE_GET_PTR(e);
            JS_FreeValue(ctx, o);
            if (same) {
                JS_FreeValue(ctx, e);
                JS_ThrowDOMException(ctx, "DataCloneError", "the transfer list names the same value twice");
                return -1;
            }
        }
        JS_FreeValue(ctx, e);
    }

    /* §2.7.7 STEP 3. */
    if (structured_serialize(ctx, v, &out->data) < 0)
        return -1;

    /* §2.7.7 STEP 5: run each transfer step, in list order, and record the holder with its [[Type]]. Nothing is
       detached until the message itself has serialized, which is what a page retrying after a DataCloneError
       depends on. */
    out->holders = JS_NewArray(ctx);
    if (JS_IsException(out->holders)) { structured_data_free(ctx, &out->data); out->holders = JS_UNDEFINED; return -1; }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, transfer, i), h, rec;
        const StructuredTransferable *t = transferable_of(e);

        DCHECK(t != NULL, "a transfer list entry stopped being transferable between §2.7.7's step 2 and its "
                          "step 5 — the list is the engine's own array and nothing may mutate it between them");
        h = t->out(ctx, e);
        JS_FreeValue(ctx, e);
        if (JS_IsException(h)) goto fail;
        rec = JS_NewArray(ctx);
        if (JS_IsException(rec)) { JS_FreeValue(ctx, h); goto fail; }
        JS_SetPropertyUint32(ctx, rec, TH_TYPE, JS_NewString(ctx, t->type));
        JS_SetPropertyUint32(ctx, rec, TH_DATA, h);
        JS_SetPropertyUint32(ctx, out->holders, i, rec);
    }
    return 0;
 fail:
    structured_data_free(ctx, &out->data);
    JS_FreeValue(ctx, out->holders);
    out->holders = JS_UNDEFINED;
    return -1;
}

JSValue structured_deserialize_transfer(JSContext *ctx, const StructuredWithTransfer *in, JSValue *pvalues)
{
    JSValue values = JS_NewArray(ctx);
    uint32_t n, i;

    *pvalues = JS_UNDEFINED;
    if (JS_IsException(values)) return JS_EXCEPTION;
    n = structured_transfer_len(ctx, in->holders);
    /* §2.7.8 STEP 2: the transfer-receiving steps run BEFORE the message is deserialized, because the standard's
       `memory` is seeded with their results. This engine cannot seed it — see structured_clone.h — but the
       ORDER is kept, so what a receiving step does to the realm has happened by the time the message arrives. */
    for (i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, in->holders, i), h, nv;
        const StructuredTransferable *t;
        const char *type;

        DCHECK(!JS_IsException(rec), "reading an engine-built holder record threw");
        {
            JSValue tv = JS_GetPropertyUint32(ctx, rec, TH_TYPE);
            type = JS_ToCString(ctx, tv);
            JS_FreeValue(ctx, tv);
        }
        DCHECK(type != NULL, "a transfer data holder carried no [[Type]] — §2.7.8 chooses the receiving steps "
                             "by it, so a holder without one names no interface at all");
        t = transferable_named(type);
        if (!t) {
            /* §2.7.8 step 2.4.2: an interface the target realm does not expose is a "DataCloneError". It is
               reachable from a ROUTED message, whose holders were written by another instance. */
            JS_ThrowDOMException(ctx, "DataCloneError",
                                 "the message transfers an object of an interface this realm does not expose");
            JS_FreeCString(ctx, type);
            JS_FreeValue(ctx, rec);
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, type);
        h = JS_GetPropertyUint32(ctx, rec, TH_DATA);
        JS_FreeValue(ctx, rec);
        nv = t->in(ctx, h);
        JS_FreeValue(ctx, h);
        if (JS_IsException(nv)) { JS_FreeValue(ctx, values); return JS_EXCEPTION; }
        JS_SetPropertyUint32(ctx, values, i, nv);
    }
    *pvalues = values;
    return structured_deserialize(ctx, &in->data);
}

void structured_with_transfer_free(JSContext *ctx, StructuredWithTransfer *d)
{
    structured_data_free(ctx, &d->data);
    JS_FreeValue(ctx, d->holders);
    d->holders = JS_UNDEFINED;
}

/* §2.7.10's `structuredClone(value, optional StructuredSerializeOptions options = {})` — three steps, and it is
   the ONE caller of this pair whose target realm is known at the call, so both halves run here. A transferable
   in `transfer` is MOVED: the source is detached and the returned copy holds what it had. */
static JSValue js_structured_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    StructuredWithTransfer swt;
    JSValue raw = JS_UNDEFINED, transfer, out, values;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "structuredClone requires a value");
    if (argc > 1 && JS_IsObject(argv[1])) {
        raw = JS_GetPropertyStr(ctx, argv[1], "transfer");
        if (JS_IsException(raw))
            return JS_EXCEPTION;
    }
    if (structured_transfer_list(ctx, raw, &transfer) < 0) { JS_FreeValue(ctx, raw); return JS_EXCEPTION; }
    JS_FreeValue(ctx, raw);
    /* §2.7.10 step 1. */
    if (structured_serialize_transfer(ctx, argv[0], transfer, &swt) < 0) {
        JS_FreeValue(ctx, transfer);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, transfer);
    /* §2.7.10 step 2, into THIS realm — `this`'s relevant realm is the one the method was reached through. */
    out = structured_deserialize_transfer(ctx, &swt, &values);
    structured_with_transfer_free(ctx, &swt);
    /* §2.7.10 step 3 returns [[Deserialized]] alone, so [[TransferredValues]] is dropped — which under the
       standard's seeded `memory` costs nothing, because a transferred object reached from the body IS one of
       these. Without that seam it is instead an extra received object nobody holds; the page still sees the
       right bytes, because the body serialized before the transfer steps detached anything. */
    JS_FreeValue(ctx, values);
    return out;
}

void structured_clone_install(JSContext *ctx, JSValueConst global)
{
    JS_SetPropertyStr(ctx, (JSValue)global, "structuredClone",
                      JS_NewCFunction(ctx, js_structured_clone, "structuredClone", 1));
}
