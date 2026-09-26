/* XMLHttpRequest — the XMLHttpRequest Standard §3, built outward from its STATE MACHINE.
 *
 * WHY THE STATE MACHINE IS THE COMPONENT. Every member of §3 is defined by which of the five states it is legal
 * in and what it throws otherwise: `send()` on an unopened object is an "InvalidStateError", `responseText` on
 * a `responseType` of "blob" is another, `withCredentials` after send() a third. A component written outward
 * from the member list gets each of those as a per-member `if` that some member will be missing; written
 * outward from the state, the states are ONE record and every member reads it.
 *
 * WHAT A FLOW OWNS HERE. The record is per-object C behind a class opaque, so it writes where no property hook
 * can see — the same shape §COW's rule is about — and the capture is in the ACCESSOR (`xhr_of`), so a record a
 * flow has reached is one it may write and there is no write site left to miss. Everything the record holds
 * that a flow can CHANGE is a JSValue: the method, the URL, the two header lists, the received bytes, the
 * response object. A malloc'd string there would be reverted as a POINTER by a context switch and leaked by
 * the arm that allocated it — the leak the runtime's own GC walk cannot see. The header lists are JS Arrays of
 * [name, value] pairs for the same reason MessagePort's queue is one: their mutations are property writes the
 * delta already captures, and the snapshot machinery already carries them to the cold tier.
 *
 * SYNCHRONOUS send() IS A SUSPEND POINT, and it is the interesting case for this engine rather than the awkward
 * one. §3.5.6's synchronous arm says "Pause until either processedResponse is true or … timeout"; a pause is
 * exactly what a flow does at an `await` or a loop back-edge. So the request is placed with the host through
 * engine_host_request — the one rendezvous in this engine that BLOCKS a flow (flow_blocked), which is what a
 * fetch's own pending register deliberately does not do — and the machine yields until the answer lands.
 * Siblings run meanwhile. The ASYNCHRONOUS arm places the identical request from a TASK, so this component has
 * ONE network edge and not two: the only difference between the two modes is who is waiting on it.
 *
 * THE ENGINE HOLDS NO NETWORK POLICY. SECURITY.md puts every byte behind the trusted zone, so this component
 * states the request (method, URL, headers, body) and the host decides SOP, CORS, method and credentials. There
 * is no `if` here about any of them — `withCredentials` is recorded and sent as part of the request, never
 * enforced.
 *
 * THE XML ARM of "set a document response" runs core/xml/xml_parse.h, which is the ONE XML parse in this
 * engine and is shared with HTML §8.5.1 `parseFromString` and §7.5.3's loader. What is THIS section's own is
 * the CONSEQUENCE of a failure: step 6 says to return null, so an ill-formed document leaves `responseXML`
 * null — where §8.5.1 instead builds a `parsererror` document over the identical report. The two must not be
 * made to agree; they are two standards answering for two different callers.
 *
 * WHAT IS ABSENT AND WHY, stated rather than stubbed:
 *   - `timed out` is a real flag with the whole request-error path behind it, and nothing sets it: under this
 *     engine's virtual clock (timer.h) time moves only when the event loop has nothing runnable, so a placed
 *     request is always answered before the clock can reach the timeout. That is the honest consequence of the
 *     clock, not a missing branch.
 *   - REDIRECTS are the host's, so `responseURL` reports the request URL: the reply carries no final URL to
 *     report and inventing one would be a value the code did not compute.
 *   - The upload's PROGRESS events report the request body's length transmitted in one go, because the host
 *     answers a request whole. `loadstart`/`progress`/`load`/`loadend` on the upload object all fire; there are
 *     no intermediate chunk lengths to report because there are no chunks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/json_buf.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/encoding/encoding.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/fetch/body.h"
#include "core/fetch/scheme_fetch.h"
#include "core/fetch/fetch.h"
#include "core/fetch/headers.h"
#include "core/fetch/port_blocking.h"
#include "core/fetch/reply_source.h"   /* the ONE spelling of a reply's source identity — shared with Fetch */
#include "core/fetch/request.h"
#include "core/file/blob.h"
#include "core/html/form_data.h"
#include "core/html/html_parse.h"   /* the ONE place a Document is parsed — that header owns the token bytes */
#include "core/mime/mime_type.h"
#include "core/url/url.h"
#include "core/url/url_search_params.h"
#include "core/xhr/progress_event.h"
#include "core/xhr/xml_http_request.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/endpoint.h"   /* the @H surface — every request host-edge funnels one endpoint into it */
#include "solver/engine.h"
#include "solver/reply_decode.h"   /* what a reply BODY teaches — see xhr_take_reply, and that header
                                      for why this door had to call it rather than be reached from one */
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */
#include "core/xml/xml_parse.h"        /* the ONE place an XML document is parsed — shared with §8.5.1 and §7.5.3 */

/* §3's five states, in the order the constants number them. */
enum { XHR_UNSENT = 0, XHR_OPENED, XHR_HEADERS_RECEIVED, XHR_LOADING, XHR_DONE };

/* §3's `XMLHttpRequestResponseType`, in the order the IDL lists it. */
enum { RT_EMPTY = 0, RT_ARRAYBUFFER, RT_BLOB, RT_DOCUMENT, RT_JSON, RT_TEXT };
static const char *const RT_NAME[] = { "", "arraybuffer", "blob", "document", "json", "text" };

typedef struct {
    /* §3's associated values. Every one a flow can write is a JSValue, for the reason the file comment gives. */
    JSValue upload;            /* the XMLHttpRequestUpload object (owned) */
    /* §3's REQUEST METHOD — a JS string, UNKNOWN EXTERNAL INPUT, or JS_NULL before open().
       The unknown is not a hole: XHR §3.5.1 The open() method step 11.2 sets this to the method its
       step 4 normalized, and over an argument the declaration crossed unconverted that normalization is
       a DERIVATION whose result is unknown in every world (see OPEN_METHOD_OP). Every reader below
       therefore states what it does with one; a reader that assumed a String defaulted it, and a method
       nobody computed reported as `GET` is the wrong report rather than a partial one. */
    JSValue method;
    JSValue url;               /* request URL, serialized — a JS string, JS_NULL before open() */
    /* THE URL AS THE PAGE COMPUTED IT, which the serialization above cannot be. XHR §3.5.1 The open() method
       step 11.3 is "Set this's request URL to parsedURL", so `url` is a `url_serialize` result of that record
       and a plain string has no example behind it.
       `fetch()` keeps both for exactly this reason (core/fetch/fetch.c §5.4: the CONCOLIC goes to the @H
       surface, its SHAPE goes to the network edge), and an address an XMLHttpRequest built out of unknown
       input must reach that surface the same way or the endpoint is a hole with no value in it. JS_NULL where
       open() was handed a plain string — a POSITIVE statement (this address is exactly what `url` says). */
    JSValue url_src;
    JSValue author_headers;    /* author request headers — an Array of [name, value] */
    JSValue request_body;      /* request body — a JS string, or JS_NULL */
    /* …AND WHETHER THOSE CHARACTERS ARE THE BODY OR A SPELLING OF ONE. The slot above is declared a JS string
       because §3.5.6 step 4's extraction is resolved to bytes here, and for a body built out of unknown
       external input there are no bytes: what lands in it is core/fetch/body.h's DISPLAY SHAPE. The two are
       indistinguishable once stored — that is the residual at the store site — so the ARM is kept beside them,
       because the @H surface must refuse to publish a shape as bytes the request sent and this is the only
       place that still knows. It is not a second copy of a fact the slot holds; the slot cannot hold it. */
    uint8_t  request_body_is_shape;
    JSValue override_mime;     /* override MIME type — a JS string, or JS_NULL */
    JSValue response_headers;  /* the response's header list — an Array of [name, value] */
    JSValue status_text;       /* the response's status message — a JS string */
    JSValue response_url;      /* the response's URL — a JS string */
    /* §3's RECEIVED BYTES, and they are BYTES: an ArrayBuffer, which is what a byte sequence is in this heap.
       It was a JS string, so the response reached every reader below already decoded — by whichever zone built
       the reply record, with UTF-8 and no charset — and §3.6.6's "get a text response", whose entire job is to
       decode with the FINAL encoding, was decoding a re-encoding of that. `responseType = "arraybuffer"` copied
       the same round trip out to the page as the server's bytes. */
    JSValue received;          /* §3's received bytes — an ArrayBuffer */
    JSValue response_object;   /* the response object — an object, or JS_NULL for §3's null */
    uint32_t timeout;          /* §3's timeout, in milliseconds */
    int32_t  status;           /* the response's status */
    uint8_t  state;
    uint8_t  send_invoked;
    uint8_t  synchronous;
    uint8_t  upload_complete;
    uint8_t  upload_listener;
    uint8_t  timed_out;
    uint8_t  cross_origin_credentials;
    uint8_t  response_type;
    uint8_t  response_object_failure;   /* §3's response object being `failure` rather than an object */
    uint8_t  network_error;             /* the response IS §3's initial "network error" */
    uint8_t  aborted;                   /* the response's aborted flag */
    /* WHAT §3.5.6 STEP 6'S REQUEST IS EVIDENCE OF, TAKEN ONCE WHERE THAT STEP RUNS — one of solver/pending.h's
       PROV_*, or -1 before there is a request to grade. CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE makes the
       grade a fact about the REQUEST, and §scheduler says an operation that becomes a work item takes its
       inputs with it rather than reading them back off the object it acts on — which is exactly what an
       XMLHttpRequest does: step 6 composes the request on one turn and the reply lands on another, with the
       flow parked in between. So every consumer below reads THIS rather than asking the running path again,
       and the three that used to ask separately (the trusted zone's record, the @H sighting, and the reply
       this diff teaches the engine to learn from) cannot come apart.
       -1 AND NOT ZERO, WHICH IS THE WHOLE REASON THE SENTINEL IS SIGNED: `calloc` leaves 0 and 0 is
       PROV_OBSERVED, the STRONGEST of the three — so an ungraded request would report as one a real client
       made, which is the fabrication §@H names. */
    int8_t   request_prov;
} XhrData;

static JSClassID g_xhr_class;
static JSClassID g_upload_class;   /* the class exists for its per-REALM prototype slot; the object wears it */
static JSClassID g_xhr_et_class;   /* XMLHttpRequestEventTarget: a prototype slot and nothing else */
static JSRuntime *g_xhr_rt;
static int g_ready;

static int g_ctor_stepid = -1, g_open_stepid = -1, g_send_stepid = -1, g_abort_stepid = -1;
static int g_id_set_request_header = -1, g_id_get_response_header = -1, g_id_get_all = -1,
           g_id_override_mime = -1;
static int g_set_timeout_id = -1, g_set_with_credentials_id = -1, g_set_response_type_id = -1;
static int g_response_getter_id = -1, g_response_xml_getter_id = -1;
static int g_run_stepid = -1;      /* the fetch/response lifecycle machine, minted as a closure per send() */

/* THE RECORD TIME-TRAVELS. Every field above is state a flow writes — an arm that sends must not have sent for
   its sibling — and the capture is in the accessor for the reason the streams and messaging components give:
   a record a flow has REACHED is one it may write, so there is no write site left to miss. The offset list is
   the same list the finalizer frees and the gc_mark walks. */
#define XO(f) (uint16_t)offsetof(XhrData, f)
static const uint16_t XHR_VALS[] = {
    XO(upload), XO(method), XO(url), XO(url_src), XO(author_headers), XO(request_body), XO(override_mime),
    XO(response_headers), XO(status_text), XO(response_url), XO(received), XO(response_object),
};
static const CowRecord XHR_REC = { sizeof(XhrData), XHR_VALS, (int)(sizeof(XHR_VALS) / sizeof(XHR_VALS[0])) };

/* ---- XHR §3.2 Garbage collection -----------------------------------------------------------------------
 *
 * THE STANDARD, VERBATIM: "An XMLHttpRequest object must not be garbage collected if its state is either
 * opened with send() invoked being true, headers received, or loading, and it has one or more event listeners
 * registered whose type is one of readystatechange, progress, abort, error, load, timeout, and loadend." Those
 * seven types are the WHOLE of the listener half and this is the one place they are stated; nothing below
 * tests them one at a time. `xhr_gc_window` is the READINESS half, and it is the same three-state disjunction
 * §3.5.7 The abort() method's step 2 tests, so both read it here rather than spelling it twice.
 *
 * WHAT DISCHARGES IT IS A REFERENCE AND NOT A FLAG. "Must not be garbage collected" is a statement about
 * REACHABILITY, and this collector has no predicate to consult — Blink answers §3.2 with
 * ActiveScriptWrappable's HasPendingActivity, which is a hook quickjs does not have — so the only honest
 * expression of it is a real reference from something already reachable. There is one, and it is the LIFECYCLE
 * MACHINE: xhr_run_closure captures the object at XHR_CD_OBJECT, and that closure is owned for exactly as long
 * as §3.5.6 The send() method's fetch, processResponse and handle response end-of-body have work left —
 * by the enqueuing flow's own job record for an asynchronous send(), and by the calling machine's `fn` (which
 * its `visit` declares) for a synchronous send() and for §3.5.7's error mode. A flow's job queue is held by
 * the scheduler's C state rather than by anything in the heap graph, so nothing subtracts those references and
 * the object below them is rooted for the whole window.
 *
 * THE ENGINE'S RULE IS BROADER THAN §3.2'S, DELIBERATELY, AND THAT IS THE ANSWER RATHER THAN A STAGE ON THE
 * WAY TO ONE. The machine holds the object whether or not one of the seven listeners is registered, because
 * the machine exists to run the request and not to answer §3.2. §3.2 FORBIDS collecting and never requires it,
 * so a broader retention conforms; what it costs is that an in-flight XMLHttpRequest nobody listens to is held
 * until its machine finishes, which the request it is waiting on bounds.
 *
 * SO IT IS NOT NARROWED TO THE CONJUNCTION, BECAUSE THE CONJUNCTION GOING FALSE LICENSES NOTHING. This block
 * used to ask for a per-TYPE query over DOM §2.7 Interface EventTarget's event listener list —
 * `event_target_has_listener_of_any(ctx, target, types, n)`, beside the event_target_has_any_listener that
 * §3.5.6 The send() method's step 5 upload test already uses — "on the day this retention stops being the
 * machine's". That reads as a schedule and is an instruction to build a defect, so it is deleted rather than
 * re-dated. A one-sided constraint has no complement to act on: the SECOND paragraph below terminates the
 * fetch controller when the object IS COLLECTED, never when the conjunction goes false, and an object the page
 * still holds is not collectable however few listeners it has. The synchronous send is the whole argument in
 * one line — `x.open(m, u, false); x.send(); return x.responseText;` registers not one of the seven, so the
 * conjunction is false for the entire §3.2 window, and a retention that ended with it would abandon the
 * request THAT VERY STATEMENT is waiting on. Ordinary reachability is what keeps that object alive, which is
 * exactly what §3.2 leaves to the collector.
 *
 * AND THE EXACT SET WOULD HAVE NO READER, WHICH IS A PROPERTY OF THE CLAUSE AND NOT OF THIS FILE. Every "must
 * not be garbage collected if …" clause this engine hosts is discharged the same way, by a working structure
 * whose lifetime strictly contains the clause's window: HTML §9.5 Broadcasting to other browsing contexts'
 * channel is held by the open-channel registry its delivery ORDER needs, and Permissions §6.3.5 Garbage
 * collection's status by the §6.3.4 chain that re-queues itself to ask whether the state changed again. A
 * query would therefore be a computed writer with no reader in any of the three. What would earn it is an
 * object whose ONLY root is its own clause — then the clause decides a reference rather than restating one
 * that already exists — and an engine that grows one will find this the place that says so.
 *
 * WHAT IS LEFT IS THE STANDARD'S SECOND PARAGRAPH, AND IT IS THE MACHINE'S TEARDOWN — see js_xhr_run_fini. */
static bool xhr_gc_window(const XhrData *d)
{
    return (d->state == XHR_OPENED && d->send_invoked) || d->state == XHR_HEADERS_RECEIVED ||
           d->state == XHR_LOADING;
}

/* THE BYTES AN UNKNOWN ARGUMENT CROSSES AS. Web IDL's DOMString / ByteString / USVString conversion PASSES
   unknown external input THROUGH untouched — idl_args.c states why, and it is deliberate: opacity has to
   survive a coercion or the value stops forking control flow and stops being solvable at a sink — so a member
   body that wants BYTES must ask for the concolic's own display SHAPE. That is the projection Fetch's §5.4
   URL and Headers' value already take. Coercing one instead reaches §7.1.19 ToString, which owes C a real
   JSString and has no answer for an unknown: that abort is what ended a real axios page at its first request,
   on this member's `url`. `plen` may be NULL, exactly as JS_ToCString's is; the result is released with
   JS_FreeCString either way. */
static const char *xhr_arg_cstring(JSContext *ctx, JSValueConst v, size_t *plen)
{
    const char *sh, *r;
    JSValue sv;

    if (!concolic_is(v))
        return JS_ToCStringLen(ctx, plen, v);
    sh = concolic_shape_c(v);
    DCHECK(sh != NULL, "unknown external input reached an XMLHttpRequest argument with no display shape — the "
                       "shape is what this edge uses in place of bytes it cannot know, so a value that has "
                       "lost it would be requested at an address spelled as an unnameable hole");
    sv = JS_NewString(ctx, sh);
    r = JS_ToCStringLen(ctx, plen, sv);
    JS_FreeValue(ctx, sv);
    return r;
}

/* THE ADDRESS THIS OBJECT'S REQUEST IS FILED UNDER, WHICH IS NOT THE SAME QUESTION AS WHICH ORIGIN IT
   REACHES — and §3.5.1 The open() method step 11.3, "Set this's request URL to parsedURL", answers only the
   second. A URL built out of unknown external input reaches that parse as its display SHAPE (xhr_arg_cstring
   above), and a parse is not an identity-preserving projection of one: URL Standard §1.3 "Percent-encoded
   bytes" states that "The path percent-encode set is a percent-encode set consisting of the query
   percent-encode set and U+003F (?), U+005E (^), U+0060 (`), U+007B ({), and U+007D (})", so `/api/{state}.id`
   serializes back as `…/api/%7Bstate%7D.id` — an absolute address whose holes are spelled in a grammar nothing
   downstream reads. core/fetch/fetch.c refuses exactly that on its own Fetch §5.4 "Request class"
   edge, in its own words `A shape stays the shape`, and this component had no NAME for the distinction — so
   the two facts it already holds were read by whichever spelling each caller happened to reach for.
   WHAT ONE SPELLING FOR TWO QUESTIONS COST: the @H sighting was filed under the SHAPE and the reply's asset
   verdict under the SERIALIZATION, and solver/endpoint.c recomputes that identity FROM THE SHAPE ALONE
   (`path_scan`'s own banner says so, which is what makes an example-free recompute answer the same string) —
   so a verdict for an unknown-input address named NO record, and a static file stayed on the learned API
   surface as an endpoint with nothing anywhere saying a record's kind had gone undecided.
   THE OTHER QUESTION KEEPS THE SERIALIZATION AND THAT IS THE SPLIT RATHER THAN AN OMISSION: which origin a
   request reaches is what the chokepoint decides SOP, CORS and credentials from, and only the parse states it
   — so `xhr_request_op` and Fetch §4.1's blocking read `url`, and everything that NAMES this request to the @H
   surface reads this. Projected to bytes by `xhr_arg_cstring` and never by a coercion, which answers
   byte-identically to the shape solver/endpoint.c's `url_display` takes: the two keys are ONE STRING by
   construction rather than two that agree today. */
static JSValueConst xhr_request_address(const XhrData *d)
{
    DCHECK(!JS_IsNull(d->url),
           "an XMLHttpRequest was asked for the address its request is filed under before XHR §3.5.1 The "
           "open() method step 11.3 stored one — every caller runs on an `opened` object, so an absent URL "
           "here is a route that reached the @H surface or the reply register without one");
    DCHECK(JS_IsNull(d->url_src) || concolic_is(d->url_src),
           "an XMLHttpRequest holds a request-address argument that is not unknown external input — §3.5.1 "
           "The open() method stores one ONLY where it carries something the serialization does not, and a "
           "plain string here would make this identity a second copy of `url` that is free to drift from it");
    return JS_IsNull(d->url_src) ? d->url : d->url_src;
}

static XhrData *xhr_of(JSValueConst v)
{
    XhrData *d = JS_GetOpaque(v, g_xhr_class);
    if (d) cow_capture_host_record(v, d, &XHR_REC);
    return d;
}

/* WRITE ONE OF THE TWELVE, and never `JS_FreeValue(ctx, d->f); d->f = <build one>;` — see cow.h. The record
   and its layout are bound HERE rather than at each call, so no site can pass a slot from one record with the
   layout of another. Every write below goes through it; the CONSTRUCTOR does not, and that is the one honest
   exception: before JS_SetOpaque the record is unreachable by the collector and its slots hold no value to
   release. */
/* THE ADDRESS PASSES THROUGH: the asserts inside are about the SLOT, so they must name the WRITE and not this
   line — see cow.h's THE SITE TRAVELS WITH THE OPERATION. */
static void xhr_set_at(JSContext *ctx, XhrData *d, JSValue *slot, JSValue v,
                       const char *file, int line)
{
    cow_record_set_at(ctx, d, &XHR_REC, slot, v, file, line);
}
#define xhr_set(ctx_, d_, slot_, v_) xhr_set_at((ctx_), (d_), (slot_), (v_), __FILE__, __LINE__)

/* THE COLLECTOR'S TWO ENTRIES REACH THE RECORD THROUGH JS_GetAnyOpaque, NEVER THROUGH g_xhr_class.
 *
 * core/agent_state.h states the obligation and why it exists: every host's teardown is platform_agent_free()
 * … JS_RunGC … JS_FreeRuntime, so both of these run with this component's class id already back at 0, and
 * `JS_GetOpaque(val, 0)` answers NULL for every object of the class. The finalizer would then have leaked the
 * XhrData and the twelve owned values XHR_VALS names, per live XMLHttpRequest — malloc'd bytes plus JSValues
 * whose references were never given back. The gc_mark is the worse of the two: an unmarked child keeps the
 * internal reference gc_decref subtracts, so gc_scan reads it as rooted from OUTSIDE the heap and the
 * `xhr` ⇄ `xhr.upload` cycle the mark exists for is never collected at all.
 * The id is not read because it is not needed — the collector dispatched HERE THROUGH the class, so the class
 * is a fact these functions already have.
 *
 * THE `if (!d) return;` STAYS, and that is the difference from a component whose mint is atomic. Between
 * JS_NewObjectProtoClass and JS_SetOpaque, js_xhr_ctor_step allocates the upload object, two Arrays, two
 * Strings and an ArrayBuffer — every one of which may collect — so a half-built XMLHttpRequest carrying no
 * record IS reachable by these entries, and a DCHECK here would fire on a correct program. */
static void xhr_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    XhrData *d = JS_GetAnyOpaque(val, &id);
    size_t i;

    (void)id;
    if (!d) return;
    for (i = 0; i < sizeof(XHR_VALS) / sizeof(XHR_VALS[0]); i++)
        JS_FreeValueRT(rt, *(JSValue *)((char *)d + XHR_VALS[i]));
    free(d);
}

static void xhr_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    XhrData *d = JS_GetAnyOpaque(val, &id);
    size_t i;

    (void)id;
    if (!d) return;
    /* THE UPLOAD OBJECT IS A CYCLE waiting to happen — a page routinely holds the XHR from a listener it
       registered on `upload`, and the XHR holds the upload. Without this the pair is unreachable to the
       collector and every `xhr.upload` a page touches is a leak the runtime's own walk reports. */
    for (i = 0; i < sizeof(XHR_VALS) / sizeof(XHR_VALS[0]); i++)
        JS_MarkValue(rt, *(JSValue *)((char *)d + XHR_VALS[i]), mark_func);
}

/* ---- Fetch §2.2.2 "Headers"'s header list, as a JS Array of [name, value] ---------------------------------
 *
 * §2.2.2's operations over the shape §COW requires — the header list and its `contains` are defined THERE, not
 * in §5.1 "Headers class", which stood on both sentences and is the IDL object that CALLS them. The NAME is
 * stored as the page wrote it (§3.5.2 sends what it was given) and every comparison is ASCII case-insensitive,
 * which is what "a header list list contains a header name name" means. */

static uint32_t hl_len(JSContext *ctx, JSValueConst arr)
{
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static int hl_ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* The index of the first entry whose name matches, or -1. */
static int hl_find(JSContext *ctx, JSValueConst arr, const char *name)
{
    uint32_t n = hl_len(ctx, arr), i;

    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, arr, i);
        JSValue nv = JS_GetPropertyUint32(ctx, pair, 0);
        const char *s = JS_ToCString(ctx, nv);
        int hit = s && hl_ci_eq(s, name);
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, nv);
        JS_FreeValue(ctx, pair);
        if (hit) return (int)i;
    }
    return -1;
}

static void hl_push(JSContext *ctx, JSValueConst arr, const char *name, const char *value)
{
    JSValue pair = JS_NewArray(ctx);

    if (JS_IsException(pair)) return;
    JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, name));
    JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, value));
    JS_SetPropertyUint32(ctx, (JSValue)arr, hl_len(ctx, arr), pair);
}

/* §5.1 "get a header": the entries with this name joined by ", ", or NULL when the list has none.
   Caller frees. */
static char *hl_get(JSContext *ctx, JSValueConst arr, const char *name)
{
    uint32_t n = hl_len(ctx, arr), i;
    char *out = NULL;
    size_t len = 0;

    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, arr, i);
        JSValue nv = JS_GetPropertyUint32(ctx, pair, 0), vv = JS_GetPropertyUint32(ctx, pair, 1);
        const char *nm = JS_ToCString(ctx, nv), *val = JS_ToCString(ctx, vv);
        if (nm && val && hl_ci_eq(nm, name)) {
            size_t add = strlen(val), sep = out ? 2 : 0;
            char *g = realloc(out, len + sep + add + 1);
            CHECK(g != NULL, "XMLHttpRequest: OOM joining a header value");
            out = g;
            if (sep) { memcpy(out + len, ", ", 2); len += 2; }
            memcpy(out + len, val, add);
            len += add;
            out[len] = 0;
        }
        if (nm) JS_FreeCString(ctx, nm);
        if (val) JS_FreeCString(ctx, val);
        JS_FreeValue(ctx, nv); JS_FreeValue(ctx, vv); JS_FreeValue(ctx, pair);
    }
    return out;
}

/* §5.1 "combine (name, value)": if the list already contains an entry with this name, append `, value` to the
   FIRST one; otherwise append the pair. §3.5.2's example — X-Test: one then two arriving as `one, two` — is
   this operation and nothing else. */
static void hl_combine(JSContext *ctx, JSValueConst arr, const char *name, const char *value)
{
    int at = hl_find(ctx, arr, name);
    JSValue pair, vv;
    const char *old;
    char *joined;

    if (at < 0) { hl_push(ctx, arr, name, value); return; }
    pair = JS_GetPropertyUint32(ctx, arr, (uint32_t)at);
    vv = JS_GetPropertyUint32(ctx, pair, 1);
    old = JS_ToCString(ctx, vv);
    if (old) {
        joined = malloc(strlen(old) + strlen(value) + 3);
        CHECK(joined != NULL, "XMLHttpRequest: OOM combining a request header");
        sprintf(joined, "%s, %s", old, value);
        JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, joined));
        free(joined);
        JS_FreeCString(ctx, old);
    }
    JS_FreeValue(ctx, vv);
    JS_FreeValue(ctx, pair);
}

/* §5.1 "set (name, value)": replace the first matching entry's value and drop the rest. */
static void hl_set(JSContext *ctx, JSValueConst arr, const char *name, const char *value)
{
    int at = hl_find(ctx, arr, name);
    JSValue pair;

    if (at < 0) { hl_push(ctx, arr, name, value); return; }
    pair = JS_GetPropertyUint32(ctx, arr, (uint32_t)at);
    JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, value));
    JS_FreeValue(ctx, pair);
}

/* The same list as the shape a FetchRequest carries. The request the host is handed is built from this, so the
   solver's endpoint surface and the trusted zone see one list rather than two representations of it. */
