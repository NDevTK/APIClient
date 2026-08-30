/* FileSystemWritableFileStream — File System Standard §2.5.
 *
 * IT IS A WritableStream, BUILT ON THE ONE THIS ENGINE HAS. §2.5 says so in the IDL — `interface
 * FileSystemWritableFileStream : WritableStream` — and in prose: "a WritableStream object with additional
 * convenience methods, which operates on a single file on disk ... getWriter() returns an instance of
 * WritableStreamDefaultWriter". So this is Streams §9.2.1 Creation and manipulation's `set up` — the operation
 * §2.5's create a new FileSystemWritableFileStream reaches at its step 8, and NOT §5.5.1's CreateWritableStream,
 * which takes a start algorithm this one does not have — with §2.5's three algorithms, wearing a prototype
 * chained to WritableStream.prototype, and NOT a second stream implementation beside core/streams/.
 * The three convenience methods are exactly what §2.5.1-3 say they are: get a writer, write one chunk through
 * it, release the writer — every one of which is an abstract operation core/streams/writable_stream.h already
 * exposes as this realm's own function object, so a page that rebinds
 * WritableStreamDefaultWriter.prototype.write does not thereby change what `stream.write()` does.
 *
 * THE THREE INTERNAL SLOTS ARE STATE A FLOW WRITES, so they are an internal-slot record (core/idl_slots.h) hung
 * off the stream, not a C struct: [[buffer]] grows on every write and [[seekOffset]] moves, and two forked flows
 * writing the same stream must not share either. A record's field is an ordinary property write, which is what
 * the per-flow COW delta captures and what the snapshot machinery parks to the cold tier.
 *
 * THE THREE ALGORITHMS RETURN NOTHING AND THROW INSTEAD OF REJECTING, which is not a shortcut: Streams §9.2.1
 * Creation and manipulation's `set up` wraps the close and abort algorithms in a step whose whole text is "If
 * this throws an exception e, return a promise rejected with e", and §2.5's own write a chunk opens with the
 * same sentence for its conversion — so a throw is caught and becomes the rejection of exactly the promise
 * §2.5 names. (The catch this engine performs is at the controller, which is why every stage may throw and not
 * only the first.) "Reject p with a TypeError" and "throw a TypeError" are the same observable through
 * that seam, and going the long way round would mean building a capability and settling it — a second run of
 * the page's code where the spec has none. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/frame/secure_context.h"
#include "core/encoding/encoding.h"
#include "core/file/blob.h"
#include "core/file/file_system.h"
#include "core/file/file_system_writable.h"
#include "core/streams/stream_work.h"
#include "core/streams/writable_stream.h"
#include "solver/concolic.h"

/* §2.5's THREE INTERNAL SLOTS, on the record the stream carries. */
#define FW_FILE   "file"    /* [[file]] — the file entry */
#define FW_BUFFER "buffer"  /* [[buffer]] — a byte sequence, "initially empty" */
#define FW_SEEK   "seek"    /* [[seekOffset]] — a number, "initially 0" */

/* §2.5's WriteCommandType. */
#define FW_CMD_WRITE    "write"
#define FW_CMD_SEEK     "seek"
#define FW_CMD_TRUNCATE "truncate"

static JSClassID g_fw_class;          /* the per-realm PROTOTYPE slot; the object itself is a WritableStream */
static JSAtom    g_slot_key = JS_ATOM_NULL;   /* where the record hangs off the stream */
static JSAtom    g_atom_type = JS_ATOM_NULL, g_atom_data = JS_ATOM_NULL;
static JSAtom    g_atom_position = JS_ATOM_NULL, g_atom_size = JS_ATOM_NULL;
static int       g_stepid_write = -1, g_stepid_close = -1, g_stepid_abort = -1;
static int       g_id_write = -1, g_id_seek = -1, g_id_truncate = -1;

/* ---- the record --------------------------------------------------------------------------------------- */

/* THE RECORD A FileSystemWritableFileStream CARRIES, or JS_UNDEFINED for anything else — which is this
   interface's BRAND. Web IDL §3.7.7 Operations' brand check — create an operation function's "If jsValue does
   not implement the interface target, throw a TypeError" — cannot be a class-id comparison here: the object IS
   a WritableStream and wears that class, exactly as File API §4 The File Interface's File wears the Blob class,
   so what tells them apart is what the interface gave it. OWNED. */
static JSValue fw_record(JSContext *ctx, JSValueConst stream)
{
    JSValue rec = JS_UNDEFINED;

    DCHECK(g_slot_key != JS_ATOM_NULL, "a FileSystemWritableFileStream's record was reached before "
                                       "fs_writable_init interned its slot key");
    if (JS_GetOwnSlot(ctx, &rec, stream, g_slot_key) > 0) return rec;
    return JS_UNDEFINED;
}

static JSValue fw_slot(JSContext *ctx, JSValueConst rec, const char *field)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, field);

    CHECK(!JS_IsException(v), "file system writable: an internal slot could not be read");
    return v;
}

