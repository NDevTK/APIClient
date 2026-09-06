/* READING A BYTE SEQUENCE AS A PROMISE — Fetch §5.3 "Body mixin"'s body readers and File API §3.3 "Methods
 * and Parameters"'s Blob readers.
 *
 * WHY IT IS NOT IN body.c, WHERE IT WAS WRITTEN. `blob.text()`, `blob.arrayBuffer()` and `blob.bytes()` are the
 * same three algorithms as `response.text()`, `response.arrayBuffer()` and `response.bytes()` — File API defines
 * them by the same "read all bytes" the Fetch body consumption is defined by. Leaving the machine under
 * core/fetch/ would have made File API depend on Fetch, and the dependency runs the OTHER way: BodyInit's union
 * names Blob, and Fetch's `blob()` reader returns one. So the machine lives where both can reach it, and each
 * spec declares its own readers into it.
 *
 * WHAT EACH SPEC KEEPS. The single-use latch is Fetch's, because a body is a stream; a Blob is an immutable byte
 * sequence and re-reads. `json()` and `formData()` are Fetch's members and not Blob's. Both facts arrive here as
 * an interface's own `take` and its own reader table, so neither is a condition this file tests.
 *
 * THE BYTES ARE ALREADY HERE, so the promise is settled before the page ever sees it — but SETTLING it is not a
 * C-private act. 27.5.1.3 CreateResolvingFunctions ( toResolve ) step 2.f reads `Get(resolution, "then")` off
 * the value, and the record `json()` parsed is an ordinary object whose prototype the page owns:
 * `Object.prototype.then = { get(){…} }` makes that read the page's code, and prototype pollution is a gadget
 * class this engine exists to run rather than assume away. Performed with a JS_Call from C it would run in an
 * activation with no flow base, so a loop in that getter would drive to completion. The resolving function is a
 * CALL REQUEST instead, and the read happens on the tramp where it can suspend and fork.
 *
 * AND THE VALUE THIS READER HANDS THE SETTLE IS NOT THAT RECORD — byte_reader_content wraps it in the solver's
 * unknown, which is a callable exotic whose [[Get]] answers every name before the prototype chain is walked at
 * all. The sentence above was therefore true of the value this file computes and FALSE of the value it hands
 * over, and the gap between the two is where every awaited reply was lost: step 2.f answered a callable
 * unknown, step 2.i took its true arm, and the promise was adopted into a `then` that settles nothing. That is
 * repaired where the read is performed rather than here — quickjs.c's resolving-function machine asks the
 * question of the value the run OBSERVED — so this paragraph is once again a true statement about a read that
 * really does reach the page's prototype. It is spelled out because the two changes were each correct and
 * their SEAM was not, which is not visible from either side alone. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/byte_reader.h"
#include "core/encoding/encoding.h"   /* §6's UTF-8 decode — what BOTH specs' `text()` and `json()` name */
#include "core/streams/readable_stream.h"
#include "solver/concolic.h"          /* the triple a byte sequence somebody else filled arrives as */

/* HOW MANY READERS ONE INTERFACE CAN DECLARE. Fetch declares five and File API three, so the ceiling is a
   DCHECK on a fixed platform rather than a growth path nobody exercises.
   THE LIST BELOW IS THE COUNT — see BYTE_READER_ROWS. This used to say the defs were static initialisers
   "because engine/check_step_visits.mjs reads them out of the source", a gate that pairs every declaration with
   the visit for its state struct. That gate no longer exists, so what the shape was preserving was a reader
   nobody had. The invariant it enforced is real and is now asserted where it cannot be written around:
   js_step_def_check runs at JS_RegisterStepDef and refuses a def declaring no visit — which covers a
   macro-generated def and a runtime-assembled one identically, where reading the source covered neither. */
#define BYTE_READER_MAX 8
#define BYTE_READER_IFACE_MAX 4

static const ByteReaderIface *g_iface[BYTE_READER_IFACE_MAX];
static int g_iface_stepid[BYTE_READER_IFACE_MAX][BYTE_READER_MAX];
static int g_iface_n;