static void hl_to_header_list(JSContext *ctx, JSValueConst arr, HeaderList *out)
{
    uint32_t n = hl_len(ctx, arr), i;

    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, arr, i);
        JSValue nv = JS_GetPropertyUint32(ctx, pair, 0), vv = JS_GetPropertyUint32(ctx, pair, 1);
        const char *nm = JS_ToCString(ctx, nv), *val = JS_ToCString(ctx, vv);
        if (nm && val) header_list_append(out, nm, val);
        if (nm) JS_FreeCString(ctx, nm);
        if (val) JS_FreeCString(ctx, val);
        JS_FreeValue(ctx, nv); JS_FreeValue(ctx, vv); JS_FreeValue(ctx, pair);
    }
}

/* ---- §3.6.6's four MIME operations, over MIME Sniffing §4's RECORD ------------------------------------------
 *
 * There were two functions here — a `mime_essence` that cut the string at the first ';' and a `mime_charset`
 * that walked for a `charset=` — with a comment naming their own limit. The limit is now a component:
 * core/mime/mime_type.c is §4.4's parser, §4.5's serializer and §4.6's groups, and Fetch §6's data: URL
 * processor needed the same record, which is exactly the second caller that comment forbade.
 *
 * WHAT THE STANDARD SAYS THESE RETURN IS A RECORD, NOT AN ESSENCE, and the difference is observable at three
 * of the four: "get a final MIME type" is what the blob arm gives `Blob.type` (its `charset` belongs there),
 * "get a final encoding" reads `parameters["charset"]` off two records, and `overrideMimeType()` stores one.
 *
 * THE OVERRIDE MIME TYPE IS HELD AS ITS §4.5 SERIALIZATION, not as a C record. Every field of an
 * XMLHttpRequest a flow can change is a JSValue so the COW delta captures it (see this file's head comment); a
 * malloc'd record on the data would be state no delta can park and no session can resume. The write parses and
 * stores the serialization — which is where §3.6.8 puts the failure substitution — and each read parses it
 * back, §4.4 and §4.5 being inverses over a parsed record. */

/* The standard names a LITERAL MIME type as a substitute in three places (§3.6.6's text/xml, §3.6.8's
   application/octet-stream, and the stored-override recovery below). Building the record by PARSING the
   literal is what keeps one way of making a record; a hand-assembled one would be a second. */
static void xhr_mime_literal(MimeType *out, const char *literal)
{
    bool ok;

    mime_type_free(out);
    ok = mime_type_parse(out, literal, strlen(literal));
    DCHECK(ok, "a MIME type literal the standard names as a substitute did not parse — §4.4 accepts every one "
               "of them, so a failure here is this engine's parser disagreeing with the standard");
    (void)ok;
}

/* The record a stored serialization names. */
static void xhr_mime_stored(MimeType *out, const char *serialized)
{
    if (!mime_type_parse(out, serialized, strlen(serialized))) {
        DFAIL("a stored MIME type serialization did not parse back to a record — §4.4 and §4.5 are inverses "
              "over a parsed record, and the only strings stored here are ones §4.5 produced");
        xhr_mime_literal(out, "application/octet-stream");
    }
}

/* §3.6.6 "get a response MIME type": Fetch §2.2.2 "Headers"'s "extract a MIME type" over the response's header list, and
   `text/xml` when that is failure. `out` always ends holding a record. */
static void xhr_response_mime(JSContext *ctx, XhrData *d, MimeType *out)
{
    char *ct = hl_get(ctx, d->response_headers, "content-type");
    bool ok = mime_type_extract(out, ct);

    free(ct);
    if (!ok) xhr_mime_literal(out, "text/xml");
}

/* §3.6.6 "get a final MIME type": the override MIME type if there is one, else the response's. */
static void xhr_final_mime(JSContext *ctx, XhrData *d, MimeType *out)
{
    mime_type_init(out);
    if (!JS_IsNull(d->override_mime)) {
        const char *s = JS_ToCString(ctx, d->override_mime);
        CHECK(s != NULL, "XMLHttpRequest: OOM reading the override MIME type back");
        xhr_mime_stored(out, s);
        JS_FreeCString(ctx, s);
        return;
    }
    xhr_response_mime(ctx, d, out);
}

/* §3.6.6 "get a final encoding": the RESPONSE MIME type's charset, overridden by the OVERRIDE MIME type's, and
   `null` (returned as -1) when neither names one or the label is not an encoding. The standard's own note says
   this deliberately does not use the final MIME type, "as it would not be web compatible" — so the two records
   are read separately here rather than through xhr_final_mime. */
static int xhr_final_encoding(JSContext *ctx, XhrData *d)
{
    MimeType resp, over;
    const char *label;
    int enc = -1;

    mime_type_init(&over);
    xhr_response_mime(ctx, d, &resp);
    label = mime_type_parameter(&resp, "charset");
    if (!JS_IsNull(d->override_mime)) {
        const char *s = JS_ToCString(ctx, d->override_mime);
        const char *ov;
        CHECK(s != NULL, "XMLHttpRequest: OOM reading the override MIME type back");
        xhr_mime_stored(&over, s);
        JS_FreeCString(ctx, s);
        ov = mime_type_parameter(&over, "charset");
        if (ov) label = ov;
    }
    if (label) enc = encoding_lookup(label, strlen(label));
    mime_type_free(&resp);
    mime_type_free(&over);
    return enc;
}

/* IS A GLOBAL A `Window` OBJECT — the question XHR §3.5.1 "The open() method" asks at step 1 and again at step
   9, XHR §3.5.3 "The timeout getter and setter" asks at its setter's step 1, and XHR §3.6.8 "The responseType
   getter and setter" asks at its setter's steps 1 and 3. FIVE asks, one answer, so a realm that must be told no
   is told no in one place rather than in five `if`s that were each right for the wrong reason.

   THE ARGUMENT THIS USED TO ANSWER BY IS RETIRED AND IS WRITTEN DOWN RATHER THAN DELETED, because a reader who
   re-derives it from this interface's own exposure set will re-add it. It said that every realm this engine
   builds is a Window realm — that there is no DedicatedWorkerGlobalScope and no SharedWorkerGlobalScope, so
   `[Exposed=(Window,DedicatedWorker,SharedWorker)]` reduces to its first member — and the body was
   `return true`. It was true of core/platform.c's per-DOCUMENT column and it stopped being true when
   core/realm.h's per-REALM column began building realms of every kind. This component declares itself to THAT
   column, so it is PLACED in a `DedicatedWorkerGlobalScope` realm, and every one of the five asks above was
   answering about a realm it was not in. The same retirement is why core/dom/document.c's reader of the
   realm-is-a-document pointer is release-fatal: a realm kind with no Document now exists.

   IT IS READ OFF WEB IDL §3.3.8 "[Global]"'s GLOBAL NAMES AND NEVER OFF A REALM KIND OF THIS FILE'S OWN. The
   mask core/realm.h resolves once per realm is the corpus's own vocabulary, and core/idl_args.h DERIVES the
   `Window` question from the generated [Global] rows rather than asserting it — so the day a second [Global]
   interface names `Window`, that derivation's own check fires instead of this line quietly answering yes.

   THE REALM IT ASKS ABOUT IS THE ONE THE MEMBER IS RUNNING IN, which is exactly "the current global object" that
   four of the five asks name. XHR §3.5.1 step 1 names "this's relevant global object" instead, and those are two
   concepts. NAMED RESIDUAL. WHAT IS NOT COVERED: an XMLHttpRequest whose `open` is reached through ANOTHER
   realm's prototype answers step 1 out of the calling prototype's realm rather than out of the receiver's.
   WHAT THE NEXT DIFF BUILDS: the receiver's own realm, recorded on XhrData where the object is constructed and
   read by step 1 in place of `ctx`. HOW ITS ABSENCE WOULD SHOW: open()'s first step throwing, or declining to
   throw, against a global that is not the one the receiver belongs to — observed where a receiver and the
   prototype its `open` came off belong to realms that disagree about whether their global is a `Window`.
   RETIREMENT: this record goes when XhrData carries that realm and step 1 reads it.

   XHR §3.5.4 "The withCredentials getter and setter" WAS ON THE OLD LIST AND ASKS NOTHING OF THE KIND. Its
   setter steps are "If this's state is not unsent or opened, then throw an "InvalidStateError" DOMException.
   If this's send() invoked is true, then throw an "InvalidStateError" DOMException. Set this's cross-origin
   credentials to the given value." — no global test at all, and the code correctly never asked one there. A list
   that names a section which does not ask is a mis-aimed citation no instrument here can see, so the correction
   is recorded where the claim was made. */
static bool xhr_global_is_window(JSContext *ctx)
{
    return idl_global_names_are_window(realm_global_names(ctx));
}

/* ---- the receiver check every member performs -------------------------------------------------------------- */

static XhrData *xhr_receiver(JSContext *ctx, JSValueConst this_val)
{
    XhrData *d = xhr_of(this_val);
    if (!d) JS_ThrowTypeError(ctx, "not an XMLHttpRequest");
    return d;
}

/* ---- §3.4 the readyState getter ------------------------------------------------------------------------------ */

static JSValue js_xhr_get_ready_state(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_NewInt32(ctx, d->state);
}

/* ---- §3.5.1 open() ------------------------------------------------------------------------------------------
 *
 * WHERE THIS MACHINE RESTS. Every step of §3.5.1 up to and including the state write runs on values Web IDL
 * has already converted, and the LAST step fires an event — which runs the page's listeners, so it is a
 * suspension point and the algorithm is a machine rather than a body. */
#define OPEN_STAGES(X) \
    X(OPEN_STEPS, "XHR §3.5.1 open() steps 1-12.1 (validate the method, parse the URL, terminate the fetch " \
                  "controller, reset the object's variables, and set the state to opened)") \
    X(OPEN_FIRE,  "XHR §3.5.1 open() step 12.2 (fire an event named readystatechange at this)")
enum { IDL_STEP_STAGE_BASE(OPEN_STAGES) OPEN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OPEN_STEPS_LABELS[] = { OPEN_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t phase;      /* the dispatch request's own phase, held across the suspension */
    JSValue ev;         /* the readystatechange event (owned) */
    EventFireCb cb;      /* the dispatch's request buffer */
} JSXhrOpenState;

static void js_xhr_open_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSXhrOpenState *s = st;
    int i;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* §3.5.1 step 11's reset, which is also what a second open() on a sent object performs. */
static void xhr_reset_request(JSContext *ctx, XhrData *d)
{
    d->send_invoked = 0;
    xhr_set(ctx, d, &d->author_headers, JS_NewArray(ctx));
    xhr_set(ctx, d, &d->request_body, JS_NULL);
    d->request_body_is_shape = 0;   /* the arm goes with the body it describes */
    d->request_prov = -1;           /* …and the grade goes with the request it is about */
    d->upload_listener = 0;
    /* "Set this's response to a network error", which is the initial value of every response field. */
    d->network_error = 1;
    d->aborted = 0;
    d->status = 0;
    xhr_set(ctx, d, &d->status_text, JS_NewString(ctx, ""));
    xhr_set(ctx, d, &d->response_url, JS_NewString(ctx, ""));
    xhr_set(ctx, d, &d->response_headers, JS_NewArray(ctx));
    xhr_set(ctx, d, &d->received, JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0));
    xhr_set(ctx, d, &d->response_object, JS_NULL);
    d->response_object_failure = 0;
}

/* THE ARGUMENT THE DECLARATION CONVERTED, which is the only one a declared member's body may read.
 *
 * Web IDL §3.7.7 "Operations" — create an operation function — runs "the method steps of operation, with
 * idlObject as this and VALUES as the argument values", where `values` is what §3.6 "Overload resolution
 * algorithm" produced out of `args`. `args` is what the effective overload set was SIZED by; it is never what
 * the steps read. core/idl_args.c hands `values` to a step body as its own (argc, argv) pair, while
 * `step_arg` reads the MACHINE's operands — which for a declared member is `args`.
 *
 * READING `step_arg` HERE SKIPPED THE WHOLE CONVERSION, and it did so silently for every call this component
 * ever served: §3.2.11 "ByteString"'s ToString and its "if the value of any element of x is greater than 255,
 * then throw a TypeError" never ran on `method`, and §3.2.12 "USVString"'s "converting string to a sequence of
 * scalar values" never ran on `url`, `username` or `password`. The visible half was an ABORT: a page object
 * reached §7.1.19 ToString ( arg ) at the byte consumer below, from a C activation with no flow base under it,
 * so `xhr.open({toString(){…}}, {toString(){…}})` ended the document. Step 8's own comment two screens down
 * asserts the opposite of what the code did — it says the declaration converts a `USVString?` null and
 * undefined to the IDL null, which is exactly true of `values` and false of `args`. That sentence is THIS
 * FILE'S and not §7.1.19's, so it is stated rather than quoted: a run of our own prose inside quotation marks
 * beside a citation is read by the audit as a claim about the STANDARD, and the nearest one wins.
 *
 * Out of range reads undefined, for the reason step_arg's does: that is what an optional argument means at
 * this level, and §3.6's required-arity TypeError has already run, so positions 0 and 1 are always present. */
static JSValueConst xhr_idl_arg(int argc, JSValueConst *argv, int i)
{
    return i < argc ? argv[i] : JS_UNDEFINED;
}

/* §3.5.1 STEPS 2-4 OVER A METHOD NOBODY KNOWS — the three worlds the algorithm has, and why they are three.
 *
 * Web IDL declares `method` a ByteString, and core/idl_args.h's IDL_CONCOLIC_CROSSES passes unknown external
 * input through that conversion UNCONVERTED on purpose, so that opacity survives it. That is why this arm has
 * to exist rather than being an exotic case: an orphan drive of any XHR wrapper reaches open() with an
 * ARGUMENT of a function the bundle shipped and never called, and `unfetch` — `request.open(options.method ||
 * 'get', url, true)` — is the shape, one arm of that `||` being the unknown itself.
 *
 * READING BYTES OFF IT WAS THE DEFECT, AND SO IS SUBSTITUTING ITS DISPLAY SHAPE. ECMAScript §7.1.19 ToString (
 * arg ) steps 9-12 hand an Object to §7.1.1 ToPrimitive ( input [ , preferredType ] ), which over an unknown
 * is the identity, so the byte consumer owed C a JSString it could not have and ended the document. Asking for
 * the SHAPE instead — the projection this file already takes for `url` — would not abort and would be worse:
 * `{orphan.f3.arg1}.method` is not a token, so step 2 would throw a "SyntaxError" DOMException and delete the
 * request, which is the endpoint this tool exists to emit. A predicate decided by running it over a string no
 * run ever computed is inventing 6 for `x > 5`, spent on a BRANCH rather than on a report.
 *
 * SO THE ALGORITHM DECLARES ITS COMPLETIONS AND THE SOLVER PICKS ONE PER FLOW. That is not this member's own
 * answer: core/idl_args.c's idl_number_of already writes the same one down for the same shape one IDL type
 * over, where an unknown's EXAMPLE lands on a conversion's throw arm while its domain still permits the
 * success arm — both arms must run, asked of solver_outcome over the value, exactly as JSON.parse forks its
 * SyntaxError arm.
 *
 * THEY ARE ENUMERATED AS OBSERVABLE OUTCOMES AND NOT AS BRANCHES OF THE PROSE. What the page can see is that
 * open() returned, or that it threw one of two named DOMExceptions; there is no fourth, because the operand is
 * a value rather than an algorithm that could settle nothing. The two refusals are disjoint by Fetch §2.2.1
 * Methods, whose "A forbidden method is a method that is a byte-case-insensitive match for `CONNECT`, `TRACE`,
 * or `TRACK`" makes a forbidden method a method — so step 2 and step 3 partition the domain rather than
 * overlapping, and the same section's "A method is a byte sequence that matches the method token production"
 * is the whole of what step 2 asks.
 *
 * OUTCOME 0 IS THE ORDINARY COMPLETION, which is quickjs-step.h's one numbering rule: a run with no forking
 * policy takes it, and an @S candidate re-fire replaying one concrete path must not be diverted down an
 * exceptional arm on its way to a sink. */
enum { OPEN_M_OK = 0, OPEN_M_NOT_A_METHOD, OPEN_M_FORBIDDEN, OPEN_M_OUTCOMES };
#define OPEN_METHOD_OP "XHR §3.5.1 open() steps 2-4 over the method"

/* WHICH OF THOSE THREE THE STEPS REACH WHEN RUN ON THE UNKNOWN'S OWN EXAMPLE — step_fork_run's `real`, and a
   DIFFERENT declaration from the numbering above: not "which completion does a run with no forking policy
   take" but "which one does this operation reach on the concrete value the run already carries". Stating it is
   what marks the ordinary completion PRIMARY and the other two FORCED, so a request built on this arm reaches
   solver/engine.h's provenance as what it is. JS_OUTCOME_REAL_UNSTATED is the POSITIVE answer for an unknown
   carrying no example — the fork still happens, both arms still run, and neither is marked forced.
   `*pnorm` is Fetch §2.2.1 Methods' "normalize a method" RUN on that example (the caller js_free's it), and it
   is NULL for every outcome but OPEN_M_OK — which is what drops the example on an arm that contradicts it.
   THE EXAMPLE MUST ALREADY BE A STRING AND THIS DOES NOT COERCE ONE: §7.1.19 ToString over an object would run
   the page's own valueOf from a C activation with no flow base under it, so an example of any other type is
   reported as NO example rather than converted. */
static int xhr_open_method_real(JSContext *ctx, JSValueConst mv, char **pnorm)
{
    JSValue ex = concolic_example(ctx, mv);
    const char *mc;
    int real = JS_OUTCOME_REAL_UNSTATED;

    DCHECK(pnorm != NULL, "§3.5.1 steps 2-4's example arm was asked with nowhere to put the normalized "
                          "method — the example and the completion it reaches are one answer in two halves");
    *pnorm = NULL;
    if (!JS_IsString(ex)) { JS_FreeValue(ctx, ex); return real; }
    mc = JS_ToCString(ctx, ex);
    if (mc) {
        *pnorm = request_method_check(ctx, mc);
        if (*pnorm) {
            real = OPEN_M_OK;
        } else {
            JSValue exc = JS_GetException(ctx);   /* the shared operation's TypeError is not this member's */
            JS_FreeValue(ctx, exc);
            real = request_method_is_token(mc) ? OPEN_M_FORBIDDEN : OPEN_M_NOT_A_METHOD;
        }
        JS_FreeCString(ctx, mc);
    }
    JS_FreeValue(ctx, ex);
    return real;
}

static int js_xhr_open_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSXhrOpenState *s = st;
    XhrData *d;
    int r;

    if (hdr->stage == OPEN_STEPS) {
        const char *u;
        JSValue mval;   /* step 4's NORMALIZED METHOD — owned, placed by steps 2-4 below and consumed at 11.2 */
        UrlRecord rec;
        bool ok;
        bool async = true;
        int i;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->ev = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = s->cb[3] = JS_UNDEFINED;
        d = xhr_receiver(ctx, hdr->this_val);
        if (!d) return JS_STEP_ABRUPT;
        /* THE DECLARATION'S PROMISE, ASSERTED WHERE THE BODY RELIES ON IT AND NOT AT THE CONSUMER THAT DIES.
           OPEN_ARGS declares ByteString, USVString, any, USVString? and USVString?, so after §3.6 every string
           position holds a real String, the IDL null, or the undefined an absent optional argument means. The
           one exception is unknown external input, which core/idl_args.h's IDL_CONCOLIC_CROSSES passes through
           UNCONVERTED on purpose so that opacity survives a coercion. Anything else means this body is reading
           the machine's raw `args` instead of §3.6's `values` — and that defect is INVISIBLE for a plain
           string and only aborts at the first page object several steps later, in a file that is not this one,
           which is exactly why the check belongs at the read rather than at the byte consumer.
           POSITION 2 IS EXEMPT BY ITS TYPE: `async` is declared `any` so that step 7's rule can be the argument
           COUNT (see below), and `any` crosses as itself. */
        for (i = 0; i < argc; i++) {
            JSValueConst a = xhr_idl_arg(argc, argv, i);
            DCHECK(i == 2 || JS_IsString(a) || JS_IsNull(a) || JS_IsUndefined(a) || concolic_is(a),
                   "a declared XMLHttpRequest.open() argument reached §3.5.1's steps unconverted — Web IDL "
                   "§3.7.7 Operations' create an operation function runs the method steps over §3.6 Overload "
                   "resolution algorithm's `values`, so a body reading the machine's raw `args` has skipped "
                   "§3.2.11 ByteString and §3.2.12 USVString for every call it ever served");
        }
        /* XHR §3.5.1 "The open() method" STEP 1 IS A CONJUNCTION AND THIS ENGINE DROPPED THE FIRST CONJUNCT:
           "If this's relevant global object is a Window object and its associated Document is not fully active,
           then throw an "InvalidStateError" DOMException." The Window half was missing, on the argument
           xhr_global_is_window used to carry, and this is the FIRST step of the FIRST member an XHR script
           calls — so in a `DedicatedWorkerGlobalScope` realm, where the step does not apply at all, it reached
           core/dom/document.c's release-fatal reader of the realm-is-a-document pointer instead.
           THE ORDER IS THE REPAIR AND NOT MERELY THE TRUTH: `&&` short-circuits, so a realm whose global is not
           a `Window` never asks the second question — which is what "the step does not apply" means. Such a
           realm CONTINUES to step 2, with nothing thrown and nothing defaulted; a `DCHECK` here would instead
           abort on a case the algorithm handles correctly, and a realm kind is not this codebase's value to
           assert about in any event (CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE).
           NOTHING EARLIER SEES THE OPERAND. What runs before step 1 is Web IDL §3.7.7 "Operations"' receiver
           check, which `xhr_receiver` performs, and Web IDL §3.6 "Overload resolution algorithm"'s conversions
           — neither of which is realm-kind-sensitive — and `open` carries no [Exposed=Window] of its own. */
        if (xhr_global_is_window(ctx) && !document_fully_active(ctx))
            return JS_ThrowDOMException(ctx, "InvalidStateError",
                                        "open() on an XMLHttpRequest whose document is not fully active"), -1;
        {
            JSValueConst mv = xhr_idl_arg(argc, argv, 0);
            char *norm;

            if (concolic_is(mv)) {
                int arm = 0, real = xhr_open_method_real(ctx, mv, &norm);

                /* NOTHING OF THIS MACHINE'S IS HELD ACROSS THE ASK, which quickjs-step.h requires because the
                   SIBLING'S SNAPSHOT IS TAKEN AT THE JS_STEP_FORK. The answer lands back on this same call:
                   the driver re-enters at this stage, everything above is a re-read of values the declaration
                   already converted, and step_fork_run's own phase hands the arm back rather than re-asking. */
                r = step_fork_run(ctx, hdr, mv, OPEN_METHOD_OP, OPEN_M_OUTCOMES, real, &arm);
                if (r > 0) { js_free(ctx, norm); return r; }
                DCHECK(arm >= 0 && arm < OPEN_M_OUTCOMES,
                       "XHR §3.5.1 steps 2-4's fork answered with a completion this member never declared");
                if (arm != OPEN_M_OK) {
                    js_free(ctx, norm);
                    return arm == OPEN_M_FORBIDDEN
                         ? (JS_ThrowDOMException(ctx, "SecurityError", "open() with a forbidden method"), -1)
                         : (JS_ThrowDOMException(ctx, "SyntaxError",
                                                 "open() with a method that is not a token"), -1);
                }
                /* STEP 4 OVER AN UNKNOWN IS A DERIVATION AND NEVER A DECISION. Fetch §2.2.1 Methods — "To
                   normalize a method, if it is a byte-case-insensitive match for `DELETE`, `GET`, `HEAD`,
                   `OPTIONS`, `POST`, or `PUT`, byte-uppercase it" — answers an unknown with an unknown in
                   BOTH of its worlds, so there is nothing here to fork over and only a new value to name. The
                   example is the real operation run on the operand's own example, and is absent on the two
                   arms that example contradicts: a forced sibling drops it. */
                mval = concolic_new_derived(ctx, "normalize a method", &mv, 1,
                                            norm ? JS_NewString(ctx, norm) : JS_UNDEFINED);
                js_free(ctx, norm);
                DCHECK(!JS_IsUninitialized(mval),
                       "§3.5.1 step 4's derivation answered that no operand was unknown — this arm is reached "
                       "only where concolic_is says one is, so a refusal here means the two disagree");
            } else {
                const char *m = JS_ToCString(ctx, mv);

                if (!m) return JS_STEP_ABRUPT;
                /* Steps 2-4 as ONE operation, through the implementation Fetch §5.3 step 25 already uses: not
                   a method is a SyntaxError here where Fetch throws a TypeError, and a forbidden one is a
                   SecurityError — so the shared operation answers and this member names its own errors. */
                norm = request_method_check(ctx, m);
                if (!norm) {
                    JSValue exc = JS_GetException(ctx);   /* the shared operation's TypeError is not this member's */
                    JS_FreeValue(ctx, exc);
                    r = request_method_is_token(m)
                      ? (JS_ThrowDOMException(ctx, "SecurityError", "open() with a forbidden method"), -1)
                      : (JS_ThrowDOMException(ctx, "SyntaxError", "open() with a method that is not a token"), -1);
                    JS_FreeCString(ctx, m);
                    return r;
                }
                JS_FreeCString(ctx, m);
                mval = JS_NewString(ctx, norm);
                js_free(ctx, norm);
            }
        }
        /* Steps 5-6: encoding-parse the URL against the relevant settings object's API base URL. */
        u = xhr_arg_cstring(ctx, xhr_idl_arg(argc, argv, 1), NULL);
        if (!u) { JS_FreeValue(ctx, mval); return JS_STEP_ABRUPT; }
        url_record_init(&rec);
        ok = fetch_parse_url(ctx, &rec, u, strlen(u));
        JS_FreeCString(ctx, u);
        if (!ok) {
            JS_FreeValue(ctx, mval);
            url_record_free(&rec);
            return JS_ThrowDOMException(ctx, "SyntaxError", "open() with a URL that cannot be parsed"), -1;
        }
        /* Step 7: an OMITTED async argument is true; an `undefined` one is not omitted, which is the legacy
           note §3.5.1 makes explicitly — so the count decides, never the value. */
        if (argc > 2) async = JS_ToBool(ctx, xhr_idl_arg(argc, argv, 2)) != 0;
        /* Step 8: the credentials, set into the parsed URL when it has a host. Step 7 already left them null
           for the omitted-argument call, and the declaration converts `USVString?` null and undefined to the
           IDL null — so the two tests below ARE step 8's two conditions and there is no third to add. */
        {
            JSValueConst user = xhr_idl_arg(argc, argv, 3), pass = xhr_idl_arg(argc, argv, 4);
            if (rec.host.kind != URL_HOST_NULL) {
                if (!JS_IsNull(user) && !JS_IsUndefined(user)) {
                    const char *v = JS_ToCString(ctx, user);
                    if (v) { url_member_set(&rec, URL_USERNAME, v, strlen(v)); JS_FreeCString(ctx, v); }
                }
                if (!JS_IsNull(pass) && !JS_IsUndefined(pass)) {
                    const char *v = JS_ToCString(ctx, pass);
                    if (v) { url_member_set(&rec, URL_PASSWORD, v, strlen(v)); JS_FreeCString(ctx, v); }
                }
            }
        }
        /* Step 9: a synchronous request on a Window may carry neither a timeout nor a response type. */
        if (!async && xhr_global_is_window(ctx) && (d->timeout != 0 || d->response_type != RT_EMPTY)) {
            JS_FreeValue(ctx, mval);
            url_record_free(&rec);
            return JS_ThrowDOMException(ctx, "InvalidAccessError",
                                        "a synchronous open() with a timeout or a responseType set"), -1;
        }
        /* Step 10 terminates the fetch controller ("A fetch can be ongoing at this point"). What "terminate"
           means for the RESPONSE is that it is discarded, which step 11's `send() invoked = false` already
           decides: §3.5.6's "handle errors" step 1 returns for an object that is not sending, and the
           lifecycle machine's XR_RESPONSE reads that and finishes without firing anything.
           NAMED RESIDUAL — that is correct for the response and NARROWER than step 10, which also stops the
           TRANSFER. NOT COVERED: the rendezvous the in-flight send parked on is left outstanding, so the
           trusted zone finishes a fetch whose reply nothing will read (§3.5.7 "The abort() method" calls that
           "any network activity"). WHAT THE NEXT DIFF BUILDS: the fetch controller reachable from the OBJECT
           rather than from the lifecycle machine's step state — this method and §3.5.7's are different
           machines and neither can see the other's `s->req` — so both can call solver/engine.h's
           `engine_host_terminate`, which js_xhr_run_fini already calls for §3.2 Garbage collection. HOW ITS
           ABSENCE SHOWS: `open()` during a send leaves one entry in the frontier's outstanding join and the
           census reports the ask unpaid and unwithdrawn (hostAsked − hostAnswered − hostTerminated) until the
           zone's own reply lands and is refused by engine_host_answer as naming no register. */
        /* Step 11. */
        xhr_reset_request(ctx, d);
        xhr_set(ctx, d, &d->method, mval);
        {
            char *ser = url_serialize(&rec, false);
            xhr_set(ctx, d, &d->url, JS_NewString(ctx, ser ? ser : ""));
            free(ser);
        }
        /* AND THE ARGUMENT ITSELF, kept beside the serialization for the @H surface — see `url_src`. Held only
           when it carries something the serialization does not: a plain string IS its own parse, so storing
           one would make every endpoint carry a second copy of its own address. */
        xhr_set(ctx, d, &d->url_src, concolic_is(xhr_idl_arg(argc, argv, 1))
                                   ? JS_DupValue(ctx, xhr_idl_arg(argc, argv, 1)) : JS_NULL);
        url_record_free(&rec);
        d->synchronous = async ? 0 : 1;
        /* Step 12: only a state that is not already opened fires. */
        if (d->state == XHR_OPENED) { *presult = JS_UNDEFINED; return 0; }
        d->state = XHR_OPENED;
        s->ev = event_new(ctx, "readystatechange", /*bubbles*/ false, /*cancelable*/ false);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(hdr->stage, OPEN_FIRE, &s->phase, NULL);
    }

    DCHECK(hdr->stage == OPEN_FIRE, "the open() machine resumed at a stage §3.5.1 does not have");
    r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), hdr->this_val, s->ev, JS_UNDEFINED, cb_result, NULL,
                              out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    *presult = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl XHR_OPEN_DECL = {
    js_xhr_open_step, sizeof(JSXhrOpenState), js_xhr_open_visit, NULL,
    "XHR §3.5.1 open(method, url, async, username, password)", OPEN_STEPS_LABELS
};

