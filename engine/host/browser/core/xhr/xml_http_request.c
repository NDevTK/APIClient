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
    JSValue method;            /* request method — a JS string, JS_NULL before open() */
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
static void xhr_set(JSContext *ctx, XhrData *d, JSValue *slot, JSValue v)
{
    cow_record_set(ctx, d, &XHR_REC, slot, v);
}

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

/* ---- §5.1's header list, as a JS Array of [name, value] ---------------------------------------------------
 *
 * Fetch §5.1's operations over the shape §COW requires. The NAME is stored as the page wrote it (§3.5.2 sends
 * what it was given) and every comparison is ASCII case-insensitive, which is what "a header list contains a
 * name" means. */

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

/* §3.5.1, §3.5.3, §3.5.4 and §3.6.8 each ask "is the current global object a Window object". Every realm this
   engine builds IS a Window realm — there is no DedicatedWorkerGlobalScope and no SharedWorkerGlobalScope, so
   `[Exposed=(Window,DedicatedWorker,SharedWorker)]` reduces to the first — and the answer is written once here
   so the day a worker global exists there is ONE place that learns to say no, rather than four `if`s that were
   each right for the wrong reason. */
static bool xhr_global_is_window(JSContext *ctx) { (void)ctx; return true; }

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
 * asserts the opposite of what the code did — "the declaration converts `USVString?` null and undefined to the
 * IDL null" — which is exactly true of `values` and false of `args`.
 *
 * Out of range reads undefined, for the reason step_arg's does: that is what an optional argument means at
 * this level, and §3.6's required-arity TypeError has already run, so positions 0 and 1 are always present. */
static JSValueConst xhr_idl_arg(int argc, JSValueConst *argv, int i)
{
    return i < argc ? argv[i] : JS_UNDEFINED;
}