/* WHICH INTERFACE THE RECEIVER BELONGS TO is found from the receiver, not carried in the def's `arg` — `arg` is
   the reader INDEX, which is what keeps the defs static. */
static const ByteReaderIface *iface_of(JSValueConst v)
{
    int i;
    for (i = 0; i < g_iface_n; i++)
        if (g_iface[i]->is(v)) return g_iface[i];
    return NULL;
}

/* WHERE THIS MACHINE RESTS. §5.3 "Body mixin"'s "consume body" is seven steps, and its sixth delegates to
   "fully read a body", whose steps 4 and 5 are the two CALLS a stream-backed body takes before there are any
   bytes — acquiring a reader and issuing the first read. Both are precisely why this is a machine: it can park on
   them. The LOOP after them is not here, which is why readable_stream_drain owns it.
   A BODY THAT IS ALREADY BYTES skips both and goes straight from step 5 to the settle. */
#define BYTE_READER_STAGES(X) \
    X(BR_TAKE, \
      "Fetch §5.3 Body mixin's consume body steps 1-5 (the unusable refusal, the promise, and — for a null " \
      "already-read body — convertBytesToJSValue over the bytes)") \
    X(BR_ACQUIRE, \
      "Fetch §5.3 Body mixin's consume body step 6 → fully read a body step 4 (get a reader for the stream)") \
    X(BR_FIRST_READ, \
      "Fetch §5.3 Body mixin's consume body step 6 → fully read a body step 5 (read all bytes from the reader)") \
    X(BR_SETTLE, \
      "Fetch §5.3 Body mixin's consume body steps 3-4 (resolve promise with the converted value, or reject " \
      "error) — 27.5.1.3 step 2.f's `then` read reaches the page's own prototype chain")