/* ---- §3.5.2 setRequestHeader() -------------------------------------------------------------------------------- */

static JSValue js_xhr_set_request_header(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    const char *name, *value;
    char *norm;
    size_t norm_len = 0, name_len = 0, value_len = 0;
    char *lower;
    size_t i;

    (void)magic;
    if (!d) return JS_EXCEPTION;
    if (d->state != XHR_OPENED)
        return JS_ThrowDOMException(ctx, "InvalidStateError", "setRequestHeader() before open()");
    if (d->send_invoked)
        return JS_ThrowDOMException(ctx, "InvalidStateError", "setRequestHeader() after send()");
    name = argc > 0 ? xhr_arg_cstring(ctx, argv[0], &name_len) : NULL;
    value = argc > 1 ? xhr_arg_cstring(ctx, argv[1], &value_len) : NULL;
    if (!name || !value) {
        if (name) JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_EXCEPTION;
    }
    /* Steps 3-4 as Fetch §5.1 states them, through the one implementation of that grammar: normalize the
       value, then refuse a name that is not a token or a value that is not a header value. */
    norm = header_value_normalize_valid(value, value_len, &norm_len);
    JS_FreeCString(ctx, value);
    if (!header_name_valid(name, name_len) || !norm) {
        free(norm);
        JS_FreeCString(ctx, name);
        return JS_ThrowDOMException(ctx, "SyntaxError", "setRequestHeader() with a bad header name or value");
    }
    /* Step 5: a forbidden request-header is a silent no-op, never a throw — a page that sets `Host`
       defensively must keep working. */
    lower = malloc(name_len + 1);
    CHECK(lower != NULL, "XMLHttpRequest: OOM lowercasing a header name");
    for (i = 0; i < name_len; i++)
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] - 'A' + 'a') : name[i];
    lower[name_len] = 0;
    if (!header_forbidden_request(lower, norm))
        hl_combine(ctx, d->author_headers, name, norm);   /* step 6 */
    free(lower);
    free(norm);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* ---- §3.5.3 timeout, §3.5.4 withCredentials, §3.5.5 upload ------------------------------------------------- */

static JSValue js_xhr_get_timeout(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_NewUint32(ctx, d->timeout);
}

static JSValue js_xhr_set_timeout(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    uint32_t n = 0;

    (void)magic;
    if (!d) return JS_EXCEPTION;
    if (xhr_global_is_window(ctx) && d->synchronous)
        return JS_ThrowDOMException(ctx, "InvalidAccessError", "timeout set on a synchronous XMLHttpRequest");
    /* The declaration has already converted the value to an `unsigned long`, so this runs none of the page's
       code. */
    JS_ToUint32(ctx, &n, val);
    d->timeout = n;
    return JS_UNDEFINED;
}

static JSValue js_xhr_get_with_credentials(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_NewBool(ctx, d->cross_origin_credentials);
}

static JSValue js_xhr_set_with_credentials(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);

    (void)magic;
    if (!d) return JS_EXCEPTION;
    if (d->state != XHR_UNSENT && d->state != XHR_OPENED)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "withCredentials set on an XMLHttpRequest past opened");
    if (d->send_invoked)
        return JS_ThrowDOMException(ctx, "InvalidStateError", "withCredentials set after send()");
    d->cross_origin_credentials = JS_ToBool(ctx, val) ? 1 : 0;
    return JS_UNDEFINED;
}

static JSValue js_xhr_get_upload(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_DupValue(ctx, d->upload);
}

/* ---- §3.6.1-§3.6.5, §3.6.7-§3.6.8 the plain response members ------------------------------------------------ */

/* §3.6.1 The responseURL getter: "return the empty string if this's response's URL is null; otherwise its
   serialization with the exclude fragment flag set". BOTH halves are already in the record — the empty string
   is what xhr_reset_response leaves, and xhr_take_reply is where the fragment is excluded, so that the source
   identity this reply's bytes carry and the address the page reads are one answer. */
static JSValue js_xhr_get_response_url(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_DupValue(ctx, d->response_url);
}

static JSValue js_xhr_get_status(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_NewInt32(ctx, d->network_error ? 0 : d->status);
}

static JSValue js_xhr_get_status_text(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    (void)magic;
    if (!d) return JS_EXCEPTION;
    return d->network_error ? JS_NewString(ctx, "") : JS_DupValue(ctx, d->status_text);
}

static JSValue js_xhr_get_response_header(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                          int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    const char *name;
    char *got;
    JSValue out;

    (void)magic;
    if (!d) return JS_EXCEPTION;
    name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!name) return JS_EXCEPTION;
    got = hl_get(ctx, d->response_headers, name);
    JS_FreeCString(ctx, name);
    out = got ? JS_NewString(ctx, got) : JS_NULL;
    free(got);
    return out;
}

/* §3.6.5's "legacy-uppercased-byte less than": compare the BYTE-UPPERCASED names. */
static int hl_legacy_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)((*a >= 'a' && *a <= 'z') ? *a - 'a' + 'A' : *a);
        unsigned char y = (unsigned char)((*b >= 'a' && *b <= 'z') ? *b - 'a' + 'A' : *b);
        if (x != y) return x < y ? -1 : 1;
    }
    return *a ? 1 : (*b ? -1 : 0);
}

static JSValue js_xhr_get_all_response_headers(JSContext *ctx, JSValueConst this_val, int argc,
                                               JSValueConst *argv, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    uint32_t n, i;
    char **names = NULL;
    int nn = 0, k;
    char *out = NULL;
    size_t len = 0;
    JSValue res;

    (void)argc; (void)argv; (void)magic;
    if (!d) return JS_EXCEPTION;
    n = hl_len(ctx, d->response_headers);
    /* "sort and combine": one entry per distinct name, values joined by ", ", then sorted by the
       legacy-uppercased comparison. The distinct names first. */
    names = calloc(n ? n : 1, sizeof *names);
    CHECK(names != NULL, "XMLHttpRequest: OOM listing the response header names");
    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, d->response_headers, i);
        JSValue nv = JS_GetPropertyUint32(ctx, pair, 0);
        const char *nm = JS_ToCString(ctx, nv);
        if (nm) {
            for (k = 0; k < nn; k++) if (hl_ci_eq(names[k], nm)) break;
            if (k == nn) {
                names[nn] = strdup(nm);
                CHECK(names[nn] != NULL, "XMLHttpRequest: OOM copying a response header name");
                nn++;
            }
            JS_FreeCString(ctx, nm);
        }
        JS_FreeValue(ctx, nv);
        JS_FreeValue(ctx, pair);
    }
    for (k = 1; k < nn; k++) {   /* insertion sort: a header list is short and this keeps the order stable */
        char *cur = names[k];
        int j = k - 1;
        while (j >= 0 && hl_legacy_cmp(names[j], cur) > 0) { names[j + 1] = names[j]; j--; }
        names[j + 1] = cur;
    }
    for (k = 0; k < nn; k++) {
        char *v = hl_get(ctx, d->response_headers, names[k]);
        size_t add = strlen(names[k]) + 2 + (v ? strlen(v) : 0) + 2;
        char *g = realloc(out, len + add + 1);
        CHECK(g != NULL, "XMLHttpRequest: OOM building getAllResponseHeaders()");
        out = g;
        len += (size_t)sprintf(out + len, "%s: %s\r\n", names[k], v ? v : "");
        free(v);
        free(names[k]);
    }
    free(names);
    res = JS_NewStringLen(ctx, out ? out : "", len);
    free(out);
    return res;
}

static JSValue js_xhr_override_mime_type(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    const char *mime;
    MimeType m;
    char *ser;

    (void)magic;
    if (!d) return JS_EXCEPTION;
    if (d->state == XHR_LOADING || d->state == XHR_DONE)
        return JS_ThrowDOMException(ctx, "InvalidStateError", "overrideMimeType() while loading or done");
    mime = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!mime) return JS_EXCEPTION;
    /* Steps 2-3: the override MIME type is the result of PARSING the argument, and an unparsable one is
       application/octet-stream. What is stored is §4.5's serialization of that record — so
       `overrideMimeType("TEXT/XML; CHARSET=\"Shift_JIS\"")` is held as `text/xml;charset=Shift_JIS`, which is
       the record and not the argument. It used to keep the argument verbatim and re-split it at every read. */
    if (!mime_type_parse(&m, mime, strlen(mime)))
        xhr_mime_literal(&m, "application/octet-stream");
    ser = mime_type_serialize(&m);
    xhr_set(ctx, d, &d->override_mime, JS_NewString(ctx, ser));
    free(ser);
    mime_type_free(&m);
    JS_FreeCString(ctx, mime);
    return JS_UNDEFINED;
}

static JSValue js_xhr_get_response_type(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_NewString(ctx, RT_NAME[d->response_type]);
}

static JSValue js_xhr_set_response_type(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);
    const char *s;
    int k, found = -1;

    (void)magic;
    if (!d) return JS_EXCEPTION;
    s = JS_ToCString(ctx, val);
    if (!s) return JS_EXCEPTION;
    for (k = 0; k < (int)(sizeof(RT_NAME) / sizeof(RT_NAME[0])); k++)
        if (!strcmp(RT_NAME[k], s)) { found = k; break; }
    JS_FreeCString(ctx, s);
    /* Web IDL §3.7.6: assigning a value outside an enumeration-typed attribute's enumeration is IGNORED, not a
       TypeError. That is the type's rule, so it is here rather than in the algorithm. */
    if (found < 0) return JS_UNDEFINED;
    /* Step 1: "document" is ignored outside a Window. */
    if (found == RT_DOCUMENT && !xhr_global_is_window(ctx)) return JS_UNDEFINED;
    if (d->state == XHR_LOADING || d->state == XHR_DONE)
        return JS_ThrowDOMException(ctx, "InvalidStateError", "responseType set while loading or done");
    if (xhr_global_is_window(ctx) && d->synchronous)
        return JS_ThrowDOMException(ctx, "InvalidAccessError",
                                    "responseType set on a synchronous XMLHttpRequest");
    d->response_type = (uint8_t)found;
    return JS_UNDEFINED;
}

/* ---- §3.6.6 the response body ------------------------------------------------------------------------------- */

/* WHERE THESE BYTES CAME FROM, ASKED OF THE TWO MEMBERS WHOSE VALUE **IS** THE CONTENT.
 *
 * §3.6.9 The response getter's text arm and §3.6.10 The responseText getter hand the page a STRING it computes
 * with, and §3.6.9's json arm hands it a RECORD it reads fields off. Those are the values §solver's trust
 * boundary is written about: "a config/data fetch is ALWAYS loaded so its fields become concrete examples,
 * while its use in a BRANCH still forks (a loaded `features.admin:false` must NOT concretize the gate, or the
 * admin endpoint is lost)". Handed over bare, `cfg.region` is right and `if (cfg.admin)` takes exactly one
 * arm — and a member the server did not send for THIS visitor answers `undefined`, which is the logged-in
 * surface buried behind a gate that never forked. The triple answers both from one place: the bytes that were
 * actually sent are the EXAMPLE, and the value is opaque for control flow, so the gate forks either way.
 *
 * THE OTHER ARMS DO NOT ASK, and that is a fact about their VALUES rather than an omission here. §3.6.9's
 * arraybuffer and blob arms answer with a CONTAINER over the same bytes — the page writes
 * `new Uint8Array(xhr.response)`, and a concolic in the container's place breaks the construction instead of
 * describing it — and the document arm answers a Document, whose CONTENT is read back through the DOM, where
 * every string a flow takes out of it is that component's to state.
 *
 * THE NAME IS core/fetch/reply_source.h's, NOT THIS FILE'S. `fetch()` and this component are two doors onto
 * one fact and a name minted at each door is two names for one unknown; the address they name it after is
 * §3.6.1 The responseURL getter's on this side and `response.url` on the other. */
static JSValue xhr_reply_content(JSContext *ctx, XhrData *d, JSValue value)
{
    const char *url;
    size_t ulen = 0;
    char *src, *shape;
    size_t n;
    JSValue r;

    if (JS_IsException(value) || !concolic_is_exploring())
        return value;
    /* TWO DOORS WRAPPING ONE READ would report a derivation the run never made — the composed shape would name
       a read of a read. §3.6.9's json arm caches its result in `response_object` and step 4 answers with the
       cached one, so a second mint over it is this file having wrapped its own value twice. */
    DCHECK(!concolic_is(value),
           "an XMLHttpRequest response value reached the provenance question already carrying one — two doors "
           "have wrapped one read, and the value would report a derivation the run never performed");
    DCHECK(JS_IsString(d->response_url),
           "an XMLHttpRequest's response URL is not a string — §3.6.1 The responseURL getter returns a "
           "USVString and this record spells one at every write, so a non-string here is a write that did not");
    url = JS_ToCStringLen(ctx, &ulen, d->response_url);
    CHECK(url != NULL, "XMLHttpRequest: OOM reading the address a reply came from");
    src = reply_source_name(url, ulen);
    JS_FreeCString(ctx, url);
    /* UNLIKE a Response, an XMLHttpRequest cannot be handed bytes the page composed: every byte it holds came
       out of xhr_take_reply, which writes the address in the same breath. The two members that reach here do
       so only at LOADING or DONE and only past their own network-error arms, so an address-less reply here is
       a reply record that arrived without one. */
    DCHECK(src != NULL,
           "an XMLHttpRequest reached §3.6.9/§3.6.10 with received bytes and no address — xhr_take_reply "
           "writes the received bytes and the response's URL together, so bytes without an address are that "
           "write having happened by halves");
    if (!src)
        return value;
    n = strlen(src) + 3;
    shape = (char *)malloc(n);
    CHECK(shape != NULL, "XMLHttpRequest: OOM spelling the provenance of a reply");
    snprintf(shape, n, "{%s}", src);   /* a declared source's shape IS its provenance in braces — concolic.h */
    /* concolic_new AND NOT concolic_source_wrap: a server's reply is unknown input the ATTACKER did not
       author, and minting it through the attacker door would increment the count an empty @S surface is read
       against — reporting a page that read no attacker source as one that read many. */
    r = concolic_new(ctx, shape, src, value);   /* consumes `value` as the example */
    free(shape);
    free(src);
    return r;
}

/* §3.6.6 "get a text response": decode the received bytes with the final encoding, defaulting to UTF-8.
   THE DECODE IS SPLIT FROM THE PROVENANCE WRAP BELOW BECAUSE IT HAS A SECOND READER AND THE WRAP MUST NOT.
   `xhr_take_reply`'s program route wants §3.6.6's CHARACTERS — the source text a page that went on to
   `eval(xhr.responseText)` would have compiled — and a concolic is not a program: handing the compiler one
   would ask it to parse a DISPLAY SHAPE. The wrap belongs to the two members whose VALUE is the content,
   which is what the paragraph above this pair is about, and a program is not one of them. */
static JSValue xhr_decode_text(JSContext *ctx, XhrData *d)
{
    const uint8_t *bytes;
    size_t len = 0;
    int enc;
    EncDecoder *dec;
    JSValue out;

    if (d->network_error) return JS_NewString(ctx, "");
    /* THE RECEIVED BYTES, DECODED HERE AND NOWHERE EARLIER. This decode is the point of the algorithm, and
       until the reply record carried bytes it was the SECOND one: the trusted zone had already run UTF-8 over
       the response, so a `charset=shift_jis` reply arrived as U+FFFD and `xhr_final_encoding` chose a decoder
       for bytes that no longer existed. */
    bytes = fetch_body_bytes(ctx, d->received, &len);
    enc = xhr_final_encoding(ctx, d);
    if (enc < 0) enc = encoding_lookup("utf-8", 5);
    dec = enc_decoder_new(enc, /*fatal*/ false, /*ignore_bom*/ false);
    CHECK(dec != NULL, "XMLHttpRequest: OOM building the response decoder");
    out = enc_decoder_decode(ctx, dec, bytes, len, /*stream*/ false);
    enc_decoder_free(dec);
    return out;
}

/* …AND THE SAME CHARACTERS AS A VALUE THE PAGE READS, which is where the provenance question is asked and
   the only place it may be: §3.6.9 The response getter's text arm and §3.6.10 The responseText getter are the
   two readers whose result the page computes with. */
static JSValue xhr_text_response(JSContext *ctx, XhrData *d)
{
    return xhr_reply_content(ctx, d, xhr_decode_text(ctx, d));
}

/* §3.6.6 "set a document response". The HTML arm is lexbor's parser over a document with NO browsing context —
   which is what "with scripting disabled" means here rather than a flag: a document.c document has no
   navigable, no Window and therefore nothing to run a script in. */
static void xhr_set_document_response(JSContext *ctx, XhrData *d)
{
    MimeType final_mime;
    char *content_type;
    const uint8_t *bytes;
    size_t len = 0;
    lxb_html_document_t *dom;
    const char *url;

    if (d->network_error) return;                        /* step 1: a null body */
    xhr_final_mime(ctx, d, &final_mime);                 /* step 2 */
    if (!mime_type_is_html(&final_mime) && !mime_type_is_xml(&final_mime)) {                     /* step 3 */
        mime_type_free(&final_mime);
        return;
    }
    if (d->response_type == RT_EMPTY && mime_type_is_html(&final_mime)) {                        /* step 4 */
        mime_type_free(&final_mime);
        return;
    }
    if (!mime_type_is_html(&final_mime)) {
        /* STEP 6's XML ARM: "Otherwise, let document be a document that represents the result of running the
           XML parser with XML scripting support disabled on xhr's received bytes. If that fails (unsupported
           character encoding, namespace well-formedness error, etc.), then return null."
           THE FAILURE IS THE SPEC'S NULL AND NOT A `parsererror`, which is where this differs from HTML
           §8.5.1's XML arm over the same parser: the `parsererror` document is that section's own consequence,
           and this section's is that `responseXML` stays null. So the report is READ and discarded — the
           partial tree with it, since `d->response_object` is only written on the success path below. */
        XmlParseReport report;
        lxb_dom_node_t *xroot;
        content_type = mime_type_essence(&final_mime);
        mime_type_free(&final_mime);
        bytes = fetch_body_bytes(ctx, d->received, &len);
        dom = dom_document_create();
        CHECK(dom != NULL, "XMLHttpRequest: OOM building the response document");
        xroot = lxb_dom_interface_node(dom);
        /* FLOW-PRIVATE: the response Document is this operation's own, made immediately above. */
        if (!xml_parse_document(lxb_dom_interface_document(dom), xroot, DOM_PARSE_ROOT_PRIVATE,
                                (const char *)bytes, len, &report)) {
            free(content_type);
            /* core/dom/node_interface.h's destroy and NOT lexbor's: a document's nodes come out of the AGENT's
               heap, so `lxb_html_document_destroy` would hand back arenas every other document in this
               instance is still allocating out of. */
            dom_document_destroy(dom);
            return;                                                                             /* null */
        }
        url = JS_ToCString(ctx, d->response_url);
        /* Steps 8-11, as on the HTML arm below and for its reasons. AN XML DOCUMENT: this arm's document is
           "the result of running the XML parser", and only the HTML arm carries §3.6.6's "Flag document as an
           HTML document" step — so §4.5's default type `xml` stands, which is what `responseXML` on an XML
           response must be for `createCDATASection` and for the parse-boundary correction to stay away.
           AND IT IMPLEMENTS `Document`: XHR §3.6.6 "set a document response" says "let document be a document
           that represents the result of running the XML parser" and the word XMLDocument does not occur
           anywhere in that standard, so `xhr.responseXML instanceof XMLDocument` is false — the same split
           HTML §8.5.1's parseFromString makes, and the reason core/dom/document.h states the interface
           separately from the type. */
        xhr_set(ctx, d, &d->response_object,
                document_new(ctx, dom, url ? url : "", DOCUMENT_IFACE_DOCUMENT,
                             document_kind(/*is_xml*/true, content_type)));
        if (url) JS_FreeCString(ctx, url);
        free(content_type);
        return;
    }
    /* Step 10's content type. DOM §4.5 gives a document a content type that is a STRING, and every other place
       the platform sets one sets an ESSENCE (createDocument's application/xml, DOMParser's own type) — so the
       essence is what a document holds, and the parameters stay on the record this algorithm read them from. */
    content_type = mime_type_essence(&final_mime);
    mime_type_free(&final_mime);
    bytes = fetch_body_bytes(ctx, d->received, &len);
    dom = dom_document_create();
    CHECK(dom != NULL, "XMLHttpRequest: OOM building the response document");
    /* Step 5's charset: the final encoding, then the prescan, then UTF-8. lexbor's parser takes UTF-8, and the
       received bytes now reach it AS THE BYTES THE SERVER SENT — they used to arrive re-encoded out of a JS
       string the reply record's producer had already decoded, so the sentence that stood here ("decoded by the
       string boundary rather than by the prescan") described a boundary that no longer exists. What is still
       owed is the WIRING, not the prescan: §3.6.6 step 5 runs the final encoding, then the meta-charset
       prescan (html_prescan_byte_stream, core/html/html_encoding_sniff.h), then UTF-8, and this arm runs the
       third of those three. The citation that stood here named §13.2.3.3 for that prescan and was WRONG: the
       prescan is defined inside HTML §13.2.3.2 "Determining the character encoding", while §13.2.3.3 is
       "Character encodings", the list of encodings a user agent must support. Steps 6 and 9 land with it —
       "a known definite encoding" is not Encoding §6.1's `decode`, which lets a BOM overrule the label — and
       both need `document_new` to take an encoding, which is one change to its contract. */
    /* FLOW-PRIVATE: the response Document is this operation's own, made immediately above. */
    CHECK(html_parse_document(dom, DOM_PARSE_ROOT_PRIVATE, HTML_SCRIPTING_DISABLED, (const lxb_char_t *)bytes, len) == LXB_STATUS_OK,
          "XMLHttpRequest: the response document could not be parsed");
    url = JS_ToCString(ctx, d->response_url);
    /* Steps 8-11: the document's encoding, content type, URL and origin. document_new takes the address and
       §4.5's creation pair; the origin is the realm's, which is what a document made in this realm has.
       "FLAG DOCUMENT AS AN HTML DOCUMENT" is §3.6.6's own step on this arm and it is the `is_xml` half — a
       fact the content type cannot stand in for, because step 9 sets that to finalMIME's essence and an HTML
       MIME type is not only "text/html" (`application/xhtml+xml` is an XML MIME type and takes the arm
       above, but `text/html;charset=…`'s essence is what lands here). */
    xhr_set(ctx, d, &d->response_object,
            document_new(ctx, dom, url ? url : "", DOCUMENT_IFACE_DOCUMENT,
                         document_kind(/*is_xml*/false, content_type)));
    if (url) JS_FreeCString(ctx, url);
    free(content_type);
}

/* §3.6.9 the response getter — a STEP GETTER because the "document" arm PARSES, which is work of the page's
   size and must be able to yield. */
#define RESPONSE_STAGES(X) \
    X(RESPONSE_BUILD, "XHR §3.6.9 the response getter steps 1-6 (the text arm, or building the response " \
                      "object for arraybuffer/blob/document/json)")
enum { IDL_STEP_STAGE_BASE(RESPONSE_STAGES) RESPONSE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RESPONSE_STEPS[] = { RESPONSE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } JSXhrResponseState;
static void js_xhr_response_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSXhrResponseState *s = st;
    (void)ctx; (void)s; (void)v;
}

static int js_xhr_response_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    XhrData *d;

    (void)st; (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == RESPONSE_BUILD, "the response getter resumed at a stage §3.6.9 does not have");
    d = xhr_receiver(ctx, hdr->this_val);
    if (!d) return JS_STEP_ABRUPT;
    /* Step 1: the text arms answer while LOADING as well as when done, which is the whole reason a page can
       stream a text response. */
    if (d->response_type == RT_EMPTY || d->response_type == RT_TEXT) {
        if (d->state != XHR_LOADING && d->state != XHR_DONE) { *presult = JS_NewString(ctx, ""); return 0; }
        *presult = xhr_text_response(ctx, d);
        return JS_IsException(*presult) ? JS_STEP_ABRUPT : 0;
    }
    if (d->state != XHR_DONE) { *presult = JS_NULL; return 0; }                  /* step 2 */
    if (d->response_object_failure) { *presult = JS_NULL; return 0; }            /* step 3 */
    if (!JS_IsNull(d->response_object)) {                                        /* step 4 */
        *presult = JS_DupValue(ctx, d->response_object);
        return 0;
    }
    if (d->response_type == RT_ARRAYBUFFER) {                                    /* step 5 */
        size_t len = 0;
        const uint8_t *bytes = fetch_body_bytes(ctx, d->received, &len);
        JSValue buf;
        /* A COPY, because §3.6.9 step 5 makes a NEW ArrayBuffer the page owns and may detach: handing back the
           received bytes themselves would let `structuredClone(xhr.response, {transfer:[…]})` detach the
           response the object still holds. */
        buf = JS_NewArrayBufferCopy(ctx, bytes, len);
        if (JS_IsException(buf)) {
            /* "If this throws an exception, then set this's response object to failure and return null." */
            JS_FreeValue(ctx, JS_GetException(ctx));
            d->response_object_failure = 1;
            *presult = JS_NULL;
            return 0;
        }
        xhr_set(ctx, d, &d->response_object, buf);
    } else if (d->response_type == RT_BLOB) {
        size_t len = 0;
        const uint8_t *bytes = fetch_body_bytes(ctx, d->received, &len);
        /* Step 6: "a new Blob object representing this's received bytes with type set to the result of get a
           final MIME type" — the RECORD, and a Blob's type is a string, so it is §4.5's serialization. The
           essence alone dropped the `charset` a page reads straight back off `blob.type`. */
        MimeType m;
        char *type;
        xhr_final_mime(ctx, d, &m);
        type = mime_type_serialize(&m);
        mime_type_free(&m);
        xhr_set(ctx, d, &d->response_object, blob_new(ctx, (const char *)bytes, len, type));
        free(type);
    } else if (d->response_type == RT_DOCUMENT) {
        xhr_set_document_response(ctx, d);
    } else {
        size_t len = 0, text_n = 0;
        const uint8_t *bytes;
        char *text;
        JSValue parsed;
        DCHECK(d->response_type == RT_JSON, "the response getter reached an arm §3.6.9 does not have");
        if (d->network_error) { *presult = JS_NULL; return 0; }
        bytes = fetch_body_bytes(ctx, d->received, &len);
        /* Step 7's "parse JSON from bytes", which is TWO steps and not one: Infra's algorithm is "let string be
           the result of running UTF-8 decode on bytes", THEN `JSON.parse` on that string. The decode is what
           turns a malformed sequence into a U+FFFD the parser can see — handing the raw bytes to JS_ParseJSON
           instead runs quickjs's own lenient decoder, which accepts UTF-8-encoded surrogates JSON's grammar
           does not produce. A throw is answered with null and the object stays unset. */
        text = encoding_utf8_decode((const char *)bytes, len, &text_n);
        CHECK(text != NULL, "XMLHttpRequest: OOM decoding a JSON response's bytes");
        parsed = JS_ParseJSON(ctx, text, text_n, "<xhr response>");
        free(text);
        if (JS_IsException(parsed)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            *presult = JS_NULL;
            return 0;
        }
        /* THE PROVENANCE RIDES THE CACHED OBJECT AND NOT THE READ. §3.6.9 step 4 answers with this's response
           object once it is set, so `xhr.response === xhr.response` — minting per read would hand the page two
           records where the spec gives it one, and only the first read's would be the one a later gate is
           deciding about. */
        xhr_set(ctx, d, &d->response_object, xhr_reply_content(ctx, d, parsed));
    }
    *presult = JS_DupValue(ctx, d->response_object);
    return 0;
}

