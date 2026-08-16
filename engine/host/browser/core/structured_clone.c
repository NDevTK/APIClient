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
#include "core/idl_args.h"
#include "core/structured_clone.h"
#include "solver/concolic.h"

/* ---- §2.7.1's `memory`, AND THE TWO THINGS THAT SEED IT ------------------------------------------------------
 *
 * §2.7.1 step 2 is "if memory[value] exists, then return memory[value]", and the standard SEEDS that map before
 * the walk begins: §2.7.7 step 1 puts every entry of the transfer list in it, which is what makes a transferable
 * reached from INSIDE the message body write its holder's index instead of being cloned or refused. §2.7.8's
 * mirror is seeded with [[TransferredValues]], so the reference comes back as the MOVED object.
 *
 * A CONCOLIC SEEDS THE SAME MAP WITH ITSELF, and that is this mechanism rather than a second one. §Solver: a
 * concolic is unknown EXTERNAL INPUT — a triple of (source identity, constraint domain, example) that STANDS FOR
 * a primitive, which is why every operator carries a hook for it and why §2.7.3's FIRST arm ("If value is
 * undefined, null, a Boolean, a Number, a BigInt, or a String, then return { [[Type]]: primitive ... }") is the
 * arm it belongs in. A primitive is its own copy. So what comes back is THE SAME concolic and never a rebuilt
 * one: its source identity is what correlates every constraint the flow narrowed it with, and a deep copy would
 * mint a SECOND symbol about which that narrowing says nothing — a WRONG answer, not a lossy one.
 *
 * IT IS THE SAME-TURN CLONE THAT CARRIES IT, AND THE SPLIT PAIR THAT CANNOT. `structured_clone` deserializes in
 * the same turn and in this heap, so "the value itself" names something on both ends of the round trip. The
 * split pair hands BYTES to a later turn — a queued port delivery, a history entry, a broadcast, a message
 * ROUTED to another instance — and §Security says a live JSValue crosses neither a park, a session nor an
 * instance. That is not a rule this file invents for the occasion: window_message_send_remote already ABORTS on
 * a routed message that carries data HOLDERS, for exactly this reason. So the write hook says the same thing at
 * the same place for a concolic, naming what the arm has to be. */
typedef struct {
    JSValueConst transfer;   /* §2.7.7's materialized transfer list, or JS_UNDEFINED for a write with none */
    uint32_t     n;          /* its length — the first n indices of the ONE numbering the reader resolves */
    JSValue      leaves;     /* the values that are their own copy, in the order reached; JS_UNDEFINED on the
                                path whose bytes outlive the turn */
} SCWriteMemory;

typedef struct {
    JSValueConst values;     /* §2.7.8's [[TransferredValues]] — the first n of that same numbering */
    uint32_t     n;
    JSValueConst leaves;     /* the array the write filled, in the order it filled it */
} SCReadMemory;

/* §2.7.7 STEP 1's map, ANSWERED. The standard's is keyed by the transferable itself and its value is that
   transferable's data holder; the holders are appended in list order at step 5, so the INDEX in the list names
   the holder without the holder having to exist yet — which it does not, because step 3 serializes the body
   first. Identity is the object pointer, which is what a map keyed by the value means, and is the same
   comparison step 2's duplicate check already makes. */
static int sc_memory_index(JSContext *ctx, void *opaque, JSValueConst obj)
{
    SCWriteMemory *m = opaque;
    uint32_t i, ln;

    for (i = 0; i < m->n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, m->transfer, i);
        bool same;

        DCHECK(!JS_IsException(e), "reading an entry of the materialized transfer list threw — it is the "
                                   "engine's own array and has no getters to run");
        same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(obj);
        JS_FreeValue(ctx, e);
        if (same) return (int)i;
    }
    if (!concolic_is(obj)) return -1;
    if (JS_IsUndefined(m->leaves)) {
        DFAIL("HTML §2.7: a CONCOLIC reached the serializer on the path that hands BYTES to a LATER turn — a "
              "queued port or window delivery, a history entry, a broadcast. A live value crosses neither a "
              "park, a session nor an instance, so this arm is the triple written AS DATA (the source identity, "
              "the display shape and the example) rebuilt by concolic_new where the bytes are read, riding the "
              "same record the holders ride and refused at the routing edge the way a holder already is. The "
              "SAME-TURN clone answers it by seeding `memory` with the value itself. Until this exists, a page "
              "posting `location.hash` is refused where a real browser delivers a string");
        return -1;
    }
    ln = structured_transfer_len(ctx, m->leaves);
    for (i = 0; i < ln; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, m->leaves, i);
        bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(obj);

        JS_FreeValue(ctx, e);
        if (same) return (int)(m->n + i);
    }
    /* A DEFINE and not an assignment: [[Set]] consults the prototype chain, and an index accessor a page put
       on Array.prototype would swallow the entry — after which the read below would resolve this index to
       nothing and the value would come back as something other than what went in. */
    JS_DefinePropertyValueUint32(ctx, m->leaves, ln, JS_DupValue(ctx, obj), JS_PROP_C_W_E);
    return (int)(m->n + ln);
}

