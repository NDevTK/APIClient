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
 * WHAT IS NOT HERE. Transferring (`StructuredSerializeWithTransfer`) is absent: it needs a transfer list of
 * transferable objects, and the only transferables this engine could have are ArrayBuffer and MessagePort —
 * the first has no detach primitive exposed and the second is 9.4.2, which is the next thing built. A page
 * passing a non-empty transfer list gets the DataCloneError its members would get anyway, which is what the
 * standard says for an object that is not transferable. */
#include <stdlib.h>

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

/* ---- transferables ---------------------------------------------------------------------------------------- */

/* The platform's transferable interfaces. An array rather than one slot because ArrayBuffer, ImageBitmap and
   ReadableStream are all transferable and each will register its own; the count is small and the lookup is a
   walk, which is what a registry of a handful of kinds should be. */
#define SC_MAX_TRANSFERABLE 8
static const StructuredTransferable *g_transferable[SC_MAX_TRANSFERABLE];
static int g_transferable_n;

void structured_register_transferable(const StructuredTransferable *t)
{
    int i;
    DCHECK(t != NULL && t->is && t->out && t->in, "a transferable was registered without all three of its steps");
    for (i = 0; i < g_transferable_n; i++)
        if (g_transferable[i] == t) return;   /* one document, one registration */
    CHECK(g_transferable_n < SC_MAX_TRANSFERABLE, "more transferable interfaces than this registry holds");
    g_transferable[g_transferable_n++] = t;
}

static const StructuredTransferable *transferable_of(JSValueConst v)
{
    int i;
    for (i = 0; i < g_transferable_n; i++)
        if (g_transferable[i]->is(v)) return g_transferable[i];
    return NULL;
}

/* The transfer list as a length and an accessor. It is a `sequence<object>`, so it is read through `length` and
   indices; the caller has already established it is an object. */
static int transfer_len(JSContext *ctx, JSValueConst list, uint32_t *pn)
{
    JSValue len;
    *pn = 0;
    if (JS_IsUndefined(list) || JS_IsNull(list)) return 1;
    if (!JS_IsObject(list)) { JS_ThrowTypeError(ctx, "the transfer list is not a sequence"); return 0; }
    len = JS_GetPropertyStr(ctx, list, "length");
    if (JS_IsException(len)) return 0;
    if (JS_ToUint32(ctx, pn, len) < 0) { JS_FreeValue(ctx, len); return 0; }
    JS_FreeValue(ctx, len);
    return 1;
}

int structured_serialize_transfer(JSContext *ctx, JSValueConst v, JSValueConst list,
                                  StructuredWithTransfer *out)
{
    uint32_t n = 0, i, j;

    out->data.buf = NULL;
    out->data.len = 0;
    out->holders = JS_UNDEFINED;
    if (!transfer_len(ctx, list, &n))
        return -1;

    /* §2.7.1 STEP 1, BEFORE THE MESSAGE IS SERIALIZED: every entry must be transferable and must appear once.
       The order matters — a page transferring a value that cannot be moved must get that error rather than one
       about the message, and a duplicate must be caught before anything has been detached. */
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        const StructuredTransferable *t;
        if (JS_IsException(e)) return -1;
        t = transferable_of(e);
        if (!t) {
            JS_FreeValue(ctx, e);
            JS_ThrowDOMException(ctx, "DataCloneError", "the transfer list names a value that is not "
                                                        "transferable");
            return -1;
        }
        for (j = 0; j < i; j++) {
            JSValue o = JS_GetPropertyUint32(ctx, list, j);
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

    if (structured_serialize(ctx, v, &out->data) < 0)
        return -1;

    /* §2.7.1 STEP 4: run each transfer step, in list order. Nothing is detached until the message itself has
       serialized, so a message that cannot be cloned leaves the transfer list untouched — which is what a page
       retrying after a DataCloneError depends on. */
    out->holders = JS_NewArray(ctx);
    if (JS_IsException(out->holders)) { structured_data_free(ctx, &out->data); out->holders = JS_UNDEFINED; return -1; }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i), h;
        const StructuredTransferable *t;
        if (JS_IsException(e)) goto fail;
        t = transferable_of(e);
        DCHECK(t != NULL, "a transfer list entry stopped being transferable between the check and the step");
        h = t->out(ctx, e);
        JS_FreeValue(ctx, e);
        if (JS_IsException(h)) goto fail;
        JS_SetPropertyUint32(ctx, out->holders, i, h);
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
    JSValue values = JS_NewArray(ctx), len;
    uint32_t n = 0, i;

    *pvalues = JS_UNDEFINED;
    if (JS_IsException(values)) return JS_EXCEPTION;
    if (!JS_IsUndefined(in->holders)) {
        len = JS_GetPropertyStr(ctx, in->holders, "length");
        if (JS_IsException(len)) { JS_FreeValue(ctx, values); return JS_EXCEPTION; }
        if (JS_ToUint32(ctx, &n, len) < 0) { JS_FreeValue(ctx, len); JS_FreeValue(ctx, values); return JS_EXCEPTION; }
        JS_FreeValue(ctx, len);
    }
    /* §2.7.2 STEP 1: the transfer-receiving steps run BEFORE the message is deserialized, because the standard's
       `memory` is seeded with their results. This engine cannot seed it — see structured_clone.h — but the
       ORDER is kept, so what a receiving step does to the realm has happened by the time the message arrives. */
    for (i = 0; i < n; i++) {
        JSValue h = JS_GetPropertyUint32(ctx, in->holders, i), nv;
        const StructuredTransferable *t;
        if (JS_IsException(h)) { JS_FreeValue(ctx, values); return JS_EXCEPTION; }
        /* WHICH interface a holder belongs to is not recorded on the holder: a holder is the registering
           component's own value, and only that component can read it. With one transferable there is no
           ambiguity; a second one will carry its kind, and this DCHECK is where that becomes required. */
        DCHECK(g_transferable_n <= 1, "a second transferable interface was registered — a data holder must now "
                                      "carry which one it belongs to, because the receiving step is chosen by it");
        t = g_transferable_n ? g_transferable[0] : NULL;
        DCHECK(t != NULL, "a transfer holder was deserialized with no transferable registered");
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

/* §2.7.3's `structuredClone(value, optional StructuredSerializeOptions options = {})`. The options dictionary
   holds only `transfer`, and a non-empty one is refused for the reason the file comment gives. */
static JSValue js_structured_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "structuredClone requires a value");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue transfer = JS_GetPropertyStr(ctx, argv[1], "transfer");
        int64_t n = 0;
        if (JS_IsException(transfer))
            return JS_EXCEPTION;
        if (JS_IsObject(transfer)) {
            JSValue len = JS_GetPropertyStr(ctx, transfer, "length");
            if (JS_IsException(len)) { JS_FreeValue(ctx, transfer); return JS_EXCEPTION; }
            if (JS_ToInt64(ctx, &n, len) < 0) { JS_FreeValue(ctx, len); JS_FreeValue(ctx, transfer); return JS_EXCEPTION; }
            JS_FreeValue(ctx, len);
        }
        JS_FreeValue(ctx, transfer);
        if (n != 0)
            return JS_ThrowDOMException(ctx, "DataCloneError",
                                        "this engine has no transferable objects, so a transfer list can only "
                                        "name values that are not transferable");
    }
    return structured_clone(ctx, argv[0]);
}

void structured_clone_install(JSContext *ctx, JSValueConst global)
{
    JS_SetPropertyStr(ctx, (JSValue)global, "structuredClone",
                      JS_NewCFunction(ctx, js_structured_clone, "structuredClone", 1));
}