static const IdlStepDecl XHR_RESPONSE_DECL = {
    js_xhr_response_step, sizeof(JSXhrResponseState), js_xhr_response_visit, NULL,
    "XHR §3.6.9 the response getter", RESPONSE_STEPS
};

/* §3.6.10 responseText. */
static JSValue js_xhr_get_response_text(JSContext *ctx, JSValueConst this_val, int magic)
{
    XhrData *d = xhr_receiver(ctx, this_val);

    (void)magic;
    if (!d) return JS_EXCEPTION;
    if (d->response_type != RT_EMPTY && d->response_type != RT_TEXT)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "responseText read with a responseType that is not '' or 'text'");
    if (d->state != XHR_LOADING && d->state != XHR_DONE)
        return JS_NewString(ctx, "");
    return xhr_text_response(ctx, d);
}

/* §3.6.11 responseXML — a STEP GETTER for the same reason `response` is: it parses. */
#define RESPONSE_XML_STAGES(X) \
    X(RESPONSE_XML_BUILD, "XHR §3.6.11 the responseXML getter steps 1-6 (set a document response for this, " \
                          "then return this's response object)")
enum { IDL_STEP_STAGE_BASE(RESPONSE_XML_STAGES) RESPONSE_XML_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RESPONSE_XML_STEPS[] = { RESPONSE_XML_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_xhr_response_xml_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    XhrData *d;

    (void)st; (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == RESPONSE_XML_BUILD, "the responseXML getter resumed at a stage §3.6.11 does not have");
    d = xhr_receiver(ctx, hdr->this_val);
    if (!d) return JS_STEP_ABRUPT;
    if (d->response_type != RT_EMPTY && d->response_type != RT_DOCUMENT)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "responseXML read with a responseType that is not '' or 'document'"), -1;
    if (d->state != XHR_DONE) { *presult = JS_NULL; return 0; }
    DCHECK(!d->response_object_failure, "responseXML was read on an object whose response object is failure — "
                                        "§3.6.11 asserts that cannot happen, and only the arraybuffer arm sets "
                                        "it");
    if (JS_IsNull(d->response_object)) xhr_set_document_response(ctx, d);
    *presult = JS_DupValue(ctx, d->response_object);
    return 0;
}

static const IdlStepDecl XHR_RESPONSE_XML_DECL = {
    js_xhr_response_xml_step, sizeof(JSXhrResponseState), js_xhr_response_visit, NULL,
    "XHR §3.6.11 the responseXML getter", RESPONSE_XML_STEPS
};

/* ---- the fetch/response LIFECYCLE machine ------------------------------------------------------------------
 *
 * ONE machine for everything after `send()` has built the request: placing it with the host, the pause, §3.5.6's
 * processResponse, "handle response end-of-body", "handle errors" and "the request error steps". It is a STEP
 * CLOSURE over [xhr, mode, event name] so it can be reached two ways from one definition:
 *   - `send()` on a SYNCHRONOUS object CALLS it (step_call_run), so the send machine parks on the call and the
 *     flow suspends inside send() exactly as §3.5.6's "Pause until…" says;
 *   - `send()` on an ASYNCHRONOUS object ENQUEUES it as a task, so send() returns and the response is processed
 *     in its own turn of the event loop;
 *   - `abort()` CALLS it in error mode, because the request error steps §3.5.7 The abort() method step 2 runs
 *     are §3.5.6 The send() method's own — that is where the standard DEFINES them — so they are the identical
 *     sequence rather than a second one.
 * Writing the sequences twice — once for the send machine and once for a task — is how two copies of an event
 * order drift, and the order IS the spec. */
enum { XHR_MODE_FETCH = 0, XHR_MODE_ERROR };
enum { XHR_CD_OBJECT = 0, XHR_CD_MODE, XHR_CD_EVENT };

#define RUN_STAGES(X) \
    X(XR_FETCH,     "XHR §3.5.6 send() step 5's \"fetching req\" (place the request with the trusted zone, " \
                    "which SECURITY.md makes the only holder of network policy)") \
    X(XR_WAIT,      "XHR §3.5.6 send() step 5's \"Pause until either processedResponse is true or …\" (the " \
                    "flow is suspended on the host's answer; siblings run)") \
    X(XR_RESPONSE,  "XHR §3.5.6 send() step 5's processResponse steps 1-4 (set this's response, handle " \
                    "errors, set the state to headers received)") \
    X(XR_RSC_HEADERS, "XHR §3.5.6 send() step 5's processResponse steps 5-7 (fire an event named " \
                      "readystatechange at this, return if a listener left headers received, and return " \
                      "through handle response end-of-body when the response's body is null)") \
    X(XR_LOADING,   "XHR §3.5.6 send() step 5's processBodyChunk steps 1-3 (append the bytes to this's " \
                    "received bytes, return unless roughly 50ms have passed since these steps were last " \
                    "invoked, and set the state to loading)") \
    X(XR_RSC_LOADING, "XHR §3.5.6 send() step 5's processBodyChunk step 4 (fire an event named " \
                      "readystatechange at this)") \
    X(XR_PROGRESS,  "XHR §3.5.6 send() step 5's processBodyChunk step 5 (fire a progress event named " \
                    "progress at this)") \
    X(XR_UPLOAD_PROGRESS, "XHR §3.5.6 send() step 5's processRequestEndOfBody step 3 (fire a progress event " \
                          "named progress at this's upload object)") \
    X(XR_UPLOAD_LOAD, "XHR §3.5.6 send() step 5's processRequestEndOfBody step 4 (fire a progress event " \
                      "named load at this's upload object)") \
    X(XR_UPLOAD_LOADEND, "XHR §3.5.6 send() step 5's processRequestEndOfBody step 5 (fire a progress event " \
                         "named loadend at this's upload object)") \
    X(XR_EOB_BEGIN, "XHR §3.5.6 handle response end-of-body steps 1-5 (handle errors, return on a network " \
                    "error, and compute transmitted and length from the RESPONSE)") \
    X(XR_EOB_PROGRESS, "XHR §3.5.6 handle response end-of-body step 6 (fire a progress event named progress " \
                       "at xhr with transmitted and length — the step is conditioned on xhr's synchronous " \
                       "being false, which XR_EOB_BEGIN honours by routing a synchronous object past it)") \
    X(XR_EOB_RSC,   "XHR §3.5.6 handle response end-of-body steps 7-9 (state done, send() invoked false, " \
                    "fire an event named readystatechange at xhr)") \
    X(XR_EOB_LOAD,  "XHR §3.5.6 handle response end-of-body step 10 (fire a progress event named load at xhr)") \
    X(XR_EOB_LOADEND, "XHR §3.5.6 handle response end-of-body step 11 (fire a progress event named loadend " \
                      "at xhr)") \
    X(XR_ERR_BEGIN, "XHR §3.5.6 the request error steps steps 1-4 (state done, send() invoked false, response " \
                    "a network error, and — when synchronous — throw the exception)") \
    X(XR_ERR_RSC,   "XHR §3.5.6 the request error steps step 5 (fire an event named readystatechange at xhr)") \
    X(XR_ERR_UP_EV, "XHR §3.5.6 the request error steps step 6.2.1 (fire a progress event named event at " \
                    "xhr's upload object with 0 and 0)") \
    X(XR_ERR_UP_END, "XHR §3.5.6 the request error steps step 6.2.2 (fire a progress event named loadend at " \
                     "xhr's upload object with 0 and 0)") \
    X(XR_ERR_EV,    "XHR §3.5.6 the request error steps step 7 (fire a progress event named event at xhr with " \
                    "0 and 0)") \
    X(XR_ERR_END,   "XHR §3.5.6 the request error steps step 8 (fire a progress event named loadend at xhr " \
                    "with 0 and 0)")
enum { RUN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_xhr_run_steps[] = { RUN_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint32_t  req;      /* the outstanding host request, or 0 */
    uint8_t   phase;    /* the dispatch request's own phase */
    JSValue   ev;       /* the event in flight (owned) */
    EventFireCb   cb;    /* the dispatch's request buffer */
    double    transmitted, length;
    /* WHICH request error this is, DECIDED ONCE at XR_ERR_BEGIN and held. It cannot be re-derived at each
       later stage: "handle errors" reads `send() invoked`, which step 2 of the request error steps has already
       cleared, so a re-derivation after the first dispatch answers XHR_ERR_NONE — and a machine that then
       defaulted would fire `abort` for a network error. The closure's own event is the ABORT entry's; this is
       the one the sequence is actually running. */
    uint8_t   which;
} JSXhrRunState;

static void js_xhr_run_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSXhrRunState *s = st;
    int i;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* Fire one event at `target`, minting it first. Returns >0 (the caller returns it), 0 when it has been
   dispatched, or -1. `progress` decides which interface §3.7's table names for that event. */
static int xhr_fire_run(JSContext *ctx, JSXhrRunState *s, JSValueConst target, const char *type,
                        bool progress, JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (JS_IsUndefined(s->ev)) {
        s->ev = progress ? progress_event_new(ctx, type, s->transmitted, s->length)
                         : event_new(ctx, type, /*bubbles*/ false, /*cancelable*/ false);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return -1; }
    }
    r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), target, s->ev, JS_UNDEFINED, in, NULL, out_cb, out_argc);
    if (r > 0) return r;
    /* §2.9 leaves `target` and the dispatch flag on the event, so the next fire gets its own. */
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    return r;
}

/* Fetch §3.4 `Content-Length` header's "extract a length from a header list", which XHR §3.5.6 The send()
   method runs over "this's response's header list": `Content-Length`, or 0 when it is not an integer. */
static double xhr_response_length(JSContext *ctx, XhrData *d)
{
    char *v = hl_get(ctx, d->response_headers, "content-length");
    double n = 0;
    char *end = NULL;

    if (!v) return 0;
    n = strtod(v, &end);
    if (end == v || (end && *end) || !(n >= 0)) n = 0;
    free(v);
    return n;
}

/* Fetch §4.1 "Main fetch"'s METHOD half of the same nulling — "either request's method is `HEAD` or
   `CONNECT`" — asked of the method this flow has actually pinned. `CONNECT` is not tested because XHR cannot
   carry one: §3.5.1 "The open() method" throws a SecurityError for a forbidden method, which Fetch §2.2.1
   "Methods" defines as "a byte-case-insensitive match for `CONNECT`, `TRACE`, or `TRACK`", and the comparison
   is against the UPPERCASE spelling because §3.5.1 has already run Fetch's "normalize a method" over it.
   AN UNPINNED METHOD CANNOT REACH THIS QUESTION, AND THIS LINE USED TO ANSWER IT FALSE AND CALL THAT A
   RESIDUAL. The silent arm is deleted and the route that makes it unreachable is asserted instead, which is
   the same shape `xhr_request_op` one function down already uses over this same field and for a neighbouring
   reason. The chain is entirely inside this component: §3 gives the response "a network error" initially and
   `d->network_error` carries it; `xhr_main_fetch_local` answers an UNKNOWN method INSIDE THIS AGENT and
   returns before it takes any reply, so nothing clears that flag; and processResponse step 3 — "If this's
   response is a network error, then return" — sends the lifecycle machine to the request error steps at
   `XR_RESPONSE`. Steps 4-13 never run, so step 7 is never asked. A concolic method arriving here is therefore
   a ROUTE this engine controls having been broken, never anything a page said, which is what makes it a
   DCHECK rather than a refusal.
   §4.1's METHOD HALF IS THEREFORE WHOLE AT THIS SITE RATHER THAN NARROWED. `HEAD` is compared against the
   spelling §3.5.1 normalized to, `CONNECT` cannot be opened at all, and the third case does not arrive. */
static int xhr_method_nulls_body(JSContext *ctx, const XhrData *d)
{
    const char *m;
    int null_body;

    DCHECK(!concolic_is(d->method),
           "an XMLHttpRequest reached Fetch §4.1 \"Main fetch\"'s null-body test with a method that is "
           "UNKNOWN EXTERNAL INPUT — such a request is answered inside this agent by xhr_main_fetch_local, "
           "which returns before any reply is taken, so §3's initial network error still stands and "
           "processResponse step 3 has already left for the request error steps. An arrival here is a route "
           "that skipped that refusal, and the coercion below would run the page's toString from a C "
           "activation with no flow base under it");
    DCHECK(JS_IsString(d->method),
           "an XMLHttpRequest was asked whether its method nulls the response's body before §3.5.1 \"The "
           "open() method\" had put a normalized method on the object — every other reader of this field "
           "asserts the same pair, and a value that is neither a string nor concolic is this file's own "
           "bookkeeping broken rather than anything a page said");
    m = JS_ToCString(ctx, d->method);
    CHECK(m != NULL, "XMLHttpRequest: OOM reading the request method back for Fetch §4.1 \"Main fetch\"'s "
                     "null-body test — a method this engine cannot read is one that test has no answer for");
    null_body = !strcmp(m, "HEAD");
    JS_FreeCString(ctx, m);
    return null_body;
}

/* §3.5.6 "handle errors": which request error the response calls for, or 0 for none. */
enum { XHR_ERR_NONE = 0, XHR_ERR_TIMEOUT, XHR_ERR_ABORT, XHR_ERR_NETWORK };
static int xhr_handle_errors(XhrData *d)
{
    if (!d->send_invoked) return XHR_ERR_NONE;
    if (d->timed_out) return XHR_ERR_TIMEOUT;
    if (d->aborted) return XHR_ERR_ABORT;
    if (d->network_error) return XHR_ERR_NETWORK;
    return XHR_ERR_NONE;
}

static const char *const XHR_ERR_EVENT[] = { NULL, "timeout", "abort", "error" };
static const char *const XHR_ERR_EXC[] = { NULL, "TimeoutError", "AbortError", "NetworkError" };

/* §3.5.6 STEP 6'S GRADE, READ BACK — see the `request_prov` field for why it is stored rather than re-asked.
   The read is asserted and never defaulted for that field's own reason: the value a miss would supply is 0,
   which is the strongest of the three. */
static int xhr_request_prov(const XhrData *d)
{
    DCHECKF(d->request_prov >= 0,
            "an XMLHttpRequest was asked what its request is evidence of before §3.5.6 \"The send() method\" "
            "step 6 composed one — every reader of this field runs at or after that step, so a -1 here is a "
            "route that reached the network, the @H surface or a reply without passing it. request_prov=%d",
            (int)d->request_prov);
    return d->request_prov;
}

/* Take the host's reply onto the record. A null reply — or none at all — leaves the response a network error,
   which is what §5.5's network error is on the Fetch side too. */
static void xhr_take_reply(JSContext *ctx, XhrData *d, JSValueConst reply)
{
    JSValue st_v, hs_v, bd_v;
    uint32_t hn = 0, i;
    int32_t status = 0;

    if (!JS_IsObject(reply)) return;
    /* §2.2.5's body, as the byte sequence every producer of this record now writes (core/fetch/fetch.h). A
       `null` or absent one is the network error §3.5.6 leaves the response as. */
    bd_v = JS_GetPropertyStr(ctx, reply, "body");
    if (JS_IsNull(bd_v) || JS_IsUndefined(bd_v)) { JS_FreeValue(ctx, bd_v); return; }
    DCHECK(JS_IsArrayBuffer(bd_v),
           "an XMLHttpRequest's reply carried a body that is not a byte sequence — the host answers §3.5.6's "
           "fetch with the reply record fetch_reply_new builds, whose body is an ArrayBuffer, and a STRING "
           "here is a zone that ran a decode §3.6.6 owns");
    st_v = JS_GetPropertyStr(ctx, reply, "status");
    hs_v = JS_GetPropertyStr(ctx, reply, "headers");
    JS_ToInt32(ctx, &status, st_v);
    d->status = status;
    /* Fetch §2.2.6 "Responses"' STATUS MESSAGE, THROUGH THE ONE READER OF THAT FIELD (core/fetch/fetch.h) —
       the second of the two hand-written reads of it, and the one whose default was written out as a test:
       `JS_IsString(stx_v) ? dup : JS_NewString(ctx, "")`. Asking `JS_IsString` and then answering `""` is not
       a check, it is the defaulted-field shape with the branch visible: §2.2.6 says a response's status message
       is "the empty byte sequence" unless stated otherwise and that HTTP/2 replies "will always have the empty
       byte sequence", so the arm taken when the producer has stopped writing the field is the same two bytes
       the commonest correct producer writes. §3.6.3 The statusText getter is what a page reads off this, and
       for a request the trusted zone REFUSED it is the refusal's own reason — the only account of it anybody
       gets. The reader asserts the field instead, and this record cannot be the network error it answers NULL
       for: `xhr_take_reply` returned above unless the reply is an object with a body. */
    {
        char *stx = fetch_reply_status_text(ctx, reply);
        /* A `CHECK` AND NOT A `DCHECK`, BECAUSE THIS POINTER IS DEREFERENCED IN EVERY BUILD. The reader
           answers NULL for the JSON `null` alone and this function returned above for anything that is not an
           object carrying a body, so a NULL here is the two disagreeing about what a network error is — and
           `JS_NewString` would read through it in the build where a dev-only guard is compiled out, which
           trades a named abort for a segfault. There is no `?: ""` under it: that is the very default this
           diff removes, and it would answer §3.6.2 The statusText getter with a phrase no producer wrote. */
        CHECK(stx != NULL,
              "XMLHttpRequest read a NULL status message off a reply that is not a network error — "
              "core/fetch/fetch.h's fetch_reply_status_text answers NULL for the JSON `null` alone, and "
              "xhr_take_reply returns above for anything that is not an object carrying a body");
        xhr_set(ctx, d, &d->status_text, JS_NewString(ctx, stx));
        free(stx);
    }
    xhr_set(ctx, d, &d->response_headers, JS_NewArray(ctx));
    {
        JSValue len_v = JS_GetPropertyStr(ctx, hs_v, "length");
        JS_ToUint32(ctx, &hn, len_v);
        JS_FreeValue(ctx, len_v);
    }
    for (i = 0; i < hn; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, hs_v, i);
        JS_SetPropertyUint32(ctx, d->response_headers, i, pair);
    }
    xhr_set(ctx, d, &d->received, JS_DupValue(ctx, bd_v));
    /* No redirect is modelled here, so the response's URL is the request's — see the file comment.
       IT IS SERIALIZED WITHOUT ITS FRAGMENT, HERE AND NOT AT THE GETTER. §3.6.1 The responseURL getter says
       "otherwise its serialization with the exclude fragment flag set", and §3.5.1 The open() method keeps the
       fragment on the REQUEST's URL, so duplicating `url` reported `…/a#frag` where a browser reports `…/a`.
       Doing it at the write rather than at the read is what makes the getter and the reply's SOURCE IDENTITY
       (core/fetch/reply_source.h) one answer: a fragment is never sent, so two addresses differing only there
       are one reply, and naming it under both would split every predicate over its bytes in two.
       The exclusion is the URL PARSER'S — the record is run back over the serialization and asked to serialize
       it again without its fragment, rather than this file deciding where a fragment starts. */
    {
        size_t ulen = 0;
        const char *u = JS_ToCStringLen(ctx, &ulen, d->url);
        UrlRecord rec;
        char *ser;
        bool ok;

        CHECK(u != NULL, "XMLHttpRequest: OOM reading the request URL a reply answered");
        url_record_init(&rec);
        /* NO BASE: XHR §3.5.1 The open() method step 5 — "Let parsedURL be the result of encoding-parsing a
           URL url, relative to this's relevant settings object" — already resolved this, so what `url` holds
           is absolute. */
        ok = url_parse(&rec, u, ulen, NULL);
        JS_FreeCString(ctx, u);
        DCHECK(ok, "an XMLHttpRequest's own URL is a string the URL parser refuses — XHR §3.5.1 The open() "
                   "method step 11.3 stores the RECORD its step 5 parse produced and this component keeps the "
                   "SERIALIZATION of it, so a refusal here is that serialization and that parser disagreeing");
        ser = ok ? url_serialize(&rec, /*exclude_fragment*/ true) : NULL;
        url_record_free(&rec);
        xhr_set(ctx, d, &d->response_url, JS_NewString(ctx, ser ? ser : ""));
        free(ser);
    }
    d->network_error = 0;
    /* AND WHAT THE BODY TEACHES, WHICH NOTHING READ. CLAUDE.md §Learning-from-replies: "a consumed reply is
       ALWAYS fetched to fill examples", and "the JS/JSON a server returns is the richest source of real
       example values" — and solver/reply_decode.h carried, correctly, a residual saying that an
       XMLHttpRequest's reply body is read by nobody. The reason was the TRANSPORT and not a policy: every
       other request in this engine parks on a (method, url) pair and is answered at solver/engine.c's
       `engine_provide`, which is where that file is called from; §3.5.6's send() is the one SYNCHRONOUS
       rendezvous, keyed by a request id, and solver/pending.c excludes that kind from the address index BY
       CONSTRUCTION — so there was no pair at `engine_host_answer` to learn under and the address stayed on
       the @H surface with no example values from any of its bodies.
       THIS IS THAT ONE SITE. It holds the reply record AND the pair it answers, and it is reached EXACTLY
       once per reply: the two callers are the two arms of Fetch §4.1 main fetch — a request this agent
       answered itself, and one the trusted host answered — and `xhr_main_fetch_local` returning true is what
       clears `s->req`, so a send takes one of them and never both.
       THE ADDRESS IS `xhr_request_address` AND NOT `response_url`, WHICH IS AN IDENTITY QUESTION RATHER THAN
       A SPEC ONE. `reply_decode_learn` uses it twice: as the BASE a body's relative addresses resolve
       against, and as half the key its asset verdict is filed under (`endpoint_mark_asset`). The second
       decides it — a verdict naming a different string retracts nothing while reading as a retraction — and
       the accessor is what makes this the string `xhr_record_endpoint` filed the @H sighting under rather
       than one that merely agreed with it wherever the page wrote a literal address.
       THIS NAMED `url` AND SAID THAT STRING WAS BOTH THE ONE `xhr_request_op` HANDS THE TRUSTED ZONE AND THE
       ONE THE SIGHTING IS FILED UNDER. The first conjunct is still true and the second was FALSE for exactly
       the addresses this tool exists to find — a request built out of unknown external input — and a sentence
       asserting a guarantee is BUILT ON rather than checked, so the arm the guarantee excluded was never
       written. It is rewritten rather than deleted because the identity is what a reader re-derives.
       AND THE BASE IS UNCHANGED FOR A LITERAL ADDRESS AND IS NOW THE SHAPE FOR AN UNKNOWN ONE, which the URL
       parser refuses where that shape is relative: no base at all is the honest answer for an address no run
       computed, and resolving a body's chunk addresses against a serialization whose holes are spelled
       `%7B…%7D` was resolving them against a directory the page never composed.
       §3.6.1 The responseURL getter's fragment exclusion is a fact about what a PAGE reads back, not about
       which record this reply answers.
       NOT A SECOND LEARNING DOOR: it is the one entry, called from the one place this transport can reach it,
       and it holds no state — everything it learns goes to solver/endpoint.c, which is global and takes no COW
       capture for the reason that header gives (what a server said is not a fact about a flow's world). */
    {
        const char *lm = JS_ToCString(ctx, d->method);
        const char *lu = xhr_arg_cstring(ctx, xhr_request_address(d), NULL);

        /* BOTH HALVES OR NEITHER, AND NEITHER IS DEFAULTABLE. An UNKNOWN method never reaches here — both
           arms answer one before a reply exists (`xhr_main_fetch_local` returns for a concolic method, and
           `xhr_request_op` refuses one by name) — so a failure is OOM or this component holding something
           §3.5.1 The open() method never wrote. */
        CHECK(lm != NULL && lu != NULL,
              "XMLHttpRequest: OOM reading back the (method, url) pair a reply answered — the reply register "
              "is keyed on that pair, so a half-named one can only file what it learns under an endpoint "
              "nobody requested");
        reply_decode_learn(ctx, lm, lu, reply, xhr_request_prov(d));
        JS_FreeCString(ctx, lm);
        JS_FreeCString(ctx, lu);
    }
    /* …AND A REPLY WHOSE BYTES ARE JAVASCRIPT IS A PROGRAM HERE TOO. THAT IS A DECISION, AND THE AXIS IT WAS
       DOUBTED ON DOES NOT SEPARATE THIS DOOR FROM THE ONE THAT ALREADY COMPILES.

       solver/reply_decode.h carried a residual asking whether a JavaScript-typed reply arriving by XHR is a
       program this engine runs, and it put the question on Fetch §2.2.5 "Requests"' DESTINATION: §3.5.6 "The
       send() method" step 6 states ELEVEN members of its request and a destination is not one of them, so an
       XMLHttpRequest keeps the default — "A request has an associated destination, which is destination type.
       Unless stated otherwise it is the empty string" — which is outside that section's own `script-like` set
       ("audioworklet", "paintworklet", "script", "serviceworker", "sharedworker", or "worker"), while a
       `<script src>` is "script" and is inside it. Every clause of that is true and it settles nothing,
       because the door this engine already compiles through is not the `<script src>` one. IT IS `fetch()`,
       AND `fetch()`'S DESTINATION IS THE EMPTY STRING TOO: §5.4 "Request class" mentions a destination only as
       an IDL attribute and its getter ("The destination getter are to return this's request's destination")
       and never sets one, §5.6 "Fetch methods" does not contain the word, and §2.2.5's own destination table
       puts `fetch()` and `XMLHttpRequest` in ONE ROW — destination "", CSP directive `connect-src`, against
       "script"/`script-src` for HTML's `<script>`. So the destination axis separates BOTH of these doors
       jointly from `<script src>`, and this engine decided that case when it built the arm in
       solver/engine.c's FLOW_PENDING_RESOLVE delivery. A difference the two doors do not have cannot be the
       reason one of them refuses.

       WHAT THIS ENGINE ACTUALLY DISCRIMINATES ON IS WRITTEN AT `engine_pending_resource_url` (solver/engine.h)
       AND IT IS NOT THE DESTINATION: HTML §4.6.8.20 Link type "preload", HTML §4.6.8.12 Link type
       "modulepreload" and HTML §4.8.4.3.5 "Updating the image data" park a kind of their own PRECISELY so a
       JavaScript-typed reply is NOT compiled, and the reason that entry gives is that "None of these standards
       evaluates anything here" — modulepreload's own example calling the module "already ready (but not
       evaluated) in the module map". What those three share is not a destination; it is that the PAGE'S OWN
       CODE never receives the bytes. The RESOLVE arm's stated reason is the same fact read positively — "if
       the page did not itself hand those bytes to a `<script>` element nothing ever compiled them" — which
       presupposes the page HAS them and COULD have. §3.6.6 "Response body" hands them over: §3.6.10 "The
       responseText getter" is the decoded text, and `xhr.responseText` into an `eval` is how a bundle loads a
       lazy chunk without a `<script>`. So an XMLHttpRequest is on the same side of this engine's own line as
       `fetch()`, and CLAUDE.md §Learning-from-replies' "a fetch whose body is JAVASCRIPT is ALWAYS fetched +
       EXECUTED" is keyed on the BODY and the RESOURCE rather than on which API asked — solver/reply_decode.c
       states the general form at its own decision: what a program is owed is to be RUN.

       WHY THIS DOOR AND NOT solver/reply_decode.c, WHICH IS WHERE THE NEXT READER WILL REACH FIRST. That file
       runs on the HOST's time, and the queue below reaches a row that requires a flow switched in; a yielded
       flow keeps its stamp up deliberately, so asking there would write the program into an arbitrary
       member's row table. This site is inside §3.5.6's own step machine — the flow that SENT, on a later turn
       than step 6 — so the program lands on the timeline that asked for it, which is what §State-isolation
       requires. It is also BEFORE §3.6's `readystatechange` and `load`, the position the RESOLVE arm takes
       ahead of the page's own reaction: a chunk that arrives after the reaction waiting for it is a chunk
       whose endpoints that reaction has already not seen. And it is not a second compile DOOR: there is one
       entry, `engine_queue_fetched_script`, and this calls it.

       THE DECODE IS §3.6.6'S AND NOT HTML §8.1.4.2 "Fetching scripts"', WHICH IS A CHOICE AND NOT A
       CONVENIENCE. The two differ — §3.6.6 runs the FINAL encoding, which §3.6.7 "The overrideMimeType()
       method" can set and which defaults to UTF-8 — and the bytes that matter here are the ones the page
       would have compiled had it gone on to eval them, which are §3.6.6's by construction. Reaching for the
       script decode would be answering a `<script src>`'s question on a transport that has none.

       THE ADDRESS IS `response_url`, WHICH DISAGREES WITH THE CALL ABOVE FOR A REASON RATHER THAN BY
       OVERSIGHT. That one is filed under the IDENTITY this request was owed under — whichever accessor states
       it, which is that paragraph's to name and not this one's — because a verdict under a different string
       retracts nothing while reading as a retraction. This asks a different question: HTML §8.1.4.2 creates
       the script with the RESPONSE'S URL and §8.1.4.1 "Scripts" keeps it as the program's base URL, so a
       chunk that redirected resolves its own relative addresses against where it came FROM. The two answers
       are allowed to differ because they are two questions, and a program's base is never an identity.
       THE GUARD IS THE RESOLVE ARM'S AND HAS ITS REASON: `engine_queue_fetched_script` refuses an
       address-less program, and a reply whose URL did not serialize is a record that cannot say where its
       bytes came from. It is guarded rather than asserted for that arm's reason too — the refusal is about a
       record, not about this component's own logic.

       WHAT IS NOT SETTLED, AND IT IS NOT SETTLED AT EITHER DOOR. A page under a CSP with no `'unsafe-eval'`
       and a `script-src` that does not admit this reply's origin cannot execute these bytes by ANY route —
       not `eval`, not an injected element — so compiling them explores a world that page cannot reach, which
       is the one thing §Solver-half's "a BRANCH the page's own code could take, not an act no client
       performs" rules out. That is true of `fetch()` identically and of this door no more, and nothing in
       this tree reads a policy before either compile. It is named here rather than answered because a
       refusal on it would have to refuse BOTH doors, which is a decision about the compile entry and not
       about this transport. */
    {
        char *ct;
        MimeType cm;

        /* THIS DOOR WAS ASKED, RECORDED BEFORE ANY TYPE IS READ. It is the denominator the queue below is the
           numerator of, and until both existed `no program was ever queued from an XMLHttpRequest reply` and
           `this door was never reached` were one silence — which take opposite work, the first being a
           question about what the replies were and the second about whether a page sends any. It is raised
           HERE and not after the gate for CLAUDE.md §AN-INVARIANT-OVER-A-GATED-OPERATION's reason: the gate
           below DECLINES CORRECTLY for every reply whose computed type is not JavaScript, which is most of
           them, so a census of what landed reports each correct refusal as this door failing.
           THE POPULATION IS `a reply RECORD this door examined for a program`, which is what the two early
           returns above already leave: this function returns for anything that is not an object and for a
           body that is absent or null, so a network error is not in it — and solver/engine.c's sibling door
           guards its own raise on the reply being a record for exactly that reason, so the two rows are one
           population rather than a ratio of two things. See solver/engine.h's `net_prog_*` block. */
        engine_note_net_prog_xhr_ask();
        ct = fetch_reply_computed_type(ctx, reply);

        /* THE TYPE IS THE HOST'S DECISION AND IS NEVER RE-SNIFFED HERE, exactly as the sibling door and
           solver/reply_decode.c both say at their own extracts: SECURITY.md puts sniffing in the trusted
           zone, `computedType` is what that zone stamped, and a record whose type does not parse is not a
           program — there is no sniff this process may run to find out otherwise. */
        if (mime_type_extract(&cm, ct) && mime_type_is_javascript(&cm)) {
            JSValue txt = xhr_decode_text(ctx, d);
            size_t src_n = 0;
            const char *src = JS_ToCStringLen(ctx, &src_n, txt);
            const char *base = JS_ToCString(ctx, d->response_url);

            /* A `CHECK` FOR THE SAME REASON THE STATUS MESSAGE ABOVE IS ONE: both are dereferenced in every
               build, and `src_n` is read rather than `strlen` because a decoded body may hold a U+0000 and a
               browser runs the whole of it (the length the queue entry takes exists for that). */
            CHECK(src != NULL && base != NULL,
                  "XMLHttpRequest: OOM reading a JavaScript reply's source text or its address — §8.1.4.2 "
                  "\"Fetching scripts\" needs both to make a program out of a response");
            if (*base) {
                engine_queue_fetched_script(document_doc(ctx), src, src_n, base);
                /* …AND THIS DOOR QUEUED ONE. Beside the call rather than inside the entry, because that entry
                   holds the bytes and the address and cannot see which transport carried them. */
                engine_note_net_prog_xhr_queued();
            }
            JS_FreeCString(ctx, base);
            JS_FreeCString(ctx, src);
            JS_FreeValue(ctx, txt);
        }
        mime_type_free(&cm);
        free(ct);
    }
    JS_FreeValue(ctx, st_v); JS_FreeValue(ctx, hs_v); JS_FreeValue(ctx, bd_v);
}