enum { BYTE_READER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_byte_reader_steps[] = { BYTE_READER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSByteReaderState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   cphase;   /* the settle call's own phase, so the stage can hold it across a suspension */
    JSValue   promise;  /* the capability's promise — this machine's result (owned) */
    JSValue   func;     /* its resolve or its reject, whichever this read settles with (owned) */
    JSValue   value;    /* what it settles WITH: the text, the parsed body, or the error (owned) */
    JSValue   stream;   /* the body's stream, when the body IS one (owned) */
    JSValue   reader;   /* the reader acquired on it (owned) */
    JSValue   cb[3];    /* the call request buffer: [this, resolving function, value] */
} JSByteReaderState;

/* WHAT THIS MACHINE OWNS. The call buffer is in here for the reason dispatch's is: a `then` getter that forks
   the flow must not leave two arms sharing one invocation. */
static void js_byte_reader_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSByteReaderState *s = st;
    int k;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    v->val(ctx, &s->stream);
    v->val(ctx, &s->reader);
    for (k = 0; k < 3; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_byte_reader_fini(JSContext *ctx, void *st, bool take_result)
{
    JSByteReaderState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;

    (void)ctx;
    if (take_result) s->promise = JS_UNDEFINED;
    return r;
}

/* The TAKE stage turns the bytes into the value this reader answers with; the SETTLE stage settles the promise
   with it, which is the request. Nothing in the take runs the page's code — the bytes are the host's and the
   latch, where there is one, is the declaring component's — which is why every reader is one machine with an
   index and not one machine each. */
static int js_byte_reader_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSByteReaderState *s = st;
    JSValue settled, funcs[2];
    int reject = 0, r;

    if (s->hdr.stage == BR_TAKE) {
        const char *bytes = NULL;
        size_t len = 0;
        const ByteReaderIface *f = iface_of(s->hdr.this_val);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->func = s->value = s->stream = s->reader = JS_UNDEFINED;
        if (!f) {
            /* THE RECEIVER CHECK, and it belongs to the reader rather than to the interface: every one of them
               is `Response.prototype.text.call({})`, and there is one answer. */
            JS_ThrowTypeError(ctx, "a byte reader was called on an object of no interface that has one");
            return JS_STEP_ABRUPT;
        }
        DCHECK(s->hdr.arg >= 0 && s->hdr.arg < f->nreaders,
               "a byte reader ran with an index its interface does not declare");
        if (f->take(ctx, s->hdr.this_val, &bytes, &len, &s->stream) < 0) {
            s->value = JS_GetException(ctx);
            reject = 1;
        } else if (!JS_IsUndefined(s->stream)) {
            STEP_GOTO(s->hdr.stage, BR_ACQUIRE, &s->cphase, NULL);
        } else {
            s->value = f->readers[s->hdr.arg].make(ctx, s->hdr.this_val, bytes ? bytes : "", bytes ? len : 0);
            if (JS_IsException(s->value)) { s->value = JS_GetException(ctx); reject = 1; }
        }
        if (s->hdr.stage == BR_TAKE) {
            /* The NATIVE capability: %Promise% with no subclass in sight, so building it constructs nothing of
               the page's. Only the settle below is the page's, and that is the request. */
            s->promise = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
            s->func = funcs[reject];
            JS_FreeValue(ctx, funcs[reject ^ 1]);
            STEP_GOTO(s->hdr.stage, BR_SETTLE, &s->cphase, NULL);
        }
    }

    if (s->hdr.stage == BR_ACQUIRE) {
        JSValue op = readable_stream_op(ctx, RS_OP_GET_READER);
        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), op, s->stream, 0, NULL,
                          cb_result, &s->reader, out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(s->reader)) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, BR_FIRST_READ, &s->cphase, NULL);
    }
    if (s->hdr.stage == BR_FIRST_READ) {
        const ByteReaderIface *f = iface_of(s->hdr.this_val);
        JSValue read_promise;
        DCHECK(f != NULL, "a byte reader lost its interface between two of its own stages");
        JSValue op = readable_stream_op(ctx, RS_OP_READ);
        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), op, s->reader, 0, NULL,
                          cb_result, &read_promise, out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(read_promise)) return JS_STEP_ABRUPT;
        /* The DRAIN owns the settle from here: it holds the accumulating bytes, it reacts to each read, and it
           resolves with what this reader's `make` builds. So this machine's result IS its promise. */
        s->promise = readable_stream_drain(ctx, s->reader, read_promise, s->hdr.this_val,
                                           f->readers[s->hdr.arg].make);
        JS_FreeValue(ctx, read_promise);
        return JS_IsException(s->promise) ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }

    DCHECK(s->hdr.stage == BR_SETTLE,
           "the byte-read machine was re-entered at a stage §5.3 Body mixin does not have");
    r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), s->func, JS_UNDEFINED, 1, (JSValueConst *)&s->value,
                      cb_result, &settled, out_cb, out_argc);
    if (r > 0) return r;          /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
    JS_FreeValue(ctx, settled);   /* a resolving function's return value is undefined and unobservable */
    return JS_STEP_DONE;
}

/* ONE machine, one def per reader INDEX — the code is one function and `arg` is the index, which with the
   receiver is the whole of the difference between them. Eight rows differing in one integer, so ONE row
   written once: the eight longhand copies existed only to be legible to a source-reading gate that has since
   been deleted, and eight chances to mistype .visit is what that legibility cost.
   BYTE_READER_ROWS IS THE COUNT, and the static assertion below is what keeps BYTE_READER_MAX honest about it:
   a ninth reader is one F(8) here, and forgetting to raise the ceiling stops the build rather than leaving a
   NULL row for a driver to be handed — which is the failure js_step_defs_check_table caught in the engine's own
   table on the day it was written. */
#define BYTE_READER_ALGORITHM "Fetch §5.3 Body mixin's consume body (also File API §3.3 Methods and " \
                              "Parameters)"
#define BYTE_READER_ROWS(F) F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7)
#define BYTE_READER_ROW(i) \
    { sizeof(JSByteReaderState), js_byte_reader_step, js_byte_reader_fini, (i), .visit = js_byte_reader_visit, \
      .algorithm = BYTE_READER_ALGORITHM, .steps = js_byte_reader_steps },
