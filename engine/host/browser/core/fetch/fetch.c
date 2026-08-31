/* THE FETCH API — Blink core/fetch, as a STEP MACHINE.
 *
 * `fetch(input, init)`: the request's URL comes from `input` (a USVString, or a Request whose `url` attribute
 * names it) and its method from `init.method`, defaulting to GET. Every request forced execution reaches
 * funnels one endpoint into the @H surface, whether or not it ever fires — that is the tool's headline output,
 * and endpoint_record owns the shape/literal decision.
 *
 * IT IS A STEP MACHINE BECAUSE ITS ARGUMENTS ARE THE PAGE'S OBJECTS. Both IDL reads are ordinary [[Get]]s on
 * values the page supplied, so each is one accessor or Proxy trap away from being the page's code — and running
 * that from a C builtin with no flow base is what the engine's twelve internal-method entries abort on. The
 * first version of this file read them with JS_GetPropertyStr and aborted on
 * `fetch(new Proxy({url:"/q"}, {get(){}}))` immediately. Each read is a REQUEST now: the machine parks, the
 * trap runs on the tramp where it can suspend and fork, and the machine is re-entered with the answer.
 *
 * IT DOES NOT FETCH. SECURITY.md puts every byte of network behind the trusted safeFetch chokepoint the sandbox
 * cannot reach, so the promise is PENDING and the URL goes on the flow's own register: the trusted host fetches
 * it, qjs_provide fills the entry, and the flow's continuation resumes with the body. The flow cannot finish
 * while a reply is owed, which is what keeps reply-gated code reachable. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/concolic.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/endpoint.h"
#include "solver/engine.h"     /* the ONE composition of what a request this running code builds is evidence of */
#include "solver/multipart_batch.h"
#include "core/agent_state.h"
#include "core/fetch/fetch.h"
#include "core/fetch/scheme_fetch.h"
#include "core/file/blob.h"
#include "core/frame/location.h"
#include "core/dom/document.h"
#include "core/fetch/headers.h"
#include "core/fetch/port_blocking.h"
#include "core/fetch/body.h"
#include "core/byte_reader.h"   /* Infra's parse JSON from bytes, shared with `Response.json()` */
#include "core/fetch/response.h"
#include "core/streams/readable_stream.h"
#include "core/fetch/request.h"

/* The id JS_RegisterStepDef handed this runtime, and the two IDL attribute names the machine reads by. Atoms
   belong to a runtime, and SECURITY.md gives this build one WASM instance per DOCUMENT — one runtime — so these
   are that runtime's. The DCHECK at install is what makes a second one impossible rather than merely unlikely. */
static const FetchProvider *g_provider;

/* §4.4's URL parse, against HTML's API base URL. Both of Fetch's entry points that take a URL STRING come
   here, so a relative endpoint — which is how a bundle names its own API — resolves the same way from both.
   A document with no address has no base, and the parse is then absolute-only: that is the honest answer for a
   platform-less build rather than a base this component invented. */
bool fetch_parse_url(JSContext *ctx, UrlRecord *rec, const char *url, size_t len)
{
    /* THE BASE IS THE READING REALM'S DOCUMENT BASE URL, asked of the Document rather than of a module-static
       copy. HTML says "the current settings object's API base URL", which §8.1.5.1 defines as "the current
       BASE URL of window's associated Document" — §2.4.3's document base URL, so a page shipping
       `<base href="/app/v2/">` moves every `fetch("api/users")` in it and this is the line that carries that.
       With one realm per same-origin document the current settings object is whichever realm is running: an
       opener and a popup have two, and one stored copy answers with whichever installed last. */
    const char *base_str = document_base_url(ctx);
    UrlRecord base;
    bool ok;

    if (!base_str)
        return url_parse(rec, url, len, NULL);
    url_record_init(&base);
    ok = url_parse(&base, base_str, strlen(base_str), NULL) && url_parse(rec, url, len, &base);
    url_record_free(&base);
    return ok;
}

void fetch_set_provider(const FetchProvider *p) { g_provider = p; }

/* FETCH §2.2.5 "Requests"' DESTINATION TYPE, ENUMERATED — see fetch.h for why it is an export of THIS
   component rather than a `static` in one of its three consumers. The list is the standard's, in the
   standard's order: "the empty string, `audio`, `audioworklet`, `document`, `embed`, `font`, `frame`,
   `iframe`, `image`, `json`, `manifest`, `object`, `paintworklet`, `report`, `script`, `serviceworker`,
   `sharedworker`, `style`, `text`, `track`, `video`, `webidentity`, `worker`, or `xslt`".
   `fetch` IS NOT IN IT, and that absence is the whole reason §2.2.7 "Miscellaneous" has to define a POTENTIAL
   destination separately ("`fetch` or a destination which is not the empty string") and a translation from one
   to the other. A caller holding an `as`-attribute keyword holds a potential destination, not a destination. */
bool fetch_is_destination_type(const char *destination)
{
    static const char *const TYPES[] = {
        "", "audio", "audioworklet", "document", "embed", "font", "frame", "iframe", "image", "json",
        "manifest", "object", "paintworklet", "report", "script", "serviceworker", "sharedworker",
        "style", "text", "track", "video", "webidentity", "worker", "xslt"
    };
    size_t i;

    if (!destination) return false;
    for (i = 0; i < sizeof TYPES / sizeof *TYPES; i++)
        if (!strcmp(destination, TYPES[i])) return true;
    return false;
}

/* THE SAME SEAM, FOR A COMPONENT WHOSE OWN STANDARD SAYS "FETCH REQUEST" — see fetch.h. `value` is the second
   parameter the provider takes and is JS_UNDEFINED here for the reason it is at `fetch()`'s own call: the
   URL form of the park owes its answer to the host, and a value supplied up front is a reply nobody asked
   for.
   AND §4.3 SCHEME FETCH RUNS HERE, WHICH IS WHY THIS IS A SEAM AND NOT A FORWARDER. Every component whose
   standard says "fetch request" reaches the host through this one function, so this is the point at which the
   question "who answers these bytes" can be asked ONCE for all of them — and it was being asked at `fetch()`
   and at XMLHttpRequest and NOWHERE ELSE, so an `<img src="data:image/svg+xml,…">` and a
   `<link rel=preload>` of one went to the trusted zone, which can fetch nothing but an HTTP(S) scheme and
   answered a refusal the page cannot tell from a network failure. §4.3 answers it with a 200 out of bytes that
   were already in this address space, and `deliver` is exactly the processResponse steps that answer needs. */
void fetch_owe(JSContext *ctx, JSValueConst deliver, const FetchRequest *req)
{
    DCHECK(g_provider != NULL && g_provider->owe != NULL,
           "a browser component owed a request with no host network provider installed — the reply would be "
           "owed to nobody and the flow that issued it could never finish");
    DCHECK(req != NULL && req->url != NULL && *req->url,
           "a request was owed to the host with no address — the reply seam is keyed on (method, url), so a "
           "request with neither names nothing the host can be asked for");
    DCHECK(req->method != NULL && *req->method,
           "a request was owed to the host without stating its METHOD — Fetch §2.2.5 \"Requests\" gives every "
           "request one (`GET` unless stated otherwise), so a component that reached this seam unnamed "
           "dropped a field rather than made a request that lacks it");
    /* AND ITS DESTINATION, ASSERTED HERE AS WELL AS AT THE PARK because THIS is where the component that
       dropped it is still on the stack. Fetch §2.2.5 "Requests" gives every request a destination, and the
       trusted zone decides from it whether the reply may be ingested as CODE — so a component that reaches
       this seam without one is not making a request that lacks the field, it is making one whose reply will be
       classified by silence. The EMPTY STRING is §2.2.5's own default and passes: what does not is NULL.
       AND IT IS THE ENUMERATION AND NOT MERELY PRESENCE, which is the correction. Asking only "is it there"
       admitted a word §2.2.5 does not define, and the consumers do not fail alike on one: CSP §6.8.1's
       trailing "Return connect-src" answers an unlisted row, the chokepoint's script-like predicate answers
       false for one, and the pending join aborts — so a component stating `fetch` (the `as` keyword, which
       §2.2.7 Miscellaneous translates to the EMPTY string and which is not a destination) reached three
       consumers, was right at two of them by accident, and killed a dev build at the third with the producing
       component long gone from the stack. Here it is still on it. */
    DCHECK(fetch_is_destination_type(req->destination),
           "a request was owed to the host stating a DESTINATION that is not one Fetch §2.2.5 Requests "
           "enumerates — §2.2.5 gives every request one (\"unless stated otherwise it is the empty string\") "
           "and the CORB class is read off it. State the destination the algorithm creating this request "
           "names (`image` at §4.8.4.3.5, `script` at §8.1.4.2, the empty string at §5.6's fetch()), and if "
           "you hold an `as`-attribute keyword run Fetch §2.2.7 Miscellaneous' translate a potential "
           "destination over it first — `fetch` is a POTENTIAL destination and not a destination");
    /* §4.3 SCHEME FETCH, FIRST. A scheme this agent answers is answered here and the host is never told about
       it; only §4.3's "HTTP(S) scheme" arm reaches the provider. The blob URL entry is JS_UNDEFINED because no
       standard but §5.4's Request constructor has one to have captured (core/fetch/scheme_fetch.h). */
    if (scheme_fetch_answer(ctx, deliver, req, JS_UNDEFINED))
        return;
    g_provider->owe(ctx, deliver, JS_UNDEFINED, req);
}

static int    g_fetch_stepid = -1;
static JSAtom g_atom_method, g_atom_url, g_atom_headers, g_atom_body;
static JSRuntime *g_fetch_rt;