/* XHR §3.5.6 "The send() method" STEP 6'S CREDENTIALS MODE, IN ONE PLACE. Step 6 is "Let req be a new
   request, initialized as follows", and one of its eleven members is "credentials mode: If this's cross-origin
   credentials is true, then `include`; otherwise `same-origin`." — the field §3.5.4 "The withCredentials getter
   and setter" writes and nothing else reads.
   IT IS A FUNCTION AND NOT A TERNARY AT EACH SITE because §3.5.6's request is composed TWICE in this file — as
   the `FetchRequest` Fetch §4.3 Scheme fetch is asked about, and as the JSON record the trusted host is handed
   — and those are the SAME request. Two spellings of one member is the shape that has nothing to make them
   agree; the wire words are core/fetch/fetch.h's `fetch_credentials_token` and appear nowhere in this file. */
static FetchCredentialsMode xhr_credentials_mode(const XhrData *d)
{
    return d->cross_origin_credentials ? FETCH_CREDENTIALS_INCLUDE : FETCH_CREDENTIALS_SAME_ORIGIN;
}

/* The request the host is owed, as one self-describing JSON record: `{method, url, credentials, provenance,
   headers, body}`. It is JSON because the ANSWER already crosses that way (main.c's qjs_host_answer parses one)
   and because a host that must route this to safeFetch needs every field — SECURITY.md's chokepoint cannot
   decide about a method it was never told, and a tab-separated line cannot carry a body. It is written with
   core/json_buf.h rather than with a JS value: every string here is one this engine built, so there is no
   `toJSON` and no Proxy trap to run, which is exactly the distinction quickjs.h makes when it deletes
   JS_JSONStringify. Caller frees with free().
   `provenance` IS WHAT THE REQUEST IS EVIDENCE OF — CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE, and the one
   field the FIRING DECISION is made from. Every OTHER request this engine builds already states it: a park
   carries it on the pending line (solver/engine.h's PENDING_PROVENANCE_*), a navigation and a route
   declaration take it from `engine_provenance_of_running_path`. This record did not, so the one seam whose
   requests are made ENTIRELY by running the page's code — which is precisely where a forced arm's values end
   up — reached `safe-fetch.js` with nothing to decide from, and the chokepoint would have to either default
   it (the permissive arm, for the population most likely to be forced) or refuse every XHR.
   IT IS `engine_provenance_of_running_path` AND NOT A LOCAL TERNARY over `flow_path_forced`, for the reason
   that function's own header gives: one composition, in one place, for every request built by running code.
   It answers `derived` or `forced` and never `observed`, which is a fact about this act rather than a
   narrowing — `observed`'s first conjunct is HTML §4.12.1.1 "Processing model"'s `parser document`, and an
   XMLHttpRequest has no parser behind it by construction.
   IT IS NOW READ OFF THE RECORD RATHER THAN ASKED HERE, AND THAT IS THE SAME COMPOSITION AND NOT A SECOND
   ONE: the ask moved to §3.5.6 step 6, where the request is created, and this line spells what that step
   answered through the mapping that already owns the vocabulary. What it buys is the field's whole reason —
   the REPLY is graded by what the request was FIRED at, so the grade the chokepoint made its firing decision
   from and the grade the engine learns the body under cannot be two different readings of one path. */
static char *xhr_request_op(JSContext *ctx, XhrData *d)
{
    JsonBuf b = { 0 };
    uint32_t n = hl_len(ctx, d->author_headers), i;
    const char *m, *u;

    /* THE ONE KIND OF REQUEST THIS OP CANNOT STATE. The trusted zone hands `method` to a real `fetch()`, so a
       value the run never computed may not reach it — and one never does, because xhr_main_fetch_local
       answers an unknown method itself and this op is composed only for what it hands on. Defaulting the
       field instead is what the assert exists to stop: a fabricated `GET` is indistinguishable at the
       chokepoint from a `GET` the bundle really built. */
    DCHECK(!concolic_is(d->method),
           "an XMLHttpRequest whose method is UNKNOWN EXTERNAL INPUT reached the trusted zone's request op — "
           "Fetch §4.1 main fetch answers that request inside this agent precisely because there are no bytes "
           "to put in this field, so an arrival here is a route that skipped it");
    m = JS_ToCString(ctx, d->method);
    u = JS_ToCString(ctx, d->url);
    CHECK(m != NULL && u != NULL,
          "XMLHttpRequest: OOM composing §3.5.6's request op — a method or URL this engine cannot read back "
          "is a request the trusted zone would perform against an address nobody derived");

    json_buf_raw(&b, "xhr.send\t{"); json_buf_key(&b, "method");
    json_buf_str(&b, m);
    json_buf_raw(&b, ","); json_buf_key(&b, "url");
    json_buf_str(&b, u);
    json_buf_raw(&b, ","); json_buf_key(&b, "credentials");
    /* §3.5.6 step 6's member, through the one spelling — see xhr_credentials_mode above and
       core/fetch/fetch.h's `fetch_credentials_token`, which is where the three words Fetch §2.2.5 "Requests"
       defines are written. This line used to hold two of them inline, which made this seam the only place in
       the engine that could say a credentials mode at all and said it in a vocabulary of its own. */
    json_buf_str(&b, fetch_credentials_token(xhr_credentials_mode(d)));
    json_buf_raw(&b, ","); json_buf_key(&b, "provenance");
    json_buf_str(&b, engine_provenance_token(xhr_request_prov(d)));
    json_buf_raw(&b, ","); json_buf_key(&b, "headers"); json_buf_raw(&b, "[");
    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, d->author_headers, i);
        JSValue nv = JS_GetPropertyUint32(ctx, pair, 0), vv = JS_GetPropertyUint32(ctx, pair, 1);
        const char *nm = JS_ToCString(ctx, nv), *val = JS_ToCString(ctx, vv);
        if (i) json_buf_raw(&b, ",");
        json_buf_raw(&b, "[");
        json_buf_str(&b, nm ? nm : "");
        json_buf_raw(&b, ",");
        json_buf_str(&b, val ? val : "");
        json_buf_raw(&b, "]");
        if (nm) JS_FreeCString(ctx, nm);
        if (val) JS_FreeCString(ctx, val);
        JS_FreeValue(ctx, nv); JS_FreeValue(ctx, vv); JS_FreeValue(ctx, pair);
    }
    json_buf_raw(&b, "],"); json_buf_key(&b, "body");
    if (JS_IsNull(d->request_body)) {
        json_buf_raw(&b, "null");
    } else {
        const char *body = JS_ToCString(ctx, d->request_body);
        json_buf_str(&b, body ? body : "");
        if (body) JS_FreeCString(ctx, body);
    }
    json_buf_raw(&b, "}");
    JS_FreeCString(ctx, m);
    JS_FreeCString(ctx, u);
    return json_buf_take(&b);
}

/* THE ENDPOINT THIS OBJECT IS ABOUT TO REQUEST, onto the @H surface — the tool's headline output, and this
   component was not on it. Every request host-edge funnels one endpoint into `endpoint_record`
   (solver/endpoint.h) and there were five such edges across six call sites — `fetch()`, a form submission
   (one site for its GET and one for its POST), a taint-carrying `<script src>`, a reply's decoded URLs and a
   multipart batch's sub-requests. Not this one. So a page whose
   client is axios — whose browser adapter IS XMLHttpRequest, and which is what a large share of real bundles
   ship — learned NOTHING, and the zero it produced was indistinguishable from a page with no API at all. A
   fixture calling `api.get('/users', {params:{page:2}})` emitted 0 endpoints against 8889 flows; the same
   request written as `fetch()` emitted 1.
 *
 * §3.5.6 step 6 IS THE PLACE, because that is where the spec itself assembles `req` out of exactly these four
 * — "method: this's request method / URL: this's request URL / header list: this's author request headers /
 * body: this's request body" — so nothing here re-derives what the record already holds. (IT SAID STEP 5 AND
 * THAT WAS OFF BY ONE, in the direction §Browser-half warns of: §3.5.6 has TWELVE top-level steps counted with
 * list depth tracked, step 5 is "If one or more event listeners are registered on this's upload object, then
 * set this's upload listener to true" and step 6 is "Let req be a new request, initialized as follows". The
 * QUOTATION above was right about the section the whole time, which is why nothing reported it: the auditor
 * checks that a step number RESOLVES, never that it is the step the prose is describing.) Before §4.1 decides
 * WHO answers, for the same reason `fetch()` records before its own network edge: a `data:` URL this agent
 * resolves itself is a request the page made, and an endpoint is what the page's code composed, never what
 * came back.
 *
 * THE CONTENT-TYPE IS READ OFF THE AUTHOR HEADERS AND NOWHERE ELSE. `fetch()` has to consult §5.4 step 37.4's
 * extracted type because its own list may not name one; here step 4 has already SET `Content-Type` into the
 * author request headers (the `text/html;charset=UTF-8` / `application/xml;charset=UTF-8` /
 * extractedContentType branches), so by this line the list is the whole answer and a second source would be a
 * second way of being right. */
static void xhr_record_endpoint(JSContext *ctx, XhrData *d)
{
    uint32_t n = hl_len(ctx, d->author_headers), i;
    EndpointHeader *eh = NULL;
    const char **owned = NULL;   /* the 2n cstrings borrowed for the call, freed together after it */
    EndpointBody eb;
    const EndpointBody *ebp = NULL;
    const char *method;
    char *body_ct = NULL;
    const char *body = NULL;
    size_t body_len = 0;

    DCHECK(!JS_IsNull(d->url), "an XMLHttpRequest reached §3.5.6's request record with no URL — send() runs "
                               "only on an `opened` object and XHR §3.5.1 The open() method step 12.1, Set "
                               "this's state to opened, is what opens one");
    /* THE METHOD AS THE PAGE COMPUTED IT — the shape where §3.5.1 step 4's normalization derived one over
       unknown external input, the bytes otherwise. It was `"GET"` for anything that was not a String, which is
       the defaulted-field defect in the one column an endpoint is read by: a method no run ever computed
       reported as the safest one there is, indistinguishable from a `GET` the bundle really builds. §@H's line
       is that an equality-pinned or run-computed value is concrete and everything else is a domain-annotated
       SHAPE, and concolic_name_cstr is the one speller of that projection. */
    DCHECK(JS_IsString(d->method) || concolic_is(d->method),
           "an XMLHttpRequest reached §3.5.6's request record with a method that is neither bytes nor unknown "
           "external input — XHR §3.5.1 The open() method step 11.2 sets it from its own step 4 and send() "
           "runs only on an `opened` object, so a third kind here is this component storing something §3.5.1 "
           "never produced");
    method = concolic_name_cstr(ctx, d->method);
    CHECK(method != NULL, "XMLHttpRequest: OOM projecting the request method onto the @H surface — a dropped "
                          "endpoint is a hole in the frontier");
    if (n) {
        eh = js_malloc(ctx, sizeof(*eh) * (size_t)n);
        owned = js_malloc(ctx, sizeof(*owned) * (size_t)n * 2);
        CHECK(eh != NULL && owned != NULL, "XMLHttpRequest: OOM projecting the author headers onto the @H "
                                           "surface — a dropped endpoint is a hole in the frontier");
        for (i = 0; i < n; i++) {
            JSValue pair = JS_GetPropertyUint32(ctx, d->author_headers, i);
            JSValue nv = JS_GetPropertyUint32(ctx, pair, 0), vv = JS_GetPropertyUint32(ctx, pair, 1);
            owned[i * 2] = JS_ToCString(ctx, nv);
            owned[i * 2 + 1] = JS_ToCString(ctx, vv);
            eh[i].name = owned[i * 2] ? owned[i * 2] : "";
            eh[i].value = owned[i * 2 + 1] ? owned[i * 2 + 1] : "";
            JS_FreeValue(ctx, nv); JS_FreeValue(ctx, vv); JS_FreeValue(ctx, pair);
        }
    }
    if (!JS_IsNull(d->request_body)) {
        /* THIS READ CANNOT MEET UNKNOWN EXTERNAL INPUT, AND THE REASON IS A LOSS RATHER THAN A GUARANTEE —
           recorded here because a reader who asks the obvious question about this line gets the answer
           backwards, and the two answers take OPPOSITE work.
           The obvious question is what happens if `request_body` holds a concolic: JS_ToCStringLen would reach
           js_force_tostring, which aborts by name over one. It never does. §3.5.6 step 4's extraction resolves
           the body to CHARACTERS at the store site below and puts a real String in this slot, so by the time
           this line runs the taint is already gone — a DE-TAINTING, not an abort, which is the silent
           direction and is exactly why it reads as the safe one. What lands here for an unknown body is that
           body's DISPLAY SHAPE, indistinguishable from a page that literally wrote those characters.
           SO THE ARM IS CARRIED BESIDE THE SLOT and is what the @H surface is told, because it cannot be
           recovered from these bytes: an abort would be a capability to build, and a de-tainting is a fact
           that must travel. The store site's own residual names what is still missing — the VALUE, not the
           arm — and nothing here may be read as having closed that. */
        body = JS_ToCStringLen(ctx, &body_len, d->request_body);
        body_ct = hl_get(ctx, d->author_headers, "content-type");
        if (body) {
            eb.mime = body_ct; eb.bytes = body; eb.len = body_len;
            /* §3.5.6 step 4's arm, carried from the extraction rather than re-derived from the characters —
               which cannot be done, since a display shape and a body that spells one are the same bytes. */
            eb.kind = d->request_body_is_shape ? EPB_SHAPE : EPB_SENT;
            /* AND NO SPAN RECORD, STATED RATHER THAN LEFT UNWRITTEN — `eb` is declared field by field, so a
               member nobody assigns is read out of whatever the stack held, and a count with no array is the
               half-record solver/endpoint.c's own assert refuses. This is a POSITIVE statement and not a
               default: this producer does not know which bytes of its payload are unknown input, and cannot.

               NAMED RESIDUAL — the same slot, and the same loss one layer deeper than the one above.
               WHAT IS NOT COVERED: `request_body` is a JS STRING, and core/fetch/body.c's span record names
               BYTE OFFSETS into the bytes §5.2's extraction produced. Those bytes are gone by this line for a
               reason sharper than the de-tainting above: JS_NewStringLen at the store site runs them through
               utf8_scan/utf8_decode (read at quickjs.c's js_new_string_len_or_null), so a BINARY payload — the
               gRPC-Web and protobuf bodies this whole capability exists for — is UTF-8-decoded on the way into
               the slot and re-encoded on the way out. A span offset into the result names a different byte
               than the one the page wrote, so carrying the record here without fixing the slot would be worse
               than not carrying it: a provenance row pointing at the wrong bytes.
               WHAT THE NEXT DIFF BUILDS: the slot carrying the extraction's BYTES and its BodyState rather
               than a String of them — which is the same repair the residual at §3.5.6 step 4's store site
               already asks for (it names the VALUE; this names the BYTES and the SPANS, and one change to the
               slot answers all three), with this line then projecting body_state_spans exactly as
               core/fetch/fetch.c and core/frame/navigator_beacon.c do.
               HOW ITS ABSENCE WOULD SHOW: one serializer's output posted through `fetch` and through
               `xhr.send` reported with a full set of body byte-range fields and with none — and the XHR
               record's `bodyBase64` differing from the fetch record's for byte-identical payloads. */
            eb.span = NULL;
            eb.nspan = 0;
            ebp = &eb;
        }
    }
    /* The CONCOLIC where open() was given one, so the surface reports the shape AND the example it carries;
       the serialization otherwise, which for a plain address is the same string XHR §3.5.1 The open() method
       step 5 parsed. */
    /* AND THE OFFER, ON THE LINE BEFORE THE DOOR. It counts OFFERS and never records — the surface's own gate
       may still suppress this one, and telling those two apart is what the ask/outcome split it feeds exists
       for. It is raised HERE, inside the LIFECYCLE machine, and not on any `send()` state: the asynchronous
       arm's send state is torn down before this task runs, so "did this construction offer an address" is a
       question no send state can answer about itself. solver/endpoint.h states why that makes it a row of its
       own rather than the partition the other three rows are over. */
    endpoint_xhr_edge_offered();
    /* …AND THE GRADE OF THE REQUEST THIS SIGHTING IS OF, off the record rather than asked again. It is the
       SAME value — this runs in §3.5.6 step 6's own turn, where the ask was made — and reading it here made
       the sighting, the trusted zone's record and the reply's learning three independent answers to one
       question about one exchange. See the `request_prov` field. */
    endpoint_record(ctx, method, xhr_request_address(d), eh, (int)n, ebp,
                    xhr_request_prov(d), EPD_XHR);
    if (body) JS_FreeCString(ctx, body);
    free(body_ct);
    if (owned) {
        for (i = 0; i < n * 2; i++) if (owned[i]) JS_FreeCString(ctx, owned[i]);
        js_free(ctx, owned);
    }
    js_free(ctx, eh);
    JS_FreeCString(ctx, method);
}

/* Fetch §4.1 MAIN FETCH, for the request §3.5.6 sends, as far as it is answered inside this agent. Returns true
   when the request is one this agent answers ITSELF — the response is already on the record when it does,
   either as a reply taken or as the network error §3 says the response starts as — so the trusted host is owed
   nothing and there is nothing to wait for.
 *
 * IT IS THE ROUTING §4.3 IS, not a test that selects a fallback: a `data:` URL has nothing for the trusted
 * host to request, and it used to be handed to it anyway. The wire then carried
 * `GET text/xml,<template …> HTTP/1.1` with an empty `Host:` and wptserve answered 400 — a malformed request
 * to a server that had never heard of the URL, for every `xhr.open("GET", "data:…")` in the corpus.
 *
 * STEP 7 IS HERE AND NOT ONLY IN core/fetch, because §3.5.6 hands `req` to the SAME algorithm `fetch()`
 * performs — its step 11 (this's synchronous is false) and its step 12 (this's synchronous is true) each
 * reach "Set this's fetch controller to the result of fetching req …", the two arms of one request — and a
 * component that owns a second door onto the network owns every step in front of it.
 * (THIS SAID §3.5.6 STEP 4 IS "Fetch req" AND BOTH HALVES WERE WRONG: step 4 is the body-and-Content-Type
 * step, and the words "Fetch req" occur nowhere in the XMLHttpRequest Standard — in this tree's committed
 * corpus or in the live edition. A two-word run in quotation marks is under the auditor's six-word floor and
 * a step number that RESOLVES is all its step channel asks, so both halves were invisible to every instrument
 * here; it was found by counting §3.5.6's twelve top-level steps with list depth tracked, which the committed
 * step index corroborates.)
 * Answering it anywhere else would leave `xhr.open("GET", "http://host:25/")` reaching the trusted
 * zone with the one request the standard says must never be made.
 *
 * AND §4.3 Scheme fetch IS NOT WRITTEN OUT HERE. This function used to carry its own copy of the switch — a `data` arm and
 * nothing else — while core/fetch carried a second copy with a `data` arm AND a `blob` arm, so `fetch(blobUrl)`
 * was answered inside this agent and the identical `xhr.open("GET", blobUrl)` was sent to a trusted zone that
 * can fetch nothing but an HTTP(S) scheme. Two copies of one switch is what that asymmetry IS. §4.3 is
 * core/fetch/scheme_fetch.c now, with its own parse, and this component runs it rather than restating it: an
 * arm added there is answered here at the same instant, which is the whole reason it is one component. */