static const JSTrampStepDef js_byte_reader_defs[BYTE_READER_MAX] = {
    BYTE_READER_ROWS(BYTE_READER_ROW)
};
#define BYTE_READER_COUNT_ONE(i) + 1
_Static_assert(BYTE_READER_ROWS(BYTE_READER_COUNT_ONE) == BYTE_READER_MAX,
               "BYTE_READER_ROWS and BYTE_READER_MAX disagree — the array would carry a row with no definition");
#undef BYTE_READER_COUNT_ONE

int byte_reader_declare(JSContext *ctx, const ByteReaderIface *d)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int handle = g_iface_n, k;

    DCHECK(g_iface_n < BYTE_READER_IFACE_MAX,
           "more interfaces declared byte readers than this table holds — grow it, the count is fixed because "
           "the platform's is");
    DCHECK(d->nreaders > 0 && d->nreaders <= BYTE_READER_MAX,
           "an interface declared more byte readers than there are step defs for — add an F(n) to "
           "BYTE_READER_ROWS and raise BYTE_READER_MAX; the static assertion beside them refuses one without "
           "the other");
    for (k = 0; k < d->nreaders; k++)
        DCHECK(d->readers[k].name && d->readers[k].make, "a byte reader was declared with no name or no maker");
    g_iface[handle] = d;
    for (k = 0; k < d->nreaders; k++) {
        g_iface_stepid[handle][k] = JS_RegisterStepDef(rt, &js_byte_reader_defs[k]);
        CHECK(g_iface_stepid[handle][k] >= 0, "byte reader: no step id — the bytes would be unreadable");
    }
    g_iface_n++;
    return handle;
}

void byte_reader_install(JSContext *ctx, JSValueConst proto, int handle)
{
    const ByteReaderIface *d;
    int k;
    DCHECK(handle >= 0 && handle < g_iface_n, "byte readers were installed with a handle nothing declared");
    d = g_iface[handle];
    for (k = 0; k < d->nreaders; k++)
        JS_SetPropertyStr(ctx, (JSValue)proto, d->readers[k].name,
                          JS_NewCFunction2(ctx, NULL, d->readers[k].name, 0, JS_CFUNC_step,
                                           g_iface_stepid[handle][k]));
}

/* ---- the readers both specs declare in the same words ------------------------------------------------------ */

/* A BYTE SEQUENCE SOMEBODY ELSE FILLED BECOMES §solver's TRIPLE, AND ONLY THE TWO CONTENT READERS ASK.
 *
 * §solver's trust boundary states the whole rule and states it about this exact case: "a config/data fetch is
 * ALWAYS loaded so its fields become concrete examples, while its use in a BRANCH still forks (a loaded
 * `features.admin:false` must NOT concretize the gate, or the admin endpoint is lost — config is
 * opaque-for-control-flow yet carries its loaded value as the example)". A record handed back as a PLAIN
 * object answers both halves wrong at once: `cfg.region` is concrete, which is right, but `if (cfg.admin)`
 * over a member the payload holds as `false` takes one arm, and `cfg.admin` over a member it does NOT hold
 * answers `undefined` — and the admin surface behind either gate is never reached. Wrapping the value in the
 * triple fixes both from one place, because solver/concolic.c's exotic [[Get]] then reads each member THROUGH
 * the example: a member the record holds arrives with the bytes that were actually sent, a member it does not
 * hold arrives with none, and both are opaque for control flow so the gate forks either way.
 *
 * IT IS NOT A SECOND JSON IMPLEMENTATION AND NOT A TAINT TRACKER. The value being wrapped is what the REAL
 * codec produced — §6's UTF-8 decode and 25.5.1's own parser, run on the real bytes — and nothing here derives
 * a transform expression from it or predicts what an operation would have made of it. The wrap states one
 * fact, provenance, and hands the computed value over as the example; every later operation over it runs for
 * real, which is what §Re-execution requires and what a recorded expression cannot do.
 *
 * THE OTHER READERS DO NOT ASK, and that is a fact about their VALUES rather than an omission. `arrayBuffer()`,
 * `bytes()` and `blob()` answer with a CONTAINER over the same bytes, not with the content: the page writes
 * `new Uint8Array(await r.arrayBuffer())`, and a concolic in the container's place breaks the construction
 * instead of describing it. `formData()`'s entries are values the page reads, and they are a separate reader's
 * to state when its parser is taught to carry them — recorded here rather than silently included, because a
 * wrap applied to a container is a defect and an unasked question is a gap.
 *
 * `value` is CONSUMED. A host that is not exploring gets exactly what its spec says to return. */