typedef struct JSFetchState JSFetchState;
struct JSFetchState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   method;   /* the answer to the `init.method` read, or undefined */
    JSValue   url;      /* the answer to the `input.url` read, or the USVString input itself */
    JSValue   input;    /* the argument ITSELF — a Request carries a captured blob URL entry the URL cannot */
    JSValue   hinit;    /* the answer to the `init.headers` read — a HeadersInit the fill converts */
    JSValue   binit;    /* the answer to the `init.body` read — a BodyInit §5.2's extraction turns into bytes */
    BodyState body;     /* those bytes. A POST that dropped its body asked the server a different question */
    /* §5.2's Content-Type for that body, as a JS STRING rather than a malloc'd one: a step state is BYTE-COPIED
       at a deep fork and only what `visit` names is re-taken, so a heap pointer here would be freed by both
       arms. JS_UNDEFINED for a body whose arm has no type. */
    JSValue   body_mime;
    /* §5.6: the promise a FAILED request build settles into. `fetch` never throws, so an abrupt completion
       becomes this and `fini` yields it instead of parking a request that was never valid. */
    JSValue   reject_promise;
    /* A FLAG, not a test on the slot. A zeroed step state's JSValue is the INTEGER 0 and not JS_UNDEFINED, so
       "is there a rejection here" asked of the slot answers YES for every request that never failed — which
       yielded the integer 0 where the promise belonged. Third time this trap has been paid for; the slot is
       never the question. */
    uint8_t   rejected;
    HeadersFill fill;   /* the fill's cursor: it parks per key, so it cannot be a loop here */
    HeaderList  hdrs;   /* what the request carries, which is half of what makes the endpoint usable */
};

/* WHAT THIS MACHINE OWNS. Declared once and read by both consumers: the teardown releases each, and the
   deep-fork clone takes a second reference — a concolic branch inside the `url` getter forks the flow at that
   depth, and the two arms must not share one answer slot. */
static void js_fetch_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFetchState *s = st;
    v->val(ctx, &s->method);
    v->val(ctx, &s->url);
    v->val(ctx, &s->input);
    v->val(ctx, &s->hinit);
    v->val(ctx, &s->binit);
    v->val(ctx, &s->body_mime);
    v->val(ctx, &s->reject_promise);
    headers_fill_visit(ctx, &s->fill, v);   /* the fill's own slots — it parks mid-conversion like any machine */
}

/* Park the request on the FLOW that issued it: the URL the trusted host must fetch, and the capability that
   delivers the body into this flow's continuation. There is ONE pending register and it is the flow's own —
   a second one beside it cannot resolve into the flow that owns the reaction, which is the whole point. */
/* THE REPLY BECOMES A RESPONSE HERE, in the component that promised one. The host delivers a reply RECORD and
   knows nothing about the Fetch API; this closure sits between them, so `fetch()` keeps its contract (a
   Promise<Response>) without the host learning what a Response is. func_data = [resolve, reject].
   A NULL delivery is §5.6's NETWORK ERROR and REJECTS with a TypeError. It used to be stringified into the
   body like any other value, so a request the host could not satisfy resolved with a Response reading "null"
   — a page's `.catch` never ran, and the failure arrived disguised as data.
   THE URL IS NOT CAPTURED HERE ANY MORE. This closure held the REQUEST's URL and handed it to the Response as
   its `url`, which is a different fact from the one §2.2.6 defines: the response's URL is the LAST item of its
   URL LIST, and the list is what the fetch — which is the trusted host's — observed. A captured request URL
   cannot express a redirect at all, so `redirected` had nothing to read and was the literal `false`. The list
   rides the reply record instead, and this capture is gone with the question it answered. */
/* THE REPLY'S DELIVERY IS A STEP MACHINE, because settling the promise is the PAGE'S code. 27.5.1.3 step 2.f
 * reads `Get(resolution, "then")` off the value being resolved with, and the value here is a Response — an
 * ordinary object whose prototype the page owns, so `Object.prototype.then = { get(){…} }` makes that read the
 * page's, and prototype pollution is a gadget class this engine exists to RUN rather than assume away. It was a
 * JS_Call out of the host's pump: an activation with no flow base, where a loop in that getter drives to
 * completion. body.c fixed exactly this for its readers; this is the same defect one layer out.
 *
 * It is a CLOSURE machine — JS_NewStepClosure — because a reaction knows which promise it belongs to only by
 * capture, and the work it does (settling) is work only a machine may do. Those two were mutually exclusive
 * before: JS_NewCFunctionData gives capture without the machine, JS_CFUNC_step gives the machine without
 * capture. What it captured is read back off the callee its own header carries. */
enum { FETCH_DELIVER_RESOLVE = 0, FETCH_DELIVER_REJECT };

/* WHERE THIS MACHINE RESTS. §5.6 step 12's processResponse is what a reply runs, and its five sub-steps split
   exactly once: everything up to and including "creating a Response object" runs on the engine's own values,
   and the settle is the page's code. */
#define FETCH_DELIVER_STAGES(X) \
    X(FETCH_DELIVER_BUILD, "Fetch §5.6 fetch(input, init) step 12's processResponse steps 3-4 (a network " \
                           "error's TypeError, or the Response object for the reply)") \
    X(FETCH_DELIVER_SETTLE, "Fetch §5.6 fetch(input, init) step 12's processResponse step 3 or 5 (reject p " \
                            "with the TypeError, or resolve p with responseObject)")
enum { FETCH_DELIVER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_fetch_deliver_steps[] = { FETCH_DELIVER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   cphase;   /* the settle call's own phase, held across a suspension */
    uint8_t   reject;   /* which of the capability's two functions this delivery settles with */
    JSValue   value;    /* the Response, or the TypeError a null delivery makes (owned) */
    JSValue   cb[3];    /* the call request buffer: [this, resolving function, value] */
} JSFetchDeliverState;

static void js_fetch_deliver_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFetchDeliverState *s = st;
    int k;
    v->val(ctx, &s->value);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static int js_fetch_deliver_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFetchDeliverState *s = st;
    JSValue settled;
    int r;