static bool xhr_main_fetch_local(JSContext *ctx, XhrData *d)
{
    const char *u;
    const char *m;
    UrlRecord rec;
    FetchRequest req = {0};
    JSValue reply = JS_UNDEFINED;
    bool parsed, local;

    /* A METHOD THIS AGENT CANNOT SPELL, AND WHY REFUSING IT HERE IS NOT A NETWORK POLICY.
       An unknown method has no bytes, and both consumers below owe real ones: Fetch §4.3 Scheme fetch's `blob`
       arm READS the method, and the trusted zone hands it to an actual `fetch()`. Neither may be given a value
       no run computed — a defaulted `GET` there is the wrong report rather than a partial one, and the request
       that went out would be one no page ever made.
       WHAT IS DECIDED HERE IS NOT WHETHER TO SEND IT BUT WHETHER IT CAN BE COMPOSED AT ALL, which is a fact
       about this engine's own state and not a claim about the world. The policy stays at safe-fetch.js, which
       never sees this request because there is nothing to state to it. CLAUDE.md's
       §A-REQUEST-CARRIES-THE-PROVENANCE arrives at the same place from the other side: an XMLHttpRequest is
       credentialed by construction, a method not established to be in RFC 9110 §9.2.1 Safe Methods' safe set
       is not established to leave the server's state alone, and this one came off a forced arm. THAT LAST
       CLAUSE USED TO READ "the one combination that is never a setting", AND IT IS REWRITTEN RATHER THAN
       DELETED BECAUSE IT IS THE REASONING A READER RE-DERIVES (CLAUDE.md
       §AND-THAT-ABSOLUTE-IS-RETIRED-BY-THE-PROJECT-OWNER): stripping the cookie never made a request
       uncorrelated with the person, since the authority can be in the ADDRESS — a presigned URL, a reset or
       invite token, a signed webhook — and the address can have been DERIVED from a credentialed read; and
       §9.2.1 grades what a client INTENDS rather than what a server does. Those three are SIGNALS the egress
       policy surfaces per-origin for a person to decide, and their correct output where nobody has widened
       the origin is the DEFAULT rather than a refusal — derive it in full, report it, do not send it, a
       derived-and-unfired request being the report rather than a gap in it. §3.5.6 step 6's endpoint record
       one frame up is where the report happened.
       IT IS THE SAME SHAPE AS THE BLOCK BELOW AND NOT A SECOND MECHANISM: §3's response IS a network error
       already, so this is nothing written and everything not done — the lifecycle machine's "handle errors"
       fires the request error steps on the way out, which for §3.5.6 is an `error` event.
       NAMED RESIDUAL — NOT COVERED: the world in which the real method IS one §9.2.1 calls safe, where a
       browser would have made the request and the reply would have been learned.
       THE MECHANISM HALF OF THIS CLAUSE IS REFUSED, AND IT IS REWRITTEN RATHER THAN DELETED BECAUSE IT IS
       THE DESIGN A READER RE-DERIVES FROM THE GAP. It asked for "one more ask over the same operand this
       member already forks at OPEN_METHOD_OP — whether this method is in that safe set — so the flow
       standing on its true arm has a method it can COMPOSE AND STATE". It was re-derived before being built
       and it fails on TWO independent legs, either of which is enough.
       FIRST, A MEMBERSHIP FORK YIELDS A SET AND THE CALLER NEEDS BYTES. RFC 9110 §9.2.1 "Safe Methods" —
       "Of the request methods defined by this specification, the GET, HEAD, OPTIONS, and TRACE methods are
       defined to be safe" — and TRACE cannot be opened, so the true arm narrows an unknown to THREE
       spellings and pins none of them. Both consumers want one: §4.3's `blob` arm reads the method and the
       trusted zone hands it to a real `fetch()`. Picking one off that arm is the defect this member's own
       endpoint record already names, a method no run ever computed being reported as the safest one there
       is — and CLAUDE.md §@H's line is that only an equality-pinned or run-computed value is
       concrete while a value known only to satisfy a GATE stays a shape. A fork whose arms pin nothing
       cannot hand this line a method.
       SECOND, THE COMPLETIONS WOULD BE NAMED AFTER THE POLICY AND NOT AFTER A STEP. Every other fork over
       this operand is a spec branch the page can observe: OPEN_METHOD_OP's three are §3.5.1 steps 2-3 and
       show as two different DOMExceptions, and SEND_METHOD_OP's two are §3.5.6 step 3 and show in whether
       the body is sent. No algorithm asks the safe question here — the string "safe method" occurs ZERO
       times in the whole Fetch Standard — and §9.2.1 states its own purpose as something a USER AGENT
       APPLIES: "it allows a user agent to apply appropriate constraints on the automated use of unsafe
       methods". A fork cut on that predicate is CLAUDE.md §Learning-from-replies' safety `if` inside the
       engine, wearing a completion set. THE TEST THAT SEPARATES THE TWO IS NOT "DOES IT DECIDE FIRING" BUT
       "WHOSE STEP IS THIS BRANCH" — a fork named after a step some standard takes is the algorithm; one
       named after a partition only a policy consults is the policy, however carefully its arms avoid
       sending anything.
       AND THE PARTITION NEXT DOOR IS THE SPEC-REAL ONE, which is worth naming because it is one line away
       and is not this: Fetch §2.2.1 "Methods" — "A CORS-safelisted method is a method that is `GET`, `HEAD`,
       or `POST`" — IS consulted by an algorithm, and it is a different set (POST is CORS-safelisted and not
       safe; OPTIONS is safe and not CORS-safelisted). A fork over the method that this engine may legitimately
       want is that one, asked where §4.x consults it, and it does not make a method spellable either.
       WHAT THE NEXT DIFF BUILDS IS THEREFORE NOT A FORK AT ALL: this refusal is keyed on `concolic_is`, and
       the question it means to ask is whether the operand carries a SPELLABLE EXAMPLE — §3.5.1 step 4 runs
       "normalize a method" on the operand's own example and `concolic_new_derived` carries the result, so
       that population is run-computed rather than invented and is exactly what §@H calls concrete. Keying
       the refusal on the example rather than on the carrier moves a decision this member is currently making
       to the one place that owns it, and leaves this line stating what it already claims to state — a fact
       about whether there are bytes to put in the field. HOW ITS ABSENCE SHOWS: a bundle whose only
       request is built from an uncalled function's argument emits its endpoint and never a reply, so that @H
       record carries no server-learned example values while the sibling arm that took a literal method does.
       AND WHAT CLOSING IT BUYS IS NOT PARITY WITH THAT SIBLING, which the sentence above invites and which
       CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE forbids: a request composed on an arm nothing observed is
       FORCED by its PATH whatever its method spells, so its reply is learned and CARRIED AS FORCED and never
       merged into the observed pool. The gain is real and it is forced example values, not the literal
       sibling's. */
    if (concolic_is(d->method))
        return true;

    u = JS_ToCString(ctx, d->url);
    CHECK(u != NULL, "XMLHttpRequest: OOM reading the request URL back to switch on its scheme");
    url_record_init(&rec);
    /* XHR §3.5.1 The open() method step 5 parsed this URL and step 11.3 stored the record, which this
       component keeps SERIALIZED, so it is absolute and carries a scheme; a re-parse that refuses it is this
       component having stored something that is not a URL. */
    parsed = fetch_parse_url(ctx, &rec, u, strlen(u)) && rec.scheme;
    DCHECK(parsed, "XMLHttpRequest: the URL XHR §3.5.1 The open() method stored will not parse back — its "
                   "step 5 parses the URL and its step 11.3 sets the request URL to that record, whose "
                   "serialization is what this component holds, and every item of that form is absolute");
    /* §4.1 MAIN FETCH STEP 7: "If should request be blocked due to a bad port, should fetching request be
       blocked as mixed content, should request be blocked by Content Security Policy, or should request be
       blocked by Integrity Policy Policy returns blocked, then set response to a network error." §3's response
       IS a network error already, so blocking is nothing
       written and everything not done: the request is never placed, and the lifecycle machine's "handle
       errors" fires the request error steps on the way out — which for §3.5.6 is an `error` event, or a
       NetworkError thrown out of a synchronous send.
       BOTH CHECKS OR NEITHER. The step is one disjunction, and this component owns it for the same reason the
       header note above gives — §3.5.6 steps 11 and 12 hand `req` to the same algorithm `fetch()` performs
       ("Set this's fetch controller to the result of fetching req …", once per synchronicity arm) — so
       answering only the port half here would make `connect-src 'none'` block a `fetch()` and permit the
       identical request written as an XMLHttpRequest: one policy answering differently depending on which
       door the page used. §6.8.1 gives an XHR the EMPTY destination exactly as it gives `fetch()` one, so both
       are governed by `connect-src`, and a request that has not been redirected has a redirect count of 0. */
    /* THE DISJUNCTION IS NO LONGER WRITTEN HERE — it was one of FOUR hand-written copies of §4.1 step 7, and
       core/fetch/fetch.h's fetch_main_blocked is the one component they collapsed into. What this site still
       states is what only it knows: §6.8.1's EMPTY destination for an XHR, and the metadata §3.5.6 sets.
       AND THIS IS THE SECOND SITE, NOT A FIFTH COPY, WHICH IS A FACT ABOUT THE TRANSPORT AND NOT A CONCESSION.
       Every other request in this engine reaches solver/engine.c's pending_park_request, which runs step 7 for
       all of them; an XMLHttpRequest does not — §3.5.6's send() is the one SYNCHRONOUS rendezvous
       (engine_host_request, the FLOW_PENDING_HOSTREQ kind), which is keyed by a request id rather than by a
       (method, url) pair and never passes that door. Reshaping it to fit would change what that door means,
       so this component asks the question itself, of the same component, with its own answers to the two
       things a caller states. */
    /* FETCH §4.1 "Main fetch" STEP 6, BEFORE STEP 7. XHR §3.5.6's request has the EMPTY destination, which is
       none of Mixed Content §4.1's three, so this call answers NULL today and the address is unchanged. It is
       made because a step some request-creating sites run and others do not is one missing capability wearing
       two names — the same argument core/fetch/fetch.h makes about the disjunction below, which had four
       hand-written copies and a fifth entry with none. */
    {
        char *up = fetch_main_upgrade(ctx, u, /*destination*/ "", /*initiator*/ NULL);
        DCHECK(up == NULL,
               "Mixed Content §4.1 upgraded an XMLHttpRequest's address — its step 1.4 ends the algorithm for "
               "a destination that is not `image`, `audio` or `video`, and XHR §3.5.6 \"The send() method\" "
               "creates its request with the EMPTY destination, so an upgrade here means this site's "
               "destination and the one it passes to step 7 below have come apart");
        free(up);
    }
    if (parsed &&
        fetch_main_blocked(ctx, u, /*destination*/ "",
                           /* FETCH §2.2.5's TWO METADATA FIELDS, UNSTATED, and here the claim is the
                              strongest of the four this engine makes: the words `nonce` and `integrity` do
                              not occur ANYWHERE in the XMLHttpRequest Standard. §3.5.6 "The send() method"
                              builds its request from §3.5.1 The open() method's stored method and URL and
                              this object's own state, and there is no element behind it for Fetch §2.2.5's
                              note — "generally populated from attributes and flags on the HTML element
                              responsible for creating a request" — to draw from. Nor does it set a parser
                              metadata, which is that same note's other field. All three are the initial
                              empty string. */
                           csp_request_metadata_unstated(),
                           /* AND THE MODE, WHICH THAT STANDARD STATES OUTRIGHT AND DOES NOT DERIVE. XHR
                              §3.5.6 "The send() method" initializes its request with "mode `cors`" as a
                              literal row of the list that builds it — beside the credentials mode, which IS
                              conditional on this object's cross-origin credentials flag. So the two fields
                              come from the same list and only one of them is a decision. */
                           FETCH_MODE_CORS)) {
        url_record_free(&rec);
        JS_FreeCString(ctx, u);
        return true;
    }
    url_record_free(&rec);

    /* §4.3 SCHEME FETCH, over §3.5.6's request. The METHOD is on it because §4.3's `blob` arm reads it — "If
       request's method is not `GET` … return a network error" — and XHR §3.5.1 The open() method step 4,
       Normalize method, normalized it, so this
       component states what it has rather than letting the switch read a field nobody filled. §5.4's captured
       blob URL entry is JS_UNDEFINED: XHR §3.5.1 parses a URL STRING and has no Request object to have
       captured with, so §4.3 reads the entry off the store as the URL's own. */
    m = JS_ToCString(ctx, d->method);
    CHECK(m != NULL, "XMLHttpRequest: the request reached §4.1 main fetch with no method — XHR §3.5.1 The "
                     "open() method step 4 normalizes one and its step 11.2 sets it on the object before the "
                     "state is `opened`, and §4.3's `blob` arm reads it. An UNKNOWN one is answered above and "
                     "never arrives here, so this is a String or the object holds what §3.5.1 never wrote");
    memset(&req, 0, sizeof req);
    req.method = m;
    req.url = u;   /* the two fields §4.3 Scheme fetch reads */
    /* …AND THE CREDENTIALS MODE, WHICH §4.3 DOES NOT READ AND THIS REQUEST HAS ANYWAY. It is the same request
       §3.5.6 step 6 built — the one the trusted host is handed when §4.3 answers `HTTP(S) scheme` — so a
       record that stated the member on one route and not the other would be two requests wearing one name.
       Stating it here is also what keeps the field's zero meaning what core/fetch/fetch.h says it means: a
       producer that never wrote it, rather than a producer whose route happened not to need it. */
    req.credentials = xhr_credentials_mode(d);
    /* …AND §3.5.6's MODE, the literal `cors` row of that same list — stated on the record for the credentials
       mode's reason exactly: the park and the scheme-fetch route must carry one request, not two. */
    req.mode = FETCH_MODE_CORS;
    switch (scheme_fetch(ctx, &req, JS_UNDEFINED, &reply)) {
    case SCHEME_FETCH_RESPONSE:
        /* Through the ONE reply object every answer to this component takes, exactly as a host reply is. */
        xhr_take_reply(ctx, d, reply);
        JS_FreeValue(ctx, reply);
        local = true;
        break;
    case SCHEME_FETCH_NETWORK_ERROR:
        /* §3 already has the response as a network error, so there is nothing to write for it: "handle errors"
           fires the request error steps on the way out. */
        local = true;
        break;
    default:
        /* §4.3 Scheme fetch's "HTTP(S) scheme" arm, which hands to §4.4 HTTP fetch — the trusted host's to answer. */
        local = false;
        break;
    }
    JS_FreeCString(ctx, m);
    JS_FreeCString(ctx, u);
    return local;
}

