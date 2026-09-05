/* THE FileReader INTERFACE — W3C File API §6 Reading Data.
 *
 * A FileReader is a STATE MACHINE and nothing else: §6.2.2 FileReader States gives it three states, §6.2's read
 * operation is one algorithm all four read methods enter with a different `type`, and every member of the
 * interface reads that one record. Written outward from the member list it would be four near-copies of an
 * event sequence; written outward from the state it is one machine with a `type` operand, which is exactly how
 * §6.2.3 Reading a File or Blob states it — "must initiate a read operation for blob with DataURL", "with Text
 * and encoding", "with ArrayBuffer", "with BinaryString".
 *
 * WHY IT IS BUILT. A page's upload handler is `new FileReader()` on the line after the change event, and
 * without the constructor the flow dies at that line: every endpoint the handler composes and every sink it
 * reaches are downstream of a ReferenceError. The interface is already on browser/platform_names.h, so the
 * throw was the honest forcing function; this is the component it names.
 *
 * THE EVENT ORDER IS THE SPEC AND IT IS THE LOAD-BEARING PART. §6.4.2 Summary of Event Invariants states it and
 * §6.2's step 10 produces it: loadstart, then one progress per chunk, then exactly one of load / error / abort,
 * then loadend — and loadend is fired ONLY IF the state is still not "loading", which is what makes
 * read-chaining (`reader.onload = () => reader.readAsText(other)`) suppress the first read's loadend. That test
 * is §6.2 step 10.5.5's own, written where the spec writes it rather than inferred at the fire.
 *
 * THE CHUNK SEQUENCE IS STATED, NOT PUMPED. §6.2 steps 5-10 read blob's stream chunk by chunk. A Blob is an
 * IMMUTABLE BYTE SEQUENCE that is already in memory (§3 The Blob Interface and Binary Data; core/file/blob.c
 * says the same at its own readers — "A Blob's bytes are always here: §3 makes it an immutable byte sequence,
 * never a stream"), so the chunk sequence that loop observes is DETERMINED: one chunk carrying every byte when
 * the blob is non-empty, and none at all when it is empty. This machine states that sequence instead of driving
 * a ReadableStream object from C, and the empty-blob event order then FALLS OUT of the algorithm rather than
 * being a special case — step 10.2 fires loadstart on the first fulfilment whether or not it carried data,
 * step 10.4 (and with it the progress event) runs only for a chunk that HAS data, and step 10.5 completes.
 * That is what wpt/FileAPI/reading-data-section/filereader_events.any.js asserts in as many words: "No progress
 * event for an empty blob, as no data is loaded."
 *
 * WHAT EACH TASK IS. §6.1 The File Reading Task Source defines a task source for exactly these, and §6.2's
 * step 10 says "Use the file reading task source for all these tasks" — so each of the three fires is its own
 * turn of the event loop, queued through the ONE frontier (JS_EnqueueCallTask) and never drained here. Each
 * task's callee is a step machine, because §2.9's dispatch runs the page's listeners and a fire this component
 * cannot park on is a fire that would have to drive the page's code out of a C activation.
 *
 * AND THE TASKS ARE QUEUED UP FRONT, which is what §6.2.3.5 The abort() method step 3 is about. Step 10's loop
 * runs "in parallel" over chunks that are all ready, so it reaches every queue-a-task step before any of them
 * runs; step 3 of abort() then has real tasks to REMOVE ("If there are any tasks from this on the file reading
 * task source in an affiliated task queue, then remove those tasks from that task queue"), which is how a read
 * aborted from inside its own loadstart handler delivers no progress and no load. Each task clears its own
 * handle off the record when it begins, so a handle the record still holds names a task that has not started —
 * and that is the invariant abort()'s removal asserts from the other side.
 *
 * WHAT IS ABSENT AND WHY, stated rather than stubbed:
 *   - §6.5.1 The FileReaderSync API is not here, and its absence is its IDL: `[Exposed=(DedicatedWorker,
 *     SharedWorker)]` (Web IDL §3.3.7 [Exposed]). This engine has no worker global, so installing it on Window
 *     would put a member in a realm the spec does not expose it in — a page's `typeof FileReaderSync` would
 *     answer "function" where every browser answers "undefined". It is one call to file_reader_package_data
 *     per method the day a worker global exists, which is why that algorithm is exported rather than private.
 *   - §7 Errors and Exceptions' file read errors are the ERROR ARM below, reached and not reachable-from-here:
 *     §7.1 Throwing an Exception or Returning an Error names the five failure reasons (NotFound, UnsafeFile,
 *     TooManyReads, SnapshotState, FileLock) as "the particular error condition that causes the get stream
 *     algorithm to fail", and an in-memory byte sequence's get stream cannot fail. What DOES reach the arm is
 *     §6.3's own throw: a byte sequence too long to be a JS string makes the Text, DataURL and BinaryString
 *     arms throw a RangeError, and step 10.5.3 makes that the reader's `error`.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/encoding/encoding.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/mime/mime_type.h"
#include "core/realm.h"
#include "core/file/blob.h"
#include "core/file/file_reader.h"
#include "core/xhr/progress_event.h"   /* §6.4.1 Event Summary: every event of §6 IS a ProgressEvent */
#include "solver/concolic.h"
#include "solver/cow.h"

/* §6.2.2 FileReader States, in the order the interface's constants number them. */
enum { FR_EMPTY = 0, FR_LOADING, FR_DONE };

typedef struct {
    /* §6.2 The FileReader API: "A FileReader has an associated result (null, a DOMString or an ArrayBuffer).
       It is initially null." and "A FileReader has an associated error (null or a DOMException)." Both are
       JSValues rather than C storage for the reason core/xhr and core/events/message_port give: a malloc'd
       payload in a record the COW delta captures BY BYTES is reverted as a POINTER on a context switch and
       leaked by the arm that allocated it — a leak the runtime's own GC walk cannot see. */
    JSValue result;
    JSValue error;
    /* §6.2.3.5 step 3's "any tasks from this on the file reading task source", by the handle the queue issued.
       CLEARED BY EACH TASK AS IT BEGINS, so a handle still here names a task that has not started — which is
       what makes abort()'s removal an assertable operation rather than a hopeful one. */
    JSTaskHandle task_loadstart, task_progress, task_done;
    uint8_t state;   /* §6.2's associated state */
} FileReaderData;