    if (s->hdr.stage == FETCH_DELIVER_BUILD) {
        JSValueConst body_v = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsNull(body_v)) {
            /* The host could not fetch it. §5.6 rejects with a TypeError, and the message is the one every
               browser uses, because a page that matches on it is matching on that. */
            JS_ThrowTypeError(ctx, "Failed to fetch");
            s->value = JS_GetException(ctx);
            s->reject = 1;
        } else {
            /* THE REPLY THE HOST BUILT. It is this engine's own object, so these reads run none of the page's
               code — the value never passes through the page's hands between the host and here. */
            /* §2.2.6's URL LIST, which is the whole of what `url` and `redirected` are, and the one field only
               the host can answer: the fetch is the TRUSTED zone's (SECURITY.md), so the redirect chain is
               observed there and crosses here as TEXT carrying its type — an Array of serialized URLs. */
            JSValue ul_v = JS_GetPropertyStr(ctx, body_v, "urlList");
            JSValue stx_v = JS_GetPropertyStr(ctx, body_v, "statusText");
            JSValue bd_v = JS_GetPropertyStr(ctx, body_v, "body");
            const char *stx = JS_ToCString(ctx, stx_v);
            /* THE BODY, AS BYTES. §2.2.5 makes a response's body a byte sequence and the record now carries one
               (fetch.h), so this is a read of the bytes rather than a re-encode of a string somebody else had
               already decoded — which is what `arrayBuffer()` used to under-report at the first interior NUL
               and what `text()` used to answer U+FFFD for on every byte its own charset would have named. */
            size_t body_len = 0;
            const uint8_t *body = fetch_body_bytes(ctx, bd_v, &body_len);
            HeaderList hl = { 0 };
            /* §2.2.6's STATUS, THROUGH THE ONE READER OF THAT FIELD (fetch.h), for the reason the header list
               one line down is read through one. This was `int32_t status = 200; JS_ToInt32(ctx, &status, v);`
               — a DEFAULT on a field every producer writes, and the worse of the two the record had: a reply
               that arrived without one settled the page's `fetch()` with `response.ok === true` and a 200 it
               was never sent, which is a refusal read as a success and is exactly the plausible datum
               CLAUDE.md §Architecture counts. The reader asserts the field instead. */
            int status = fetch_reply_status(ctx, body_v);
            /* THE RECORD'S HEADER LIST, through the ONE reader of that field (above). The pair walk stood here
               written out, and a second consumer of the same record — the script decode that needs the
               `Content-Type` charset — would have been a second copy of it. */
            fetch_reply_header_list(ctx, body_v, &hl);
            /* THE ENGINE↔HOST CONTRACT, ASSERTED AT THE EDGE IT CROSSES. A reply without a URL list is not a
               reply this component can answer `url` or `redirected` from, and taking `undefined` for it would
               make every response report no redirect — the exact silence the literal `false` used to be. */
            DCHECK(JS_IsArray(ul_v),
                   "the host delivered a reply carrying no `urlList` — §2.2.6's URL list is what `url` and "
                   "`redirected` are, and the host is the only zone that performed the fetch that grew it");
            s->value = response_new(ctx, ul_v, status, stx ? stx : "",
                                    hl.n ? &hl : NULL, (const char *)body, body_len);
            header_list_free(&hl);
            if (stx) JS_FreeCString(ctx, stx);
            JS_FreeValue(ctx, ul_v);
            JS_FreeValue(ctx, stx_v);
            JS_FreeValue(ctx, bd_v);
            if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
        }
        STEP_GOTO(s->hdr.stage, FETCH_DELIVER_SETTLE, &s->cphase, NULL);
    }

    DCHECK(s->hdr.stage == FETCH_DELIVER_SETTLE,
           "the fetch delivery was re-entered at a stage §5.6 step 12's processResponse does not have");
    r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb),
                      JS_StepClosureData(&s->hdr, s->reject ? FETCH_DELIVER_REJECT : FETCH_DELIVER_RESOLVE),
                      JS_UNDEFINED, 1, (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
    if (r > 0) return r;          /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
    JS_FreeValue(ctx, settled);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_fetch_deliver_def = {
    /* a resolving function's return value is undefined and unobservable, so there is no completion to state */
    sizeof(JSFetchDeliverState), js_fetch_deliver_step, NULL, 0,
    .visit = js_fetch_deliver_visit,
    .algorithm = "Fetch §5.6 fetch(input, init) step 12's processResponse",
    .steps = js_fetch_deliver_steps
};
static int g_deliver_stepid = -1;

/* THE RECORD'S HEADER LIST, READ BACK — see fetch.h. It runs none of the page's code: the record is this
   engine's own object, built by fetch_reply_new or parsed by qjs_provide out of the trusted zone's JSON, so
   every read below is an ordinary own-property read on a plain Object. */
void fetch_reply_header_list(JSContext *ctx, JSValueConst reply, HeaderList *out)
{
    JSValue hs_v, len_v;
    uint32_t hi, hn = 0;

    DCHECK(out != NULL && out->n == 0,
           "a reply's header list was read into a list that already holds entries — a response has ONE header "
           "list, and a second fill would make `get` join two responses' values into one header");
    DCHECK(JS_IsObject(reply) || JS_IsNull(reply),
           "a header list was asked of something that is not the host's reply record — the record is an object "
           "and a network error is the JSON `null`, and there is no third thing this edge carries");
    if (!JS_IsObject(reply)) return;   /* a network error has no headers, which is not the same as none named */
    hs_v = JS_GetPropertyStr(ctx, reply, "headers");
    /* THE FIELD IS ASSERTED, NEVER DEFAULTED. Every producer of this record fills it — fetch_reply_new writes
       an Array whatever the host knew, and every host that crosses JSON writes `[[name, value], …]` — so a
       record without one is a producer that stopped, not a response that had no headers (that is an EMPTY
       array, which this walk reads as the zero headers it is). */
    DCHECK(JS_IsArray(hs_v),
           "the host's reply record carries no `headers` array — a response with no headers is the EMPTY one, "
           "so an absent field is a producer that did not build this record, and every read below would be a "
           "property read on `undefined`");
    len_v = JS_GetPropertyStr(ctx, hs_v, "length");
    JS_ToUint32(ctx, &hn, len_v);
    JS_FreeValue(ctx, len_v);
    for (hi = 0; hi < hn; hi++) {
        JSValue pair = JS_GetPropertyUint32(ctx, hs_v, hi);
        JSValue nv = JS_GetPropertyUint32(ctx, pair, 0), vv = JS_GetPropertyUint32(ctx, pair, 1);
        const char *nn = JS_ToCString(ctx, nv), *vc = JS_ToCString(ctx, vv);
        if (nn && vc) header_list_append(out, nn, vc);
        if (nn) JS_FreeCString(ctx, nn);
        if (vc) JS_FreeCString(ctx, vc);
        JS_FreeValue(ctx, nv); JS_FreeValue(ctx, vv); JS_FreeValue(ctx, pair);
    }
    JS_FreeValue(ctx, hs_v);
}

/* ---- §2.2.5's BODY on that same record — see fetch.h for why it is bytes and not text ------------------- */

void fetch_reply_set_body(JSContext *ctx, JSValueConst reply, const uint8_t *bytes, size_t n)
{
    JSValue v;

    DCHECK(JS_IsObject(reply),
           "a body was written onto something that is not the host's reply record — a network error is the "
           "JSON `null` and carries no body at all, so a body arriving with one is a host that answered a "
           "failure and a payload in the same breath");
    DCHECK(bytes != NULL || n == 0,
           "a reply's body arrived as a null pointer with a length — the two describe ONE byte sequence, and a "
           "host with no body at all passes an empty one");
#if APICLIENT_DEV
    {
        /* THE HALF OF THE SEAM THAT CANNOT BE SEEN FROM THE OTHER SIDE. The trusted zone's record is JSON, so
           the only way a decoded string can still reach the engine is a producer that kept writing `body` into
           that text — and it would arrive here as a plausible reply whose body is a STRING, which every reader
           below would then read as zero bytes. Asked before the write, because after it the field is ours. */
        JSValue had = JS_GetPropertyStr(ctx, reply, "body");
        DCHECK(JS_IsUndefined(had),
               "the host's reply record already carried a `body` — the bytes cross BESIDE that JSON and never "
               "inside it, so a record that arrived with one is a producer still running Fetch §5.3 Body "
               "mixin's `text()` (a UTF-8 decode, whatever the response's charset says) in a zone that does "
               "not own decodes — §5.4 stood on this sentence and is Request class, which includes the mixin "
               "rather than defining it");
        JS_FreeValue(ctx, had);
    }
#endif
    v = JS_NewArrayBufferCopy(ctx, bytes ? bytes : (const uint8_t *)"", n);
    CHECK(!JS_IsException(v), "fetch: OOM copying a delivered reply's body into the engine's heap — the flow "
                              "parked on it would resume reading a successful response with nothing in it");
    CHECK(JS_SetPropertyStr(ctx, reply, "body", v) >= 0, "fetch: a reply record refused its body");
}

JSValue fetch_reply_body(JSContext *ctx, JSValueConst reply)
{
    DCHECK(JS_IsObject(reply) || JS_IsNull(reply),
           "a body was asked of something that is not the host's reply record — qjs_provide parses the trusted "
           "zone's JSON and every host builds the same record, and a network error is the JSON `null`");
    /* A NETWORK ERROR HAS NO BODY, and the empty byte sequence is what its readers measure: a script that did
       not load runs nothing, and a reader of a reply that never arrived reads zero bytes. The fetch kind never
       reaches here — it rejects on the same value, in the delivery machine. */
    if (!JS_IsObject(reply))
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    return JS_GetPropertyStr(ctx, reply, "body");
}

const uint8_t *fetch_body_bytes(JSContext *ctx, JSValueConst body, size_t *out_n)
{
    static const uint8_t EMPTY[1] = { 0 };
    size_t n = 0;
    uint8_t *p;

    /* ASKED BEFORE `JS_GetArrayBuffer`, WHICH THROWS. A body that is not a byte sequence is a producer that
       did not build this record, and leaving that as a live TypeError would surface three frames later inside
       whatever the caller does next — a page's own error, for a host's mistake. */
    DCHECK(JS_IsArrayBuffer(body),
           "a reply's body is not a byte sequence — Fetch §2.2.5 makes a body one, every producer of this "
           "record writes an ArrayBuffer, and a STRING here is a zone that ran a decode this engine owns");
    if (!JS_IsArrayBuffer(body)) { if (out_n) *out_n = 0; return EMPTY; }
    p = JS_GetArrayBuffer(ctx, &n, body);
    DCHECK(p != NULL || n == 0,
           "a reply's body answered no bytes and a non-zero length — the only way JS_GetArrayBuffer does that "
           "is a DETACHED buffer, and nothing in this engine transfers a reply's body away from it");
    if (out_n) *out_n = n;
    return p ? p : EMPTY;
}

/* THE REPLY, as one engine-built object every host delivers. It is engine-built, so the reads on the other
   side run none of the page's code — which is why this is an object and not a second provider callback.
   `url_list` is §2.2.6's URL list, and it is a PARAMETER rather than a field this function invents: a host
   that followed a redirect saw a list of more than one, a host that did not saw the request's own URL, and
   only the host knows which. It is the same shape the trusted zone's JSON reply carries, so there is ONE reply
   record and not one per host. */
JSValue fetch_reply_new(JSContext *ctx, int status, const char *status_text, const HeaderList *headers,
                        const char *body, size_t body_len, const char *const *url_list, int url_list_n,
                        const char *computed_type)
{
    JSValue o = JS_NewObject(ctx), h, v;
    int i;

    /* A RECORD THIS FUNCTION FAILED TO BUILD IS NOT A REPLY, AND IT USED TO BE RETURNED AS ONE. Eleven
       allocations and property writes stood here and not one return value was read, so every way this can fail
       produced the SAME answer as success: an object. A caller cannot tell those apart — `JS_IsObject` says yes
       to both — and the flow parked on the reply is resumed with a Response built out of nothing, which is the
       consumer-defaults rule from the other side: the producer handed over a plausible datum in place of a
       measurement.
       IT WAS NOT HYPOTHETICAL. The smoke fixture aborted in the delivery on a record whose own property list
       was EMPTY — `fields=(none)`, engine.c's provide-side contract assert — which is exactly what this
       function returns when the writes below fail and it reports nothing.
       THEY ARE `CHECK`, NOT `DCHECK`, AND THAT IS LOAD-BEARING TWICE OVER. An allocation failure is CLAUDE.md's
       named CHECK case — a dropped work item corrupts the frontier, and a reply is the work item a suspended
       flow is waiting on — so it must abort in release too, where there is no version of this the engine may
       proceed past. And a DCHECK's condition is UNEVALUATED in release: writing the record inside one would
       compile the whole build away and return the empty object always. The side effects belong in the
       condition here precisely because this macro keeps them. */
    CHECK(!JS_IsException(o), "fetch: OOM allocating the host's reply record — the flow parked on this reply "
                              "would resume holding a Response with no status, no headers and no body");
    /* EVERY FIELD BELOW IS AN OWN DEFINE, NEVER A [[Set]], AND THAT IS A STATEMENT ABOUT WHOSE OBJECT THIS IS.
     *
     * This record belongs to the TRUSTED ZONE — it is what a host hands the engine when a flow's fetch is
     * answered — but it is built out of an ordinary `JS_NewObject`, so its prototype is the PAGE'S
     * `Object.prototype`. `JS_SetPropertyStr` is [[Set]] — ECMA-262 §10.1.9 "[[Set]] ( propertyKey, value,
     * receiver )", which delegates to §10.1.9.2 "OrdinarySetWithOwnDescriptor ( obj, propertyKey, value,
     * receiver, ownDesc )", whose first step is "If ownDesc is undefined, then Let parent be ? obj.[[GetPrototypeOf]]()"
     * — the prototype-chain walk. So writing `body`, `status`, `headers`, `statusText`, `urlList` or `computedType` through it
     * consults whatever the page put on that chain. Three things follow, and each of them has been observed as
     * a different-looking bug elsewhere in this tree:
     *   (1) an ACCESSOR on Object.prototype named for one of these fields makes this function CALL PAGE CODE —
     *       and it is called in the HOST's own time, between two scheduler slices, where a flow may be parked
     *       and there is no flow base to run anything on. That is the exact state quickjs.c's parked-flow
     *       assert aborts on ("the page's own code was entered while a flow is PARKED"), reached from a host
     *       that never meant to run anything at all — and an accessor IS page bytecode, so it is caught at the
     *       bytecode dispatch where that assert now stands rather than at this function's own door;
     *   (2) a FROZEN Object.prototype, or a non-writable data property on it, makes the [[Set]] REFUSE, and
     *       JS_SetPropertyStr refuses by THROWING — leaving a completion standing in the per-runtime exception
     *       slot during the host's own time, which the next flow the scheduler resumes then finds as its own;
     *   (3) prototype pollution is a thing this engine exists to FIND (§@S names it beside DOM-clobbering), so
     *       a page that pollutes Object.prototype could read every URL, status and body the trusted zone
     *       delivers, and substitute values in them. The untrusted side must not be able to see this record
     *       being built, let alone intercept it.
     * solver/cow.c reached the identical conclusion for the identical reason and wrote it down at its own
     * restore ("This was a JS_SetProperty: a [[Set]], an operation THROUGH the slot, which walks the prototype
     * chain, calls a setter, and consults `writable` and `extensible` — and refuses by THROWING"). Same
     * sentence, different file. A define creates an OWN data property: no chain walk, no setter, no consult,
     * nothing for a page to reach. JS_PROP_C_W_E is exactly the shape a plain assignment would have produced,
     * so every reader — including engine.c's own provide-side field census — sees what it saw before. */
    DCHECK(url_list_n >= 1,
           "a host built a reply with an empty URL list — §4.1 gives a fetched response at least a clone of "
           "the request's URL list, and `url`/`redirected` are read off nothing else");
    /* §2.2.6's URL LIST FIRST, and checked on its own line, because it is the field the delivery reads before
       any other and the one a host cannot invent (fetch.h). A list that failed to build must never reach the
       record as `undefined`. */
    v = response_url_list(ctx, url_list, url_list_n);
    CHECK(!JS_IsException(v), "fetch: OOM building a reply's §2.2.6 URL list — `url` and `redirected` are read "
                              "off nothing else, so the Response would report a different address than the one "
                              "that was fetched");
    CHECK(JS_DefinePropertyValueStr(ctx, o, "urlList", v, JS_PROP_C_W_E) >= 0, "fetch: a reply record refused its URL list");
    CHECK(JS_DefinePropertyValueStr(ctx, o, "status", JS_NewInt32(ctx, status), JS_PROP_C_W_E) >= 0,
          "fetch: a reply record refused its status");
    v = JS_NewString(ctx, status_text ? status_text : "");
    CHECK(!JS_IsException(v), "fetch: OOM allocating a reply's status message");
    CHECK(JS_DefinePropertyValueStr(ctx, o, "statusText", v, JS_PROP_C_W_E) >= 0, "fetch: a reply record refused its status message");
    /* §2.2.5's BODY, AS THE BYTE SEQUENCE IT IS. This was `JS_NewStringLen`, and that call is a DECODE — the
       lenient UTF-8 one cutils.h describes ("encoding errors are converted as 0xFFFD and use a single byte"),
       run by the record's BUILDER on bytes no standard had looked at yet. So every C host destroyed the
       evidence HTML §8.1.4.2's classic decode exists to read, a step before the extension's own `resp.text()`
       did: a `charset=windows-1252` script's 0x92 reached script_fetch.c as C2 92 at best and as EF BF BD when
       the byte stood alone. The record carries bytes now and each consumer runs its own standard's decode. */
    v = JS_NewArrayBufferCopy(ctx, (const uint8_t *)(body ? body : ""), body_len);
    CHECK(!JS_IsException(v), "fetch: OOM allocating a reply's body — a body that silently became empty is a "
                              "page reading a successful response with nothing in it");
    CHECK(JS_DefinePropertyValueStr(ctx, o, "body", v, JS_PROP_C_W_E) >= 0, "fetch: a reply record refused its body");
    h = JS_NewArray(ctx);
    CHECK(!JS_IsException(h), "fetch: OOM allocating a reply's header list");
    for (i = 0; headers && i < headers->n; i++) {
        JSValue pair = JS_NewArray(ctx);
        CHECK(!JS_IsException(pair), "fetch: OOM allocating a reply header's name/value pair");
        v = JS_NewString(ctx, headers->e[i].name);
        CHECK(!JS_IsException(v), "fetch: OOM allocating a reply header's name");
        CHECK(JS_DefinePropertyValueUint32(ctx, pair, 0, v, JS_PROP_C_W_E) >= 0, "fetch: a header pair refused its name");
        v = JS_NewString(ctx, headers->e[i].value);
        CHECK(!JS_IsException(v), "fetch: OOM allocating a reply header's value");
        CHECK(JS_DefinePropertyValueUint32(ctx, pair, 1, v, JS_PROP_C_W_E) >= 0, "fetch: a header pair refused its value");
        CHECK(JS_DefinePropertyValueUint32(ctx, h, (uint32_t)i, pair, JS_PROP_C_W_E) >= 0,
              "fetch: a reply's header list refused a pair — the Content-Type that decides whether a body "
              "parses is one of these");
    }
    CHECK(JS_DefinePropertyValueStr(ctx, o, "headers", h, JS_PROP_C_W_E) >= 0,
          "fetch: a reply record refused its header list");
    /* AND WHAT THE HOST DECIDED THIS RESOURCE IS (fetch.h). Asserted rather than defaulted at the WRITE, so a
       host that has not run the sniff aborts here — where the omission is — instead of at the reader, which
       would only be able to say that some producer somewhere left the field off. */
    DCHECK(computed_type != NULL,
           "a host built a reply without stating what it computed the resource to be — the sniff belongs to "
           "whoever read the bytes, and a host that has not decided is not finished building this record; a "
           "server that named nothing and bytes that named nothing is the EMPTY string, which is a value");
    v = JS_NewString(ctx, computed_type ? computed_type : "");
    CHECK(!JS_IsException(v), "fetch: OOM allocating a reply's computed MIME type");
    CHECK(JS_DefinePropertyValueStr(ctx, o, "computedType", v, JS_PROP_C_W_E) >= 0,
          "fetch: a reply record refused its computed MIME type");
    return o;
}

/* THE COMPUTED TYPE, READ BACK — see fetch.h for why it is read and never derived. */
char *fetch_reply_computed_type(JSContext *ctx, JSValueConst reply)
{
    JSValue v;
    const char *s;
    char *out;

    DCHECK(JS_IsObject(reply) || JS_IsNull(reply),
           "a computed MIME type was asked of something that is not the host's reply record — qjs_provide "
           "parses the trusted zone's JSON and every C host builds the same record, and a network error is "
           "the JSON `null`");
    if (!JS_IsObject(reply)) return NULL;   /* a network error: nothing answered, so nothing was computed */
    v = JS_GetPropertyStr(ctx, reply, "computedType");
    /* THE FIELD IS ASSERTED, NEVER DEFAULTED — CLAUDE.md's greppable contract, from the reader's end. The
       trusted zone's `safeFetch` stamps it on every path it has and `fetch_reply_new` takes it as a parameter,
       so `undefined` here is a producer that stopped and would otherwise become a plausible "no type" that is
       indistinguishable from a server that really named none. */
    DCHECK(JS_IsString(v),
           "the host's reply record carries no `computedType` — the zone that READ THE BYTES is the one that "
           "may decide what a resource is, and a record without that decision is a producer that did not "
           "finish building it; a resource nothing could name is the EMPTY string");
    s = JS_ToCString(ctx, v);
    out = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return out;
}

/* §2.2.6 "Responses"' STATUS, READ BACK — see fetch.h for why it is one reader and not a literal per caller. */
int fetch_reply_status(JSContext *ctx, JSValueConst reply)
{
    JSValue v;
    int32_t st = 0;
    int coerced;

    DCHECK(JS_IsObject(reply) || JS_IsNull(reply),
           "a status was asked of something that is not the host's reply record — qjs_provide parses the "
           "trusted zone's JSON and every C host builds the same record, and a network error is the JSON "
           "`null`");
    if (!JS_IsObject(reply)) return 0;   /* §2.2.6: a network error's status IS 0 — the answer, not a sentinel */
    v = JS_GetPropertyStr(ctx, reply, "status");
    /* THE FIELD IS ASSERTED, NEVER DEFAULTED. `fetch_reply_new` defines it on every record and the trusted
       zone stamps it on the JSON qjs_provide parses, so `undefined` here is a producer that stopped — and it
       is the one absence that cannot show as one, because its coercion is 0 and 0 is a status §2.2.6 gives a
       meaning to. */
    DCHECK(JS_IsNumber(v),
           "the host's reply record carries no `status` — every producer writes a NUMBER there, and an absent "
           "one coerces to 0, which is Fetch §2.2.6 \"Responses\"' NETWORK-ERROR status: every reply this "
           "engine ever fetched would read as a request that never reached a server, with nothing to say so");
    coerced = JS_ToInt32(ctx, &st, v);
    JS_FreeValue(ctx, v);
    /* ALWAYS FATAL, BECAUSE THERE IS NO ANSWER TO CONTINUE WITH. A status that would not coerce is a value the
       page put where the trusted zone's number belongs, and every consumer of this reader decides from the
       number whether the server ANSWERED the request or REFUSED it — so proceeding on a guess is the reply of
       a refusal being mined as though it described the resource. */
    CHECK(coerced == 0,
          "fetch: a reply record's `status` refused an integer coercion — the field is written as a number by "
          "every producer, so a value that throws out of ToNumber is not a status this seam can report");
    DCHECK(st >= 0 && st <= 999,
           "the host's reply record carries a `status` outside Fetch §2.2.3 \"Statuses\"' own range — \"A "
           "status is an integer in the range 0 to 999, inclusive.\" — so a number beyond it is a producer "
           "inventing one rather than a server having sent it");
    return st;
}

/* SETTLING A REQUEST §4.1 MAIN FETCH STEP 7 REFUSED — a bad port, or a Content Security Policy that blocks it.
   The settle runs through a FLOW, because resolving reads `then` off the value and the page owns that
   prototype. `value` is consumed.
   §4.3's two locally-answered schemes used to settle through here as well, out of two arms written into this
   function. They are core/fetch/scheme_fetch.c's now, and they settle through the SAME `deliver` closure a
   host reply settles through — which is why this is left with the one caller that has no reply at all. */
static void fetch_settle_local(JSContext *ctx, JSValue *resolving, JSValue value, int reject)
{
    if (JS_IsException(value))
        return;
    if (JS_CallAsFlow(ctx, resolving[reject], value) < 0) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);   /* the page's own handler threw; that is its completion, not this call's */
    }
    JS_FreeValue(ctx, value);
}