static JSValue byte_reader_content(JSContext *ctx, JSValueConst recv, JSValue value)
{
    const ByteReaderIface *f;
    bool attacker = false;
    char *src, *shape;
    size_t n;
    JSValue r;

    if (JS_IsException(value) || !concolic_is_exploring())
        return value;
    f = iface_of(recv);
    /* The reader machine's own receiver check has already answered for this, so a NULL here is an interface's
       `is` and the machine that dispatched to its reader disagreeing about what the receiver is. */
    DCHECK(f != NULL, "a byte reader's value reached the provenance question with a receiver of no interface "
                      "that has readers — the machine checks the receiver before it calls a reader at all");
    if (!f || !f->source)
        return value;
    src = f->source(ctx, recv, &attacker);
    if (!src)
        return value;   /* these bytes are the page's own — a positive statement, see ByteReaderIface.source */
    /* TWO DOORS WRAPPING ONE READ would report a derivation the run never made: the composed shape would name
       a read of a read. The readers below are the only callers and each calls this once, so an arrival that is
       already the triple is a reader having wrapped its own value before handing it here. */
    DCHECK(!concolic_is(value),
           "a byte reader's value reached the provenance question already carrying one — two doors have "
           "wrapped one read, and the value would report a derivation the run never performed");
    n = strlen(src) + 3;
    shape = (char *)malloc(n);
    CHECK(shape != NULL, "byte reader: OOM spelling the provenance of a byte sequence");
    snprintf(shape, n, "{%s}", src);   /* a declared source's shape IS its provenance in braces — concolic.h */
    /* THE TWO KINDS ARE MINTED THROUGH DIFFERENT DOORS BECAUSE ONE OF THEM IS COUNTED. concolic_source_wrap is
       "the one point at which this document's run acquires attacker-controlled input" and increments the count
       an empty @S surface is read against; server-injected state is unknown input the attacker did not author,
       so minting it there would report a page that read no attacker source as one that read many. */
    r = attacker ? concolic_source_wrap(ctx, shape, src, value)
                 : concolic_new(ctx, shape, src, value);
    free(shape);
    free(src);
    return r;
}

JSValue byte_reader_text(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    /* Fetch §5.3 "Body mixin" / File API §3.3 "Methods and Parameters": "run consume body with this and
       UTF-8 decode" — ENCODING §6's UTF-8 decode, which is a named algorithm and not a synonym for
       whatever the host's string constructor does. This read
       `JS_NewStringLen`, and quickjs's decoder is not that algorithm in either direction: cutils.h converts an
       encoding error to U+FFFD "and uses a single byte" (Encoding consumes the whole maximal subpart) and it
       "accepts UTF-8 encoded surrogates as JavaScript allows them in strings" (Encoding answers U+FFFD). It
       also does not run §6's step 2, the three-byte peek that DROPS a leading UTF-8 BOM, so `res.text()` on a
       BOM'd body answered a string starting U+FEFF. The difference was invisible while the body reaching this
       reader was itself the output of a decode; it is not invisible now that a body is bytes. */
    char *text;
    size_t n = 0;
    JSValue out;

    text = encoding_utf8_decode(bytes, len, &n);
    CHECK(text != NULL, "byte reader: OOM running §6's UTF-8 decode over a body");
    out = JS_NewStringLen(ctx, text, n);
    free(text);
    return byte_reader_content(ctx, recv, out);
}