static JSClassID  g_fr_class;
static JSRuntime *g_fr_rt;
static int        g_ready;
static int        g_id_read[4] = { -1, -1, -1, -1 };   /* one pool entry per §6.2.3 read method */
static int        g_abort_stepid = -1;
static int        g_task_stepid = -1;

/* THE RECORD TIME-TRAVELS. `state`, `result`, `error` and the three task handles are state a FLOW writes where
   no property hook can see — an arm that read must not have read for its sibling, and an arm that aborted must
   not have aborted for it — and the capture is in the ACCESSOR for the reason the streams and messaging
   components give: a record a flow has REACHED is one it may write, so there is no write site left to miss.
   The offset list is the same list the finalizer frees and the gc_mark walks. */
#define FRO(f) (uint16_t)offsetof(FileReaderData, f)
static const uint16_t FR_VALS[] = { FRO(result), FRO(error) };
static const CowRecord FR_REC = { sizeof(FileReaderData), FR_VALS, (int)(sizeof(FR_VALS) / sizeof(FR_VALS[0])) };

static FileReaderData *fr_of(JSValueConst v)
{
    FileReaderData *d = JS_GetOpaque(v, g_fr_class);
    if (d) cow_capture_host_record(v, d, &FR_REC);
    return d;
}

/* WRITE ONE OF THE TWO, and never `JS_FreeValue(ctx, d->f); d->f = <build one>;` — see cow.h for the order and
   the defect. The record and its layout are bound HERE rather than at each call, so no site can pass a slot
   from one record with the layout of another. Every write of `result` and `error` below goes through it; the
   CONSTRUCTOR does not, and that is the one honest exception: before JS_SetOpaque the record is unreachable by
   the collector and its calloc'd slots hold no value to release.
   THE ADDRESS PASSES THROUGH: the asserts inside are about the SLOT, so they must name the WRITE and not this
   line — see cow.h's THE SITE TRAVELS WITH THE OPERATION. */
static void fr_set_at(JSContext *ctx, FileReaderData *d, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, d, &FR_REC, slot, v, file, line);
}
#define fr_set(ctx_, d_, slot_, v_) fr_set_at((ctx_), (d_), (slot_), (v_), __FILE__, __LINE__)

static void fr_finalizer(JSRuntime *rt, JSValue val)
{
    FileReaderData *d = JS_GetOpaque(val, g_fr_class);
    size_t i;

    if (!d) return;
    for (i = 0; i < sizeof(FR_VALS) / sizeof(FR_VALS[0]); i++)
        JS_FreeValueRT(rt, *(JSValue *)((char *)d + FR_VALS[i]));
    free(d);
}

static void fr_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    FileReaderData *d = JS_GetOpaque(val, g_fr_class);
    size_t i;

    /* A page routinely holds the reader from a listener registered on it and the reader holds its `result`;
       an ArrayBuffer result whose page kept a view on it closes the cycle. Without this walk the pair is
       unreachable to the collector and every completed read is a leak the runtime's own walk reports. */
    if (!d) return;
    for (i = 0; i < sizeof(FR_VALS) / sizeof(FR_VALS[0]); i++)
        JS_MarkValue(rt, *(JSValue *)((char *)d + FR_VALS[i]), mark_func);
}

/* ---- §6.3 Packaging data ------------------------------------------------------------------------------------
 *
 * "A Blob has an associated package data algorithm, given bytes, a type, a mimeType, and an optional
 * encodingLabel, which switches on type and runs the associated steps." */

/* §6.3's Text arm, steps 1-5. The encoding is chosen by the label, then by the mimeType's charset parameter,
 * then UTF-8 — and the DECODE is Encoding §6.1 Legacy hooks for standards' `decode`, whose BOM sniff overrules
 * whatever those three chose. That is not this component being lax: the Encoding Standard states the override
 * as a deliberate compatibility violation, and it is what makes a BOM'd UTF-16BE blob read as UTF-16BE with no
 * label at all (wpt/FileAPI/reading-data-section/Determining-Encoding.any.js checks both directions).
 */
static JSValue fr_package_text(JSContext *ctx, const char *bytes, size_t len, const char *mime_type,
                               const char *encoding_label)
{
    int enc = -1;   /* §6.3 step 1: "Let encoding be failure." */
    char *out;
    size_t out_n = 0;
    JSValue s;

    /* Step 2: "If the encodingLabel is present, set encoding to the result of getting an encoding from
       encodingLabel." — Encoding §4.2 Names and labels' "get an encoding", whose failure is -1. */
    if (encoding_label)
        enc = encoding_lookup(encoding_label, strlen(encoding_label));
    /* Step 3: "If encoding is failure: Let type be the result of parse a MIME type given mimeType. If type is
       not failure, set encoding to the result of getting an encoding from type's parameters["charset"]." */
    if (enc < 0 && mime_type && *mime_type) {
        MimeType m;
        mime_type_init(&m);
        if (mime_type_parse(&m, mime_type, strlen(mime_type))) {
            const char *charset = mime_type_parameter(&m, "charset");
            if (charset)
                enc = encoding_lookup(charset, strlen(charset));
        }
        mime_type_free(&m);
    }
    /* Step 4: "If encoding is failure, then set encoding to UTF-8." */
    if (enc < 0)
        enc = encoding_utf8();
    /* Step 5: "Decode bytes using fallback encoding encoding, and return the result." */
    out = encoding_decode(bytes, len, enc, &out_n);
    CHECK(out != NULL, "FileReader: OOM decoding a byte sequence for File API §6.3's Text arm");
    s = JS_NewStringLen(ctx, out, out_n);
    free(out);
    return s;
}