static int js_xhr_open_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSXhrOpenState *s = st;
    XhrData *d;
    int r;

    if (hdr->stage == OPEN_STEPS) {
        const char *m, *u;
        char *norm;
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
        /* Step 1: a Window whose document is not fully active. */
        if (!document_fully_active(ctx))
            return JS_ThrowDOMException(ctx, "InvalidStateError",
                                        "open() on an XMLHttpRequest whose document is not fully active"), -1;
        m = JS_ToCString(ctx, xhr_idl_arg(argc, argv, 0));
        if (!m) return JS_STEP_ABRUPT;
        /* Steps 2-4 as ONE operation, through the implementation Fetch §5.3 step 25 already uses: not a method
           is a SyntaxError here where Fetch throws a TypeError, and a forbidden one is a SecurityError — so the
           shared operation answers and this member names its own errors. */
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
        /* Steps 5-6: encoding-parse the URL against the relevant settings object's API base URL. */
        u = xhr_arg_cstring(ctx, xhr_idl_arg(argc, argv, 1), NULL);
        if (!u) { js_free(ctx, norm); return JS_STEP_ABRUPT; }
        url_record_init(&rec);
        ok = fetch_parse_url(ctx, &rec, u, strlen(u));
        JS_FreeCString(ctx, u);
        if (!ok) {
            js_free(ctx, norm);
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
            js_free(ctx, norm);
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
        xhr_set(ctx, d, &d->method, JS_NewString(ctx, norm));
        js_free(ctx, norm);
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

/* §3.6.6 "get a text response": decode the received bytes with the final encoding, defaulting to UTF-8. */
static JSValue xhr_text_response(JSContext *ctx, XhrData *d)
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
    return xhr_reply_content(ctx, d, out);
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
           response must be for `createCDATASection` and for the parse-boundary correction to stay away. */
        xhr_set(ctx, d, &d->response_object,
                document_new(ctx, dom, url ? url : "", document_kind(/*is_xml*/true, content_type)));
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
            document_new(ctx, dom, url ? url : "", document_kind(/*is_xml*/false, content_type)));
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
    X(XR_RSC_HEADERS, "XHR §3.5.6 send() step 5's processResponse step 5 (fire an event named " \
                      "readystatechange at this)") \
    X(XR_LOADING,   "XHR §3.5.6 send() step 5's processBodyChunk steps 1-3 (append the bytes to this's " \
                    "received bytes and set the state to loading)") \
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
    X(XR_EOB_PROGRESS, "XHR §3.5.6 handle response end-of-body step 6 (fire a progress event named progress " \
                       "at xhr with transmitted and length)") \
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

/* Take the host's reply onto the record. A null reply — or none at all — leaves the response a network error,
   which is what §5.5's network error is on the Fetch side too. */
static void xhr_take_reply(JSContext *ctx, XhrData *d, JSValueConst reply)
{
    JSValue st_v, stx_v, hs_v, bd_v;
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
    stx_v = JS_GetPropertyStr(ctx, reply, "statusText");
    hs_v = JS_GetPropertyStr(ctx, reply, "headers");
    JS_ToInt32(ctx, &status, st_v);
    d->status = status;
    xhr_set(ctx, d, &d->status_text, JS_IsString(stx_v) ? JS_DupValue(ctx, stx_v) : JS_NewString(ctx, ""));
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
    JS_FreeValue(ctx, st_v); JS_FreeValue(ctx, stx_v); JS_FreeValue(ctx, hs_v); JS_FreeValue(ctx, bd_v);
}

/* The request the host is owed, as one self-describing JSON record: `{method, url, credentials, headers,
   body}`. It is JSON because the ANSWER already crosses that way (main.c's qjs_host_answer parses one) and
   because a host that must route this to safeFetch needs every field — SECURITY.md's chokepoint cannot decide
   about a method it was never told, and a tab-separated line cannot carry a body. It is written with
   core/json_buf.h rather than with a JS value: every string here is one this engine built, so there is no
   `toJSON` and no Proxy trap to run, which is exactly the distinction quickjs.h makes when it deletes
   JS_JSONStringify. Caller frees with free(). */
static char *xhr_request_op(JSContext *ctx, XhrData *d)
{
    JsonBuf b = { 0 };
    uint32_t n = hl_len(ctx, d->author_headers), i;
    const char *m = JS_ToCString(ctx, d->method), *u = JS_ToCString(ctx, d->url);

    json_buf_raw(&b, "xhr.send\t{"); json_buf_key(&b, "method");
    json_buf_str(&b, m ? m : "GET");
    json_buf_raw(&b, ","); json_buf_key(&b, "url");
    json_buf_str(&b, u ? u : "");
    json_buf_raw(&b, ","); json_buf_key(&b, "credentials");
    json_buf_str(&b, d->cross_origin_credentials ? "include" : "same-origin");
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
    if (m) JS_FreeCString(ctx, m);
    if (u) JS_FreeCString(ctx, u);
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
 * §3.5.6 step 5 IS THE PLACE, because that is where the spec itself assembles `req` out of exactly these four
 * — "method: this's request method / URL: this's request URL / header list: this's author request headers /
 * body: this's request body" — so nothing here re-derives what the record already holds. Before §4.1 decides
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
    const char *mc = NULL, *method = "GET";
    char *body_ct = NULL;
    const char *body = NULL;
    size_t body_len = 0;

    DCHECK(!JS_IsNull(d->url), "an XMLHttpRequest reached §3.5.6's request record with no URL — send() runs "
                               "only on an `opened` object and XHR §3.5.1 The open() method step 12.1, Set "
                               "this's state to opened, is what opens one");
    if (JS_IsString(d->method)) {
        mc = JS_ToCString(ctx, d->method);
        if (mc) method = mc;
    }
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
        body = JS_ToCStringLen(ctx, &body_len, d->request_body);
        body_ct = hl_get(ctx, d->author_headers, "content-type");
        if (body) { eb.mime = body_ct; eb.bytes = body; eb.len = body_len; ebp = &eb; }
    }
    /* The CONCOLIC where open() was given one, so the surface reports the shape AND the example it carries;
       the serialization otherwise, which for a plain address is the same string XHR §3.5.1 The open() method
       step 5 parsed. */
    endpoint_record(ctx, method, JS_IsNull(d->url_src) ? d->url : d->url_src, eh, (int)n, ebp);
    if (body) JS_FreeCString(ctx, body);
    free(body_ct);
    if (owned) {
        for (i = 0; i < n * 2; i++) if (owned[i]) JS_FreeCString(ctx, owned[i]);
        js_free(ctx, owned);
    }
    js_free(ctx, eh);
    if (mc) JS_FreeCString(ctx, mc);
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
 * STEP 7 IS HERE AND NOT ONLY IN core/fetch, because §3.5.6 step 4 is "Fetch req" and the fetch it names is the
 * SAME algorithm `fetch()` performs — a component that owns a second door onto the network owns every step in
 * front of it. Answering it anywhere else would leave `xhr.open("GET", "http://host:25/")` reaching the trusted
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
    const char *u = JS_ToCString(ctx, d->url);
    const char *m;
    UrlRecord rec;
    FetchRequest req;
    JSValue reply = JS_UNDEFINED;
    bool parsed, local;

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
       blocked as mixed content, or should request be blocked by Content Security Policy returns blocked, then
       set response to a network error." §3's response IS a network error already, so blocking is nothing
       written and everything not done: the request is never placed, and the lifecycle machine's "handle
       errors" fires the request error steps on the way out — which for §3.5.6 is an `error` event, or a
       NetworkError thrown out of a synchronous send.
       BOTH CHECKS OR NEITHER. The step is one disjunction, and this component owns it for the same reason the
       header note above gives — §3.5.6 step 4 is "Fetch req", the same algorithm `fetch()` performs — so
       answering only the port half here would make `connect-src 'none'` block a `fetch()` and permit the
       identical request written as an XMLHttpRequest: one policy answering differently depending on which
       door the page used. §6.8.1 gives an XHR the EMPTY destination exactly as it gives `fetch()` one, so both
       are governed by `connect-src`, and a request that has not been redirected has a redirect count of 0. */
    if (parsed &&
        (fetch_block_bad_port(&rec) == FETCH_PORT_BLOCKED ||
         policy_should_block_request(document_policy(ctx), &rec, /*destination*/ "",
                                     /*redirect count*/ 0) == CSP_REQUEST_BLOCKED)) {
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
    m = JS_IsString(d->method) ? JS_ToCString(ctx, d->method) : NULL;
    CHECK(m != NULL, "XMLHttpRequest: the request reached §4.1 main fetch with no method — XHR §3.5.1 The "
                     "open() method step 4 normalizes one and its step 11.2 sets it on the object before the "
                     "state is `opened`, and §4.3's `blob` arm reads it");
    memset(&req, 0, sizeof req);
    req.method = m;
    req.url = u;   /* the two fields §4.3 Scheme fetch reads; a §4.3 answer never reaches the host, so it owes it nothing */
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
        /* §3.5.6 step 5's request record, onto the @H surface, before §4.1 chooses who answers it. */
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
               processResponse, no headers-received state, no progress event. */
            err = xhr_handle_errors(d);
            if (err != XHR_ERR_NONE) { s->hdr.stage = XR_ERR_BEGIN; goto error_steps; }
            s->transmitted = 0;
            s->length = 0;
            d->state = XHR_DONE;
            d->send_invoked = 0;
            s->hdr.stage = XR_EOB_RSC;
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
        s->hdr.stage = XR_LOADING;
    }
    if (s->hdr.stage == XR_LOADING) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* processBodyChunk steps 1-3. The whole body arrives as one chunk, so `received bytes` is already what
           the reply carried and the length is the header's — §3.5.6's "extract a length", 0 when there is
           none, which is exactly what leaves `lengthComputable` false. */
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
        /* "handle response end-of-body" begins with "handle errors", which is a no-op for a healthy reply. */
        err = xhr_handle_errors(d);
        if (err != XHR_ERR_NONE) { s->hdr.stage = XR_ERR_BEGIN; goto error_steps; }
        s->hdr.stage = XR_EOB_PROGRESS;
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

static int js_xhr_send_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
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
        const char *m;
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        s->ev = s->body = s->fn = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = s->cb[3] = JS_UNDEFINED;
        if (!d) return JS_ThrowTypeError(ctx, "not an XMLHttpRequest"), -1;
        if (d->state != XHR_OPENED)
            return JS_ThrowDOMException(ctx, "InvalidStateError", "send() before open()"), -1;
        if (d->send_invoked)
            return JS_ThrowDOMException(ctx, "InvalidStateError", "send() while a request is in flight"), -1;
        m = JS_ToCString(ctx, d->method);
        if (m && (!strcmp(m, "GET") || !strcmp(m, "HEAD"))) body = JS_NULL;   /* step 3 */
        if (m) JS_FreeCString(ctx, m);
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
            if (body_extract(ctx, &b, s->body, /*keepalive*/ false, &mime) < 0) { free(mime); return -1; }
            xhr_set(ctx, d, &d->request_body, JS_NewStringLen(ctx, b.bytes ? b.bytes : "", b.len));
            s->body_len = (double)b.len;
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
        JS_EnqueueCallTask(ctx, fn, 0, NULL);
        JS_FreeValue(ctx, fn);
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

static const IdlStepDecl XHR_SEND_DECL = {
    js_xhr_send_step, sizeof(JSXhrSendState), js_xhr_send_visit, NULL,
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
    realm_declare_intrinsic(xhr_install_protos);
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

void xhr_install_protos(JSContext *ctx)
{
    JSValue et_p, up_p, xhr_p, prev;

    DCHECK(g_ready, "a realm asked for XMLHttpRequest.prototype before xhr_init declared it");
    prev = JS_GetClassProto(ctx, g_xhr_class);
    DCHECK(JS_IsNull(prev), "xhr_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* `interface XMLHttpRequestEventTarget : EventTarget` — the seven event handler attributes and nothing
       else. It is a real prototype in the chain because a page reads it: `XMLHttpRequest.prototype.__proto__
       .__proto__ === EventTarget.prototype`. */
    et_p = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, et_p, "XMLHttpRequestEventTarget");
    event_target_install_handlers(ctx, et_p, EH_XHR);
    JS_SetClassProto(ctx, g_xhr_et_class, et_p);

    /* `interface XMLHttpRequestUpload : XMLHttpRequestEventTarget` — no members of its own. */
    up_p = JS_NewObjectProto(ctx, et_p);
    CHECK(!JS_IsException(up_p), "XMLHttpRequestUpload.prototype could not be allocated");
    idl_interface_tag(ctx, up_p, "XMLHttpRequestUpload");
    JS_SetClassProto(ctx, g_upload_class, up_p);

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
    JS_SetClassProto(ctx, g_xhr_class, xhr_p);
    /* §5's prototype is NOT built here: progress_event_init declared it as a per-realm intrinsic of its own,
       and building it a second time would leave everything already chained to the first answering out of a
       discarded object. */
}

void xhr_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto, ctor;

    DCHECK(g_ready, "XMLHttpRequest was installed before xhr_init declared it");
    proto = JS_GetClassProto(ctx, g_xhr_et_class);
    DCHECK(!JS_IsNull(proto), "XMLHttpRequest was installed into a realm that never ran its proto build");
    JS_SetPropertyStr(ctx, (JSValue)global, "XMLHttpRequestEventTarget",
                      idl_interface_object(ctx, "XMLHttpRequestEventTarget", proto));
    JS_FreeValue(ctx, proto);

    proto = JS_GetClassProto(ctx, g_upload_class);
    JS_SetPropertyStr(ctx, (JSValue)global, "XMLHttpRequestUpload",
                      idl_interface_object(ctx, "XMLHttpRequestUpload", proto));
    JS_FreeValue(ctx, proto);

    proto = JS_GetClassProto(ctx, g_xhr_class);
    ctor = idl_step_constructor(ctx, "XMLHttpRequest", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the XMLHttpRequest interface object could not be allocated");
    JS_SetPropertyFunctionList(ctx, ctor, XHR_CONSTANTS,
                               (int)(sizeof(XHR_CONSTANTS) / sizeof(XHR_CONSTANTS[0])));
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "XMLHttpRequest", ctor);

    progress_event_install(ctx, global);
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