JSValue byte_reader_json(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    /* Fetch §5.3 "Body mixin"'s `json()` is "run consume body with this and parse JSON from bytes", and
       Infra's parse JSON FROM BYTES is TWO steps: "let string be the result of running UTF-8 decode on
       bytes", then parse JSON from string. Handing the bytes straight to the parser ran quickjs's lenient
       decoder instead, so a body whose bytes are not well-formed UTF-8 parsed as something Encoding's
       decoder would have replaced.
       The REAL parser runs on the decoded string, so a malformed body rejects with the SyntaxError the page
       would actually catch rather than a placeholder this engine invented. */
    char *text;
    size_t n = 0;
    JSValue out;

    text = encoding_utf8_decode(bytes, len, &n);
    CHECK(text != NULL, "byte reader: OOM running §6's UTF-8 decode over a body before parsing it");
    out = JS_ParseJSON(ctx, text, n, "<body>");
    free(text);
    /* THE PARSE ARM ONLY. A body that is not a JSON text left a SyntaxError standing and `out` is the
       exception, which byte_reader_content hands straight back so §5.3 rejects with the throw the page
       catches — a record this reader never built has no provenance to carry. */
    return byte_reader_content(ctx, recv, out);
}

/* A COPY, for both of these, because what the page gets is ITS OWN to detach, transfer or write through —
   handing out the object's storage would let one flow mutate a reply another is still reading, and a detach
   would leave that flow reading freed memory.

   AND NEITHER OF THESE TWO CALLS byte_reader_content, WHICH IS THE ONE ASYMMETRY IN THIS TABLE AND WAS
   UNSTATED. `text` and `json` hand their value through the provenance door and these hand back the raw
   container, so ONE byte sequence answers a page as unknown through two readers and as concrete through
   the other two. That is not an oversight and the `(void)recv` is not laziness: the solver's unknown is a
   callable exotic whose [[Get]] answers every name before the prototype chain is walked, so wrapping an
   ArrayBuffer in one would make `ab instanceof ArrayBuffer` and every typed-array operation over it answer
   about the wrapper instead of the buffer — a Web IDL §3.2 brand this reader would be breaking to carry a
   taint. The wrap is right for a STRING and a parsed RECORD, whose members a page reads by name, and wrong
   for a container a page reads by index and brand.

   NAMED RESIDUAL — THE BYTES' UNKNOWN-NESS IS LOST AT THE CONTAINER BOUNDARY.
   WHAT IS NOT COVERED: a reply read as a container carries no provenance at all, so `ab.byteLength`,
   `by.length` and `by[0]` are CONCRETE and a page's branch on any of them is DECIDED where the identical
   bytes through `text()` or `json()` FORK. §Solver-half forbids collapsing a modelable value to
   bare-concrete for exactly this reason — it deletes the fork and every arm behind it — and this is that
   collapse, arriving at a container rather than at a scalar.
   WHAT THE NEXT DIFF BUILDS: NOT a wrapped container, which the paragraph above rules out. It is a buffer
   whose ELEMENTS are the unknown — the triple riding the storage the brand still describes — so `by[0]` is
   unknown while `by instanceof Uint8Array` stays true. That is a change to what a byte sequence IS in this
   engine and not a change to this table, which is why it is named here rather than attempted.
   HOW ITS ABSENCE WOULD SHOW: a page that gates on a reply's length or leading byte takes exactly one arm,
   with no sibling and nothing anywhere to say a world was lost. The fixture already asks it — the
   `body-bytes` row asserts `len`, `n` and `b0` off one reply read two ways — so a run in which that row
   answers while a byte-gated arm is still missing is this residual firing rather than being retired. */
JSValue byte_reader_array_buffer(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    (void)recv;
    return JS_NewArrayBufferCopy(ctx, (const uint8_t *)bytes, len);
}

JSValue byte_reader_bytes(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    (void)recv;
    return JS_NewUint8ArrayCopy(ctx, (const uint8_t *)bytes, len);
}