static double fw_seek_offset(JSContext *ctx, JSValueConst rec)
{
    JSValue v = fw_slot(ctx, rec, FW_SEEK);
    double d = 0;

    DCHECK(JS_IsNumber(v), "§2.5's [[seekOffset]] is not a number — it is \"a number, initially 0\" and every "
                           "writer here sets one");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

/* ---- §2.5's byte-sequence arithmetic --------------------------------------------------------------------- */

/* THE TAINT RIDES THE RESULT. §2.5's buffer edit is a concatenation of real bytes, and this engine performs the
   REAL concatenation — the example is what the file will actually contain. What must not be lost on the way is
   the FACT that some of those bytes came from outside: a page that reads an attacker's file and writes it back
   through a stream has produced a file whose contents are still the attacker's. So where either operand carries
   a source identity, the result carries it too, with the incoming data preferred because it is the newer
   provenance. This is the example propagating through a real operation, never a derived transform-expression:
   the bytes below are the concatenation itself. `computed` is CONSUMED. */
static JSValue fw_keep_taint(JSContext *ctx, JSValue computed, JSValueConst a, JSValueConst b)
{
    JSValueConst src = concolic_is(a) ? a : (concolic_is(b) ? b : JS_UNDEFINED);

    if (!concolic_is(src)) return computed;
    return concolic_source_wrap(ctx, concolic_shape_c(src), concolic_src_c(src), computed);
}

/* §2.5's write command, over the buffer: pad to `pos` with NUL, splice `data` in, keep the tail. The old
   [[buffer]] is replaced. 0 with `*pnext` the new [[seekOffset]], or -1 with a throw live. */
static int fw_splice(JSContext *ctx, JSValueConst rec, double pos, const char *data, size_t dlen,
                     JSValueConst data_value, double *pnext)
{
    JSValue old, fresh;
    size_t oldlen = 0;
    char *oldbytes;
    size_t head, total, tail = 0;
    char *out;

    DCHECK(pos >= 0, "§2.5's write command was given a negative write position — the IDL types `position` as an "
                     "`unsigned long long`, so a negative one is a conversion that did not happen");
    /* THE QUOTA IS ASKED BEFORE THE CAST, not after. `write({position: 2**64-1})` is a real thing a page can
       write, and converting that double to a size_t is undefined behaviour rather than a large number — so the
       size the algorithm asks for is compared against what this address space can name, and a request past it
       takes §2.5's own QuotaExceededError. */
    if (!(pos <= (double)(SIZE_MAX - dlen - 1))) {
        JS_ThrowDOMException(ctx, "QuotaExceededError",
                             "writing at that position needs more storage than this agent can address");
        return -1;
    }
    old = fw_slot(ctx, rec, FW_BUFFER);
    oldbytes = file_system_value_bytes(ctx, old, &oldlen);
    head = (size_t)pos;
    /* "If writePosition is larger than oldSize, append writePosition - oldSize 0x00 (NUL) bytes to the end of
       stream's [[buffer]]" — the padding and the splice are one allocation here, which is the same byte
       sequence the two steps produce. */
    if (head < oldlen) tail = oldlen - head;
    if (tail > dlen) tail -= dlen; else tail = 0;
    total = head + dlen + tail;
    /* §2.5: "If the operations modifying stream's [[buffer]] in the previous steps failed due to EXCEEDING THE
       STORAGE QUOTA, reject p with a QuotaExceededError and abort these steps, leaving stream's [[buffer]]
       unmodified." This engine's storage is RAM, so the quota is what it can hold — and `write({type:"write",
       position:2**64-1, data:"x"})` is a page reaching it deliberately, which is the case the standard names.
       The buffer is LEFT UNMODIFIED, which the ordering here gives for free: nothing has been written yet. */
    out = malloc(total + 1);
    if (!out) {
        free(oldbytes);
        JS_FreeValue(ctx, old);
        JS_ThrowDOMException(ctx, "QuotaExceededError",
                             "writing at that position needs more storage than this agent has");
        return -1;
    }
    if (head <= oldlen) {
        memcpy(out, oldbytes, head);
    } else {
        memcpy(out, oldbytes, oldlen);
        memset(out + oldlen, 0, head - oldlen);
    }
    if (dlen) memcpy(out + head, data, dlen);
    if (tail) memcpy(out + head + dlen, oldbytes + (oldlen - tail), tail);
    out[total] = 0;
    fresh = fw_keep_taint(ctx, file_system_bytes_value(ctx, out, total), data_value, old);
    JS_SetPropertyStr(ctx, rec, FW_BUFFER, fresh);
    free(out);
    free(oldbytes);
    JS_FreeValue(ctx, old);
    *pnext = pos + (double)dlen;   /* "set stream's [[seekOffset]] to writePosition + data's length" */
    return 0;
}

/* §2.5's truncate command. */
static int fw_truncate(JSContext *ctx, JSValueConst rec, double newsize)
{
    JSValue old, fresh;
    size_t oldlen = 0, want;
    char *oldbytes, *out;

    if (!(newsize >= 0 && newsize <= (double)(SIZE_MAX - 1))) {
        JS_ThrowDOMException(ctx, "QuotaExceededError",
                             "that size needs more storage than this agent can address");
        return -1;
    }
    old = fw_slot(ctx, rec, FW_BUFFER);
    oldbytes = file_system_value_bytes(ctx, old, &oldlen);
    want = (size_t)newsize;
    out = malloc(want + 1);

    /* §2.5's truncate command has the SAME quota clause as its write command, and the same "leaving stream's
       [[buffer]] unmodified" — which the ordering gives, since nothing has been written yet. */
    if (!out) {
        free(oldbytes);
        JS_FreeValue(ctx, old);
        JS_ThrowDOMException(ctx, "QuotaExceededError", "that size needs more storage than this agent has");
        return -1;
    }
    if (want <= oldlen) {
        memcpy(out, oldbytes, want);                       /* "the first newSize bytes" */
    } else {
        memcpy(out, oldbytes, oldlen);
        memset(out + oldlen, 0, want - oldlen);            /* "newSize-oldSize 0x00 bytes" */
    }
    out[want] = 0;
    fresh = fw_keep_taint(ctx, file_system_bytes_value(ctx, out, want), old, old);
    JS_SetPropertyStr(ctx, rec, FW_BUFFER, fresh);
    free(out);
    free(oldbytes);
    JS_FreeValue(ctx, old);
    /* "If stream's [[seekOffset]] is bigger than newSize, set stream's [[seekOffset]] to newSize." */
    if (fw_seek_offset(ctx, rec) > newsize)
        JS_SetPropertyStr(ctx, rec, FW_SEEK, JS_NewFloat64(ctx, newsize));
    return 0;
}

/* ---- File System §2.5's WRITE A CHUNK algorithm, as a machine ---------------------------------------------- */

#define FWW_STAGES(X) \
    X(FWW_INPUT,    "File System §2.5 write a chunk step 1 (convert chunk to a FileSystemWriteChunkType — the " \
                    "union's dictionary arm is read one member at a time, and each read is the page's code)") \
    X(FWW_TYPE,     "File System §2.5 write a chunk step 3.2.2 (input[\"type\"], the WriteCommandType this " \
                    "chunk is)") \
    X(FWW_POSITION, "File System §2.5 write a chunk steps 3.2.3.4 and 3.2.4.2-3.2.4.3 (the READ of " \
                    "input[\"position\"], which on a Proxy or an accessor is the page's code)") \
    X(FWW_POS_NUM,  "File System §2.5 write a chunk step 1's conversion of WriteParams' `unsigned long long? " \
                    "position`, deferred to the read above (its valueOf is the page's code — a stage of its " \
                    "own because a machine may hold exactly one sub-sequence in flight per rest point)") \
    X(FWW_SIZE,     "File System §2.5 write a chunk steps 3.2.5.2-3.2.5.3 (the existence check on " \
                    "input[\"size\"] and the READ of it)") \
    X(FWW_SIZE_NUM, "File System §2.5 write a chunk step 1's conversion of WriteParams' `unsigned long long? " \
                    "size`, deferred to the read above") \
    X(FWW_DATA,     "File System §2.5 write a chunk steps 3.2.3.1-3.2.3.2 (the existence check on " \
                    "input[\"data\"] and the READ of it)") \
    X(FWW_STRINGIFY, "File System §2.5 write a chunk step 1's union conversion reaching step 3.2.3.8's " \
                     "USVString arm — ToString on a page object runs its @@toPrimitive, valueOf or toString, " \
                     "which is the page's code") \
    X(FWW_APPLY,    "File System §2.5 write a chunk steps 3.2.3.5-3.2.3.16, 3.2.4.3-3.2.4.4 and " \
                    "3.2.5.3-3.2.5.8 (the buffer edit this command is)")
enum { FWW_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FWW_STEPS[] = { FWW_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   rec;      /* the stream's internal-slot record (owned) */
    JSValue   input;    /* the chunk, once classified (owned) */
    JSValue   data;     /* the union's `data` arm (owned) */
    /* THE VALUE A DICTIONARY READ PRODUCED, held between the read and its CONVERSION — two sub-sequences, so
       two stages, so a value that has to survive the suspension between them. */
    JSValue   num;
    uint8_t   is_dict;  /* the chunk took the WriteParams arm */
    uint8_t   cmd;      /* 0 = write, 1 = seek, 2 = truncate */
    uint8_t   have_pos;
    double    position;
    double    size;
} FwWriteState;

static void fww_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FwWriteState *s = st;

    v->val(ctx, &s->rec);
    v->val(ctx, &s->input);
    v->val(ctx, &s->data);
    v->val(ctx, &s->num);
}

/* THE THREE SHAPES A BufferSource IS, asked exactly as core/idl_args.c's IDL_BUFFERSOURCE position asks them —
   an ArrayBuffer, a typed array or a DataView — because a union arm is one brand test and two spellings of it
   are two things that can come apart. */
static bool fww_is_buffer_source(JSValueConst v)
{
    return JS_IsArrayBuffer(v) || JS_GetTypedArrayType(v) >= 0 || JS_IsDataView(v);
}

/* WHICH ARM OF `(BufferSource or Blob or USVString or WriteParams)` DOES THE CHUNK TAKE? Web IDL §3.2.25 Union
   types reaches the DICTIONARY from exactly two places — step 4.1, for null and undefined, and step 11.4, for
   an Object none of the brand tests claimed — so `write()` with no `type` member throws the TypeError §2.5's
   own prose names. Everything else is a VALUE arm, and that includes every primitive that is not null or
   undefined: a Number falls past steps 12-14 (types holds no numeric type) to step 15's string type, so
   `stream.write(42)` writes the two bytes `42` rather than complaining about a missing `type`.
   THIS QUESTION IS NOT THE ONE FWW_STRINGIFY ASKS, and one predicate answering both is how a Number came to
   take the dictionary here: the brand tests below run none of the page's code, while step 15's conversion runs
   ToString, which is why that arm is a stage of its own and is not decided at this call. */
static bool fww_chunk_is_dictionary(JSValueConst v)
{
    if (blob_is(v) || fww_is_buffer_source(v)) return false;
    return JS_IsObject(v) || JS_IsNull(v) || JS_IsUndefined(v);
}

/* THE BYTES OF THE `data` ARM. §2.5: a BufferSource is copied, a Blob is READ, and a USVString is UTF-8
   ENCODED. Caller frees the returned bytes; `*phold` is a value the caller frees once it is done with them
   (a BufferSource's backing buffer, which core/encoding's extraction hands over that way). */
static char *fww_data_bytes(JSContext *ctx, JSValueConst data, size_t *plen, JSValue *phold)
{
    const uint8_t *p = NULL;
    size_t len = 0;
    char *out;

    *phold = JS_UNDEFINED;
    if (blob_is(data)) {
        const char *bytes = blob_bytes_of(data, &len, NULL);

        p = (const uint8_t *)bytes;
    } else if (JS_IsString(data) || concolic_is(data)) {
        /* "Assert: data is a USVString. Let dataBytes be the result of UTF-8 encoding data." A CONCOLIC takes
           this arm carrying its example, because the union's own conversion has already ToString'd it — the
           bytes written are the real ones and the taint is kept by the caller. */
        JSValue str = concolic_is(data) ? concolic_example(ctx, data) : JS_DupValue(ctx, data);
        const char *utf8 = JS_ToCStringLen(ctx, &len, str);

        JS_FreeValue(ctx, str);
        CHECK(utf8 != NULL, "file system writable: a chunk's string could not be UTF-8 encoded");
        out = malloc(len + 1);
        CHECK(out != NULL, "file system writable: OOM encoding a chunk");
        memcpy(out, utf8, len);
        out[len] = 0;
        JS_FreeCString(ctx, utf8);
        if (plen) *plen = len;
        return out;
    } else {
        /* The only arm left is the BufferSource, because the machine's FWW_STRINGIFY stage has already turned
           everything that is not one into the union's USVString. */
        DCHECK(fww_is_buffer_source(data),
               "File System §2.5's write command reached its BufferSource arm with something that is not one");
        if (encoding_buffer_source(ctx, data, &p, &len, phold) < 0) return NULL;
    }
    out = malloc(len + 1);
    CHECK(out != NULL, "file system writable: OOM copying a chunk");
    if (len) memcpy(out, p, len);
    out[len] = 0;
    if (plen) *plen = len;
    return out;
}

static int fww_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    FwWriteState *s = st;
    JSValue got = JS_UNDEFINED;
    int r;

    if (s->hdr.stage == FWW_INPUT) {
        JSValueConst chunk = step_arg(&s->hdr, 0);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY OWNED FIELD IN PLACE BEFORE THE FIRST THING THAT CAN THROW — the failure path tears this state
           down through the declaration, which frees exactly what the state holds and nothing else. */
        s->rec = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
        s->input = JS_DupValue(ctx, chunk);
        s->data = JS_UNDEFINED;
        s->num = JS_UNDEFINED;
        s->is_dict = 0;
        s->cmd = 0;
        s->have_pos = 0;
        s->position = 0;
        s->size = 0;
        DCHECK(JS_IsObject(s->rec),
               "§2.5's write algorithm ran with no internal slots — the algorithm is a CLOSURE over the record "
               "it was created beside, so a missing one means the stream was built without going through "
               "fs_writable_new_run");
        if (!fww_chunk_is_dictionary(chunk)) {
            s->data = JS_DupValue(ctx, chunk);
            s->hdr.stage = FWW_STRINGIFY;
        } else {
            DCHECK(JS_IsObject(chunk) || JS_IsNull(chunk) || JS_IsUndefined(chunk),
                   "a FileSystemWritableFileStream chunk took Web IDL §3.2.25 Union types' DICTIONARY arm "
                   "without being an Object (step 11.4) or null or undefined (step 4.1) — step 15 sends every "
                   "other primitive to the USVString arm");
            s->is_dict = 1;
            s->hdr.stage = FWW_TYPE;
        }
    }

    if (s->hdr.stage == FWW_TYPE) {
        const char *t;

        r = step_getprop_run(ctx, &s->hdr, s->input, g_atom_type, cb_result, &got, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        /* `required WriteCommandType type` — an absent required member is a TypeError, and so is a value
           outside the enumeration. This is where null, undefined and a plain object all land. */
        if (JS_IsUndefined(got)) {
            JS_FreeValue(ctx, got);
            JS_ThrowTypeError(ctx, "a FileSystemWritableFileStream chunk has no `type` — WriteParams declares "
                                   "it required");
            return JS_STEP_ABRUPT;
        }
        t = JS_ToCString(ctx, got);
        JS_FreeValue(ctx, got);
        if (!t) return JS_STEP_ABRUPT;
        if (!strcmp(t, FW_CMD_WRITE))         s->cmd = 0;
        else if (!strcmp(t, FW_CMD_SEEK))     s->cmd = 1;
        else if (!strcmp(t, FW_CMD_TRUNCATE)) s->cmd = 2;
        else {
            JS_ThrowTypeError(ctx, "`%s` is not a WriteCommandType", t);
            JS_FreeCString(ctx, t);
            return JS_STEP_ABRUPT;
        }
        JS_FreeCString(ctx, t);
        s->hdr.stage = (s->cmd == 2) ? FWW_SIZE : FWW_POSITION;
    }

    if (s->hdr.stage == FWW_POSITION) {
        r = step_getprop_run(ctx, &s->hdr, s->input, g_atom_position, cb_result, &got, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->num = got;
        got = JS_UNDEFINED;
        s->hdr.stage = FWW_POS_NUM;
    }

    if (s->hdr.stage == FWW_POS_NUM) {
        if (!JS_IsUndefined(s->num)) {
            double raw = 0;

            /* WEB IDL's `unsigned long long` IS ToNumber AND THEN THE MODULO, never a saturating integer read:
               the coercion is the part that runs the page's code (a `valueOf` with a loop in it) and the
               arithmetic after it belongs to the type. */
            r = step_todouble_run(ctx, &s->hdr, s->num, cb_result, &raw, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            s->position = idl_unsigned_long_long_of(raw);
            s->have_pos = 1;
        }
        JS_FreeValue(ctx, s->num);
        s->num = JS_UNDEFINED;
        /* "Otherwise, if command is seek: if chunk["position"] does not exist, reject p with a TypeError." */
        if (s->cmd == 1 && !s->have_pos) {
            JS_ThrowTypeError(ctx, "a `seek` chunk has no `position`");
            return JS_STEP_ABRUPT;
        }
        s->hdr.stage = (s->cmd == 1) ? FWW_APPLY : FWW_DATA;
    }

    if (s->hdr.stage == FWW_SIZE) {
        r = step_getprop_run(ctx, &s->hdr, s->input, g_atom_size, cb_result, &got, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        /* "If chunk["size"] does not exist, reject p with a TypeError." */
        if (JS_IsUndefined(got)) {
            JS_ThrowTypeError(ctx, "a `truncate` chunk has no `size`");
            return JS_STEP_ABRUPT;
        }
        s->num = got;
        got = JS_UNDEFINED;
        s->hdr.stage = FWW_SIZE_NUM;
    }

    if (s->hdr.stage == FWW_SIZE_NUM) {
        double raw = 0;

        r = step_todouble_run(ctx, &s->hdr, s->num, cb_result, &raw, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->size = idl_unsigned_long_long_of(raw);
        JS_FreeValue(ctx, s->num);
        s->num = JS_UNDEFINED;
        s->hdr.stage = FWW_APPLY;
    }

    if (s->hdr.stage == FWW_DATA) {
        r = step_getprop_run(ctx, &s->hdr, s->input, g_atom_data, cb_result, &got, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        /* "If input is undefined or input is a dictionary and input["data"] does not exist, reject p with a
           TypeError." A `null` data is NOT absent — the member is `(BufferSource or Blob or USVString)?` — and
           its USVString arm stringifies it, which is what a browser does with `write({type:"write",data:null})`. */
        if (JS_IsUndefined(got)) {
            JS_FreeValue(ctx, got);
            JS_ThrowTypeError(ctx, "a `write` chunk has no `data`");
            return JS_STEP_ABRUPT;
        }
        s->data = got;
        got = JS_UNDEFINED;
        s->hdr.stage = FWW_STRINGIFY;
    }

    if (s->hdr.stage == FWW_STRINGIFY) {
        /* WEB IDL §3.2.25 Union types' STRING ARM, step 15. A `data` that is not a Blob and not a BufferSource
           is a USVString, and converting one is ToString — which on a page OBJECT runs its @@toPrimitive, its
           valueOf or its toString, so it is a request and not a call. Here the member's own union is
           `(BufferSource or Blob or USVString)?`, which has no dictionary arm, so step 11.4 is not on the way
           and every object lands on step 15. `write({type:"write", data:{}})` writes the eleven bytes
           `[object Object]` in a browser, and a page that hands over a Date or a number expects the same
           conversion. THIS STAGE ALSO CONVERTS A BARE CHUNK the union sent to its own step 15 — `write(42)` —
           which is why the question here is asked of the VALUE and never of which arm the chunk took.
           A CONCOLIC is left alone: it is already a string-shaped value carrying its example, and running the
           conversion over it would be a second ToString of a value the interpreter's own hook has answered. */
        if (!blob_is(s->data) && !fww_is_buffer_source(s->data) && !JS_IsString(s->data) &&
            !concolic_is(s->data)) {
            JSValue str = JS_UNDEFINED;

            r = step_tostring_run(ctx, &s->hdr, s->data, cb_result, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            JS_FreeValue(ctx, s->data);
            s->data = str;
        }
        s->hdr.stage = FWW_APPLY;
    }

    DCHECK(s->hdr.stage == FWW_APPLY,
           "File System §2.5's write a chunk resumed into a stage the algorithm does not have");
    JS_FreeValue(ctx, cb_result);
    if (s->cmd == 1) {                                            /* "seek" */
        JS_SetPropertyStr(ctx, s->rec, FW_SEEK, JS_NewFloat64(ctx, s->position));
        return JS_STEP_DONE;
    }
    if (s->cmd == 2) {                                            /* "truncate" */
        return fw_truncate(ctx, s->rec, s->size) < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }
    {                                                             /* "write" */
        JSValue hold = JS_UNDEFINED;
        size_t dlen = 0;
        char *bytes;
        double pos, next = 0;
        bool ok;

        DCHECK(blob_is(s->data) || JS_IsString(s->data) || concolic_is(s->data) ||
               fww_is_buffer_source(s->data),
               "File System §2.5's write command reached a `data` that is none of the union's arms — "
               "FWW_STRINGIFY converts everything that is not a Blob or a BufferSource to the USVString arm, "
               "so a value that is still something else means that stage was skipped");
        bytes = fww_data_bytes(ctx, s->data, &dlen, &hold);
        if (!bytes) return JS_STEP_ABRUPT;
        pos = s->have_pos ? s->position : fw_seek_offset(ctx, s->rec);
        ok = fw_splice(ctx, s->rec, pos, bytes, dlen, s->data, &next) == 0;
        if (ok) JS_SetPropertyStr(ctx, s->rec, FW_SEEK, JS_NewFloat64(ctx, next));
        free(bytes);
        JS_FreeValue(ctx, hold);
        if (!ok) return JS_STEP_ABRUPT;
    }
    return JS_STEP_DONE;
}

/* ---- §2.5's CLOSE and ABORT algorithms -------------------------------------------------------------------- */

#define FWC_STAGES(X) \
    X(FWC_COMMIT, "File System §2.5 create a new FileSystemWritableFileStream step 4.2.2.3 (set stream's " \
                  "[[file]]'s binary data to stream's [[buffer]]) and step 4.2.2.4.1 (release the lock) — the " \
                  "close algorithm is that step 4")
enum { FWC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FWC_STEPS[] = { FWC_STAGES(JS_STEP_STAGE_LABEL) NULL };

#define FWA_STAGES(X) \
    X(FWA_RELEASE, "File System §2.5 create a new FileSystemWritableFileStream step 5.1.1 (release the lock " \
                   "on stream's [[file]]) — the abort algorithm is that step 5")
enum { FWA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FWA_STEPS[] = { FWA_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { JSStepHdr hdr; } FwEndState;

static void fwe_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
/* THE RECORD IS THE CLOSURE'S CAPTURE — §2.5's three algorithms are written as closures over `stream`, and
   what they actually reach through it is its three internal slots. Capturing the RECORD rather than the stream
   is what breaks the only circularity in the creation: Streams §9.2.1 Creation and manipulation's `set up`
   wants the algorithms in order to build the stream, and the algorithms want the stream in order to reach its
   slots. The record exists before either. */
static JSValue fwe_record(JSContext *ctx, const JSStepHdr *h)
{
    JSValue rec = JS_DupValue(ctx, JS_StepClosureData(h, 0));

    DCHECK(JS_IsObject(rec), "§2.5's close or abort algorithm ran with no internal slots — both are closures "
                             "over the record created beside them");
    return rec;
}

static int fwc_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    FwEndState *s = st;
    JSValue rec, file, buffer;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == FWC_COMMIT, "§2.5's close algorithm resumed into a stage it does not have");
    rec = fwe_record(ctx, &s->hdr);
    file = fw_slot(ctx, rec, FW_FILE);
    buffer = fw_slot(ctx, rec, FW_BUFFER);
    /* "Set stream's [[file]]'s binary data to stream's [[buffer]]" — and the modification timestamp with it,
       because §2.3.1 The getFile() method answers `lastModified` from entry's modification timestamp, and a
       commit that left it alone would report a file that had never changed. The standard's note calls this an
       atomic update of the file's contents, which for one property write it is. */
    file_system_set_data(ctx, file, buffer);
    file_system_touch(ctx, file);
    file_system_release_lock(ctx, file);
    JS_FreeValue(ctx, file);
    JS_FreeValue(ctx, rec);
    /* THE MALWARE SCANS AND SAFE BROWSING CHECKS the algorithm names between those two steps are the user
       agent's own; this one runs none, which is a decision and not a gap — a scan that rejected would make the
       engine's model of what a page can write disagree with what the page's real browser writes. */
    return JS_STEP_DONE;
}

static int fwa_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    FwEndState *s = st;
    JSValue rec, file;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == FWA_RELEASE, "§2.5's abort algorithm resumed into a stage it does not have");
    rec = fwe_record(ctx, &s->hdr);
    file = fw_slot(ctx, rec, FW_FILE);
    file_system_release_lock(ctx, file);
    JS_FreeValue(ctx, file);
    JS_FreeValue(ctx, rec);
    return JS_STEP_DONE;
}

static const JSTrampStepDef FWW_DEF = {
    /* Streams §5.5.4 Default controllers' ProcessWrite and ProcessClose, and §5.4.4 Internal methods'
       [[AbortSteps]], read nothing from a sink algorithm but whether it threw — the fulfilment VALUE is never
       looked at — so there is no completion to state */
    sizeof(FwWriteState), fww_step, NULL, 0, .visit = fww_visit,
    .algorithm = "File System §2.5 write a chunk", .steps = FWW_STEPS
};
static const JSTrampStepDef FWC_DEF = {
    sizeof(FwEndState), fwc_step, NULL, 0, .visit = fwe_visit,
    .algorithm = "File System §2.5 the close algorithm of a FileSystemWritableFileStream", .steps = FWC_STEPS
};
static const JSTrampStepDef FWA_DEF = {
    sizeof(FwEndState), fwa_step, NULL, 0, .visit = fwe_visit,
    .algorithm = "File System §2.5 the abort algorithm of a FileSystemWritableFileStream", .steps = FWA_STEPS
};

/* ---- §2.5's CREATE A NEW FileSystemWritableFileStream ------------------------------------------------------ */

JSValue fs_writable_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_fw_class);

    DCHECK(!JS_IsNull(proto), "a FileSystemWritableFileStream was minted in a realm that never ran its install");
    return proto;
}

int fs_writable_new_run(JSContext *ctx, StreamWork *w, JSValueConst entry, bool keep_existing_data,
                        JSValue in, JSValue *pstream, JSValue **out_cb, int *out_argc)
{
    int r;

    /* `*pstream` IS THE CALLER'S SLOT, and it is what says whether the first half has run. It has to be the
       caller's because the start sub-sequence below SUSPENDS, and a value a suspension carries is one the
       parked machine's own `visit` names — a local here would not survive the park, and a field on the shared
       StreamWork would collide with the sub-sequence's own use of it. */
    if (JS_IsUndefined(*pstream)) {
        JSValue rec, stream, fns[3], proto, data;
        JSValueConst cap;
        int i;

        DCHECK(file_system_is_file(entry),
               "File System §2.5's create a new FileSystemWritableFileStream was given something that is not a "
               "FILE ENTRY — §2.3.2 The createWritable() method step 5.5 asserts that before it ever reaches "
               "here");
        rec = idl_slots_new(ctx);
        CHECK(!JS_IsException(rec), "file system writable: the stream's internal slots could not be allocated");
        JS_SetPropertyStr(ctx, rec, FW_FILE, JS_DupValue(ctx, entry));
        /* "[[buffer]] ... is initially empty", and §2.3.2's step 5.7.3.1 makes it "a copy of entry's
           binary data" instead — a copy that is the VALUE, so a file whose contents are attacker input starts
           the stream tainted and the taint survives every splice. */
        data = keep_existing_data ? file_system_data(ctx, entry) : JS_NewStringLen(ctx, "", 0);
        JS_SetPropertyStr(ctx, rec, FW_BUFFER, data);
        JS_SetPropertyStr(ctx, rec, FW_SEEK, JS_NewFloat64(ctx, 0));
        cap = rec;
        fns[0] = JS_NewStepClosure(ctx, g_stepid_write, 1, 1, &cap);
        fns[1] = JS_NewStepClosure(ctx, g_stepid_close, 0, 1, &cap);
        fns[2] = JS_NewStepClosure(ctx, g_stepid_abort, 1, 1, &cap);
        for (i = 0; i < 3; i++)
            CHECK(!JS_IsException(fns[i]), "file system writable: a §2.5 algorithm could not be allocated");
        /* §2.5's steps 6 and 7, "Let highWaterMark be 1. Let sizeAlgorithm be an algorithm that returns 1" —
           which is what Streams §9.2.1 Creation and manipulation's `set up` already defaults to (highWaterMark
           default 1, and "If sizeAlgorithm was not given, then set it to an algorithm that returns 1"), so the
           size algorithm is undefined here rather than a function that returns the number this stream would
           then have to keep a realm's copy of. */
        stream = writable_stream_create(ctx, fns[0], fns[1], fns[2], /*highWaterMark*/ 1, JS_UNDEFINED);
        for (i = 0; i < 3; i++) JS_FreeValue(ctx, fns[i]);
        if (JS_IsException(stream)) { JS_FreeValue(ctx, rec); return -1; }
        /* THE RECORD GOES ON THE STREAM before anything can reach it, non-writable, non-enumerable and
           non-configurable: it is an internal slot, so a page must not be able to move it — and it is also this
           interface's BRAND, which a page must not be able to forge onto a plain WritableStream. */
        JS_DefinePropertyValue(ctx, stream, g_slot_key, rec, 0);
        /* `interface FileSystemWritableFileStream : WritableStream`. Streams §9.2.1 Creation and manipulation's
           `set up` operates on an object its CALLER minted — §2.5 step 1 mints a FileSystemWritableFileStream,
           not a WritableStream — and this engine's helper mints the §5.2 The WritableStream class one, so this
           is the derived interface saying which prototype it wanted. */
        proto = fs_writable_proto(ctx);
        JS_SetPrototype(ctx, stream, proto);
        JS_FreeValue(ctx, proto);
        *pstream = stream;
    }
    /* §2.5's step 8, "Set up stream", reaches Streams §5.5.4 Default controllers' SetUpWritableStreamDefault-
       Controller, which builds a START PROMISE out of the start algorithm's result; §9.2.1's `set up` declares
       that algorithm to be one that "returns undefined" — so the promise is one resolved with undefined, built
       through the same PromiseResolve sub-sequence Streams §6 Transform streams' TransformStream starts its two
       halves through. Resolving is where ECMAScript §27.5.1.3 CreateResolvingFunctions step 2.f reads `then`,
       which is why this is a request and not a call. */
    r = stream_promise_of_run(ctx, w, 0, in, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    if (writable_stream_start(ctx, *pstream, w->func) < 0) return -1;
    return 0;
}

/* ---- §2.5.1-3's THREE CONVENIENCE METHODS ------------------------------------------------------------------
 *
 * All three are the SAME three steps over a different chunk: "let writer be the result of getting a writer for
 * this", "let result be the result of writing a chunk to writer given …", "release writer", "return result".
 * One machine with a magic, because the difference between them is the chunk and nothing else. */
#define FWM_STAGES(X) \
    X(FWM_GET_WRITER, "File System §2.5.1-3 step 1 (get a writer for this — Streams §9.2.2 Writing's get a " \
                      "writer, which is §5.5.1 Working with writable streams' AcquireWritableStreamDefault" \
                      "Writer and throws for a locked stream)") \
    X(FWM_WRITE,      "File System §2.5.1-3 step 2 (write a chunk to writer, whose promise IS this member's " \
                      "result)") \
    X(FWM_RELEASE,    "File System §2.5.1-3 step 3 (release writer)")
enum { IDL_STEP_STAGE_BASE(FWM_STAGES) FWM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FWM_STEPS[] = { FWM_STAGES(JS_STEP_STAGE_LABEL) NULL };

enum { FWM_WRITE_M = 0, FWM_SEEK_M, FWM_TRUNCATE_M };

typedef struct {
    uint8_t cphase;
    JSValue writer;    /* owned */
    JSValue result;    /* the write's promise — this member's answer (owned) */
    JSValue cb[3];
} FwMethodState;

static void fwm_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FwMethodState *s = st;
    int k;

    v->val(ctx, &s->writer);
    v->val(ctx, &s->result);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

/* §2.5.2 and §2.5.3's chunks — «[ "type" → "seek", "position" → position ]» and its truncate twin. Built with
   DEFINE rather than assignment (core/idl_slots.h's sibling rule): an engine-built object creates OWN
   properties, and a page that put a setter on Object.prototype must not intercept them. */
static JSValue fwm_command(JSContext *ctx, const char *type, const char *field, JSValueConst value)
{
    JSValue o = JS_NewObject(ctx);

    CHECK(!JS_IsException(o), "file system writable: a §2.5 command could not be allocated");
    JS_DefinePropertyValueStr(ctx, o, "type", JS_NewString(ctx, type), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, field, JS_DupValue(ctx, value), JS_PROP_C_W_E);
    return o;
}

static int fwm_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FwMethodState *s = st;
    int magic = idl_step_magic(hdr);
    int r;

    *presult = JS_UNDEFINED;

    if (hdr->stage == FWM_GET_WRITER) {
        JSValue rec = fw_record(ctx, hdr->this_val), op;

        s->writer = s->result = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, r) s->cb[r] = JS_UNDEFINED;
        s->cphase = 0;
        /* WEB IDL §3.7.7 Operations' BRAND CHECK.
           `FileSystemWritableFileStream.prototype.write.call(new WritableStream)` is a TypeError, and a page
           tells that apart from the stream simply refusing the chunk. */
        if (!JS_IsObject(rec)) {
            JS_FreeValue(ctx, rec);
            JS_ThrowTypeError(ctx, "a FileSystemWritableFileStream method was called on something that is not "
                                   "a FileSystemWritableFileStream");
            return -1;
        }
        JS_FreeValue(ctx, rec);
        op = writable_stream_op(ctx, WS_OP_GET_WRITER);
        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), op, hdr->this_val, 0, NULL, cb_result, &s->writer,
                          out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(s->writer)) return -1;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, FWM_WRITE, &s->cphase, NULL);
    }

    if (hdr->stage == FWM_WRITE) {
        JSValue op = writable_stream_op(ctx, WS_OP_WRITE), chunk;
        JSValueConst arg;

        JSValueConst a0 = argc > 0 ? argv[0] : JS_UNDEFINED;

        if (magic == FWM_SEEK_M)          chunk = fwm_command(ctx, FW_CMD_SEEK, "position", a0);
        else if (magic == FWM_TRUNCATE_M) chunk = fwm_command(ctx, FW_CMD_TRUNCATE, "size", a0);
        else                              chunk = JS_DupValue(ctx, a0);
        arg = chunk;
        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), op, s->writer, 1, &arg, cb_result, &s->result,
                          out_cb, out_argc);
        JS_FreeValue(ctx, op);
        JS_FreeValue(ctx, chunk);
        if (r > 0) return r;
        if (JS_IsException(s->result)) return -1;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, FWM_RELEASE, &s->cphase, NULL);
    }

    DCHECK(hdr->stage == FWM_RELEASE, "a §2.5 convenience method resumed into a stage it does not have");
    {
        JSValue op = writable_stream_op(ctx, WS_OP_RELEASE), ignored = JS_UNDEFINED;

        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), op, s->writer, 0, NULL, cb_result, &ignored,
                          out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        JS_FreeValue(ctx, ignored);
    }
    *presult = s->result;
    s->result = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl FWM_DECL = {
    fwm_step, sizeof(FwMethodState), fwm_visit, NULL,
    "File System §2.5.1-3 FileSystemWritableFileStream.write() / .seek() / .truncate()", FWM_STEPS
};