static int js_xhr_run_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSXhrRunState *s = st;
    JSValueConst self = JS_StepClosureData(&s->hdr, XHR_CD_OBJECT);
    XhrData *d = xhr_of(self);
    JSValue in = cb_result;
    int r, err;

    DCHECK(d != NULL, "the XMLHttpRequest lifecycle machine was minted over something that is not one");

    if (s->hdr.stage == XR_FETCH) {
        int mode = JS_VALUE_GET_INT(JS_StepClosureData(&s->hdr, XHR_CD_MODE));
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        s->ev = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = s->cb[3] = JS_UNDEFINED;
        s->transmitted = s->length = 0;
        if (mode == XHR_MODE_ERROR) { s->hdr.stage = XR_ERR_BEGIN; goto error_steps; }
        /* §3.5.6 STEP 6 — "Let req be a new request, initialized as follows" — AND THE ONE FACT ABOUT THAT
           REQUEST THIS ENGINE STATES THAT THE STANDARD'S ELEVEN MEMBERS DO NOT: what it is evidence of.
           ASKED HERE, ONCE, BECAUSE THIS IS THE TURN THE REQUEST IS CREATED ON. `engine_prov_of_running_path`
           reads the path that is STANDING, so it answers about this act only while this act is what is
           happening — and the reply to this request lands on a LATER turn with the flow parked in between,
           which is §scheduler's "an operation that becomes a work item takes its inputs with it; anything it
           reads back off the object it acts on is read at the wrong TIME" with a network round trip in the
           middle. Every consumer below takes it from the record. */
        d->request_prov = (int8_t)engine_prov_of_running_path();
        /* §3.5.6 step 6's request record, onto the @H surface, before §4.1 chooses who answers it. */
        xhr_record_endpoint(ctx, d);
        /* Fetch §4.1: main fetch decides WHO answers. A request this agent answers itself — a port §2.9 blocks,
           or a scheme §4.3 resolves here — has its response on the record already and owes the host nothing, so
           the wait below has nothing to wait for. */
        if (xhr_main_fetch_local(ctx, d)) {
            s->req = 0;
            s->hdr.stage = XR_WAIT;
        } else {
            char *op = xhr_request_op(ctx, d);
            if (!op) return JS_STEP_ABRUPT;
            s->req = engine_host_request(ctx, op);
            free(op);
            s->hdr.stage = XR_WAIT;
            return JS_STEP_YIELD;   /* the flow is BLOCKED on the answer; siblings run meanwhile */
        }
    }

    if (s->hdr.stage == XR_WAIT) {
        JSValueConst answer = JS_UNDEFINED;
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        if (s->req) {
            if (!engine_host_answered(s->req, &answer))
                return JS_STEP_YIELD;
            xhr_take_reply(ctx, d, answer);
            {
                /* §3.5.6's fetch is answered by the trusted zone out of the network, not by a peer running a
                   program, so its completion is normal or the host answered a question nobody asked. A network
                   FAILURE is a reply this component reads off the record, never a throw completion. */
                int completion = ENGINE_COMPLETION_NORMAL;
                JSValue taken = engine_host_take(ctx, s->req, &completion);
                DCHECK(completion == ENGINE_COMPLETION_NORMAL,
                       "an XMLHttpRequest's fetch was answered with a THROW completion — the host answers it "
                       "out of the network, and a network error is a reply rather than a thrown value");
                JS_FreeValue(ctx, taken);
            }
            s->req = 0;
        }
        /* The upload has finished transmitting the moment the request is answered — the host answers a request
           whole, so there are no chunk boundaries between and processRequestBodyChunkLength never runs.
           requestBodyTransmitted therefore equals requestBodyLength exactly once. */
        {
            size_t blen = 0;
            const char *b = JS_IsNull(d->request_body) ? NULL : JS_ToCStringLen(ctx, &blen, d->request_body);
            s->length = b ? (double)blen : 0;
            s->transmitted = s->length;
            if (b) JS_FreeCString(ctx, b);
        }
        if (d->synchronous) {
            /* §3.5.6's synchronous arm runs "handle response end-of-body" and NOTHING ELSE — no
               processResponse, no headers-received state, no progress event. It reaches that algorithm
               through its one entry, which is what makes step 6's synchronous test and steps 3-5's operands
               a property of the algorithm rather than of this caller. */
            s->hdr.stage = XR_EOB_BEGIN;
        } else {
            /* processRequestEndOfBody steps 1-2: upload complete becomes true, and an object with no upload
               listener fires none of the three. */
            bool had_listener = d->upload_listener && !d->upload_complete;
            d->upload_complete = 1;
            s->hdr.stage = had_listener ? XR_UPLOAD_PROGRESS : XR_RESPONSE;
        }
    }

    /* ---- the ASYNCHRONOUS upload's end-of-body, then processResponse ---- */
    if (s->hdr.stage == XR_UPLOAD_PROGRESS) {
        r = xhr_fire_run(ctx, s, d->upload, "progress", /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_UPLOAD_LOAD;
    }
    if (s->hdr.stage == XR_UPLOAD_LOAD) {
        r = xhr_fire_run(ctx, s, d->upload, "load", /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_UPLOAD_LOADEND;
    }
    if (s->hdr.stage == XR_UPLOAD_LOADEND) {
        r = xhr_fire_run(ctx, s, d->upload, "loadend", /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_RESPONSE;
    }

    if (s->hdr.stage == XR_RESPONSE) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        err = xhr_handle_errors(d);
        if (err != XHR_ERR_NONE) { s->hdr.stage = XR_ERR_BEGIN; goto error_steps; }
        if (!d->send_invoked) return JS_STEP_DONE;   /* aborted between the placement and the answer */
        d->state = XHR_HEADERS_RECEIVED;
        s->hdr.stage = XR_RSC_HEADERS;
    }
    if (s->hdr.stage == XR_RSC_HEADERS) {
        r = xhr_fire_run(ctx, s, self, "readystatechange", /*progress*/ false, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        /* "If this's state is not headers received, then return" — a listener may have aborted or reopened. */
        if (d->state != XHR_HEADERS_RECEIVED) return JS_STEP_DONE;
        /* STEP 7, WHICH STOOD UNASKED AND MADE EVERY BODYLESS REPLY LOOK LIKE A BODY. §3.5.6's processResponse
           step 7 is "If this's response's body is null, then run handle response end-of-body for this and
           return", and the three stages below it are steps 8-13's — the length, processBodyChunk, and
           "Incrementally read this's response's body". Without this arm a 204 answered `readyState === 3`,
           fired a `readystatechange` a browser never fires there, and fired a `progress` event before
           end-of-body's own; the reply carries no bytes either way, so the defect was the EVENTS rather than
           the data and nothing downstream could report it.
           THE RECORD CANNOT SAY "NULL BODY" AND THE STATUS CAN, WHICH IS FETCH'S OWN KEYING RATHER THAN A
           SUBSTITUTE FOR IT. Fetch §4.1 "Main fetch" is what nulls the body — "If response is not a network
           error and either request's method is `HEAD` or `CONNECT`, or internalResponse's status is a null
           body status, set internalResponse's body to null and disregard any enqueuing toward it (if any)" —
           and a null body reaches this engine as the EMPTY byte sequence, because the trusted zone's reader
           answers that for a stream that was never there. Step 3 above has already returned for a network
           error, the other reason §5.3 "Body mixin" gives for a null body, so what is left of the disjunction
           at THIS step is the method and the status. `CONNECT` is unreachable: §3.5.1 "The open() method"
           throws a SecurityError for it, Fetch §2.2.1 "Methods" making it a forbidden method.
           A NAMED RESIDUAL STOOD HERE CLAIMING §4.1's METHOD HALF WAS NARROWED, AND IT WAS FALSE AT BIRTH —
           REWRITTEN RATHER THAN DELETED, BECAUSE THE READING THAT PRODUCED IT IS THE ONE A READER
           RE-DERIVES. It said a method still CONCOLIC here takes the not-null-body arm, that §3.5.6 step 3's
           fork leaves `GET` and `HEAD` both in its bodyless arm, and that the next diff was therefore a
           SECOND declared fork over that same operand. Every clause of that is true ABOUT STEP 3 and the
           conclusion does not follow, because the population it names is EMPTY: an unknown method is
           answered inside this agent and never reaches a response at all, so this line is never asked about
           one. `xhr_method_nulls_body` now asserts that route instead of answering past it, and §4.1's
           method half is WHOLE at this site rather than narrowed.
           THE METHOD THAT PRODUCED IT IS THE FINDING AND THE CLAUSE IS ONLY ITS SYMPTOM. Both halves of the
           disjunction were verified against the standards, and the question never asked was the one a
           residual owes its own operand: WHICH STEP FIRST SEES THIS INPUT. Fetch §4.1 "Main fetch" runs
           BEFORE this one and disposes of the unknown-method case two steps upstream, so a clause reasoning
           only about §3.5.6's own steps could not see it. A NOT-COVERED clause naming a POPULATION is a
           hypothesis about this tree; one naming a PROPERTY is a statement about the spec, and only the
           second is worth what this file's conventions rate a residual at. */
        if (fetch_status_is_null_body(d->status) || xhr_method_nulls_body(ctx, d)) {
            s->hdr.stage = XR_EOB_BEGIN;
        } else {
            s->hdr.stage = XR_LOADING;
        }
    }
    if (s->hdr.stage == XR_LOADING) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* processBodyChunk steps 1-3, run ONCE because the reply arrives whole.
           NAMED RESIDUAL — these three stages are CORRECT for a body delivered in one piece and are NARROWER
           than step 13's "Incrementally read this's response's body", which Fetch §2.2.4 "Bodies" defines as
           a LOOP ("Perform the incrementally-read loop given reader, taskDestination, processBodyChunk,
           processEndOfBody, and processBodyError"). NOT COVERED: this algorithm's own step 2, which XHR
           §3.5.6 states as "If not roughly 50ms have passed since these steps were last invoked, then
           return" — it has no operand to read, because nothing on this
           object records when these steps last ran; with one invocation the condition is vacuously satisfied,
           so the omission is unobservable and only a second invocation could expose it. WHAT THE NEXT DIFF
           BUILDS: the chunk seam itself, in spec order — a reply record whose body GROWS (core/fetch/fetch.c
           asserts today that a record does NOT already carry one), a park the delivery RESUMES without
           RETIRING (solver/pending_index.h: a record "leaves the outstanding set for good" when answered, and
           is "keyed at most once"), and only then this stage looping back to the wait. HOW ITS ABSENCE SHOWS:
           a page counting its own `progress` events, or accumulating in `onprogress`, sees exactly one before
           end-of-body for a reply of any size, where a browser fires one per ~50ms of arrival. */
        d->state = XHR_LOADING;
        s->length = xhr_response_length(ctx, d);
        {
            size_t len = 0;
            (void)fetch_body_bytes(ctx, d->received, &len);
            /* THE RECEIVED BYTES' COUNT, which is now the response's own byte count. It was the length of a JS
               string re-encoded to UTF-8, so `progress.loaded` disagreed with `Content-Length` for every
               response holding a byte outside ASCII. */
            s->transmitted = (double)len;
        }
        s->hdr.stage = XR_RSC_LOADING;
    }
    if (s->hdr.stage == XR_RSC_LOADING) {
        r = xhr_fire_run(ctx, s, self, "readystatechange", /*progress*/ false, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_PROGRESS;
    }
    if (s->hdr.stage == XR_PROGRESS) {
        r = xhr_fire_run(ctx, s, self, "progress", /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_EOB_BEGIN;
    }
    /* ---- "handle response end-of-body", through its ONE entry ------------------------------------------
       THREE CALLERS REACHED THIS ALGORITHM AND EACH INLINED A DIFFERENT PREFIX OF IT, which is the defect
       the `RUN_STAGES` machine's own banner already names ("Writing the sequences twice — once for the send machine
       and once for a task — is how two copies of an event order drift, and the order IS the spec"). The
       asynchronous arm ran steps 1-2 above and left 3-5 to a stage that belongs to a DIFFERENT algorithm;
       the synchronous arm ran 1-2 and 7-8 for itself and answered 3-5 with zeroes; and processResponse step
       7 had no way in at all, which is why it was never built. Steps 1-5 are stated here once and every
       caller routes to this stage.
       STEPS 3-5 ARE THE RESPONSE'S NUMBERS AND THEY ARE COMPUTED WHERE THE ALGORITHM COMPUTES THEM. "Let
       transmitted be xhr's received bytes's length", then "Let length be the result of extracting a length
       from this's response's header list" and "If length is not an integer, then set it to 0" — and the two
       fields they land in are SHARED with processRequestEndOfBody's upload events, which fire with the
       REQUEST body's numbers. A shared carrier is not a shared answer: the synchronous arm zeroed them
       precisely so the request's length would not be reported on the response's `load`, and that workaround
       is deleted here because the values this stage writes are the ones step 10 and step 11 are owed. A
       synchronous object's `load` and `loadend` therefore stop reporting `loaded: 0` for every reply.
       STEP 6 IS CONDITIONED AND THE CONDITION IS THIS ROUTE. "If xhr's synchronous is false, then fire a
       progress event named progress at xhr with transmitted and length" — so a synchronous object goes
       straight to steps 7-8, which is what it did before by jumping past XR_EOB_PROGRESS and is stated here
       as the step's own test rather than as a jump a reader has to reconstruct. */
    if (s->hdr.stage == XR_EOB_BEGIN) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* Steps 1-2. */
        err = xhr_handle_errors(d);
        if (err != XHR_ERR_NONE) { s->hdr.stage = XR_ERR_BEGIN; goto error_steps; }
        /* Steps 3-5. */
        {
            size_t len = 0;
            (void)fetch_body_bytes(ctx, d->received, &len);
            s->transmitted = (double)len;
        }
        s->length = xhr_response_length(ctx, d);
        /* Step 6's condition, as the route. Steps 7-8 for the arm that skips it. */
        if (d->synchronous) {
            d->state = XHR_DONE;
            d->send_invoked = 0;
            s->hdr.stage = XR_EOB_RSC;
        } else {
            s->hdr.stage = XR_EOB_PROGRESS;
        }
    }
    if (s->hdr.stage == XR_EOB_PROGRESS) {
        r = xhr_fire_run(ctx, s, self, "progress", /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        d->state = XHR_DONE;
        d->send_invoked = 0;
        s->hdr.stage = XR_EOB_RSC;
    }
    if (s->hdr.stage == XR_EOB_RSC) {
        r = xhr_fire_run(ctx, s, self, "readystatechange", /*progress*/ false, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_EOB_LOAD;
    }
    if (s->hdr.stage == XR_EOB_LOAD) {
        r = xhr_fire_run(ctx, s, self, "load", /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_EOB_LOADEND;
    }
    if (s->hdr.stage == XR_EOB_LOADEND) {
        r = xhr_fire_run(ctx, s, self, "loadend", /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
    }

error_steps:
    if (s->hdr.stage == XR_ERR_BEGIN) {
        int which = JS_VALUE_GET_INT(JS_StepClosureData(&s->hdr, XHR_CD_EVENT));
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        if (which == XHR_ERR_NONE) which = xhr_handle_errors(d);
        DCHECK(which != XHR_ERR_NONE, "the request error steps ran with no error to report");
        s->which = (uint8_t)which;
        d->state = XHR_DONE;
        d->send_invoked = 0;
        d->network_error = 1;
        s->transmitted = s->length = 0;
        /* Step 4: a synchronous object THROWS the exception rather than firing anything. */
        if (d->synchronous)
            return JS_ThrowDOMException(ctx, XHR_ERR_EXC[which],
                                        "a synchronous XMLHttpRequest ended in a %s", XHR_ERR_EVENT[which]),
                   JS_STEP_ABRUPT;
        s->hdr.stage = XR_ERR_RSC;
    }
    if (s->hdr.stage == XR_ERR_RSC) {
        r = xhr_fire_run(ctx, s, self, "readystatechange", /*progress*/ false, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_ERR_UP_EV;
        if (d->upload_complete) s->hdr.stage = XR_ERR_EV;
        else { d->upload_complete = 1; if (!d->upload_listener) s->hdr.stage = XR_ERR_EV; }
    }
    if (s->hdr.stage == XR_ERR_UP_EV) {
        r = xhr_fire_run(ctx, s, d->upload, XHR_ERR_EVENT[s->which], /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_ERR_UP_END;
    }
    if (s->hdr.stage == XR_ERR_UP_END) {
        r = xhr_fire_run(ctx, s, d->upload, "loadend", /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_ERR_EV;
    }
    if (s->hdr.stage == XR_ERR_EV) {
        r = xhr_fire_run(ctx, s, self, XHR_ERR_EVENT[s->which], /*progress*/ true, in, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = XR_ERR_END;
    }
    DCHECK(s->hdr.stage == XR_ERR_END, "the XMLHttpRequest lifecycle machine resumed at a stage §3.5.6 and "
                                       "§3.5.7 do not have between them");
    r = xhr_fire_run(ctx, s, self, "loadend", /*progress*/ true, in, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

/* XHR §3.2 Garbage collection's SECOND PARAGRAPH, at the one moment it can apply: "If an XMLHttpRequest object
 * is garbage collected while its connection is still open, the user agent must terminate the XMLHttpRequest
 * object's fetch controller."
 *
 * THE MACHINE'S TEARDOWN IS WHEN THE CONNECTION LOSES ITS ONLY READER, which is why the requirement lands here
 * and not in xhr_finalizer. The block above states that this closure IS the reference discharging §3.2's first
 * paragraph, so by the time the object itself is collected this machine is already gone — and it is gone
 * through here. `s->req` is this component's fetch controller: XR_WAIT clears it the instant the answer is
 * taken, so a non-zero one at teardown says the machine was abandoned with a request still outstanding with
 * the trusted zone (a flow dropped under HTML §7.5.10 Destroying documents, or a chain freed under a throw).
 * Nothing takes that entry afterwards: it is a register slot whose only reader has been freed, which is
 * precisely the leak §COW names — malloc'd platform state the runtime's own GC walk cannot see.
 *
 * AND IT IS DISCHARGED BY WITHDRAWING THE RENDEZVOUS, which is what `engine_host_terminate` is: Fetch §2
 * Infrastructure's "To terminate a fetch controller controller, set controller's state to 'terminated'" —
 * the entry leaves whichever register holds it, the flow that was BLOCKED on it (pending_blocked) is made
 * askable again, and the trusted zone is asked to stop the transfer. Terminate and not ABORT: §2
 * Infrastructure's abort carries an "AbortError" DOMException to deliver, and §3.2's object has been collected,
 * so there is no continuation left standing at the read for one to be raised at.
 *
 * A REPLY THAT HAS ALREADY LANDED ON THE ENTRY GOES WITH IT, and that is this paragraph's correction rather
 * than a loss: it used to say taking an already-arrived answer and dropping it "is not the same act and must
 * not stand in for it". The act it warned about was taking through `engine_host_take`, which is a machine
 * consuming its answer and would leave an UNANSWERED sibling entry outstanding for ever. Withdrawal asks the
 * register a different question — it removes the entry whatever state it is in — and a value whose only reader
 * has been freed is a reply, never a work item. §3.5.1 "The open() method" step 10 says the same thing from the
 * other end: "Terminate this's fetch controller. A fetch can be ongoing at this point."
 *
 * `take_result` is not read because this machine's completion is undefined on every path — the driver's own
 * `fini ? fini(…) : JS_UNDEFINED` is what it returned before this existed, and the return is unchanged. */
static JSValue js_xhr_run_fini(JSContext *ctx, void *st, bool take_result)
{
    JSXhrRunState *s = st;

    (void)take_result;
    if (s->req) {
        engine_host_terminate(ctx, s->req);
        /* AND THE CONTROLLER IS SPENT. `fini` is called once per state, so a second read of this field is
           unreachable through the driver — which is exactly why it is cleared here rather than left: the field
           is this component's fetch controller and a torn-down machine has none, so anything that could still
           see the state must see that. */
        s->req = 0;
    }
    return JS_UNDEFINED;
}

static const JSTrampStepDef js_xhr_run_def = {
    sizeof(JSXhrRunState), js_xhr_run_step, js_xhr_run_fini, 0, .visit = js_xhr_run_visit,
    .algorithm = "XHR §3.5.6 send()'s fetch, processResponse, handle response end-of-body and the request "
                 "error steps",
    .steps = js_xhr_run_steps
};

/* Mint the lifecycle machine over `xhr`, in `mode`, reporting `which` error. OWNED. */
static JSValue xhr_run_closure(JSContext *ctx, JSValueConst xhr, int mode, int which)
{
    JSValueConst data[3];

    DCHECK(g_run_stepid >= 0, "the XMLHttpRequest lifecycle machine was minted before xhr_init declared it");
    data[XHR_CD_OBJECT] = xhr;
    data[XHR_CD_MODE] = JS_NewInt32(ctx, mode);
    data[XHR_CD_EVENT] = JS_NewInt32(ctx, which);
    return JS_NewStepClosure(ctx, g_run_stepid, 0, 3, data);
}

/* ---- §3.5.6 send() ------------------------------------------------------------------------------------------ */

#define SEND_STAGES(X) \
    X(SEND_CHECKS, "XHR §3.5.6 send() steps 1-3 (the state and send() invoked checks, and dropping the body " \
                   "for a GET or a HEAD)") \
    X(SEND_BODY_STR, "XHR §3.5.6 send() step 4's \"safely extracting body\" (the union's USVString arm is " \
                     "ToString on the page's value)") \
    X(SEND_BODY, "XHR §3.5.6 send() step 4 (extract the request body and set the author Content-Type)") \
    X(SEND_FLAGS, "XHR §3.5.6 send() steps 5-11 (the upload listener, the request, upload complete, timed " \
                  "out, and send() invoked)") \
    X(SEND_LOADSTART, "XHR §3.5.6 send() step 12.1 (fire a progress event named loadstart at this with 0 " \
                      "and 0)") \
    X(SEND_UPLOAD_LOADSTART, "XHR §3.5.6 send() step 12.5 (fire a progress event named loadstart at this's " \
                             "upload object with requestBodyTransmitted and requestBodyLength)") \
    X(SEND_RUN, "XHR §3.5.6 send() step 12 or step 13 (the fetch: enqueued as a task when asynchronous, and " \
                "PERFORMED HERE when synchronous, which is where the flow pauses)")
enum { IDL_STEP_STAGE_BASE(SEND_STAGES) SEND_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SEND_STEPS[] = { SEND_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t phase;      /* the dispatch/call request's own phase */
    JSValue ev;         /* the event in flight (owned) */
    EventFireCb cb;
    JSValue body;       /* the converted body, held across the extraction (owned) */
    JSValue fn;         /* the lifecycle machine a SYNCHRONOUS send calls (owned) */
    double  body_len;
    /* WHETHER THIS STATE ENTERED §3.5.6 AT ALL, AND WHETHER ITS CONSTRUCTION COMPLETED — the two facts the @H
       edge census below needs and cannot ask anyone for. `began` is a FLAG and not a test on a slot for
       core/fetch's reason exactly: SEND_CHECKS can PARK (§3.5.6 step 3's declared fork over a concolic
       method) and a parked stage is re-entered at its first line, so a raise on entry would count one call
       many times. `placed` is the same for the SYNCHRONOUS arm, which parks inside its own call to the
       lifecycle machine and re-enters SEND_RUN.
       BOTH ARE PLAIN BYTES AND NEITHER IS DECLARED TO `visit`, which is correct and is the one thing to check
       when adding a field here: a deep fork BYTE-COPIES this struct, so a scalar is carried to both arms with
       no ownership to split — and carrying them is what makes a forked arm's own teardown file itself under
       the stage IT was standing at rather than under its parent's. */
    uint8_t began;
    uint8_t placed;
    /* THE MIRROR IS THE SAME WIDTH AS THE THING IT MIRRORS. JSStepHdr::stage is a `uint16_t`, and a narrower
       copy of it would not be a smaller number, it would be a DIFFERENT stage: stage 258 truncates to 2,
       which this machine's table names, so the teardown would file a state under an arm it never stood at and
       the partition would still sum. `release` is handed the body state alone, so `hdr->stage` is not in its
       hand and the stage has to be mirrored here. */
    uint16_t stage_at;
} JSXhrSendState;

static void js_xhr_send_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSXhrSendState *s = st;
    int i;
    v->val(ctx, &s->ev);
    v->val(ctx, &s->body);
    v->val(ctx, &s->fn);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* §3's `XMLHttpRequestBodyInit` = `(Blob or BufferSource or FormData or URLSearchParams or USVString)`. Web
   IDL §3.2.25 picks an INTERFACE arm for a platform object of that interface and the USVString arm for
   everything else — including a plain `{}`, which reaches the server as "[object Object]" and is what a page
   that forgot JSON.stringify actually sends. The four brand tests are each the owning component's own, so this
   is a list of who to ASK and never a second copy of what a Blob is. */
static bool xhr_body_is_interface_arm(JSValueConst v)
{
    size_t n = 0;
    const char *t = NULL;

    if (!JS_IsObject(v)) return false;
    if (blob_bytes_of(v, &n, &t) != NULL) return true;                 /* Blob (and File, which is one) */
    if (JS_IsArrayBuffer(v) || JS_GetTypedArrayType(v) >= 0 || JS_IsDataView(v)) return true;  /* BufferSource */
    if (form_data_is(v)) return true;
    if (usp_list_of(v) != NULL) return true;
    return false;
}

/* §3.5.6 STEP 3 OVER A METHOD NOBODY KNOWS — "If this's request method is `GET` or `HEAD`, then set body to
 * null." §3.5.1's steps 2-4 leave an unknown method narrowed to a method that is not a forbidden one, and
 * nothing in that narrowing says which method it is, so this predicate has both its worlds and the algorithm
 * declares them rather than reading bytes that do not exist.
 *
 * OUTCOME 0 IS THE ONE THAT KEEPS THE BODY, which is quickjs-step.h's numbering rule applied to a predicate
 * whose arms are not success and failure. What a run with no forking policy takes must be the arm an @S
 * candidate re-fire can reach a sink through, and the other arm DELETES the request body — the @H payload and
 * the bytes a sink is reached with. The two arms are otherwise symmetric, so the rule is decided by which one
 * loses something. */
enum { SEND_M_BODY_RIDES = 0, SEND_M_BODYLESS, SEND_M_OUTCOMES };
#define SEND_METHOD_OP "XHR §3.5.6 send() step 3 over the request method"

/* WHICH OF THE TWO STEP 3 REACHES ON THE UNKNOWN'S OWN EXAMPLE — step_fork_run's `real`, exactly as
   xhr_open_method_real answers it for §3.5.1, and JS_OUTCOME_REAL_UNSTATED where the method carries no example
   or carries one that is not already a String (coercing one here would run the page's valueOf from a C
   activation with no flow base under it). */
static int xhr_send_method_real(JSContext *ctx, JSValueConst mv)
{
    JSValue ex = concolic_example(ctx, mv);
    const char *mc;
    int real = JS_OUTCOME_REAL_UNSTATED;

    if (!JS_IsString(ex)) { JS_FreeValue(ctx, ex); return real; }
    mc = JS_ToCString(ctx, ex);
    if (mc) {
        real = (!strcmp(mc, "GET") || !strcmp(mc, "HEAD")) ? SEND_M_BODYLESS : SEND_M_BODY_RIDES;
        JS_FreeCString(ctx, mc);
    }
    JS_FreeValue(ctx, ex);
    return real;
}

static int js_xhr_send_step_1(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSXhrSendState *s = st;
    XhrData *d = xhr_of(hdr->this_val);
    JSValue in = cb_result;
    int r;

    if (hdr->stage == SEND_CHECKS) {
        /* §3.5.6's `optional Document or XMLHttpRequestBodyInit? body = null`, declared IDL_ANY because every
           arm of that union crosses as itself. It is read from §3.6's `values` and not from the machine's raw
           `args` for the reason xhr_idl_arg records: `(void)argc; (void)argv;` stood here, discarding the very
           vector the declaration exists to produce, and the two happen to be the same VALUE only for as long
           as the declared type stays `any` — a silence that would end the day SEND_ARGS names a real type. */
        JSValueConst body = xhr_idl_arg(argc, argv, 0);
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        s->ev = s->body = s->fn = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = s->cb[3] = JS_UNDEFINED;
        /* THE CONSTRUCTION BEGAN — the ASK this edge owes the @H surface, raised at the one place that runs
           exactly once per page-level `send()` call that reached this body, and BEFORE the three throws
           below, because a call the page made and the engine threw out of is a network call site REACHED and
           is the whole population these rows exist to make visible. Gated on a flag: this stage PARKS on
           §3.5.6 step 3's fork and a parked stage is re-entered at its first line. solver/endpoint.h states
           why it may not be paired with the teardown rows by a containment — a deep-fork copy inherits the
           flag and does NOT come through here. */
        if (!s->began) {
            s->began = 1;
            endpoint_xhr_edge_began();
        }
        if (!d) return JS_ThrowTypeError(ctx, "not an XMLHttpRequest"), -1;
        if (d->state != XHR_OPENED)
            return JS_ThrowDOMException(ctx, "InvalidStateError", "send() before open()"), -1;
        if (d->send_invoked)
            return JS_ThrowDOMException(ctx, "InvalidStateError", "send() while a request is in flight"), -1;
        if (concolic_is(d->method)) {
            /* Step 3 as a declared fork — see SEND_METHOD_OP. NOTHING OF THIS MACHINE'S IS HELD AT THE ASK:
               `s->body` is not dup'd until below, and everything above is a re-read of the object's own state
               that the re-entry repeats harmlessly (send() invoked is not set until SEND_FLAGS). */
            int arm = 0;

            r = step_fork_run(ctx, hdr, d->method, SEND_METHOD_OP, SEND_M_OUTCOMES,
                              xhr_send_method_real(ctx, d->method), &arm);
            if (r > 0) return r;
            DCHECK(arm == SEND_M_BODY_RIDES || arm == SEND_M_BODYLESS,
                   "XHR §3.5.6 step 3's fork answered with a completion this member never declared");
            if (arm == SEND_M_BODYLESS) body = JS_NULL;   /* step 3 */
        } else {
            const char *m = JS_ToCString(ctx, d->method);

            CHECK(m != NULL, "XMLHttpRequest: OOM reading the request method back for §3.5.6 step 3 — a "
                             "method this engine cannot read is one step 3's predicate has no answer for");
            if (!strcmp(m, "GET") || !strcmp(m, "HEAD")) body = JS_NULL;   /* step 3 */
            JS_FreeCString(ctx, m);
        }
        s->body = JS_DupValue(ctx, body);
        STEP_GOTO(hdr->stage, SEND_BODY_STR, &s->phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == SEND_BODY_STR) {
        /* The union's USVString arm: a value that is not one of the interface arms is ToString'd, and that is
           the page's code. body_extract takes the string arm as an ALREADY-CONVERTED string, which is what
           this stage produces. */
        if (!JS_IsNull(s->body) && !JS_IsUndefined(s->body) && !xhr_body_is_interface_arm(s->body)) {
            JSValue str;
            r = step_tostring_run(ctx, hdr, s->body, in, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            JS_FreeValue(ctx, s->body);
            s->body = str;
        } else {
            JS_FreeValue(ctx, in);
        }
        in = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, SEND_BODY, &s->phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == SEND_BODY) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        xhr_set(ctx, d, &d->request_body, JS_NULL);
        d->request_body_is_shape = 0;   /* the arm goes with the body it describes */
        s->body_len = 0;
        if (!JS_IsNull(s->body) && !JS_IsUndefined(s->body)) {
            BodyState b = { 0 };
            char *mime = NULL;
            /* §3.5.6 step 4's Document arm is not reachable here: a Document is not one of BodyInit's arms and
               this engine has no `(Document or XMLHttpRequestBodyInit)` serializer, so a Document takes the
               union's USVString arm above. That is a fidelity gap in the ARM CHOICE and it is named rather
               than hidden — building it is "serialize a Document", which the DOM half owes. */
            /* §3.5.6 step 4 extracts with no keepalive flag — XHR has no such concept, which is exactly what
               §5.2's `= false` default states for every caller that does not name one. */
            const char *bbytes = NULL;
            size_t blen = 0;
            if (body_extract(ctx, &b, s->body, /*keepalive*/ false, &mime) < 0) { free(mime); return -1; }
            /* WHAT THE BODY IS, ASKED OF THE UNION RATHER THAN OF `bytes == NULL`. `b.bytes ? b.bytes : ""`
               stood here and answered a body built out of UNKNOWN EXTERNAL INPUT as the EMPTY STRING, so
               `xhr.send(cfg.payload)` recorded a POST this engine reported as bodyless.
               NAMED RESIDUAL — the taint does not survive this slot. WHAT IS NOT COVERED: `request_body` is
               declared "a JS string, or JS_NULL" and three sites downstream `JS_ToCString` it (the host's
               request JSON, §3.5.6's endpoint record, and the upload progress lengths), so an unknown body
               reaches them as its DISPLAY SHAPE and its LENGTH is that shape's — a spelling of a hole, not the
               body's length, which is what `loadstart`/`progress` then report. WHAT THE NEXT DIFF BUILDS: this
               slot carrying the VALUE, with each of those three asking core/fetch/body.h's question the way
               core/fetch/fetch.c's host edge now does, and §3.5.6's requestBodyLength stating UNKNOWN rather
               than a number. HOW ITS ABSENCE SHOWS: an `xhr.send(unknown)` whose progress events carry the
               character count of "{cfg.payload}", and a re-fire that cannot substitute the hole because the
               value's identity was dropped here. */
            /* THE ARM IS KEPT, WHICH IS THE HALF THE RESIDUAL ABOVE COULD NOT DO FROM HERE. It cannot carry
               the VALUE — that is the next diff it names — but the one fact the @H surface must not guess is
               whether these characters are the body or a spelling of it, and this is where that is known. */
            d->request_body_is_shape = (body_state_content(&b, &bbytes, &blen) == BODY_SHAPE);
            xhr_set(ctx, d, &d->request_body, JS_NewStringLen(ctx, bbytes ? bbytes : "", blen));
            s->body_len = (double)blen;
            body_state_free(JS_GetRuntime(ctx), &b);
            /* Step 4's Content-Type: the author's own wins, and only an absent one takes the extracted type. */
            if (mime) {
                char *have = hl_get(ctx, d->author_headers, "content-type");
                if (!have) hl_set(ctx, d->author_headers, "Content-Type", mime);
                free(have);
                free(mime);
            }
        }
        STEP_GOTO(hdr->stage, SEND_FLAGS, &s->phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == SEND_FLAGS) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* Step 5: a listener on the upload object is what makes the request preflighted, and what makes the
           upload's own progress events fire. */
        d->upload_listener = event_target_has_any_listener(ctx, d->upload) ? 1 : 0;
        d->upload_complete = 0;                                  /* step 7 */
        d->timed_out = 0;                                        /* step 8 */
        d->aborted = 0;
        if (JS_IsNull(d->request_body)) d->upload_complete = 1;  /* step 9 */
        d->send_invoked = 1;                                     /* step 10 */
        s->body_len = JS_IsNull(d->request_body) ? 0 : s->body_len;
        STEP_GOTO(hdr->stage, d->synchronous ? SEND_RUN : SEND_LOADSTART, &s->phase, &hdr->get_phase, NULL);
    }
    if (hdr->stage == SEND_LOADSTART) {
        if (JS_IsUndefined(s->ev)) {
            s->ev = progress_event_new(ctx, "loadstart", 0, 0);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return -1; }
        }
        r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), hdr->this_val, s->ev, JS_UNDEFINED, in, NULL,
                                  out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return -1;
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, SEND_UPLOAD_LOADSTART, &s->phase, &hdr->get_phase, NULL);
        /* Step 12.6: a listener that aborted or reopened stops the send here. */
        if (d->state != XHR_OPENED || !d->send_invoked) { *presult = JS_UNDEFINED; return 0; }
    }
    if (hdr->stage == SEND_UPLOAD_LOADSTART) {
        if (!d->upload_complete && d->upload_listener) {
            if (JS_IsUndefined(s->ev)) {
                s->ev = progress_event_new(ctx, "loadstart", 0, s->body_len);
                if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return -1; }
            }
            r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), d->upload, s->ev, JS_UNDEFINED, in, NULL,
                                      out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return -1;
            JS_FreeValue(ctx, s->ev);
            s->ev = JS_UNDEFINED;
        } else {
            JS_FreeValue(ctx, in);
            in = JS_UNDEFINED;
        }
        if (d->state != XHR_OPENED || !d->send_invoked) { *presult = JS_UNDEFINED; return 0; }
        STEP_GOTO(hdr->stage, SEND_RUN, &s->phase, &hdr->get_phase, NULL);
    }

    DCHECK(hdr->stage == SEND_RUN, "the send() machine resumed at a stage §3.5.6 does not have");
    if (!d->synchronous) {
        /* ASYNCHRONOUS: the fetch and everything that processes its response are a TASK, so send() returns to
           the page here — which is the whole of what `async` means. */
        JSValue fn = xhr_run_closure(ctx, hdr->this_val, XHR_MODE_FETCH, XHR_ERR_NONE);
        JS_FreeValue(ctx, in);
        if (JS_IsException(fn)) return -1;
        /* NO TASK SOURCE, AND THAT IS A STATEMENT ABOUT §3.5.6 RATHER THAN A GAP. Its asynchronous arm
           QUEUES NOTHING: it fires `loadstart`, binds the processResponse callbacks, sets the fetch controller
           from a fetch with `useParallelQueue set to true`, and returns. No task source put this work item
           anywhere, so inventing one would order the rest of `send()` against the page's real tasks by a fact
           no standard states. What this engine does with a fetch run off a parallel queue is make it a work
           item on the one frontier, which is why it reaches a TASK queue at all. */
        JS_EnqueueCallTask(ctx, fn, 0, NULL, TASK_SOURCE_NOT_A_TASK);
        JS_FreeValue(ctx, fn);
        /* AND THE CONSTRUCTION COMPLETED — §3.5.6 step 12's fetch is placed and this state is done with it.
           It is a PLACEMENT and not an offer: the address reaches the @H surface from the machine enqueued
           above, which this state will be torn down before. Asserted rather than assumed to happen once,
           because a second placement from one send would double this edge's share of the completions while
           the partition below still summed. */
        DCHECK(!s->placed,
               "an XMLHttpRequest send state placed §3.5.6's fetch twice — this arm returns to the page, so a "
               "second arrival is a re-entry the machine's own stage assert did not refuse, and the edge "
               "census would count one construction as two completions");
        s->placed = 1;
        endpoint_xhr_edge_placed();
        *presult = JS_UNDEFINED;
        return 0;
    }
    /* SYNCHRONOUS: §3.5.6's "Pause until either processedResponse is true or …". The machine CALLS the
       lifecycle machine, so this flow parks on that call and the lifecycle machine parks on the host — one
       suspension, at the exact line the page wrote, resumable at any depth. A request error inside it THROWS,
       which is §3.5.6's request error steps step 4 and reaches the page as send()'s own exception. */
    if (JS_IsUndefined(s->fn)) {
        s->fn = xhr_run_closure(ctx, hdr->this_val, XHR_MODE_FETCH, XHR_ERR_NONE);
        if (JS_IsException(s->fn)) { s->fn = JS_UNDEFINED; JS_FreeValue(ctx, in); return -1; }
        /* THE SAME COMPLETION FOR §3.5.6 STEP 13, INSIDE THIS `if` AND NOT BELOW IT. The synchronous arm PARKS
           in the call beneath and re-enters SEND_RUN, and this block is the one that runs once — so the flag
           and the raise sit where the closure is minted rather than where the call is made. */
        s->placed = 1;
        endpoint_xhr_edge_placed();
    }
    {
        JSValue out;
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->fn, JS_UNDEFINED, 0, NULL, in, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        JS_FreeValue(ctx, out);
    }
    *presult = JS_UNDEFINED;
    return 0;
}

/* THE STAGE THIS STATE IS STANDING AT, MIRRORED ONTO THE STATE SO THE TEARDOWN CAN READ IT — the same
 * wrapper core/fetch/fetch.c carries and for the same three reasons. `release` is handed the body state and
 * nothing else, so `hdr->stage` is not in its hand, and WHICH STAGE a construction died at is this census's
 * whole content.
 *
 * IT IS A WRAPPER AND NOT A LINE AT THE TOP OF THE BODY. This machine's stages FALL THROUGH: one entry at
 * SEND_CHECKS can run §3.5.6 steps 1-11 end to end and leave standing at SEND_RUN, so a mirror taken on the
 * way IN records the stage a state was ENTERED at, which for every state that dies after its first entry is
 * the wrong answer — and wrong in the direction that piles the whole population onto the first row. Taken on
 * the way OUT it is right on both edges a state can leave by: the body sets `hdr->stage` to the stage it will
 * resume at before returning a park, and a body that THREW leaves it at the stage the throw came from.
 *
 * IT IS ALSO WHY THERE IS NO MIRROR AT THE `STEP_GOTO`s INSIDE. Writing it at each of them is the same fact
 * in six places, with an obligation at every stage a later diff adds and nothing to catch the one that is
 * missed — the second-list shape core/idl_args.h's own `visit` contract exists to end, one field over. */
static int js_xhr_send_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    int r = js_xhr_send_step_1(ctx, hdr, st, argc, argv, cb_result, presult, out_cb, out_argc);

    ((JSXhrSendState *)st)->stage_at = hdr->stage;
    return r;
}

/* THE OUTCOME HALF OF THE EDGE CENSUS, HERE BECAUSE HERE IS WHERE EVERY SEND STATE ENDS — originals and
 * deep-fork copies alike, whether the member completed or was abandoned parked, since core/idl_args.c's
 * teardown discharges this release on both edges.
 *
 * IT RELEASES NOTHING AND THAT IS NOT A GAP. `release` is core/idl_args.h's entry for what the DECLARATION
 * CANNOT NAME — a foreign C allocation, a lexbor handle — and this machine has none: every slot it owns is a
 * JSValue `js_xhr_send_visit` names, which the driver re-takes at a fork and gives back at the teardown, and
 * idl_args_result's own assert folds the visit into a number to check that a release did not touch them. So
 * this entry exists for the census alone, and touching a declared slot from here is what that assert fires on.
 *
 * `began` IS THE GATE AND NOT A GUARD PAST A BROKEN INVARIANT: a state torn down before SEND_CHECKS ran never
 * entered §3.5.6 at all, so it belongs to the ARGUMENT CONVERSION's population and not to this one, which
 * solver/endpoint.h names as this census's own residual rather than folding in as an extra arm. */
static void js_xhr_send_release(JSContext *ctx, void *st)
{
    JSXhrSendState *s = st;

    (void)ctx;
    if (s->began) endpoint_xhr_edge_freed(s->stage_at, s->placed);
}

static const IdlStepDecl XHR_SEND_DECL = {
    js_xhr_send_step, sizeof(JSXhrSendState), js_xhr_send_visit, js_xhr_send_release,
    "XHR §3.5.6 send(body)", SEND_STEPS
};

/* ---- §3.5.7 abort() ------------------------------------------------------------------------------------------ */

#define ABORT_STAGES(X) \
    X(ABORT_BEGIN, "XHR §3.5.7 abort() steps 1-2 (abort the fetch controller, then decide whether the " \
                   "request error steps run)") \
    X(ABORT_ERROR, "XHR §3.5.7 abort() step 2 (run the request error steps for this and abort)") \
    X(ABORT_RESET, "XHR §3.5.7 abort() step 3 (a done object returns to unsent with a network error, and " \
                   "no readystatechange is dispatched)")
enum { IDL_STEP_STAGE_BASE(ABORT_STAGES) ABORT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ABORT_STEPS[] = { ABORT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t phase;
    JSValue fn;      /* the lifecycle machine, in error mode (owned) */
    EventFireCb cb;
} JSXhrAbortState;

static void js_xhr_abort_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSXhrAbortState *s = st;
    int i;
    v->val(ctx, &s->fn);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

static int js_xhr_abort_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSXhrAbortState *s = st;
    XhrData *d = xhr_of(hdr->this_val);
    JSValue in = cb_result;
    int r;

    (void)argc; (void)argv;
    if (hdr->stage == ABORT_BEGIN) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        s->fn = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = s->cb[3] = JS_UNDEFINED;
        if (!d) return JS_ThrowTypeError(ctx, "not an XMLHttpRequest"), -1;
        d->aborted = 1;
        /* Step 2's "if this's state is opened with this's send() invoked being true, headers received, or
           loading" — the same three-state disjunction §3.2 Garbage collection states, read from the one
           place that states it. */
        if (xhr_gc_window(d)) {
            DCHECK(!d->synchronous,
                   "abort() reached the request error steps on a SYNCHRONOUS XMLHttpRequest — §3.5.6's step 4 "
                   "would throw an exception abort() was never given, and the flow that called send() is "
                   "parked inside it, so nothing in this agent can be the caller");
            s->fn = xhr_run_closure(ctx, hdr->this_val, XHR_MODE_ERROR, XHR_ERR_ABORT);
            if (JS_IsException(s->fn)) { s->fn = JS_UNDEFINED; return -1; }
            STEP_GOTO(hdr->stage, ABORT_ERROR, &s->phase, NULL);
        } else {
            STEP_GOTO(hdr->stage, ABORT_RESET, &s->phase, NULL);
        }
    }
    if (hdr->stage == ABORT_ERROR) {
        JSValue out;
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->fn, JS_UNDEFINED, 0, NULL, in, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        JS_FreeValue(ctx, out);
        STEP_GOTO(hdr->stage, ABORT_RESET, &s->phase, NULL);
        in = JS_UNDEFINED;
    }
    DCHECK(hdr->stage == ABORT_RESET, "the abort() machine resumed at a stage §3.5.7 does not have");
    JS_FreeValue(ctx, in);
    if (d->state == XHR_DONE) {
        d->state = XHR_UNSENT;
        d->network_error = 1;
    }
    *presult = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl XHR_ABORT_DECL = {
    js_xhr_abort_step, sizeof(JSXhrAbortState), js_xhr_abort_visit, NULL,
    "XHR §3.5.7 abort()", ABORT_STEPS
};

/* ---- §3.1 the constructor ------------------------------------------------------------------------------------ */

#define XHR_CTOR_STAGES(X) \
    X(XHR_CTOR_BUILD, "XHR §3.1 new XMLHttpRequest() step 1 (set this's upload object to a new " \
                      "XMLHttpRequestUpload object)")
enum { IDL_STEP_STAGE_BASE(XHR_CTOR_STAGES) XHR_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const XHR_CTOR_STEPS[] = { XHR_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } JSXhrCtorState;
static void js_xhr_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSXhrCtorState *s = st;
    (void)ctx; (void)s; (void)v;
}

static int js_xhr_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj, proto, upload;
    XhrData *d;

    (void)st; (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == XHR_CTOR_BUILD, "the XMLHttpRequest constructor resumed at a stage §3.1 does not have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor XMLHttpRequest requires 'new'"), -1;
    proto = JS_GetClassProto(ctx, g_xhr_class);
    DCHECK(!JS_IsNull(proto), "an XMLHttpRequest was constructed in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_xhr_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return -1;
    proto = JS_GetClassProto(ctx, g_upload_class);
    DCHECK(!JS_IsNull(proto), "an XMLHttpRequestUpload was built in a realm that never ran its install");
    upload = JS_NewObjectProtoClass(ctx, proto, g_upload_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(upload)) { JS_FreeValue(ctx, obj); return -1; }
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "XMLHttpRequest: OOM building an XMLHttpRequest");
    /* INITIALIZATION, NOT A WRITE — so this is the one place the twelve are assigned directly rather than
       through xhr_set. The record is calloc'd and unreachable until JS_SetOpaque below, so no slot holds a
       value to release and no collector can walk one; xhr_set here would free whatever calloc's zeroes decode
       to. Every assignment AFTER this function is a write and goes through xhr_set. */
    d->upload = upload;
    d->method = d->url = d->url_src = d->request_body = d->override_mime = d->response_object = JS_NULL;
    d->author_headers = JS_NewArray(ctx);
    d->response_headers = JS_NewArray(ctx);
    d->status_text = JS_NewString(ctx, "");
    d->response_url = JS_NewString(ctx, "");
    d->received = JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    d->state = XHR_UNSENT;
    d->network_error = 1;   /* §3: "response — a response, initially a network error" */
    d->request_prov = -1;   /* no request has been composed — see the field */
    JS_SetOpaque(obj, d);
    *presult = obj;
    return 0;
}

static const IdlStepDecl XHR_CTOR_DECL = {
    js_xhr_ctor_step, sizeof(JSXhrCtorState), js_xhr_ctor_visit, NULL,
    "XHR §3.1 new XMLHttpRequest()", XHR_CTOR_STEPS
};

/* ---- install -------------------------------------------------------------------------------------------------- */

/* XHR's `XMLHttpRequest` interface declares two `open` overloads and the longer one writes
   `undefined open(ByteString method, USVString url, boolean async, optional USVString? username = null,
   optional USVString? password = null)` — so `async` is a `boolean` and is declared as one.
   IT WAS IDL_ANY, WHICH LEFT THE BODY TO COERCE IT, AND THE BODY CANNOT. §7.1.2 ToBoolean's last step is
   "Return true" and unknown external input wears an ordinary Object, so `xhr.open(m, u, cfg.sync)` was an
   asynchronous request in every world and the SYNCHRONOUS one — which §3.5.1 step 9 makes observable, since
   `async` false with a non-zero timeout is an "InvalidAccessError" — was never explored. (Step 9 counted with
   list DEPTH tracked: step 8's host branch holds a nested list whose two items are ITS sub-steps, and a flat
   `<li>` count promotes them to peers and reports this one as step 11.) IDL_BOOLEAN is
   IDL_CONCOLIC_FORKS, so §7.1.2 ToBoolean is asked at the branch seam at the boundary.
   The step-7 reading below is UNCHANGED by that, and it is why this position may be declared a boolean at all:
   §3.5.1 step 7 is "If the async argument is omitted, set async to true, and set username and password to
   null", with the note "Unfortunately legacy content prevents treating the async argument being undefined
   identical from it being omitted" — so the ARGUMENT COUNT decides omission, and an explicit `undefined` is a
   value §3.2.3 converts to false. The conversion places `undefined` for the absent position and the body's
   `argc > 2` is what tells the two apart. */
static const IdlArgType OPEN_ARGS[5] = {
    IDL_BYTESTRING, IDL_USVSTRING, IDL_BOOLEAN, IDL_USVSTRING_NULLABLE, IDL_USVSTRING_NULLABLE
};
static const IdlArgType SET_HEADER_ARGS[2] = { IDL_BYTESTRING, IDL_BYTESTRING };
static const IdlArgType GET_HEADER_ARGS[1] = { IDL_BYTESTRING };
static const IdlArgType OVERRIDE_ARGS[1] = { IDL_DOMSTRING };
static const IdlArgType SEND_ARGS[1] = { IDL_ANY };

void xhr_init(JSContext *ctx)
{
    JSClassDef xd = { "XMLHttpRequest", .finalizer = xhr_finalizer, .gc_mark = xhr_gc_mark };
    JSClassDef ud = { "XMLHttpRequestUpload" };
    JSClassDef ed = { "XMLHttpRequestEventTarget" };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_xhr_rt == NULL || g_xhr_rt == rt, "XMLHttpRequest was declared into a second runtime");
    /* NOT `if (g_ready) return;`. This component has exactly ONE declaration site — core/platform.c's row —
       so the test could never be true, and what it could do was hide a release that left the latch set: the
       second agent would then get an XMLHttpRequest reporting itself declared with every step id belonging to
       a runtime that is gone. core/agent_state.h names the three components where that had already happened. */
    DCHECK(!g_ready, "xhr_init ran twice — one instance is one document is one agent");
    g_xhr_rt = rt;
    JS_NewClassID(rt, &g_xhr_class);
    JS_NewClass(rt, g_xhr_class, &xd);
    JS_NewClassID(rt, &g_upload_class);
    JS_NewClass(rt, g_upload_class, &ud);
    JS_NewClassID(rt, &g_xhr_et_class);
    JS_NewClass(rt, g_xhr_et_class, &ed);

    g_ctor_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &XHR_CTOR_DECL, 0);
    g_open_stepid = idl_method_id_step(ctx, OPEN_ARGS, 5, NULL, 0, &XHR_OPEN_DECL, 0);
    idl_optional_from(2);
    g_send_stepid = idl_method_id_step(ctx, SEND_ARGS, 1, NULL, 0, &XHR_SEND_DECL, 0);
    idl_optional_from(0);
    g_abort_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &XHR_ABORT_DECL, 0);
    g_id_set_request_header = idl_method_id(ctx, SET_HEADER_ARGS, 2, js_xhr_set_request_header, 0);
    g_id_get_response_header = idl_method_id(ctx, GET_HEADER_ARGS, 1, js_xhr_get_response_header, 0);
    g_id_get_all = idl_method_id(ctx, NULL, 0, js_xhr_get_all_response_headers, 0);
    g_id_override_mime = idl_method_id(ctx, OVERRIDE_ARGS, 1, js_xhr_override_mime_type, 0);
    g_set_timeout_id = idl_setter_id(ctx, IDL_UNSIGNED_LONG, false, js_xhr_set_timeout, 0);
    g_set_with_credentials_id = idl_setter_id(ctx, IDL_BOOLEAN, false, js_xhr_set_with_credentials, 0);
    g_set_response_type_id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_xhr_set_response_type, 0);
    g_response_getter_id = idl_getter_id_step(ctx, &XHR_RESPONSE_DECL, 0);
    g_response_xml_getter_id = idl_getter_id_step(ctx, &XHR_RESPONSE_XML_DECL, 0);
    g_run_stepid = JS_RegisterStepDef(rt, &js_xhr_run_def);
    /* THIS COMPONENT'S CONSTRUCTION MACHINE, HANDED TO THE @H SURFACE'S EDGE CENSUS. It is `SEND_STEPS` and
       NOT `js_xhr_run_steps`: solver/endpoint.h holds the refutation of the clause that named the other one,
       and the short of it is that `send()` is where an XMLHttpRequest request is CONSTRUCTED and where it can
       throw or park without ever reaching a door, while the lifecycle machine's stages are all downstream of
       the record. The table is `SEND_STEPS` itself and never a copy — literals with static storage, so the
       surface may key rows on it for the life of the session and a stage added to SEND_STAGES adds a row
       there with no edit at all. `IDL_STEP_FIRST` is PASSED rather than assumed by the reader, because a
       member's stages are numbered from it and which constant that is belongs to core/idl_args.h. */
    endpoint_xhr_edge_declare(SEND_STEPS, IDL_STEP_FIRST);
    /* §5 is part of THIS standard and every event this component fires is one of its instances, so it is
       declared from here rather than by each host separately — the same rule fetch_init follows for §5's
       Headers and §6's Response. */
    progress_event_init(ctx);
    g_ready = 1;
    agent_state_flag("xml_http_request", &g_ready, "the declaration latch");
    agent_state_ptr("xml_http_request", &g_xhr_rt, "the runtime this interface's machines were registered in");
    /* THE THREE CLASSES, which were held by this component and declared to nobody — so core/agent_state.h's
       pairing never asked about them and xhr_free never gave them back. JS_NewClassID returns the EXISTING
       value when the slot is non-zero, so a second agent's xhr_init handed JS_NewClass1 an id registered in a
       runtime that is gone; §The class id doubles as nothing here (this component latches on `g_ready`), so
       the re-registration DID happen and it happened against a number the new runtime never allocated. */
    agent_state_class("xml_http_request", &g_xhr_class, "§3's XMLHttpRequest class");
    agent_state_class("xml_http_request", &g_upload_class, "§3.4's XMLHttpRequestUpload class");
    agent_state_class("xml_http_request", &g_xhr_et_class, "§3.3's XMLHttpRequestEventTarget class");
    /* THE SECTION NUMBERS BELOW WERE BOTH WRONG AND THIS FILE ALREADY HELD THE RIGHT ONES: XHR_CTOR_STAGES
       says "§3.1 new XMLHttpRequest()" and XHR_OPEN_DECL says "§3.5.1 open(...)", while these two labels said
       §3.5.1 and §3.5.2 — one row of the standard's own list off, so the constructor was named after open()
       and open() after §3.5.2 "The setRequestHeader() method". Verified against the live standard's own
       heading list rather than recalled. */
    agent_state_id("xml_http_request", &g_ctor_stepid, "§3.1's constructor machine");
    agent_state_id("xml_http_request", &g_open_stepid, "§3.5.1's open machine");
    agent_state_id("xml_http_request", &g_send_stepid, "§3.5.6's send machine");
    agent_state_id("xml_http_request", &g_abort_stepid, "§3.5.7's abort machine");
    agent_state_id("xml_http_request", &g_run_stepid, "the request-running machine");
    /* AND THE NINE MEMBER-POOL ENTRIES. They are the same kind of slot as the five machines above and were
       simply not on the list — which is the arm the pairing passes in silence, because a component that
       declares SOME of what it holds produces character-for-character the report of one that holds only
       that. */
    agent_state_id("xml_http_request", &g_id_set_request_header, "§3.5.2's setRequestHeader()");
    agent_state_id("xml_http_request", &g_id_get_response_header, "§3.6.4's getResponseHeader()");
    agent_state_id("xml_http_request", &g_id_get_all, "§3.6.5's getAllResponseHeaders()");
    agent_state_id("xml_http_request", &g_id_override_mime, "§3.6.7's overrideMimeType()");
    agent_state_id("xml_http_request", &g_set_timeout_id, "§3.5.3's timeout setter");
    agent_state_id("xml_http_request", &g_set_with_credentials_id, "§3.5.4's withCredentials setter");
    agent_state_id("xml_http_request", &g_set_response_type_id, "§3.6.8's responseType setter");
    agent_state_id("xml_http_request", &g_response_getter_id, "§3.6.9's response getter");
    agent_state_id("xml_http_request", &g_response_xml_getter_id, "§3.6.11's responseXML getter");
    realm_declare_intrinsic(xhr_install_realm);
}

/* §3's states, as `const unsigned short` on BOTH the interface object and its prototype — which is what Web IDL
   §3.7.5 says a constant is, and what `client.DONE` in §3.6.4's own example reads. */
static const JSCFunctionListEntry XHR_CONSTANTS[] = {
    JS_PROP_INT32_DEF("UNSENT", XHR_UNSENT, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("OPENED", XHR_OPENED, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("HEADERS_RECEIVED", XHR_HEADERS_RECEIVED, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("LOADING", XHR_LOADING, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("DONE", XHR_DONE, JS_PROP_ENUMERABLE),
};

/* THE ONE MEMBER OF `XMLHttpRequest` THIS USER AGENT MUST NOT HAVE. PRIVATE STATE TOKEN API §8 Integration
   with XMLHttpRequest puts `undefined setPrivateToken(PrivateToken privateToken)` on this interface by a
   partial, and the published corpus carries that partial FLAT — with no way to say that a user agent which
   does not ship the API does not have it. Most do not. So the engine says it here, where the gap auditor and
   the next reader of this prototype read one answer instead of each re-deriving it.
   EVERY SECTION NUMBER BELOW REPEATS ITS STANDARD'S NAME, because a bare one in this file names the WRONG
   DOCUMENT: XMLHttpRequest is this file's own standard and its numbers run in the same range.
   ITS THREE STEPS ARE PERFORMABLE AND THAT IS EXACTLY WHY INSTALLING THEM WOULD BE WRONG. PRIVATE STATE TOKEN
   API §8.1 Attach PrivateToken states them in full — throw "InvalidStateError" unless the state is "opened",
   throw again if the send() flag is set, then "Set this's private state token to privateToken" — and the whole
   observable effect of that store is PRIVATE STATE TOKEN API §8.2 send() monkeypatch, which adds one step
   running "set private token properties for request from private token" over the stored token and the request.
   What consumes THAT is PRIVATE STATE TOKEN API §12.1's `Sec-Private-State-Token` header field, its §9
   Issuing Protocol and its §10 Redeeming Tokens — none of them built, and none of them a conversion. A member
   installed with those three steps and nothing behind them answers `typeof xhr.setPrivateToken === "function"`
   — which IS the feature detection a page writes — with a yes, and then attaches no token: a capability CLAIM
   with no capability under it, which is §NO STUBS' dedicated no-effect member standing where the spec states a
   real effect. Absent, this engine is exactly a user agent that does not ship the API, which most browsers
   are, and the page's own TypeError is the forcing function.
   THE SAME DECISION'S OTHER HALF IS core/fetch/request.c's REQUEST_INIT, which declines to declare the
   `privateToken` member PRIVATE STATE TOKEN API §6.1 "Definitions" adds to `RequestInit` — FOR THE REASON
   ABOVE AND NO OTHER. The two halves state ONE argument, so a reason retired at either is retired here, and
   that file holds the full record of the one that was.
   A SECOND REASON STOOD IN THIS PARAGRAPH AND IS RETIRED, named rather than deleted because a reader who
   re-derives it will re-introduce it: it said `PrivateToken`'s `issuers` is `sequence<USVString>` and that
   IdlArgType had no such row, which made the member undeclarable whatever anyone decided about the feature.
   Its REASONING was right and is why that row exists at all — `IDL_SEQUENCE_DOMSTRING` is a DIFFERENT type,
   since WEB IDL §3.2.12 "USVString" converts its value to a sequence of scalar values (every unpaired
   surrogate replaced) and an issuer is an origin that goes on the wire. Only its claim about THIS TREE
   moved: `IDL_SEQUENCE_USVSTRING` is a row of IdlArgType at core/idl_args.h, added for File System Access's
   accepted file types, so all four of `PrivateToken`'s members are declarable today and no re-derivation of
   the retired reason survives one grep of that enum — which is what retires this record rather than a date.
   IT ALSO CONTRADICTED THE LINE IT SAT ABOVE, which is how a drifting half announces itself from inside one
   of them: this paragraph ended "never the signature" while the sentence before it gave a signature reason,
   so the file held two answers to one question and a reader got whichever they reached last. Nothing
   mechanical reports that — both sentences are prose, and the audit row the decision is kept visible by
   (`node engine/idlgen.mjs` naming `RequestInit: privateToken`) says nothing about the reason either half
   gives for it.
   WHAT WOULD CHANGE THIS IS THE FEATURE, AND NOW NOTHING ELSE — the issuing protocol, the redemption and
   the header field named above, no one of them a signature and no one of them supplied by installing a
   member. */
static const char *const XHR_ABSENT[] = { "setPrivateToken" };

/* XHR §3 "Interface XMLHttpRequest"'s THREE Web IDL §3.7.3 INTERFACE PROTOTYPE OBJECTS, their §3.7.1 INTERFACE
   OBJECTS, AND §3.8's PROPERTY REFERENCES FOR ALL THREE — FOR ONE REALM.
   THE INTERFACE OBJECTS ARE HERE BECAUSE §3.8 IS GIVEN A REALM. Web IDL §3.8 "Platform objects implementing
   interfaces" is "To define the global property references on target, given realm realm", step 1 being "Let
   interfaces be a list that contains every interface that is exposed in realm" — the population is a REALM's
   and the algorithm names no Document. XHR §3 declares all three of `XMLHttpRequestEventTarget`,
   `XMLHttpRequestUpload` and `XMLHttpRequest` `[Exposed=(Window,DedicatedWorker,SharedWorker)]`, so a realm
   whose global object implements a `DedicatedWorker` or `SharedWorker` interface is owed all three — and while
   they were placed from core/platform.c's per-document column, such a realm reaches no
   platform_document_install and got none of them.
   NOTHING THIS COMPONENT READS AT INSTALL TIME IS A DOCUMENT'S. XHR does hold per-document state — XHR §3.5.1
   The open() method's steps 5-6 parse the URL — "Let parsedURL be the result of encoding-parsing a URL url",
   relative to the relevant settings object — and
   XHR §3.5.4 The withCredentials getter and setter feeds §3.5.6 send()'s credentials mode — but every one of
   those is read from the RUNNING realm when the member is CALLED, not when the interface object is minted, so
   the move takes no input with it and strands none. The install ignored its `PlatformDocument` argument
   outright, which is the same fact stated by the column.
   THE THREE PROTOTYPES ARE IN HAND AT THE MINT, so the three JS_GetClassProto re-reads the per-document entry
   made are gone — each was a second answer to a question this function had just settled. The three
   JS_SetClassProto handovers move to the END for that reason: the locals own their objects until the realm
   does, rather than being borrowed back out of the class slot they were already given to. */
void xhr_install_realm(JSContext *ctx)
{
    JSValue et_p, up_p, xhr_p, prev;

    DCHECK(g_ready, "a realm asked for XMLHttpRequest.prototype before xhr_init declared it");
    prev = JS_GetClassProto(ctx, g_xhr_class);
    DCHECK(JS_IsNull(prev), "xhr_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* `interface XMLHttpRequestEventTarget : EventTarget` — the seven event handler attributes and nothing
       else. It is a real prototype in the chain because a page reads it: `XMLHttpRequest.prototype.__proto__
       .__proto__ === EventTarget.prototype`. */
    et_p = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, et_p, "XMLHttpRequestEventTarget");
    event_target_install_handlers(ctx, et_p, EH_XHR);

    /* `interface XMLHttpRequestUpload : XMLHttpRequestEventTarget` — no members of its own. */
    up_p = JS_NewObjectProto(ctx, et_p);
    CHECK(!JS_IsException(up_p), "XMLHttpRequestUpload.prototype could not be allocated");
    idl_interface_tag(ctx, up_p, "XMLHttpRequestUpload");

    xhr_p = JS_NewObjectProto(ctx, et_p);
    CHECK(!JS_IsException(xhr_p), "XMLHttpRequest.prototype could not be allocated");
    idl_interface_tag(ctx, xhr_p, "XMLHttpRequest");
    event_target_install_handlers(ctx, xhr_p, EH_XHR_READYSTATE);
    JS_SetPropertyFunctionList(ctx, xhr_p, XHR_CONSTANTS,
                               (int)(sizeof(XHR_CONSTANTS) / sizeof(XHR_CONSTANTS[0])));
    idl_install_accessor(ctx, xhr_p, "readyState", js_xhr_get_ready_state, 0, -1);
    idl_install_method(ctx, xhr_p, "open", g_open_stepid);
    idl_install_method(ctx, xhr_p, "setRequestHeader", g_id_set_request_header);
    idl_install_accessor(ctx, xhr_p, "timeout", js_xhr_get_timeout, 0, g_set_timeout_id);
    idl_install_accessor(ctx, xhr_p, "withCredentials", js_xhr_get_with_credentials, 0,
                         g_set_with_credentials_id);
    idl_install_accessor(ctx, xhr_p, "upload", js_xhr_get_upload, 0, -1);
    idl_install_method(ctx, xhr_p, "send", g_send_stepid);
    idl_install_method(ctx, xhr_p, "abort", g_abort_stepid);
    idl_install_accessor(ctx, xhr_p, "responseURL", js_xhr_get_response_url, 0, -1);
    idl_install_accessor(ctx, xhr_p, "status", js_xhr_get_status, 0, -1);
    idl_install_accessor(ctx, xhr_p, "statusText", js_xhr_get_status_text, 0, -1);
    idl_install_method(ctx, xhr_p, "getResponseHeader", g_id_get_response_header);
    idl_install_method(ctx, xhr_p, "getAllResponseHeaders", g_id_get_all);
    idl_install_method(ctx, xhr_p, "overrideMimeType", g_id_override_mime);
    idl_install_accessor(ctx, xhr_p, "responseType", js_xhr_get_response_type, 0, g_set_response_type_id);
    idl_install_accessor_step(ctx, xhr_p, "response", g_response_getter_id, -1);
    idl_install_accessor(ctx, xhr_p, "responseText", js_xhr_get_response_text, 0, -1);
    idl_install_accessor_step(ctx, xhr_p, "responseXML", g_response_xml_getter_id, -1);
    idl_members_excluded(ctx, xhr_p, "XMLHttpRequest", XHR_ABSENT,
                         (int)(sizeof(XHR_ABSENT) / sizeof(XHR_ABSENT[0])),
                         "PRIVATE STATE TOKEN API §8.1 Attach PrivateToken's three steps end in \"Set this's "
                         "private state token to privateToken\", and the only reader of that store is PRIVATE "
                         "STATE TOKEN API §8.2 send() monkeypatch, feeding a header field and an issuing and "
                         "redemption protocol this user agent does not ship — so an installed member would "
                         "answer a page's typeof detection yes and then attach no token");

    /* WEB IDL §3.8's STEP 3.1.3 FOR EACH OF THE THREE — "Perform DefineMethodProperty(target, id,
       interfaceObject, false)" — with each prototype the local this function just built rather than one read
       back out of a class slot. `XMLHttpRequestEventTarget` and `XMLHttpRequestUpload` declare no constructor,
       so their §3.7.1 interface objects are minted over idl_illegal_ctor; `XMLHttpRequest` declares one, and
       its object carries §3's five states a second time because Web IDL §3.7.5 Constants puts a constant on
       the interface object AND its prototype.
       THE EXPOSURE IS THE DOOR'S ANSWER, NOT A CONDITION HERE: idl_define_global_property_reference asks Web
       IDL §3.3.7 [Exposed] step 1 against this realm's §3.3.8 [Global] global names, keyed by the identifier
       it is already handed, so nothing at this site re-derives what the corpus states — and nothing about
       WHICH realms carry these three names changed when they left the per-document column. */
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor;

        idl_define_global_property_reference(ctx, global, "XMLHttpRequestEventTarget",
                                            idl_interface_object(ctx, "XMLHttpRequestEventTarget", et_p));
        idl_define_global_property_reference(ctx, global, "XMLHttpRequestUpload",
                                            idl_interface_object(ctx, "XMLHttpRequestUpload", up_p));

        DCHECK(g_ctor_stepid >= 0, "XMLHttpRequest was installed before xhr_init declared its constructor");
        ctor = idl_step_constructor(ctx, "XMLHttpRequest", g_ctor_stepid);
        CHECK(!JS_IsException(ctor), "the XMLHttpRequest interface object could not be allocated");
        JS_SetPropertyFunctionList(ctx, ctor, XHR_CONSTANTS,
                                   (int)(sizeof(XHR_CONSTANTS) / sizeof(XHR_CONSTANTS[0])));
        JS_SetConstructor(ctx, ctor, xhr_p);
        idl_define_global_property_reference(ctx, global, "XMLHttpRequest", ctor);
        JS_FreeValue(ctx, global);
    }
    /* The realm owns the three from here. */
    JS_SetClassProto(ctx, g_xhr_et_class, et_p);
    JS_SetClassProto(ctx, g_upload_class, up_p);
    JS_SetClassProto(ctx, g_xhr_class, xhr_p);
    /* XHR §5 IS NOT BUILT OR PLACED HERE, AND THE SECOND HALF OF THAT IS NEW. progress_event_init declared
       its own per-realm intrinsic, so building XHR §5's prototype a second time would leave everything
       already chained to the first answering out of a discarded object — and now that its Web IDL §3.7.1
       Interface object rides that same intrinsic, the `progress_event_install(ctx, global)` this function's
       per-document half used to end with is gone rather than moved. A conversion that carried the three names
       above across and dropped that tail call would take `ProgressEvent` out of EVERY realm, Window included,
       with nothing to say so: XHR §5's prototype would still be built, `progress_event_new` would still mint
       every event this component and core/file/file_reader.c fire, and only the page-visible constructor name
       would be missing. engine/host/test_forced.c's exposure_selftest carries the row that catches it.
       EVERY NUMBER HERE REPEATS ITS STANDARD'S NAME for the reason this file's XHR_ABSENT banner already
       gives: a bare `§5` in this file is resolved by FILE VOTE, and `§5's INTERFACE OBJECT` was placed on
       Web IDL — whose §5 is "Extensibility" — because "interface object" is Web IDL's term and not this
       standard's. */
}

void xhr_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — the declare pass of core/platform.c's one list is unconditional. */
    DCHECK(g_ready, "XMLHttpRequest was released in an agent that never declared it");
    DCHECK(rt == g_xhr_rt, "XMLHttpRequest was released against a runtime that is not the one it declared in");
    progress_event_free(rt);
    g_ready = 0;
    g_xhr_rt = NULL;
    g_ctor_stepid = g_open_stepid = g_send_stepid = g_abort_stepid = -1;
    g_run_stepid = -1;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because a carried id
       names a class in a RUNTIME that is gone. What this costs is stated there and paid above —
       xhr_finalizer and xhr_gc_mark reach the record through JS_GetAnyOpaque, since both run after this
       column and neither may look their record up under an id this line has already returned. */
    g_xhr_class = g_upload_class = g_xhr_et_class = 0;
    g_id_set_request_header = g_id_get_response_header = g_id_get_all = g_id_override_mime = -1;
    g_set_timeout_id = g_set_with_credentials_id = g_set_response_type_id = -1;
    g_response_getter_id = g_response_xml_getter_id = -1;
}
