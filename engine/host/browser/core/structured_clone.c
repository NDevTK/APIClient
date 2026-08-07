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