/* ---- declaration and per-realm install --------------------------------------------------------------------- */

static void fs_writable_install_realm(JSContext *ctx)
{
    JSValue base, proto, prev, global;

    /* `[Exposed=(Window,Worker), SecureContext]` — Web IDL §3.3.13 [SecureContext] removes the whole interface
       in a non-secure realm, interface object and prototype alike, so there is nothing to install rather than
       something that throws. It is asked ONCE, for the interface, because that is the level the extended
       attribute is written at; a per-member ask would be the hand-picked list core/idl_args.h warns about.
       Nothing can reach a FileSystemWritableFileStream in such a realm either: the only mint is File System
       §2.3.2 The createWritable() method, on a handle whose own interface is gated by the same attribute. */
    if (!secure_context_is(ctx)) return;
    base = writable_stream_proto(ctx);
    prev = JS_GetClassProto(ctx, g_fw_class);
    DCHECK(JS_IsNull(prev), "fs_writable_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "FileSystemWritableFileStream.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "FileSystemWritableFileStream");
    idl_install_method(ctx, proto, "write", g_id_write);
    idl_install_method(ctx, proto, "seek", g_id_seek);
    idl_install_method(ctx, proto, "truncate", g_id_truncate);
    JS_SetClassProto(ctx, g_fw_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FileSystemWritableFileStream",
                      idl_interface_object(ctx, "FileSystemWritableFileStream", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void fs_writable_init(JSContext *ctx)
{
    JSClassDef d = { "FileSystemWritableFileStream" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* §2.5.1-3 each take exactly one argument, and only `write`'s is a union the body reads back off the
       value — `seek` and `truncate` take an `unsigned long long`, whose conversion is the declaration's. */
    static const IdlArgType WRITE_ARGS[] = { IDL_ANY };
    static const IdlArgType NUM_ARGS[]   = { IDL_UNSIGNED_LONG_LONG };

    DCHECK(g_stepid_write < 0, "fs_writable_init ran twice — §2.5's machines are declared once per AGENT");
    JS_NewClassID(rt, &g_fw_class);
    CHECK(JS_NewClass(rt, g_fw_class, &d) == 0,
          "FileSystemWritableFileStream: the per-realm prototype slot could not be declared");
    g_slot_key = JS_NewAtom(ctx, "__fileSystemWritableSlots");
    g_atom_type = JS_NewAtom(ctx, "type");
    g_atom_data = JS_NewAtom(ctx, "data");
    g_atom_position = JS_NewAtom(ctx, "position");
    g_atom_size = JS_NewAtom(ctx, "size");
    CHECK(g_slot_key != JS_ATOM_NULL && g_atom_type != JS_ATOM_NULL && g_atom_data != JS_ATOM_NULL &&
          g_atom_position != JS_ATOM_NULL && g_atom_size != JS_ATOM_NULL,
          "file system writable: an internal name could not be interned");
    g_stepid_write = JS_RegisterStepDef(rt, &FWW_DEF);
    g_stepid_close = JS_RegisterStepDef(rt, &FWC_DEF);
    g_stepid_abort = JS_RegisterStepDef(rt, &FWA_DEF);
    g_id_write = idl_method_id_step(ctx, WRITE_ARGS, 1, NULL, 0, &FWM_DECL, FWM_WRITE_M);
    g_id_seek = idl_method_id_step(ctx, NUM_ARGS, 1, NULL, 0, &FWM_DECL, FWM_SEEK_M);
    g_id_truncate = idl_method_id_step(ctx, NUM_ARGS, 1, NULL, 0, &FWM_DECL, FWM_TRUNCATE_M);
    agent_state_id("file_system_writable", &g_stepid_write,
                   "File System §2.5's write-a-chunk machine, and the declaration latch");
    agent_state_id("file_system_writable", &g_stepid_close, "§2.5's close-algorithm machine (its step 4)");
    agent_state_id("file_system_writable", &g_stepid_abort, "§2.5's abort-algorithm machine (its step 5)");
    agent_state_atom("file_system_writable", &g_slot_key, "§2.5's internal-slot key");
    agent_state_atom("file_system_writable", &g_atom_type, "the write-params `type` member name");
    agent_state_atom("file_system_writable", &g_atom_data, "the write-params `data` member name");
    agent_state_atom("file_system_writable", &g_atom_position, "the write-params `position` member name");
    agent_state_atom("file_system_writable", &g_atom_size, "the write-params `size` member name");
    realm_declare_intrinsic(fs_writable_install_realm);
}

void fs_writable_free(JSRuntime *rt)
{
    g_stepid_write = g_stepid_close = g_stepid_abort = -1;
    g_id_write = g_id_seek = g_id_truncate = -1;
    /* THE ATOMS ARE GIVEN BACK, and the sentence that used to stand here — "the atoms and the class id belong
       to a runtime that is going away with them" — was HALF true and that is what made it dangerous. It is
       true of the class id, which is a registration and not a reference. It is false of an INTERNED name:
       JS_NewAtom takes a COUNTED reference in the runtime's own table, and JS_FreeRuntime walks that table and
       reports every id still holding one. This function nulled five handles and dropped no reference at all,
       and FOUR of the five — `__fileSystemWritableSlots`, `type`, `data`, `position` — were named by the atom
       walk on 118 of the 190 files of `css/cssom`.
       IT COULD NOT HAVE DONE OTHERWISE: this release took `void`, so it had no runtime to free them against.
       That is the same defect as the one that kept element_free off core/platform.h's release column, one
       level down — a row whose release cannot express what the component holds is a row that silently holds
       it — and the fix is the same one: take the JSRuntime the column already has.

       `size` IS THE FIFTH AND IT NEVER LEAKED, WHICH IS WORTH THE PARAGRAPH BECAUSE THE WRONG RULE IS THE
       TEMPTING ONE. It is one of quickjs's own built-in atoms (`quickjs-atom.h`'s `DEF(size, "size")`), and the
       whole atom API is refcount-INVARIANT on those: `__JS_NewAtom`'s found-in-hash branch increments only
       `if (!__JS_AtomIsConst(i))`, and JS_DupAtom/JS_DupAtomRT/JS_FreeAtom/JS_FreeAtomRT each open with the
       same test and return. So `JS_NewAtom(ctx, "size")` takes NOTHING, its refcount is whatever JS_InitAtoms
       set and nothing moves it, and the line below for it is a test and a return. The commit that wrote this
       release justified that line as "JS_NewAtom took a reference and a release must give it back", which is
       true of the four above and FALSE of this one.
       IT IS STILL CALLED, and deliberately, because the two candidate rules are not equally safe. "Free every
       name you interned" is correct for every atom and requires the caller to know nothing — the guard lives
       in the API, not here. "Free every name you interned EXCEPT the built-in ones" would make this release's
       correctness depend on a table inside the submodule that is synced to upstream, so an upstream edit
       removing `DEF(size, …)` would turn a deliberate omission into a silent leak with nothing to say so. A
       no-op call is the cheaper of the two mistakes by a wide margin.
       WHAT MUST NOT BE GENERALISED FROM THIS FILE is the reverse reading: a built-in atom SURVIVING to
       JS_FreeRuntime's census is not a missing JS_FreeAtom and never can be — it is a leaked JSValue, because
       `__JS_AtomToValue` is the one path that hands the struct out as a string and dups it. The census says so
       itself now.
       The prototypes are the realms' and go with their contexts. */
    JS_FreeAtomRT(rt, g_slot_key);
    JS_FreeAtomRT(rt, g_atom_type);
    JS_FreeAtomRT(rt, g_atom_data);
    JS_FreeAtomRT(rt, g_atom_position);
    JS_FreeAtomRT(rt, g_atom_size);
    g_slot_key = g_atom_type = g_atom_data = g_atom_position = g_atom_size = JS_ATOM_NULL;
}