/* §6.3's DataURL arm. "Return bytes as a DataURL [RFC2397] subject to the considerations below: Use mimeType as
 * part of the Data URL if it is not the empty string in keeping with the Data URL specification [RFC2397].
 * Otherwise, return a Data URL without a media-type."
 *
 * THE EMPTY-mimeType CASE IS DECIDED BY THE TEST SUITE AND NOT BY THAT SENTENCE, and the spec says so itself:
 * the arm carries an open issue — "Better specify how the DataURL is generated. [Issue #104]" — so its own
 * text is not an authority on the byte. The interoperable answer, which the spec authors' own corpus encodes
 * (wpt/FileAPI/reading-data-section/filereader_readAsDataURL.any.js asserts
 * `data:application/octet-stream;base64,VEVTVA==` for a Blob with no type), is the `application/octet-stream`
 * default. Reading "a Data URL without a media-type" literally would emit `data:;base64,…`, which no engine
 * produces and which the suite fails.
 */
static JSValue fr_package_data_url(JSContext *ctx, const char *bytes, size_t len, const char *mime_type)
{
    const char *mime = (mime_type && *mime_type) ? mime_type : "application/octet-stream";
    size_t b64_cap = JS_Base64EncodedSize(len) + 1, n, cap;
    char *b64, *url;
    JSValue s;

    b64 = malloc(b64_cap);
    CHECK(b64 != NULL, "FileReader: OOM base64-encoding a byte sequence for File API §6.3's DataURL arm");
    /* THE ENGINE'S OWN CODEC — the one the platform already made it implement for `btoa` — rather than a
       second base64 grown here. §Solver's rule is the same one: an encoding builtin is modelled faithfully,
       never re-implemented beside itself. */
    n = JS_Base64Encode(b64, b64_cap, (const uint8_t *)bytes, len);
    CHECK(n > 0 || len == 0, "FileReader: the base64 buffer was sized wrong for this byte sequence");
    b64[n] = 0;
    cap = strlen("data:") + strlen(mime) + strlen(";base64,") + n + 1;
    url = malloc(cap);
    CHECK(url != NULL, "FileReader: OOM building File API §6.3's Data URL");
    snprintf(url, cap, "data:%s;base64,%s", mime, b64);
    s = JS_NewStringLen(ctx, url, cap - 1);
    free(url);
    free(b64);
    return s;
}

JSValue file_reader_package_data(JSContext *ctx, const char *bytes, size_t len, FileReadType type,
                                 const char *mime_type, const char *encoding_label)
{
    DCHECK(bytes != NULL || len == 0,
           "File API §6.3's package data was given no byte sequence with a non-zero length — the operand is a "
           "byte sequence and an absent one is not the empty one");
    switch (type) {
    case FILE_READ_DATA_URL:
        return fr_package_data_url(ctx, bytes ? bytes : "", len, mime_type);
    case FILE_READ_TEXT:
        return fr_package_text(ctx, bytes ? bytes : "", len, mime_type, encoding_label);
    case FILE_READ_ARRAY_BUFFER:
        /* "Return a new ArrayBuffer whose contents are bytes." */
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)(bytes ? bytes : ""), len);
    default:
        DCHECK(type == FILE_READ_BINARY_STRING,
               "File API §6.3's package data was asked for a result kind its switch does not name");
        /* "Return bytes as a binary string, in which every byte is represented by a code unit of equal value
           [0..255]." That is a UTF-16 code unit per BYTE and not a decode: the string is built out of the byte
           values directly, so a byte that is not valid UTF-8 survives as its own code unit rather than
           becoming U+FFFD. */
        {
            uint16_t *units;
            size_t i;
            JSValue s;

            units = malloc((len ? len : 1) * sizeof *units);
            CHECK(units != NULL, "FileReader: OOM building File API §6.3's binary string");
            for (i = 0; i < len; i++)
                units[i] = (uint16_t)(unsigned char)bytes[i];
            s = JS_NewStringUTF16(ctx, units, len);
            free(units);
            return s;
        }
    }
}

/* THE SOURCE THE BYTE SEQUENCE CARRIES, over the value §6.3 packaged out of it.
 *
 * A File's contents are external input in the same sense `location.hash` is — the bytes were chosen by whoever
 * put the file on the device — and core/file/blob.c mints that identity AT THE READ, per read, because a
 * candidate substitution reaches a source at its mint. This is the same seam for the same bytes: §6.2's read
 * operation is what turns them into a value the page concatenates, tests and hands to a sink.
 *
 * THE THREE STRING ARMS AND NOT THE BUFFER. A DOMString the page computes with is a value the solver's domain
 * is over and the delivered candidate is decided by RE-EXECUTION — the real decode, the real base64, run again
 * on the delivered bytes — so no arm has to be exempted for transforming them. An ArrayBuffer is not a value of
 * that domain (blob.c states the same rule for `arrayBuffer()`), and wrapping one would break
 * `new Uint8Array(reader.result)`, which is the ordinary way a page reads it. */
static JSValue fr_source_wrap(JSContext *ctx, JSValueConst blob, FileReadType type, JSValue computed)
{
    const char *shape = NULL, *src = NULL;

    if (type == FILE_READ_ARRAY_BUFFER || JS_IsException(computed))
        return computed;
    if (!blob_source_of(blob, &shape, &src))
        return computed;
    return concolic_source_wrap(ctx, shape, src, computed);
}

/* ---- §6.1 The File Reading Task Source's tasks ---------------------------------------------------------- */

/* WHICH OF STEP 10'S THREE QUEUE-A-TASK STEPS THIS TASK IS. Carried in the closure because §scheduler's rule is
   that an operation which becomes a work item TAKES ITS INPUTS WITH IT — a task that re-derived "am I the
   progress one?" off the reader would be reading a record three other tasks are also writing. */