static JSValue fetch_park(JSContext *ctx, JSValueConst url, JSValueConst method, JSValueConst input,
                          const HeaderList *hdrs, const char *body, size_t body_len)
{
    JSValue promise, resolving[2], deliver;
    JSValueConst data[2];
    const char *u;
    /* §5.4 step 2'S PARSED URL, kept for the host below. See where it is filled. */
    char *abs = NULL;

    promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise))
        return promise;
    /* THE CONCRETE PROJECTION IS ASKED FOR EXPLICITLY. A URL built out of unknown external input is a CONCOLIC,
       and the ToString boundary owes C a real string rather than one — so this edge reads the concolic's own
       SHAPE, which is the display form the @H surface reports ("/api/{location.hash}"). Coercing it
       generically would either crash at that boundary or, worse, quietly de-taint the URL. */
    if (concolic_is(url)) {
        /* the SHAPE, made a real string first, so there is exactly ONE ownership rule for `u` below. A live
           concolic always HAS one (solver/concolic.c mints it with the value), so the `: "{}"` that stood here
           guarded a state that cannot arise and would have sent this request to an address spelled `{}`. */
        const char *sh = concolic_shape_c(url);
        JSValue sv;
        DCHECK(sh != NULL, "a concolic URL reached §5.4 with no display shape — the shape is what this edge "
                           "sends in place of an address it cannot know, so a value that has lost it would be "
                           "fetched as the two characters an unnameable hole prints as");
        sv = JS_NewString(ctx, sh);
        u = JS_ToCString(ctx, sv);
        JS_FreeValue(ctx, sv);
    } else {
        u = JS_ToCString(ctx, url);
    }
    if (u) {
        /* §4.3 SCHEME FETCH: "Switch on request's current URL's scheme". The scheme is what the URL PARSER
           says it is and not what the string starts with, so the URL is parsed ONCE here and every arm below
           reads that record — which is also what lets §6 have the parsed URL it asserts on. A relative
           reference with no base is the one input the parse refuses, and it names no scheme, so it is none of
           the local arms and belongs to the trusted host below. */
        UrlRecord rec;
        const char *scheme;

        url_record_init(&rec);
        scheme = fetch_parse_url(ctx, &rec, u, strlen(u)) ? rec.scheme : NULL;
        /* WHETHER THIS FLOW HAS A URL AT ALL, asked ONCE because two different steps below turn on the same
           one fact. A parse that FAILED produced no record, and a CONCOLIC's parse produced a record out of a
           DISPLAY FORM: its shape `/api/u?uid={state}.id` does parse, and it is not the address the request
           will go to — §5.4 step 2's URL is unknown for exactly as long as the value is. Every algorithm that
           reads a URL record as the request's URL is therefore off for both, which is why the serialization
           below and §2.9 above it read this and not the parse's own success. */
        bool url_is_real = scheme && !concolic_is(url);

        /* §4.1 MAIN FETCH STEP 7, and it runs HERE — before §4.3's scheme fetch, which is step 12 — because
           that is the order the two steps are in. "If should request be blocked due to a bad port, should
           fetching request be blocked as mixed content, or should request be blocked by Content Security
           Policy returns blocked, then set response to a network error", and §5.6 step 12's processResponse
           step 3 makes a network error a rejected TypeError. It is the same settle §4.3's two local schemes
           reject through, and it returns before the host is owed anything: a blocked request must never
           become a pending entry, or the flow parks forever on a reply the trusted zone was never asked for.
           THE STEP IS ONE DISJUNCTION AND IS WRITTEN AS ONE. CSP §4.1.2 is the second of its three checks
           (core/frame/policy_container.h), asked of THIS document's policy container with Fetch §2.2.5's
           DESTINATION for a `fetch()`, which is the EMPTY STRING — §6.8.1's first row, and the reason
           `connect-src` is the directive that governs this call. A fresh request has a REDIRECT COUNT of 0,
           which is what makes a `script-src https://cdn/a/` path-part mean the path it says.
           A CONCOLIC IS ALLOWED, and soundly so rather than by omission: §2.9 asks whether the port is one of
           83 numbers, an unknown value answers neither yes nor no, and the solver's rule for an undecided
           predicate is that uncertainty KEEPS the arm. Blocking on the accident that a shape's text carried
           `:25` would delete a real endpoint from the surface. The same sentence covers §4.1.2: a policy is
           matched against a URL, and a shape is not the URL the request will go to. */
        if (url_is_real &&
            (fetch_block_bad_port(&rec) == FETCH_PORT_BLOCKED ||
             policy_should_block_request(document_policy(ctx), &rec, /*destination*/ "",
                                         /*redirect count*/ 0) == CSP_REQUEST_BLOCKED)) {
            JSValue value;

            JS_ThrowTypeError(ctx, "Failed to fetch");
            value = JS_GetException(ctx);
            url_record_free(&rec);
            fetch_settle_local(ctx, resolving, value, /*reject*/ 1);
            JS_FreeCString(ctx, u);
            JS_FreeValue(ctx, resolving[0]);
            JS_FreeValue(ctx, resolving[1]);
            return promise;
        }
        /* §5.4 STEP 2'S ANSWER, KEPT RATHER THAN THROWN AWAY — and this is the whole of the fix. The parse
           above already resolved the page's string against the API base URL, which is what step 2 says the
           request's URL IS; the record was then freed and the host was handed `u`, the RAW string the page
           wrote. So a bundle's ordinary `fetch('/api/users')` asked the trusted zone to fetch the nine
           characters `/api/users`, and §4.1 — "the response's URL list is a clone of the request's URL list" —
           gave the reply a list whose one member is a relative reference. `response.url` then runs the URL
           parser back over that member with NO BASE, because every item of a URL list is absolute by the time
           it is in one, and refuses it: `a response's URL list held a string the URL parser refuses`, which is
           where the WPT corpus aborts. Every host was papering over it differently — the smoke fixture
           re-resolves against the document address before building its reply, the WPT runner passes `req.url`
           straight through and aborts — which is exactly the shape of a contract that was never stated.
           IT ALSO GIVES THE ENDPOINT SURFACE ITS HOST. `<METHOD> <host><path>` is the identity a learned schema
           is filed under (engine.c), and a relative reference has no host to file it by, so two origins'
           `/api/users` were one key.
           A CONCOLIC IS EXEMPT, and that is not an oversight to tidy later: its shape is a DISPLAY form, not a
           URL. `/api/u?uid={state}.id` does parse — the parser percent-encodes the braces — and serializing it
           would hand back `%7Bstate%7D.id`, which is the quiet de-tainting the projection above exists to
           prevent. A shape stays the shape. */
        if (url_is_real)
            abs = url_serialize(&rec, /*exclude_fragment*/ false);
        url_record_free(&rec);
    }
    if (u) {
        /* TWO CAPTURES, not three. The request's URL used to ride the closure and become the Response's `url`;
           §2.2.6 makes that the last item of the response's URL LIST, which the trusted host observes and
           delivers on the reply — so the capture is gone rather than kept beside the list as a second answer
           to one question. */
        data[0] = resolving[0];
        data[1] = resolving[1];
        DCHECK(g_deliver_stepid >= 0, "fetch() parked a reply before its delivery machine was declared");
        deliver = JS_NewStepClosure(ctx, g_deliver_stepid, 1, 2, data);
        if (!JS_IsException(deliver)) {
            FetchRequest req;
            const char *m = JS_IsUndefined(method) ? NULL : JS_ToCString(ctx, method);
            /* `GET` IS THE REQUEST'S OWN METHOD, NOT A HOLE FILLED HERE — Fetch §2.2.5 "Requests": "A request has an
               associated method (a method). Unless stated otherwise it is `GET`." The park is keyed on
               (method, url) now (solver/engine.h), so this is the one place that can state it and a request that
               reached the seam unnamed would collect another request's reply. It belongs on the Request record
               itself, which is where it moves the day §5.4's constructor builds one here. */
            req.method = m ? m : "GET";
            /* THE PARSED URL WHERE THERE IS ONE. `u` survives as the fallback for the two inputs that have no
               parsed form: a concolic's shape, and a relative reference in a document with no address at all
               (fetch_parse_url is absolute-only then, which is the honest answer for a platform-less build). */
            req.url = abs ? abs : u;
            /* AND ITS DESTINATION IS THE EMPTY STRING, WHICH IS A VALUE AND NOT AN OMISSION. Fetch §2.2.5
               "Requests" gives a request a destination and says "unless stated otherwise it is the empty
               string"; §5.4's Request constructor states nothing otherwise, and §2.2.5's own table puts
               `fetch()` and XMLHttpRequest on the row whose destination is "" and whose CSP directive is
               `connect-src`. So this is the request's real destination — the positive statement "these bytes
               are data, not a resource of a type", which is what makes the trusted zone's CORB class a fact
               read off the request rather than a guess made about the address. */
            req.destination = "";
            req.headers = hdrs;
            req.body = body;
            req.body_len = body_len;
            /* §4.3 SCHEME FETCH, ASKED HERE AND NOT ONLY AT THE SEAM BELOW, BECAUSE THIS CALLER HOLDS ONE INPUT
               NO OTHER CALLER HAS: §5.4's CAPTURED blob URL entry. A `Request` resolved its `blob:` URL when it
               was built, so a page that revoked the URL afterwards still fetches; a URL STRING has nothing to
               have captured and the store is consulted for it. Every other component reaches §4.3 through
               `fetch_owe`, which asks with no capture because no other standard has a Request to have made one.
               The seam asks AGAIN on the way past and cannot disagree: the capture is read by §4.3's `blob` arm
               alone, and a request that reached the line below took the `HTTP(S) scheme` arm, which no capture
               can move. */
            if (!scheme_fetch_answer(ctx, deliver, &req, request_blob_entry(input)))
                fetch_owe(ctx, deliver, &req);
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, deliver);
        }
        JS_FreeCString(ctx, u);
    }
    free(abs);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static JSValue js_fetch_fini(JSContext *ctx, void *st, bool take_result)
{
    JSFetchState *s = st;
    JSValue promise = JS_UNDEFINED;

    if (take_result && s->rejected) {
        promise = s->reject_promise;
        s->reject_promise = JS_UNDEFINED;
    } else if (take_result) {
        if (!JS_IsUndefined(s->body_mime)) {
            char *have = header_list_get(&s->hdrs, "content-type");
            const char *m = have ? NULL : JS_ToCString(ctx, s->body_mime);
            if (m) { header_list_append(&s->hdrs, "content-type", m); JS_FreeCString(ctx, m); }
            free(have);
        }
        /* A STREAM-BACKED BODY HAS NO BYTES YET, AND THE PARK TAKES BYTES. §5.2's ReadableStream arm sets the
           body's stream and leaves `source` null — there is nothing to send until the stream is READ — while
           every other arm produces the bytes during extraction. This park read `bytes`, which the stream arm
           never writes, so `fetch(u, {method:"POST", body: stream})` went to the host as a POST with an EMPTY
           body and the endpoint surface recorded that request as the one the page made: a producer writing
           `stream` and a consumer reading `bytes`, with the mismatch coming out as a plausible datum instead
           of a crash. What to build is §5.2's transmission: fully read the body's stream BEFORE this point —
           a stage of this machine that acquires a reader and reads to the end, the way body.c's readers do,
           driven as a FLOW because each read answers a promise — and hand the park the bytes it produced. */
        DCHECK(!(s->body.has && !s->body.bytes),
               "fetch() reached the host edge with a STREAM-BACKED request body: §5.2's ReadableStream arm "
               "leaves the bytes in the stream and this edge takes bytes. Build the full read as a stage of "
               "this machine (acquire a reader, read to the end, accumulate) and park with those bytes");
        promise = fetch_park(ctx, s->url, s->method, s->input, &s->hdrs,
                             s->body.has ? s->body.bytes : NULL, s->body.len);
    }
    /* WHAT NO DECLARATION NAMES. The request's values — method, url, input, the two inits, the mime and the
       rejection promise — and the header-fill's own slots are js_fetch_visit's, and the teardown discharges
       that list. These two are foreign C allocations: the body bytes and the parsed header list. */
    body_state_free(JS_GetRuntime(ctx), &s->body);
    header_list_free(&s->hdrs);
    return promise;
}