/* §2.7.8's map, ANSWERED — the object the transfer-receiving steps already built for that holder, or the leaf
   the write interned. It is THE SAME object in both halves, never a copy: a moved port reached from the message
   body and the same port in the event's `ports` are one object and a page compares them, and a concolic reached
   twice is one symbol or the constraints narrowed on it name nothing. */
static JSValue sc_memory_value(JSContext *ctx, void *opaque, uint32_t index)
{
    const SCReadMemory *m = opaque;
    JSValue v;

    if (index < m->n) {
        v = JS_GetPropertyUint32(ctx, m->values, index);
        DCHECK(JS_IsObject(v), "a transfer reference resolved to something that is not a received object — the "
                               "writer's index into the transfer list and this list of received values are the "
                               "ONE numbering §2.7.7 and §2.7.8 build in step order, so a hole here means the "
                               "two halves stopped agreeing about what that numbering counts");
        return v;
    }
    v = JS_GetPropertyUint32(ctx, m->leaves, index - m->n);
    DCHECK(concolic_is(v), "a reference past the transfer list resolved to something that is not a value that "
                           "is its own copy — the leaves are appended by the write in the order it reached "
                           "them and read back by that same index, so this is those two halves disagreeing");
    return v;
}

/* THE ONE SERIALIZATION, under whatever seeded `memory` its caller has. There is no unseeded form: the map is
   what answers for an object the writer would otherwise refuse, so a path that seeds nothing still supplies the
   map that says so. */