enum { FR_TASK_LOADSTART = 0, FR_TASK_PROGRESS, FR_TASK_DONE };

/* The closure a queued task carries: everything §6.2's step 10 had in scope when it queued it. */
enum { FR_CD_READER = 0, FR_CD_BLOB, FR_CD_MODE, FR_CD_TYPE, FR_CD_LABEL, FR_CD_N };

#define FR_TASK_STAGES(X) \
    X(FRT_BEGIN, "File API §6.2 The FileReader API read operation step 10 (this file reading task begins: " \
                 "which of step 10.2's loadstart, step 10.4.3's progress and step 10.5's completion it is)") \
    X(FRT_FIRE, "File API §6.2 The FileReader API read operation step 10.2 or step 10.4.3 (fire a progress " \
                "event called loadstart / progress at fr)") \
    X(FRT_PACKAGE, "File API §6.2 The FileReader API read operation steps 10.5.1-10.5.2 (set fr's state to " \
                   "done, and package data given bytes, type, blob's type and encodingLabel)") \
    X(FRT_RESULT_EVENT, "File API §6.2 The FileReader API read operation step 10.5.3 or step 10.5.4 (fire a " \
                        "progress event called error or load at fr)") \
    X(FRT_LOADEND, "File API §6.2 The FileReader API read operation step 10.5.5 (if fr's state is not " \
                   "loading, fire a progress event called loadend at fr)")
enum { FR_TASK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FR_TASK_STEPS[] = { FR_TASK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;         /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t     fphase;      /* the dispatch request's own phase, held across the suspension */
    JSValue     ev;          /* the event in flight (owned) */
    EventFireCb cb;          /* the dispatch's request buffer — the type carries §2.9's argument count */
    /* §5.1's two operands, decided where the fire is decided. §6.2 does not name them — each of step 10's
       fires says only "fire a progress event called loadstart at fr" and the same shape for `progress`,
       `error`, `load` and `loadend`, and §6.4's definition of what it MEANS to "fire a progress event called
       e" is about the event's bubbles and cancelable flags and nothing else — so they are the algorithm's OWN state at that point: the bytes it has
       accumulated, and the byte sequence's size. A completion that failed has neither, so both are 0 — which is
       also what leaves `lengthComputable` false, exactly as §5.1 says a length of 0 does. */
    double      transmitted, length;
} FrTask;

static void js_fr_task_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FrTask *s = st;
    int i;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* The label §6.3's "optional encodingLabel" is, or NULL for the spec's "not present". The closure holds the
   page's own string; nothing here runs its code, because Web IDL converted it at the declaration. */
static const char *fr_label_of(JSContext *ctx, JSValueConst label)
{
    return JS_IsUndefined(label) || JS_IsNull(label) ? NULL : JS_ToCString(ctx, label);
}

static int js_fr_task_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    FrTask *s = st;
    JSValueConst reader = JS_StepClosureData(&s->hdr, FR_CD_READER);
    JSValueConst blob = JS_StepClosureData(&s->hdr, FR_CD_BLOB);
    FileReaderData *d = fr_of(reader);
    size_t len = 0;
    const char *bytes = blob_bytes_of(blob, &len, NULL);
    int r;

    DCHECK(d != NULL, "a file reading task ran over something that is not a FileReader — the task is minted by "
                      "the read operation over the object that entered it, so nothing else can be here");
    DCHECK(bytes != NULL, "a file reading task ran over something that is not a Blob — Web IDL §3.2.15 brands "
                          "the read methods' argument, so the only value that reaches the closure is one");