/* §5.6 step 1-3: `fetch` NEVER THROWS. It creates the promise FIRST, and an exception from building the
   request — an invalid header value, a bad method, a URL that will not parse — REJECTS that promise instead of
   propagating. A page writes `fetch(u, init).catch(...)`, and a synchronous throw goes straight past the catch
   it wrote; wpt asserts it directly, with `promise_rejects_js` over a fetch whose header value carries a NUL.
   The reject is called AS A FLOW like every other settle: 27.5.1.3's rejectSteps runs none of the page's code, but the
   rule that a settle has a flow base under it is not a per-call judgement about whether this one happens to. */
static int fetch_reject_pending(JSContext *ctx, struct JSFetchState *s)
{
    JSValue exc = JS_GetException(ctx);
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);

    if (JS_IsException(promise)) { JS_FreeValue(ctx, exc); return JS_STEP_ABRUPT; }
    if (JS_CallAsFlow(ctx, resolving[1], exc) < 0) {
        JSValue e2 = JS_GetException(ctx);
        JS_FreeValue(ctx, e2);
    }
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    s->reject_promise = promise;
    s->rejected = 1;
    return JS_STEP_DONE;
}

/* WHERE THIS MACHINE RESTS, AS THE TWO STANDARDS NUMBER IT.
 *
 * §5.6's `fetch(input, init)` is three steps, and its SECOND one INVOKES the Request constructor — so the
 * algorithm this machine walks is §5.4's, performed inline because the request it builds never becomes a
 * Request object. The `init` members it reads belong to neither: Web IDL §3.2.17 dictionary types converts the
 * RequestInit dictionary BEFORE step 1 of either algorithm, member by member IN LEXICOGRAPHICAL ORDER (its
 * step 4.1). The Web IDL numbers below were §3.2.18/§3.2.14, which this edition gives to enumeration types and
 * to `symbol`; they are checked against the spec text.
 *
 * THAT ORDER IS OBSERVABLE, and it was wrong. The reads were method, then body, then headers — the order this
 * file grew in — so `fetch(u, new Proxy({}, {get(t, k){ log.push(k) }}))` reported an order no browser
 * produces, and `input.url` was read in the MIDDLE of them where §5.4 reads it after all of them. The stages
 * are the spec's order now, and each one names the step it rests at. */