static int sc_serialize(JSContext *ctx, JSValueConst v, StructuredData *out, SCWriteMemory *memory)
{
    JSTransferWriteHook hook;

    hook.index_of = sc_memory_index;
    hook.opaque = memory;
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
    out->buf = JS_WriteObject3(ctx, &out->len, v, JS_WRITE_OBJ_REFERENCE, NULL, &hook);
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

int structured_serialize(JSContext *ctx, JSValueConst v, StructuredData *out)
{
    /* NO TRANSFER LIST AND NO LEAVES. These bytes are the half of §2.7 that is handed to a LATER turn — a
       history entry, a broadcast, the byte record an ArrayBuffer's transfer steps build — so nothing live may
       be named from them, and the map above is what states that rather than a silence. */
    SCWriteMemory memory = { JS_UNDEFINED, 0, JS_UNDEFINED };

    return sc_serialize(ctx, v, out, &memory);
}

void structured_data_free(JSContext *ctx, StructuredData *d)
{
    js_free(ctx, d->buf);
    d->buf = NULL;
    d->len = 0;
}

static JSValue sc_deserialize(JSContext *ctx, const StructuredData *in, const JSTransferReadHook *transfer)
{
    JSValue out;
    DCHECK(in->buf != NULL, "a serialized record was deserialized after it had been freed");
    out = JS_ReadObject3(ctx, in->buf, in->len, JS_READ_OBJ_REFERENCE, NULL, transfer);
    if (JS_IsException(out)) {
        /* A GRAPH THE WRITER PRODUCED AND THE READER REFUSED is not a page error — it is these two halves
           disagreeing, which is a should-never-happen and is worth a crash rather than a DOMException that
           sends the next reader looking at the page's value. */
        DFAIL("the engine serialized a value it could not then deserialize — the writer and the reader "
              "disagree about the format, which no page input can cause");
    }
    return out;
}

JSValue structured_deserialize(JSContext *ctx, const StructuredData *in)
{
    return sc_deserialize(ctx, in, NULL);
}

/* THE SAME-TURN, SAME-HEAP COPY — StructuredDeserialize(StructuredSerialize(v)) with nothing between the two
   halves. That is what lets `memory` be seeded with a value that is ITS OWN COPY: the read happens in this turn
   and in this heap, so the leaf the write interned is still the same live symbol when the read resolves it.
   Its callers are exactly the operations for which that is true — Streams §4.9.7's tee with cloneForBranch2 and
   Indexed Database's §5.11/§6.2, whose store holds the copy as a live value. */
JSValue structured_clone(JSContext *ctx, JSValueConst v)
{
    SCWriteMemory memory = { JS_UNDEFINED, 0, JS_UNDEFINED };
    SCReadMemory back = { JS_UNDEFINED, 0, JS_UNDEFINED };
    JSTransferReadHook hook;
    StructuredData d;
    JSValue out;

    memory.leaves = JS_NewArray(ctx);
    CHECK(!JS_IsException(memory.leaves), "structured clone: the seeded `memory` could not be allocated");
    if (sc_serialize(ctx, v, &d, &memory) < 0) {
        JS_FreeValue(ctx, memory.leaves);
        return JS_EXCEPTION;
    }
    back.leaves = memory.leaves;
    hook.value_at = sc_memory_value;
    hook.count = structured_transfer_len(ctx, memory.leaves);
    hook.opaque = &back;
    out = sc_deserialize(ctx, &d, &hook);
    structured_data_free(ctx, &d);
    JS_FreeValue(ctx, memory.leaves);
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
    /* THE PAGE ACTUALLY PASSED A LIST, so from here down this walks a value of the page's from C. Two things
       are wrong with that and they are the same thing: `list.length` and every `list[i]` below can be an
       accessor or a Proxy trap, which is the page's code in an activation with no flow base — a loop in it
       drives to completion — and length-plus-indices is not §3.2.21 at all, it is the array-like algorithm, so
       an iterable that is not an array converts to nothing here where the standard iterates it.
       IDL_SEQUENCE_OBJECT is the declared type that performs it on the tramp; `structuredClone` takes it, and
       so does window.postMessage — whose §3.6 overload split is IDL_USVSTRING_OR_DICT and whose third position
       is the sequence itself. ONE caller is left, and it is the one whose overload this file cannot yet
       express: MessagePort.postMessage's second argument is `sequence<object> transfer` in one entry and
       `optional StructuredSerializeOptions options = {}` in the other, and BOTH entries are two positions
       long — so §3.6 step 4 removes neither and step 12 decides between them by performing
       GetMethod(V, @@iterator), which is the page's code and therefore a REST POINT before either arm is
       chosen. IterCursor already performs that read as its first phase (IT_GET_ITERFN) but THROWS "not
       iterable" where §3.6 needs the answer reported, and the method it found must be handed to §3.2.21
       rather than read a second time. Give the cursor that probe, declare the row, and this function goes
       with its last caller. */
    DFAIL("§3.2.21's `sequence<object>` was converted from C — MessagePort.postMessage's second argument is "
          "still IDL_ANY. Declare §3.6's `sequence<object>`-or-dictionary split (the arm is chosen by "
          "GetMethod(V, @@iterator), so IterCursor must REPORT an absent @@iterator instead of throwing, and "
          "keep the method it found for the walk), then delete structured_transfer_list");
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

    /* §2.7.7 STEP 3, under the seeded `memory` step 1 built: an entry of the list reached from inside `v` is
       written as its holder's index rather than cloned or refused. Nothing has been transferred yet, so a
       transferable is still whole here — the ArrayBuffer arm below reads its bytes AFTER this. */
    {
        /* NO LEAVES, for the reason the map's own comment gives: this record is DELIVERED in a later task, and
           a routed one is delivered in another instance — window_message_send_remote already aborts rather
           than dropping a holder, and a live leaf is the same fact one step earlier. */
        SCWriteMemory memory = { transfer, n, JS_UNDEFINED };

        if (sc_serialize(ctx, v, &out->data, &memory) < 0)
            return -1;
    }

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
    /* §2.7.8 STEP 2: the transfer-receiving steps run BEFORE the message is deserialized, because `memory` is
       seeded with their results — so this loop is not merely ordered ahead of the body, it is what the body's
       transfer references are resolved against. */
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
    /* §2.7.8's LAST STEP, under the `memory` the loop above filled: a reference the writer left for a holder
       resolves to the value that holder produced, so the message body carries the MOVED object and not a copy
       of it — and reached twice, the one object. */
    {
        SCReadMemory back = { values, n, JS_UNDEFINED };
        JSTransferReadHook hook;

        hook.value_at = sc_memory_value;
        hook.count = n;
        hook.opaque = &back;
        return sc_deserialize(ctx, &in->data, &hook);
    }
}

void structured_with_transfer_free(JSContext *ctx, StructuredWithTransfer *d)
{
    structured_data_free(ctx, &d->data);
    JS_FreeValue(ctx, d->holders);
    d->holders = JS_UNDEFINED;
}

/* §2.7.10's `structuredClone(value, optional StructuredSerializeOptions options = {})` — three steps, and it is
   the ONE caller of this pair whose target realm is known at the call, so both halves run here. A transferable
   in `transfer` is MOVED: the source is detached and the returned copy holds what it had.
   THE ARGUMENTS ARRIVE CONVERTED. `options` is a dictionary and `options.transfer` a `sequence<object>`, so the
   member read and the whole iterator walk are the page's code — read from C they had no flow base under them,
   which is why the declaration performs them and this body sees a plain engine-built object holding an
   engine-built array. §3.6.2 step 1's "value is required" is the declaration's too. */
static JSValue js_structured_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    StructuredWithTransfer swt;
    JSValue transfer, out, values;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "structuredClone ran with no value — §3.6.2 step 1 is the declaration's, and "
                      "idl_optional_from names position 1 as the first optional one");
    /* §2.7.6's `= []`: an options dictionary with no `transfer` member is a transfer list of nothing. That is
       the IDL's own default and not a hole filled at the reader — an absent member of a dictionary IS the empty
       sequence here, which is why the list is built rather than the walk skipped. */
    transfer = argc > 1 ? idl_dict_get(ctx, argv[1], "transfer") : JS_UNDEFINED;
    if (JS_IsUndefined(transfer)) {
        transfer = JS_NewArray(ctx);
        if (JS_IsException(transfer)) return JS_EXCEPTION;
    }
    /* §2.7.10 step 1. */
    if (structured_serialize_transfer(ctx, argv[0], transfer, &swt) < 0) {
        JS_FreeValue(ctx, transfer);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, transfer);
    /* §2.7.10 step 2, into THIS realm — `this`'s relevant realm is the one the method was reached through. */
    out = structured_deserialize_transfer(ctx, &swt, &values);
    structured_with_transfer_free(ctx, &swt);
    /* §2.7.10 step 3 returns [[Deserialized]] alone, so [[TransferredValues]] is dropped — and that costs
       nothing, because `memory` is seeded: a transferred object reached from the body IS one of these and the
       returned value already holds it. `structuredClone(buf, {transfer: [buf]})` therefore returns the MOVED
       buffer rather than a copy beside one, which is the whole reason the standard drops this list. */
    JS_FreeValue(ctx, values);
    return out;
}

/* THE DECLARATION, ONCE PER AGENT. A member has one pool entry and every realm's global carries that same one,
   which is why the id is a file static and the install below only names it. */
static int g_id_clone = -1;

void structured_clone_init(JSContext *ctx)
{
    static const IdlArgType CLONE_ARGS[2] = { IDL_ANY, IDL_DICT };
    /* §2.7.6 `dictionary StructuredSerializeOptions { sequence<object> transfer = []; };` — one member, and it
       is the whole reason this member is declared rather than read from its body. */
    static const IdlDictMember CLONE_OPTS[] = { { "transfer", IDL_SEQUENCE_OBJECT } };

    g_id_clone = idl_method_id_dict(ctx, CLONE_ARGS, 2, CLONE_OPTS,
                                    (int)(sizeof CLONE_OPTS / sizeof CLONE_OPTS[0]),
                                    js_structured_clone, 0);
    idl_optional_from(1);   /* `structuredClone(value, optional StructuredSerializeOptions options = {})` */
}

void structured_clone_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_id_clone >= 0, "structuredClone was installed before structured_clone_init declared it");
    idl_install_method(ctx, global, "structuredClone", 1, g_id_clone);
}