    STEP_DISPATCH(FR_TASK_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(FRT_BEGIN);
    {
        int mode = JS_VALUE_GET_INT(JS_StepClosureData(&s->hdr, FR_CD_MODE));
        int i;

        JS_FreeValue(ctx, cb_result);
        /* EVERY OWNED FIELD BEFORE ANYTHING THAT CAN FAIL — the failure path tears this machine down through
           js_fr_task_visit, which releases exactly what the state holds, and a zeroed block is not a block of
           JS_UNDEFINEDs (JS_TAG_INT is 0, so an unwritten slot is the NUMBER 0). */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
        s->fphase = 0;
        s->transmitted = s->length = 0;
        if (mode == FR_TASK_DONE) {
            DCHECK(d->task_done != JS_TASK_HANDLE_NONE,
                   "File API §6.2 step 10.5's completion task ran with its handle already cleared off the "
                   "reader — a handle is cleared by the task that OWNS it as it begins and by §6.2.3.5 step "
                   "3's removal, so this task either ran twice or ran after abort() removed it");
            d->task_done = JS_TASK_HANDLE_NONE;
            /* A REST POINT AND NOT A JUMP. §6.3's packaging decodes, base64-encodes or copies the WHOLE byte
               sequence, which is work of the page's size — so the boundary in front of it is a point the
               ENGINE may have to park at, exactly as quickjs-step.h says a stage boundary is. */
            STEP_GOTO(s->hdr.stage, FRT_PACKAGE, &s->fphase, NULL);
            return JS_STEP_YIELD;
        }
        if (mode == FR_TASK_LOADSTART) {
            DCHECK(d->task_loadstart != JS_TASK_HANDLE_NONE,
                   "File API §6.2 step 10.2's loadstart task ran with its handle already cleared off the "
                   "reader — see the completion task's assert for what the handle means");
            d->task_loadstart = JS_TASK_HANDLE_NONE;
            /* §6.2 step 10.2 queues this on the FIRST fulfilment, before step 10.4 has appended anything, so
               nothing has been transmitted and no total is yet in hand. */
            s->ev = progress_event_new(ctx, "loadstart", 0, 0);
        } else {
            DCHECK(mode == FR_TASK_PROGRESS,
                   "a file reading task carried a mode File API §6.2's step 10 does not queue");
            DCHECK(d->task_progress != JS_TASK_HANDLE_NONE,
                   "File API §6.2 step 10.4.3's progress task ran with its handle already cleared off the "
                   "reader — see the completion task's assert for what the handle means");
            d->task_progress = JS_TASK_HANDLE_NONE;
            /* Step 10.4.1-10.4.2 appended the whole chunk before step 10.4.3 queued this. */
            s->transmitted = s->length = (double)len;
            s->ev = progress_event_new(ctx, "progress", s->transmitted, s->length);
        }
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(s->hdr.stage, FRT_FIRE, &s->fphase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(FRT_FIRE);
    DCHECK(JS_IsObject(s->ev), "File API §6.2's loadstart / progress task resumed at its fire with no event");
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), reader, s->ev, JS_UNDEFINED, cb_result,
                              NULL, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    return JS_STEP_DONE;

    STEP_ARM(FRT_PACKAGE);
    {
        JSValueConst label = JS_StepClosureData(&s->hdr, FR_CD_LABEL);
        FileReadType type = (FileReadType)JS_VALUE_GET_INT(JS_StepClosureData(&s->hdr, FR_CD_TYPE));
        const char *mime = NULL, *lab;
        JSValue packaged;

        JS_FreeValue(ctx, cb_result);
        /* Step 10.5.1: "Set fr's state to done." BEFORE the packaging and before the event, which is what
           makes `readyState` read DONE inside the load handler and what lets that handler start a new read. */
        d->state = FR_DONE;
        blob_bytes_of(blob, &len, &mime);
        lab = fr_label_of(ctx, label);
        /* Step 10.5.2: "Let result be the result of package data given bytes, type, blob's type, and
           encodingLabel." */
        packaged = file_reader_package_data(ctx, bytes, len, type, mime, lab);
        if (lab) JS_FreeCString(ctx, lab);
        if (JS_IsException(packaged)) {
            /* Step 10.5.3: "If package data threw an exception error: Set fr's error to error. Fire a progress
               event called error at fr." */
            fr_set(ctx, d, &d->error, JS_GetException(ctx));
            s->transmitted = s->length = 0;
            s->ev = progress_event_new(ctx, "error", 0, 0);
        } else {
            /* Step 10.5.4: "Set fr's result to result. Fire a progress event called load at the fr." */
            fr_set(ctx, d, &d->result, fr_source_wrap(ctx, blob, type, packaged));
            s->transmitted = s->length = (double)len;
            s->ev = progress_event_new(ctx, "load", s->transmitted, s->length);
        }
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(s->hdr.stage, FRT_RESULT_EVENT, &s->fphase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(FRT_RESULT_EVENT);
    DCHECK(JS_IsObject(s->ev), "File API §6.2 step 10.5's completion resumed at its event with none to fire");
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), reader, s->ev, JS_UNDEFINED, cb_result,
                              NULL, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    /* Step 10.5.5: "If fr's state is not loading, fire a progress event called loadend at the fr." The state
       is "done" unless the load or error handler that just ran STARTED ANOTHER READ, which is §6.4.2's
       read-chaining: "the loadend event for this load is not fired". */
    if (d->state == FR_LOADING)
        return JS_STEP_DONE;
    s->ev = progress_event_new(ctx, "loadend", s->transmitted, s->length);
    if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
    STEP_GOTO(s->hdr.stage, FRT_LOADEND, &s->fphase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(FRT_LOADEND);
    DCHECK(JS_IsObject(s->ev), "File API §6.2 step 10.5.5 resumed with no loadend event to fire");
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), reader, s->ev, JS_UNDEFINED, cb_result,
                              NULL, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_fr_task_def = {
    sizeof(FrTask), js_fr_task_step, NULL, 0, .visit = js_fr_task_visit,
    .algorithm = "File API §6.1 The File Reading Task Source's task for §6.2's read operation",
    .steps = FR_TASK_STEPS
};

/* Queue ONE of step 10's tasks. The handle is what §6.2.3.5 step 3 removes it by. */
static JSTaskHandle fr_queue(JSContext *ctx, JSValueConst reader, JSValueConst blob, int mode,
                             FileReadType type, JSValueConst label)
{
    JSValueConst data[FR_CD_N];
    JSValue fn;
    JSTaskHandle h;

    DCHECK(g_task_stepid >= 0, "a file reading task was queued before file_reader_init declared its machine");
    data[FR_CD_READER] = reader;
    data[FR_CD_BLOB] = blob;
    data[FR_CD_MODE] = JS_NewInt32(ctx, mode);
    data[FR_CD_TYPE] = JS_NewInt32(ctx, (int)type);
    data[FR_CD_LABEL] = label;
    /* MINTED IN THE READER'S REALM, which is `ctx`: a C member runs in the realm that DEFINED it, so a task
       callee held in a module static would answer every document out of whichever realm built it first — and
       every ProgressEvent this task mints would then wear that realm's prototype. */
    fn = JS_NewStepClosure(ctx, g_task_stepid, 0, FR_CD_N, data);
    CHECK(!JS_IsException(fn), "FileReader: OOM building a file reading task — a dropped task is a read that "
                               "never completes and a handler this solver never reaches");
    h = JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
    DCHECK(h != JS_TASK_HANDLE_NONE,
           "a file reading task was queued and issued no handle — §6.2.3.5 step 3 removes these tasks BY "
           "handle, so a task with none is one abort() can never take off the queue");
    return h;
}

/* ---- §6.2's read operation, and §6.2.3's four methods over it ---------------------------------------------- */

static JSValue js_fr_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    FileReaderData *d = fr_of(this_val);
    FileReadType type = (FileReadType)magic;
    JSValueConst blob, label;
    const char *bytes;
    size_t len = 0;

    DCHECK(argc >= 1, "a §6.2.3 read method's body was entered with no blob — Web IDL §3.6 Overload resolution "
                      "algorithm refuses the call before the algorithm's step 1 when a required argument is "
                      "missing, and `Blob blob` is required in all four");
    blob = argv[0];
    label = (argc > 1) ? argv[1] : JS_UNDEFINED;
    if (!d)
        return JS_ThrowTypeError(ctx, "a FileReader read method was called on something that is not one");
    /* Step 1: "If fr's state is "loading", throw an InvalidStateError DOMException." */
    if (d->state == FR_LOADING)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "a read is already in progress on this FileReader");
    /* Steps 2-4. */
    d->state = FR_LOADING;
    fr_set(ctx, d, &d->result, JS_NULL);
    fr_set(ctx, d, &d->error, JS_NULL);
    /* Steps 5-10. See the file comment for why the chunk sequence a Blob's stream produces is STATED here: it
       is one chunk carrying every byte, or none at all for an empty byte sequence.
         step 10.2  — the first fulfilment queues loadstart, whether or not it carried data;
         step 10.4.3 — a chunk that HAS data queues one progress;
         step 10.5  — the done fulfilment queues the completion.
       All three are reached before any of them runs, because step 10's loop is "in parallel" over chunks that
       are all ready — which is what gives §6.2.3.5 step 3 tasks to remove. */
    /* NOT inside the DCHECK: `len` is what decides whether step 10.4.3's task is queued at all, and a
       condition with a side effect vanishes with the assert in a release build — which would leave every read
       there firing no `progress` event. */
    bytes = blob_bytes_of(blob, &len, NULL);
    DCHECK(bytes != NULL,
           "a §6.2.3 read method reached its read operation with an argument that is not a Blob — the "
           "declaration brands it (Web IDL §3.2.15 Interface types), so this is the brand and this component's "
           "own `is` disagreeing rather than a value the page could pass");
    (void)bytes;
    d->task_loadstart = fr_queue(ctx, this_val, blob, FR_TASK_LOADSTART, type, label);
    d->task_progress = len ? fr_queue(ctx, this_val, blob, FR_TASK_PROGRESS, type, label)
                           : JS_TASK_HANDLE_NONE;
    d->task_done = fr_queue(ctx, this_val, blob, FR_TASK_DONE, type, label);
    return JS_UNDEFINED;
}

/* ---- §6.2.3.5 The abort() method ---------------------------------------------------------------------------
 *
 * A MACHINE, because steps 5 and 6 fire events SYNCHRONOUSLY — §2.9's dispatch runs the page's listeners inside
 * the abort() call, which is what wpt/FileAPI/fileReader.any.js pins by reassigning `onabort` on the line after
 * the call ("abort event should fire sync"). */

#define FR_ABORT_STAGES(X) \
    X(FRA_BEGIN, "File API §6.2.3.5 The abort() method steps 1-4 (an empty or done reader returns with its " \
                 "result nulled; a loading one goes done, drops its result, has its queued file reading tasks " \
                 "removed and its read operation terminated)") \
    X(FRA_ABORT, "File API §6.2.3.5 The abort() method step 5 (fire a progress event called abort at this)") \
    X(FRA_LOADEND, "File API §6.2.3.5 The abort() method step 6 (if this's state is not loading, fire a " \
                   "progress event called loadend at this)")
enum { IDL_STEP_STAGE_BASE(FR_ABORT_STAGES) FR_ABORT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FR_ABORT_STEPS[] = { FR_ABORT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t     phase;
    JSValue     ev;
    EventFireCb cb;
} FrAbortState;

static void js_fr_abort_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FrAbortState *s = st;
    int i;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* §6.2.3.5 step 3: "If there are any tasks from this on the file reading task source in an affiliated task
   queue, then remove those tasks from that task queue." A handle the record still holds names a task that has
   not STARTED — every task clears its own as its first act — so each removal must succeed, and one that does
   not is a task the queue has already handed to a flow with no JS in between to have called abort(). */
static void fr_remove_task(JSRuntime *rt, JSTaskHandle *h)
{
    int removed;

    if (*h == JS_TASK_HANDLE_NONE)
        return;
    removed = JS_RemoveQueuedTask(rt, *h);
    DCHECK(removed,
           "File API §6.2.3.5 step 3 could not remove a queued file reading task the reader still names — a "
           "handle is cleared by the task that owns it as it begins, so a handle still on the record names a "
           "task that has not started and must therefore still be on a queue. If this fires, the task and the "
           "handle disagree about which flow's timeline the queue belongs to");
    (void)removed;   /* the verdict is the assert's, and the assert compiles out of a release build */
    *h = JS_TASK_HANDLE_NONE;
}

static int js_fr_abort_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FrAbortState *s = st;
    FileReaderData *d = fr_of(hdr->this_val);
    JSValue in = cb_result;
    int r;

    (void)argc; (void)argv;

    STEP_DISPATCH(FR_ABORT_STAGES, hdr->stage, hdr->def->algorithm, -1);

    STEP_ARM(FRA_BEGIN);
    {
        int i;

        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;   /* CONSUMED — every arm below hands `in` to a request that takes ownership */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
        s->phase = 0;
        if (!d)
            return JS_ThrowTypeError(ctx, "abort called on something that is not a FileReader"), -1;
        /* Step 1: "If this's state is "empty" or if this's state is "done" set this's result to null and
           terminate this algorithm." — no event of any kind. */
        if (d->state != FR_LOADING) {
            fr_set(ctx, d, &d->result, JS_NULL);
            *presult = JS_UNDEFINED;
            return 0;
        }
        /* Step 2: "If this's state is "loading" set this's state to "done" and set this's result to null." */
        d->state = FR_DONE;
        fr_set(ctx, d, &d->result, JS_NULL);
        /* Step 3, and step 4's "Terminate the algorithm for the read method being processed" — which for a
           read whose whole remainder is queued tasks is the SAME operation: with the tasks off the queue there
           is no step of §6.2's step 10 left to run. */
        {
            JSRuntime *rt = JS_GetRuntime(ctx);
            fr_remove_task(rt, &d->task_loadstart);
            fr_remove_task(rt, &d->task_progress);
            fr_remove_task(rt, &d->task_done);
        }
        /* Step 5's event. §6.2.3.5 names neither of §5.1's operands and this read delivered nothing — its
           result has just been dropped — so both are 0. */
        s->ev = progress_event_new(ctx, "abort", 0, 0);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return -1; }
        STEP_GOTO(hdr->stage, FRA_ABORT, &s->phase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(FRA_ABORT);
    DCHECK(d != NULL, "§6.2.3.5's abort resumed at its event with no reader record");
    DCHECK(JS_IsObject(s->ev), "§6.2.3.5 step 5 resumed with no abort event to fire");
    r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), hdr->this_val, s->ev, JS_UNDEFINED, in, NULL,
                              out_cb, out_argc);
    in = JS_UNDEFINED;   /* CONSUMED by the request, on both of its legs */
    if (r > 0) return r;
    if (r < 0) return -1;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    /* Step 6: "If this's state is not "loading", fire a progress event called loadend at this." The abort
       handler may have started a new read, and §6.4.2 says that read's loadend is not this one's. */
    if (d->state == FR_LOADING) { *presult = JS_UNDEFINED; return 0; }
    s->ev = progress_event_new(ctx, "loadend", 0, 0);
    if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return -1; }
    STEP_GOTO(hdr->stage, FRA_LOADEND, &s->phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(FRA_LOADEND);
    DCHECK(JS_IsObject(s->ev), "§6.2.3.5 step 6 resumed with no loadend event to fire");
    r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), hdr->this_val, s->ev, JS_UNDEFINED, in, NULL,
                              out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    *presult = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl FR_ABORT_DECL = {
    js_fr_abort_step, sizeof(FrAbortState), js_fr_abort_visit, NULL,
    "File API §6.2.3.5 abort()", FR_ABORT_STEPS
};

/* ---- §6.2's three attributes -------------------------------------------------------------------------------- */

enum { FR_GET_READY_STATE = 0, FR_GET_RESULT, FR_GET_ERROR };

static JSValue js_fr_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    FileReaderData *d = fr_of(this_val);