#define FETCH_STAGES(X) \
    X(FETCH_INIT_DICT,    "Web IDL §3.2.17 dictionary types step 1 (the RequestInit argument is an Object, or " \
                          "is undefined or null — anything else is a TypeError, before any member is read)") \
    X(FETCH_INIT_BODY,    "Web IDL §3.2.17 dictionary types step 4.1.3.1 (Get(init, \"body\") — RequestInit's " \
                          "members are read in lexicographical order, per its step 4.1)") \
    X(FETCH_INIT_HEADERS, "Web IDL §3.2.17 dictionary types step 4.1.3.1 (Get(init, \"headers\"))") \
    X(FETCH_INIT_METHOD,  "Web IDL §3.2.17 dictionary types step 4.1.3.1 (Get(init, \"method\"))") \
    X(FETCH_METHOD_STR,   "Web IDL §3.2.17 dictionary types step 4.1.4.1 (converting init[\"method\"] to " \
                          "§3.2.11 ByteString's IDL type)") \
    X(FETCH_INPUT_URL,    "Fetch §5.4 new Request(input, init) steps 5-6 (the request's URL: the input string, " \
                          "or the input Request's own)") \
    X(FETCH_INPUT_URL_STR, "Web IDL §3.2.12 USVString (converting a non-Request `input` to its USVString — the " \
                          "union's other member, which runs the page's own toString)") \
    X(FETCH_METHOD,       "Fetch §5.4 new Request(input, init) step 25 (init[\"method\"] is a method, is not a " \
                          "forbidden method, and is normalized)") \
    X(FETCH_HEADERS,      "Fetch §5.4 new Request(input, init) step 33 (fill the request's headers with " \
                          "init[\"headers\"], under the \"request\" guard)") \
    X(FETCH_BODY,         "Fetch §5.4 new Request(input, init) steps 35-37 (a body on GET or HEAD is a " \
                          "TypeError; extract init[\"body\"] and its Content-Type)") \
    X(FETCH_CALL,         "Fetch §5.6 fetch(input, init) step 12 (fetch given request — the endpoint the flow " \
                          "parks on until the trusted host answers)")
enum { FETCH_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_fetch_steps[] = { FETCH_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* Web IDL §3.2.17 dictionary types step 1: a dictionary argument that is neither an object nor null/undefined is
   a TypeError — `fetch(u, 5)` rejects. It was simply skipped, which made every member of such an init silently
   absent. */
static bool fetch_init_is_dict(JSValueConst init)
{
    return JS_IsObject(init);
}

static int js_fetch_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFetchState *s = st;
    JSValueConst input = step_arg(&s->hdr, 0), init = step_arg(&s->hdr, 1);
    const char *method = "GET", *mc = NULL;
    int r, prov;

    if (s->hdr.stage == FETCH_INIT_DICT) {
        /* THE ONE-TIME WORK LIVES IN A STAGE THAT HOLDS NO REQUEST, and that is the only reason this stage
           exists. A stage that PARKS is re-entered at its first line when the answer arrives — that is the
           two-phase contract, and every other machine in this tree does its capture in a stage that STEP_GOTOs
           before it can park (readable_stream's RSIX_START, TS_START, FS_START, DS_START, writable_stream's
           WSC_START). This capture stood at the top of the `body` read's stage instead, so every `fetch(u, {…})`
           — every fetch with an init object, which is every fetch a bundle actually writes — took a SECOND
           reference to the URL argument on the resume and lost the first. A string literal's JSString IS the
           atom the compiler interned for it, so what leaked was the page's own fetch URL: not a GC object, so
           the object census cannot see it, and JS_FreeRuntime's atom walk aborted the instance at teardown with
           every finding of the session still in it.
           THE INPUT IS KEPT rather than read back off the header, because `fini` needs it and cannot: the shared
           teardown releases the captured arguments and zeroes `argc` BEFORE it calls `fini`, so `step_arg` there
           answers undefined. A Request also carries a captured blob URL entry that its `url` string cannot
           express.
           EVERY ANSWER SLOT IS SPELLED HERE for the reason the capture is here: a zeroed step state's JSValue is
           the INTEGER 0 and not JS_UNDEFINED, so "is it set yet" asked of a slot always reads as already-set. */
        DCHECK(JS_VALUE_GET_TAG(s->input) == JS_TAG_INT,
               "the fetch machine's invocation capture ran twice — tramp_step_state_new js_mallocz's the state "
               "and a ZEROED JSValue is the INTEGER 0 (JS_TAG_INT is 0), so anything else in this slot means "
               "this stage was re-entered and the reference it took last time is leaked for the runtime's life");
        s->input = JS_DupValue(ctx, input);
        s->method = s->url = s->hinit = s->binit = JS_UNDEFINED;
        s->body_mime = s->reject_promise = JS_UNDEFINED;
        if (!JS_IsUndefined(init) && !JS_IsNull(init) && !fetch_init_is_dict(init)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "the fetch init is not an object");
            return fetch_reject_pending(ctx, s);
        }
        /* cb_result is NOT consumed here: this stage issues no request, so the delivery it was called with
           belongs to the read below, which frees it. */
        STEP_GOTO(s->hdr.stage, FETCH_INIT_BODY, &s->hdr.get_phase, NULL);
    }
    if (s->hdr.stage == FETCH_INIT_BODY) {
        /* §5.4 step 37: `init.body`. The READ is the page's code; the EXTRACTION is §5.2's, which body.c owns
           for every interface that takes a BodyInit, and which runs at its own step below. */
        if (fetch_init_is_dict(init)) {
            r = step_getprop_run(ctx, &s->hdr, init, g_atom_body, cb_result, &s->binit, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return fetch_reject_pending(ctx, s);
        } else {
            JS_FreeValue(ctx, cb_result);
        }
        cb_result = JS_UNDEFINED;
        s->hdr.stage = FETCH_INIT_HEADERS;
    }
    if (s->hdr.stage == FETCH_INIT_HEADERS) {
        /* Without this read the endpoint's transport requirement was simply invisible: every request that needs
           an `Authorization` or an `X-Api-Version` was reported as if it needed nothing. */
        if (fetch_init_is_dict(init)) {
            r = step_getprop_run(ctx, &s->hdr, init, g_atom_headers, cb_result, &s->hinit, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return fetch_reject_pending(ctx, s);
        } else {
            JS_FreeValue(ctx, cb_result);
        }
        cb_result = JS_UNDEFINED;
        s->hdr.stage = FETCH_INIT_METHOD;
    }
    if (s->hdr.stage == FETCH_INIT_METHOD) {
        if (fetch_init_is_dict(init)) {
            r = step_getprop_run(ctx, &s->hdr, init, g_atom_method, cb_result, &s->method, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return fetch_reject_pending(ctx, s);
        } else {
            JS_FreeValue(ctx, cb_result);
        }
        cb_result = JS_UNDEFINED;
        s->hdr.stage = FETCH_METHOD_STR;
    }
    if (s->hdr.stage == FETCH_METHOD_STR) {
        /* The member's TYPE is a ByteString, so a non-string method is ToString'd — the page's code, and its
           OWN step, because Web IDL §3.2.17 runs it after the [[Get]] that produced the value. It was never converted
           at all: `fetch(u, {method: {toString(){ return "POST" }}})` went out as a GET. */
        if (!JS_IsUndefined(s->method) && !JS_IsString(s->method)) {
            JSValue str;
            r = step_tostring_run(ctx, &s->hdr, s->method, cb_result, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return fetch_reject_pending(ctx, s);
            JS_FreeValue(ctx, s->method);
            s->method = str;
        } else {
            JS_FreeValue(ctx, cb_result);
        }
        cb_result = JS_UNDEFINED;
        s->hdr.stage = FETCH_INPUT_URL;
    }
    if (s->hdr.stage == FETCH_INPUT_URL) {
        /* §5.4: a USVString names the URL directly; a Request names it through its `url` attribute, and THAT is
           the read that has to be a request.
           WHICH ONE IT IS, IS A BRAND — Web IDL §3.2's union resolution for `RequestInfo = Request or
           USVString` matches the interface member against a PLATFORM OBJECT OF THAT INTERFACE, and sends every
           other value to the string member. This asked `JS_IsObject`, which is a SHAPE test, and the two
           disagree about the most important argument this engine ever sees: a CONCOLIC value is carried as an
           object, so `fetch('/api/u?uid=' + state.id)` — a URL built out of unknown external input, which is
           the whole surface this tool exists to report — took the Request arm. The `url` read is
           opaque-infectious, so it answered with a NEW concolic one field deeper, and the address this flow
           parked on and recorded was `/api/u?uid={state}.id.url`: an endpoint no page ever requested, keyed
           under a property name welded onto the end of the real shape. Every gated endpoint in the @H surface
           was reported that way. It also fetched `undefined` for `fetch({toString(){ return '/x' }})`, which is
           the same defect wearing its ordinary-web clothes. */
        if (request_is(input)) {
            r = step_getprop_run(ctx, &s->hdr, input, g_atom_url, cb_result, &s->url, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return fetch_reject_pending(ctx, s);
        } else {
            JS_FreeValue(ctx, cb_result);
            s->url = JS_DupValue(ctx, input);
        }
        /* THE DELIVERED VALUE IS CONSUMED HERE. Either branch above has taken it — the request answers with it
           and the other frees it — so the local must be cleared before the next stage, which now hands it to
           another request. It was left stale when this was the last stage and nothing read it again; the moment
           a stage was added after it, that stale copy was freed a second time. */
        cb_result = JS_UNDEFINED;
        s->hdr.stage = FETCH_INPUT_URL_STR;
    }
    if (s->hdr.stage == FETCH_INPUT_URL_STR) {
        /* THE UNION'S OTHER MEMBER, CONVERTED — and it is its own stage for the reason init["method"]'s is:
           ToString on a page object runs the PAGE's code, so it is a request the machine parks on rather than
           a coercion performed from C.
           A CONCOLIC IS NOT CONVERTED HERE, and that is the one exception this stage must state rather than
           discover. Its value is not known yet, so there is no string to produce; fetch_park asks it for its
           SHAPE explicitly at the ToString boundary, and stringifying it here would hand that boundary a plain
           string and DE-TAINT the URL — the endpoint would still be recorded, and it would be recorded as
           though the page had computed it. Primitives need no stage either: `JS_ToCString` in fetch_park is
           already ToString for every one of them. */
        if (JS_IsObject(s->url) && !concolic_is(s->url)) {
            JSValue str;
            r = step_tostring_run(ctx, &s->hdr, s->url, cb_result, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return fetch_reject_pending(ctx, s);
            JS_FreeValue(ctx, s->url);
            s->url = str;
        } else {
            JS_FreeValue(ctx, cb_result);
        }
        cb_result = JS_UNDEFINED;
        s->hdr.stage = FETCH_METHOD;
    }
    if (s->hdr.stage == FETCH_METHOD) {
        /* §5.4 step 25, through the ONE implementation §5.4's own constructor uses: a method that is not a
           token, or is CONNECT/TRACE/TRACK however it is spelled, is a TypeError — and the rest are NORMALIZED,
           so `{method:"post"}` reaches the endpoint surface as POST rather than as a second spelling of it. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsString(s->method)) {
            const char *m = JS_ToCString(ctx, s->method);
            char *norm;
            if (!m) return fetch_reject_pending(ctx, s);
            norm = request_method_check(ctx, m);
            JS_FreeCString(ctx, m);
            if (!norm) return fetch_reject_pending(ctx, s);
            JS_FreeValue(ctx, s->method);
            s->method = JS_NewString(ctx, norm);
            js_free(ctx, norm);
            if (JS_IsException(s->method)) { s->method = JS_UNDEFINED; return fetch_reject_pending(ctx, s); }
        }
        headers_fill_init(&s->fill);
        s->hdr.stage = FETCH_HEADERS;
    }
    if (s->hdr.stage == FETCH_HEADERS) {
        /* §5.1: a request's header list has guard "request" — the names the browser owns (Host, Cookie,
           Origin, the method-override family carrying CONNECT/TRACE/TRACK) are dropped rather than sent. */
        r = headers_fill_run(ctx, &s->hdr, &s->fill, s->hinit, &s->hdrs, HEADERS_GUARD_REQUEST,
                             cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return fetch_reject_pending(ctx, s);
        cb_result = JS_UNDEFINED;
        s->hdr.stage = FETCH_BODY;
    }
    if (s->hdr.stage == FETCH_BODY) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (!JS_IsUndefined(s->binit) && !JS_IsNull(s->binit)) {
            char *mime = NULL;
            /* §5.4 step 35: a body with a GET or a HEAD is a TypeError. It was missing, so a page's own bug
               went out as a GET carrying a payload and the endpoint surface reported that request as real. */
            if (JS_IsString(s->method)) {
                const char *m = JS_ToCString(ctx, s->method);
                bool bad = m && (!strcmp(m, "GET") || !strcmp(m, "HEAD"));
                if (m) JS_FreeCString(ctx, m);
                if (bad) {
                    JS_ThrowTypeError(ctx, "a fetch with a GET or HEAD method cannot have a body");
                    return fetch_reject_pending(ctx, s);
                }
            } else {
                /* No method member at all, so §5.4 step 12 left it GET — the same refusal. */
                JS_ThrowTypeError(ctx, "a fetch with a GET or HEAD method cannot have a body");
                return fetch_reject_pending(ctx, s);
            }
            /* §5.4 step 40's "with keepalive set to request's keepalive" — see the same line in
               core/fetch/request.c: `keepalive` is a member neither RequestInit nor Request installs here, so
               the value every request built through this path has is false, and the IDL gap audit is what
               reports the missing member rather than a comment. */
            if (body_extract(ctx, &s->body, s->binit, /*keepalive*/ false, &mime) < 0) { free(mime); return fetch_reject_pending(ctx, s); }
            /* §5.4 step 37.4's type: appended only where the init's own headers do not already name one, which
               the fill above has decided — so `fini` appends it after asking the list. */
            JS_FreeValue(ctx, s->body_mime);
            s->body_mime = mime ? JS_NewString(ctx, mime) : JS_UNDEFINED;
            free(mime);
        }
        s->hdr.stage = FETCH_CALL;
    }

    DCHECK(s->hdr.stage == FETCH_CALL,
           "the fetch machine was re-entered at a stage §5.4 and §5.6 do not have between them");
    if (JS_IsString(s->method)) {
        mc = JS_ToCString(ctx, s->method);
        if (mc) method = mc;
    }
    /* WHAT THIS REQUEST IS EVIDENCE OF, READ ONCE FOR THE WHOLE ACT. The outer endpoint below and the
       sub-requests its body names are the same `fetch()` call being composed by the same running code, so
       they are one act and take one answer — asking the flow twice would let two records of one act disagree
       if the path forked between the two reads. §scheduler's "an operation takes its inputs with it", read at
       the smallest scale there is. */
    prov = engine_prov_of_running_path();
    {
        /* The surface's own header shape, built from the list — two fields either way, so this is a projection
           and not a second representation. The solver must not learn what a HeaderList is. */
        EndpointHeader *eh = NULL;
        int i;
        /* AND THE BODY, WHICH IS HALF OF WHAT A POST IS. The surface recorded the address of a
           `POST /v1/users` and nothing about what it posts, so `doc.schemas[…Request]` had no producer at all.
           The type that governs the read is the one the request will SEND, and Fetch §5.4 "Request class"
           step 37.4 says which that is: the extracted mime is appended only "if type is non-null and this's
           headers's header list does not contain `Content-Type`". So the header list wins here for the same
           reason it wins in `fini`. */
        EndpointBody eb;
        const EndpointBody *ebp = NULL;
        char *body_ct = NULL;
        const char *ext_mime = NULL;   /* step 37.4's extracted type, used only where the list names none */
        if (s->hdrs.n) {
            eh = js_malloc(ctx, sizeof(*eh) * (size_t)s->hdrs.n);
            if (!eh) { if (mc) JS_FreeCString(ctx, mc); return fetch_reject_pending(ctx, s); }
            for (i = 0; i < s->hdrs.n; i++) {
                eh[i].name = s->hdrs.e[i].name;
                eh[i].value = s->hdrs.e[i].value;
            }
        }
        if (s->body.has && s->body.bytes) {
            body_ct = header_list_get(&s->hdrs, "content-type");
            if (!body_ct && !JS_IsUndefined(s->body_mime)) ext_mime = JS_ToCString(ctx, s->body_mime);
            eb.mime = body_ct ? body_ct : ext_mime;
            eb.bytes = s->body.bytes;
            eb.len = s->body.len;
            ebp = &eb;
        }
        endpoint_record(ctx, method, s->url, eh, s->hdrs.n, ebp, prov);
        if (ext_mime) JS_FreeCString(ctx, ext_mime);
        free(body_ct);
        js_free(ctx, eh);
    }
    /* AND THE SUB-REQUESTS THE BODY ITSELF NAMES. A batch API takes N calls in ONE request — `POST /batch`
       whose `multipart/mixed` body holds `GET /v1/animals/pony HTTP/1.1`, `POST /v1/farms HTTP/1.1`, … — and
       those N addresses were composed by the page's own code, so each is evidence about the app exactly as
       the request carrying them is. The line above recorded exactly one of them and the other N were discarded.
       HERE rather than in a reply path, because this is the point that holds the request's header list, the
       extracted body and §5.2's body Content-Type together (solver/multipart_batch.h), and because what is
       being read is what the page SENDS — solver/reply_decode.c is what a reply TEACHES, a different fact
       about a different record even though both end at `endpoint_record`. Nothing is fired: a sub-request is
       recorded exactly as the outer one is.
       AND AT THE OUTER REQUEST'S OWN GRADE, handed down rather than re-read: a sub-request is written inside
       a body this same running code composed, so it is evidence of exactly what the request carrying it is
       evidence of. */
    multipart_batch_learn(ctx, &s->hdrs, s->body_mime, s->body.bytes, s->body.len, prov);
    if (mc) JS_FreeCString(ctx, mc);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_fetch_def = {
    sizeof(JSFetchState), js_fetch_step, js_fetch_fini, 0, .visit = js_fetch_visit,
    .algorithm = "Fetch §5.6 fetch(input, init), performing §5.4 new Request(input, init) inline",
    .steps = js_fetch_steps
};

/* §5.1, §5.5 and §5.4's AGENT-WIDE DECLARATIONS. They were made from inside fetch_install, which runs once per
   DOCUMENT — so the three components declared their per-realm prototypes into core/realm.h's list AFTER the
   agent's own realm had already run it, and the first document's Headers had no prototype at all. A
   declaration belongs at agent init beside every other one; the GLOBAL is still the host's to expose or not. */
void fetch_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_fetch_rt == NULL || g_fetch_rt == rt,
           "fetch was declared into a second runtime — its atoms and step id belong to the first, and one WASM "
           "instance is one document");
    /* NOT `if (g_fetch_stepid >= 0) return;`. fetch has exactly ONE declaration site — core/platform.c's row —
       so this test could never be true, and what it DID do was turn the release below forgetting this latch
       into a silent wrong answer instead of a crash: a second agent in one process reached here, was told it
       had already been declared, and went on with four atom handles reading JS_ATOM_NULL — which is a VALID
       atom id, so every read of §5's `method`/`body`/`url`/`headers` would have answered `<null>` with nothing
       anywhere to say so. See core/agent_state.h. */
    DCHECK(g_fetch_stepid < 0, "fetch_init ran twice — §5's machines are declared once per AGENT");
    g_fetch_rt = rt;
    /* The reply's delivery, declared once for the runtime — every parked fetch mints a CLOSURE over this
       one definition rather than a definition per request. */
    g_deliver_stepid = JS_RegisterStepDef(rt, &js_fetch_deliver_def);
    /* §5.4's BODY IS A STREAM — `.body` answers one, and "clone a body" tees one — so §4 is not an
       optional neighbour of this component, it is a dependency. Each host installing it separately meant
       the dependency held only where a host had remembered: test_forced's fixture reached `res.clone()`
       with §4's class ids never minted and aborted at the DCHECK that says so. */
    readable_stream_init(ctx);
    response_init(ctx);
    headers_init(ctx);
    request_init(ctx);
    g_atom_method = JS_NewAtom(ctx, "method");
    g_atom_body   = JS_NewAtom(ctx, "body");
    g_atom_url    = JS_NewAtom(ctx, "url");
    g_atom_headers = JS_NewAtom(ctx, "headers");
    g_fetch_stepid = JS_RegisterStepDef(rt, &js_fetch_def);
    agent_state_id("fetch", &g_fetch_stepid, "§5.4's fetch machine, and the declaration latch");
    agent_state_id("fetch", &g_deliver_stepid, "the reply-delivery machine every parked fetch closes over");
    agent_state_ptr("fetch", &g_fetch_rt, "the runtime §5's four names were interned in");
    agent_state_atom("fetch", &g_atom_method, "§5.4's `method` member name");
    agent_state_atom("fetch", &g_atom_body, "§5.4's `body` member name");
    agent_state_atom("fetch", &g_atom_url, "§5.4's `url` member name");
    agent_state_atom("fetch", &g_atom_headers, "§5.4's `headers` member name");
}

/* §5's FOUR REQUEST/RESPONSE FIELD NAMES, given back. This component had NO release at all — it was one of
   the forty-three rows on core/platform.h's list with a declare and an empty third column — so the four names
   it interns once per agent were held for the runtime's whole life and reported by JS_FreeRuntime's atom walk
   on 118 files of `css/cssom`, which is a CSSOM area that touches fetch only incidentally.
   WHAT IS NOT FREED HERE is the four components fetch_init DECLARES (§4's ReadableStream, and §5's Response,
   Headers and Request): each is its own component with its own release, and those are still hand-written into
   the three host teardowns. They are present in all three, so they are not a divergence — they are the next
   rows for this column, and freeing them from here would make this component the owner of state it does not
   own. This releases exactly what fetch.c interned. */
void fetch_free(JSRuntime *rt)
{
    JS_FreeAtomRT(rt, g_atom_method);
    JS_FreeAtomRT(rt, g_atom_body);
    JS_FreeAtomRT(rt, g_atom_url);
    JS_FreeAtomRT(rt, g_atom_headers);
    g_atom_method = g_atom_body = g_atom_url = g_atom_headers = JS_ATOM_NULL;
    /* AND THE THREE HANDLES THE DECLARATION SET, which this release did not touch while fetch_init opened by
       reading one of them to decide it had nothing to do. Giving the four names back and keeping the latch is
       strictly worse than keeping both: the next agent then finds a fetch that says it is declared and whose
       every name is JS_ATOM_NULL. core/agent_state.h asserts all seven of these at the end of the release
       column, which is what makes this a crash rather than a wrong answer an agent later. */
    g_fetch_stepid = -1;
    g_deliver_stepid = -1;
    g_fetch_rt = NULL;
}

void fetch_install(JSContext *ctx, JSValueConst global)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(JS_IsObject(global), "the fetch install was handed something that is not the global object");
    DCHECK(g_fetch_rt == NULL || g_fetch_rt == rt,
           "fetch was installed into a second runtime — its atoms and step id belong to the first, and one WASM "
           "instance is one document");
    DCHECK(g_fetch_stepid >= 0, "fetch was installed into a realm before fetch_init declared it — §5.1's and §5.5's "
                                "prototypes are per-realm intrinsics, and a component that declares itself from "
                                "inside a per-DOCUMENT install declares itself after the agent's own realm has "
                                "already built its list");
    headers_install(ctx, global);   /* §5.1's interface object — a page builds an init with it before it fetches */
    response_install(ctx, global);  /* §5.5's — a page constructs one to seed a cache or a service-worker path */
    request_install(ctx, global);   /* §5.4's — `fetch(new Request(u, init))` is how half of real code calls it */
    JS_SetPropertyStr(ctx, (JSValue)global, "fetch",
                      JS_NewCFunction2(ctx, NULL, "fetch", 1, JS_CFUNC_step, g_fetch_stepid));
}