    if (!d)
        return JS_ThrowTypeError(ctx, "a FileReader attribute was read on something that is not one");
    switch (magic) {
    /* "The readyState attribute's getter, when invoked, switches on this's state and runs the associated
       step" — and the three constants below number them in the same order. */
    case FR_GET_READY_STATE:
        DCHECK(d->state <= FR_DONE, "a FileReader holds a state §6.2.2 does not name");
        return JS_NewInt32(ctx, d->state);
    /* "The result attribute's getter, when invoked, must return this's result." */
    case FR_GET_RESULT:
        return JS_DupValue(ctx, d->result);
    /* "The error attribute's getter, when invoked, must return this's error." */
    default:
        DCHECK(magic == FR_GET_ERROR,
               "a FileReader attribute was declared with a magic this component does not answer");
        return JS_DupValue(ctx, d->error);
    }
}

/* ---- §6.2's constructor ------------------------------------------------------------------------------------ */

/* "The FileReader() constructor, when invoked, must return a new FileReader object." It runs none of the page's
   code, so it is a plain constructor rather than a machine. */
static JSValue js_fr_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValue obj, proto;
    FileReaderData *d;

    (void)argc; (void)argv;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor FileReader requires 'new'");
    proto = JS_GetClassProto(ctx, g_fr_class);
    DCHECK(!JS_IsNull(proto), "a FileReader was constructed in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_fr_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "FileReader: OOM building a FileReader");
    /* §6.2's three initial values, stated rather than left to calloc: a zeroed JSValue is the NUMBER 0. */
    d->state = FR_EMPTY;
    d->result = JS_NULL;
    d->error = JS_NULL;
    d->task_loadstart = d->task_progress = d->task_done = JS_TASK_HANDLE_NONE;
    JS_SetOpaque(obj, d);
    return obj;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

/* §6.2.2's three states, as `const unsigned short` on BOTH the interface object and its prototype — which is
   what Web IDL §3.7.5 Constants says a constant is, and what `reader.EMPTY` and `FileReader.LOADING` both read
   (wpt/FileAPI/reading-data-section/FileReader-multiple-reads.any.js uses each spelling). */
static const JSCFunctionListEntry FR_CONSTANTS[] = {
    JS_PROP_INT32_DEF("EMPTY", FR_EMPTY, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("LOADING", FR_LOADING, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("DONE", FR_DONE, IDL_CONSTANT_PROP_FLAGS),
};

/* Indexed by FileReadType, so a slot's declaration names the member it belongs to rather than a number. */
static const char *const FR_READ_SLOT[] = {
    "§6.2.3.1's readAsDataURL pool entry", "§6.2.3.2's readAsText pool entry",
    "§6.2.3.3's readAsArrayBuffer pool entry", "§6.2.3.4's readAsBinaryString pool entry",
};

void file_reader_init(JSContext *ctx)
{
    JSClassDef def = { "FileReader", .finalizer = fr_finalizer, .gc_mark = fr_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType READ_ARGS[1] = { IDL_INTERFACE };
    static const IdlArgType READ_TEXT_ARGS[2] = { IDL_INTERFACE, IDL_DOMSTRING };
    int i;

    DCHECK(!g_ready, "file_reader_init ran twice — one instance is one document is one agent");
    g_fr_rt = rt;
    JS_NewClassID(rt, &g_fr_class);
    JS_NewClass(rt, g_fr_class, &def);

    /* §6.2.3's four read methods. ONE body, four pool entries, because the four differ only by §6.3's `type`
       and by whether the IDL declares an `encoding` argument — which is exactly what a magic is for. Each
       brands its `Blob blob` argument against §3's class (Web IDL §3.2.15 Interface types), so
       `reader.readAsText("nope")` is a TypeError from the TYPE and not from a test written into the body. */
    g_id_read[FILE_READ_DATA_URL] = idl_method_id(ctx, READ_ARGS, 1, js_fr_read, FILE_READ_DATA_URL);
    idl_iface_brand(blob_class_id());
    g_id_read[FILE_READ_ARRAY_BUFFER] = idl_method_id(ctx, READ_ARGS, 1, js_fr_read, FILE_READ_ARRAY_BUFFER);
    idl_iface_brand(blob_class_id());
    g_id_read[FILE_READ_BINARY_STRING] = idl_method_id(ctx, READ_ARGS, 1, js_fr_read, FILE_READ_BINARY_STRING);
    idl_iface_brand(blob_class_id());
    g_id_read[FILE_READ_TEXT] = idl_method_id(ctx, READ_TEXT_ARGS, 2, js_fr_read, FILE_READ_TEXT);
    idl_iface_brand(blob_class_id());
    idl_optional_from(1);   /* §6.2's IDL: `readAsText(Blob blob, optional DOMString encoding)` */

    g_abort_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &FR_ABORT_DECL, 0);
    g_task_stepid = JS_RegisterStepDef(rt, &js_fr_task_def);

    g_ready = 1;
    agent_state_flag("file_reader", &g_ready, "the declaration latch");
    agent_state_ptr("file_reader", &g_fr_rt, "the runtime this interface's machines were registered in");
    agent_state_id("file_reader", &g_abort_stepid, "§6.2.3.5's abort machine");
    agent_state_id("file_reader", &g_task_stepid, "§6.1's file reading task machine");
    for (i = 0; i < 4; i++)
        agent_state_id("file_reader", &g_id_read[i], FR_READ_SLOT[i]);
    /* THE CLASS ID IS DELIBERATELY NOT DECLARED. core/agent_state.h states the consequence of declaring one:
       the release column runs BEFORE the collection that finalizes the page's object graph, so a class id
       reset there leaves this component's own finalizer reading `JS_GetOpaque(val, 0)` — NULL for every live
       reader, and a silent leak of the record and both of its owned values. The id is a registration in the
       runtime and goes with it. */
    realm_declare_intrinsic(file_reader_install_proto);
}

/* FILE API §6.2 "The FileReader API"' INTERFACE PROTOTYPE OBJECT *AND* ITS INTERFACE OBJECT, FOR ONE REALM.
   §6.2 declares `[Exposed=(Window,Worker)] interface FileReader: EventTarget`, and Web IDL §3.8 "Platform
   objects implementing interfaces"' `define the global property references` is "To define the global property
   references on target, given realm realm" whose step 1 is "Let interfaces be a list that contains every
   interface that is exposed in realm" — a REALM, with no Document in the algorithm. The interface object was
   placed from core/platform.c's per-DOCUMENT column, so a realm that reaches no platform_document_install got
   no `FileReader`: a worker realm always, and a Window realm until a Document was installed over it. Minting
   it here also removes the JS_GetClassProto re-read that entry made, which was a second answer to a question
   this function had just settled. */
void file_reader_install_proto(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_ready, "a realm asked for FileReader.prototype before file_reader_init declared it");
    prev = JS_GetClassProto(ctx, g_fr_class);
    DCHECK(JS_IsNull(prev), "file_reader_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* `interface FileReader: EventTarget` — the prototype is CREATED over §2.7's, so `addEventListener` is the
       same function rather than a copy of it, which is what the inheritance MEANS. */
    proto = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, proto, "FileReader");
    /* §6.2.1 Event Handler Content Attributes' six, which that section lists ON this interface. */
    event_target_install_handlers(ctx, proto, EH_FILE_READER);
    JS_SetPropertyFunctionList(ctx, proto, FR_CONSTANTS,
                               (int)(sizeof(FR_CONSTANTS) / sizeof(FR_CONSTANTS[0])));
    idl_install_accessor(ctx, proto, "readyState", js_fr_get, FR_GET_READY_STATE, -1);
    idl_install_accessor(ctx, proto, "result", js_fr_get, FR_GET_RESULT, -1);
    idl_install_accessor(ctx, proto, "error", js_fr_get, FR_GET_ERROR, -1);
    idl_install_method(ctx, proto, "readAsArrayBuffer", g_id_read[FILE_READ_ARRAY_BUFFER]);
    idl_install_method(ctx, proto, "readAsBinaryString", g_id_read[FILE_READ_BINARY_STRING]);
    idl_install_method(ctx, proto, "readAsText", g_id_read[FILE_READ_TEXT]);
    idl_install_method(ctx, proto, "readAsDataURL", g_id_read[FILE_READ_DATA_URL]);
    idl_install_method(ctx, proto, "abort", g_abort_stepid);

    /* §6.2's INTERFACE OBJECT, on THIS realm's global. Web IDL §3.7.5 "Constants" puts the four state values
       on the interface object AND on the prototype, which is why FR_CONSTANTS is installed twice and not
       moved. */
    ctor = JS_NewCFunction2(ctx, js_fr_ctor, "FileReader", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the FileReader interface object could not be allocated");
    JS_SetPropertyFunctionList(ctx, ctor, FR_CONSTANTS,
                               (int)(sizeof(FR_CONSTANTS) / sizeof(FR_CONSTANTS[0])));
    JS_SetConstructor(ctx, ctor, proto);
    /* THE HANDOVER IS LAST: JS_SetClassProto TAKES the reference, so `proto` is this function's until the realm
       owns it, and the Web IDL §3.7.1 Interface object pairing above reads a local rather than a class slot it
       has given away. */
    JS_SetClassProto(ctx, g_fr_class, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "FileReader", ctor);
    JS_FreeValue(ctx, global);
}

void file_reader_free(JSRuntime *rt)
{
    int i;

    DCHECK(g_ready, "FileReader was released in an agent that never declared it");
    DCHECK(rt == g_fr_rt, "FileReader was released against a runtime that is not the one it declared in");
    (void)rt;
    /* the prototype is the REALM'S — released with its context */
    g_ready = 0;
    g_fr_rt = NULL;
    g_abort_stepid = g_task_stepid = -1;
    for (i = 0; i < 4; i++)
        g_id_read[i] = -1;
}
