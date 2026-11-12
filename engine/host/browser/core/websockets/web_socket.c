/* WebSockets §3 "The WebSocket interface" and WebSockets §4 "Feedback from the protocol".
 *
 * The opening handshake is a fetch (WebSockets §2.2 "Opening handshake" step 11): a request in mode
 * "websocket" parked on the running flow's register like every other request, so the trusted zone judges it
 * under the one egress rule and Fetch §4.1 "Main fetch" applies CSP, mixed content and port blocking to it
 * (solver/engine.c's park). The reply is the handshake's processResponse; every state change a page observes
 * after it is a task on the WebSocket task source.
 *
 * The request states WebSockets §2.2 step 8's `Sec-WebSocket-Protocol` alone. Steps 3-7 and 9-10 (`Upgrade`,
 * `Connection`, the key nonce, the version, the extension offer) are the connection's: the zone that opens
 * it composes them and checks RFC 6455 §4.1 "Client Requirements"' `Sec-WebSocket-Accept` against its nonce.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/core/base.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/events/close_event.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/fetch/fetch.h"
#include "core/fetch/headers.h"
#include "core/file/blob.h"
#include "core/frame/policy_container.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/url/url.h"
#include "core/websockets/web_socket.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/reclaim.h"

/* WebSockets §3.1's ready state values, which are also the interface's four constants. */
enum { WS_CONNECTING = 0, WS_OPEN, WS_CLOSING, WS_CLOSED };
/* WebSockets §3.1's `enum BinaryType`, indexed by the record's `binary_type`. */
static const char *const WS_BINARY_TYPE[] = { "blob", "arraybuffer", NULL };
/* RFC 6455 §7.4.1 "Defined Status Codes": 1006 is the close code of a connection that failed. */
enum { WS_CLOSE_ABNORMAL = 1006 };

typedef struct {
    JSValue    url;          /* WebSockets §3.1's url, serialized; written once by the constructor */
    JSValue    protocols;    /* the constructor's protocols, an Array of strings */
    JSValue    protocol;     /* the subprotocol in use; "" until WebSockets §4's open task */
    JSValue    extensions;   /* the extensions in use; "" until WebSockets §4's open task */
    JSValue    handshake;    /* the parked handshake's processResponse; undefined once answered or withdrawn */
    JSContext *realm;        /* the relevant realm; borrowed, the agent owns its realms */
    double     buffered;     /* WebSockets §3.1's bufferedAmount, in bytes */
    uint8_t    ready_state;
    uint8_t    binary_type;  /* an index into WS_BINARY_TYPE */
    uint8_t    failed;       /* the user agent was required to fail the connection, so WebSockets §4's closed task fires error */
    uint8_t    closed_queued;   /* WebSockets §4's closed task is queued or has run */
} WebSocketData;

static JSClassID  g_ws_class;
static JSClassID  g_list_slot = JS_INVALID_CLASS_ID;   /* the per-realm Array of sockets not yet closed */
static JSRuntime *g_ws_rt;
static JSValue    g_task_fn = JS_UNDEFINED;            /* WebSockets §4's task callee, one per agent */
static int        g_task_stepid = -1;
static int        g_ctor_id = -1, g_id_close = -1, g_id_send = -1, g_set_binary_type_id = -1;

/* The record's owned values, read by the capture, the collector and the finalizer alike. */
#define WSO(f) (uint16_t)offsetof(WebSocketData, f)
static const uint16_t WS_VALS[] = { WSO(url), WSO(protocols), WSO(protocol), WSO(extensions), WSO(handshake) };
static const CowRecord WS_REC = { sizeof(WebSocketData), WS_VALS, (int)(sizeof(WS_VALS) / sizeof(WS_VALS[0])) };

/* The record, captured for the running flow before it can be written: an arm that closes a socket must not
   close it for its sibling. */
static const WebSocketData *ws_of(JSValueConst v)
{
    const WebSocketData *d = JS_GetOpaque(v, g_ws_class);
    return d;
}

/* The write accessor: captures the record into the running flow's head before answering it mutable. */
static WebSocketData *ws_of_for_write(JSValueConst v)
{
    WebSocketData *d = JS_GetOpaque(v, g_ws_class);

    if (d) cow_capture_host_record(v, d, &WS_REC);
    return d;
}

#define ws_set(ctx_, owner_, d_, slot_, v_) \
    cow_record_set_at((ctx_), (owner_), (d_), &WS_REC, (slot_), (v_), __FILE__, __LINE__)

/* The collector runs after the release column, so neither callback may look the class id up. */
static void ws_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    WebSocketData *d = JS_GetAnyOpaque(val, &id);
    size_t i;

    (void)id;
    if (!d) return;
    for (i = 0; i < sizeof(WS_VALS) / sizeof(WS_VALS[0]); i++)
        JS_FreeValueRT(rt, *(JSValue *)((char *)d + WS_VALS[i]));
    free(d);
}

static void ws_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    const WebSocketData *d = JS_GetAnyOpaque(val, &id);
    size_t i;

    (void)id;
    if (!d) return;
    for (i = 0; i < sizeof(WS_VALS) / sizeof(WS_VALS[0]); i++)
        JS_MarkValue(rt, *(JSValue *)((char *)d + WS_VALS[i]), mark_func);
}

/* ---- the realm's sockets (WebSockets §7 "Garbage collection") ------------------------------------------- */

static JSValue ws_list(JSContext *ctx)
{
    JSValue list;

    DCHECK(g_ws_rt != NULL, "a realm's sockets were reached before web_socket_init declared them");
    list = realm_value_get(ctx, g_list_slot);
    DCHECK(JS_IsArray(list), "a realm has no socket list; web_socket_install_realm is on realm.h's one list, "
                             "so this realm skipped it");
    return list;
}

static uint32_t ws_list_len(JSContext *ctx, JSValueConst list)
{
    JSValue lv = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    CHECK(!JS_IsException(lv) && JS_ToUint32(ctx, &n, lv) == 0, "a realm's socket list has no readable length");
    JS_FreeValue(ctx, lv);
    return n;
}

/* Takes `sock` off its realm's list once WebSockets §4's closed task has run: nothing can reach the object's
   connection after it, so WebSockets §7 no longer keeps it alive. */
static void ws_list_remove(JSContext *rctx, JSValueConst sock)
{
    JSValue list = ws_list(rctx);
    uint32_t n = ws_list_len(rctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(rctx, list, i);
        bool hit = JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(sock);

        JS_FreeValue(rctx, v);
        if (!hit) continue;
        if (i + 1 < n) JS_SetPropertyUint32(rctx, list, i, JS_GetPropertyUint32(rctx, list, n - 1));
        JS_SetPropertyStr(rctx, list, "length", JS_NewUint32(rctx, n - 1));
        break;
    }
    JS_FreeValue(rctx, list);
}

/* ---- WebSockets §4 "Feedback from the protocol": the tasks ------------------------------------------- */

/* Which of WebSockets §4's tasks a queued call is; its second argument. */
enum { WS_TASK_CLOSED = 0 };

#define WS_TASK_STAGES(X) \
    X(WT_STATE,      "WebSockets §4 the task's ready-state change") \
    X(WT_FIRE_ERROR, "WebSockets §4 closed task step 2 (fire an event named error at the WebSocket object)") \
    X(WT_FIRE_CLOSE, "WebSockets §4 closed task step 3 (fire an event named close at the WebSocket object, " \
                     "using CloseEvent)")
enum { WS_TASK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const WS_TASK_STEPS[] = { WS_TASK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;
    uint8_t     fphase;   /* the fire request's own phase */
    JSValue     ev;       /* the event being fired, held across the suspension (owned) */
    EventFireCb cb;
} WsTask;

/* WebSockets §4's closed task's CloseEvent, from the task's arguments: wasClean, the close code and the reason. */
static JSValue ws_task_close_event(JSContext *rctx, const JSStepHdr *hdr)
{
    const char *reason = JS_ToCString(rctx, step_arg(hdr, 4));
    int32_t code = 0;
    JSValue ev;

    CHECK(reason != NULL && JS_ToInt32(rctx, &code, step_arg(hdr, 3)) == 0,
          "WebSocket: a closed task's code or reason could not be read");
    DCHECK(code >= 0 && code <= 65535, "a closed task was queued with a close code outside unsigned short");
    ev = close_event_new(rctx, JS_ToBool(rctx, step_arg(hdr, 2)) > 0, (uint16_t)code, reason);
    JS_FreeCString(rctx, reason);
    return ev;
}

static int js_ws_task_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    WsTask *s = st;
    JSValueConst sock = step_arg(&s->hdr, 0);
    WebSocketData *d = ws_of_for_write(sock);
    JSContext *rctx;
    int r;

    STEP_DISPATCH(WS_TASK_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(WT_STATE);
    {
        int32_t kind = -1;
        int k;

        JS_FreeValue(ctx, cb_result);
        /* The state is complete before anything below can throw. */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->fphase = 0;
        DCHECK(d != NULL, "a WebSocket task was queued for something that is not a WebSocket");
        rctx = d->realm;
        CHECK(JS_ToInt32(ctx, &kind, step_arg(&s->hdr, 1)) == 0, "WebSocket: a task's kind could not be read");
        DCHECK(kind == WS_TASK_CLOSED, "a WebSocket task was queued with a kind WebSockets §4 does not have");
        /* The closed task, step 1. */
        DCHECK(d->closed_queued && d->ready_state != WS_CLOSED,
               "WebSockets §4's closed task ran for a socket that never queued it, or ran twice");
        d->ready_state = WS_CLOSED;
        ws_list_remove(rctx, sock);
        if (d->failed) {
            s->ev = event_new(rctx, "error", /*bubbles*/ false, /*cancelable*/ false);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            STEP_GOTO(s->hdr.stage, WT_FIRE_ERROR, &s->fphase, NULL);
            return JS_STEP_YIELD;
        }
        s->ev = ws_task_close_event(rctx, &s->hdr);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(s->hdr.stage, WT_FIRE_CLOSE, &s->fphase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(WT_FIRE_ERROR);
    DCHECK(d != NULL && JS_IsObject(s->ev), "WebSockets §4's closed task resumed at its error fire with no socket or event");
    rctx = d->realm;
    r = event_target_fire_run(rctx, &s->fphase, STEP_CB(s->cb), sock, s->ev, JS_UNDEFINED, cb_result, NULL,
                              out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(rctx, s->ev);
    s->ev = ws_task_close_event(rctx, &s->hdr);
    if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
    STEP_GOTO(s->hdr.stage, WT_FIRE_CLOSE, &s->fphase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(WT_FIRE_CLOSE);
    DCHECK(d != NULL && JS_IsObject(s->ev), "WebSockets §4's closed task resumed at its close fire with no socket or event");
    rctx = d->realm;
    r = event_target_fire_run(rctx, &s->fphase, STEP_CB(s->cb), sock, s->ev, JS_UNDEFINED, cb_result, NULL,
                              out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(rctx, s->ev);
    s->ev = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static void js_ws_task_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    WsTask *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static const JSTrampStepDef js_ws_task_def = {
    sizeof(WsTask), js_ws_task_step, NULL, 0,
    .visit = js_ws_task_visit,
    .algorithm = "WebSockets §4 a task on the WebSocket task source",
    .steps = WS_TASK_STEPS
};

/* WebSockets §4: "When the WebSocket connection is closed, possibly cleanly, the user agent must queue a task". Once
   per socket; `reason` is borrowed UTF-8. */
static void ws_queue_closed(WebSocketData *d, JSValueConst sock, bool was_clean, int code, const char *reason)
{
    JSContext *rctx = d->realm;
    JSValue args[5];
    int i;

    DCHECK(!d->closed_queued, "WebSockets §4's closed task was queued twice for one socket; a connection closes once");
    d->closed_queued = 1;
    args[0] = JS_DupValue(rctx, sock);
    args[1] = JS_NewInt32(rctx, WS_TASK_CLOSED);
    args[2] = JS_NewBool(rctx, was_clean);
    args[3] = JS_NewInt32(rctx, code);
    args[4] = JS_NewString(rctx, reason);
    CHECK(!JS_IsException(args[4]), "WebSocket: OOM keeping a close reason for its task");
    JS_EnqueueCallTask(rctx, g_task_fn, 5, (JSValueConst *)args, TASK_SOURCE_WEBSOCKET);
    for (i = 0; i < 5; i++) JS_FreeValue(rctx, args[i]);
}

/* RFC 6455 §7.1.7 "Fail the WebSocket Connection", for a connection with no transport yet: it is closed
   with RFC 6455 §7.4.1's 1006 and not cleanly, and WebSockets §4's closed task fires error before close. */
static void ws_fail(WebSocketData *d, JSValueConst sock)
{
    d->failed = 1;
    if (!d->closed_queued) ws_queue_closed(d, sock, /*was_clean*/ false, WS_CLOSE_ABNORMAL, "");
}

/* ---- WebSockets §2.2 "Opening handshake" --------------------------------------------------------------- */

/* Named residual: the established connection.
   Not covered: a reply of status 101 has no frame channel behind it, so no connection is established: the
   engine fails it, and a declined handshake forks its waiting and failure arms only.
   Next diff builds: WebSockets §4's open task from a 101 reply, send() and incoming frames as requests and
   arrivals on the flow's register, the closing handshake, and the established arm of a declined handshake.
   Its absence shows as: every socket's `close` event carries code 1006 and no `open` event fires. */
/* WebSockets §2.2 step 11's processResponse. A network error (the host's null) or a status other than 101 fails the
   connection (step 11.1). A socket close() already failed is no longer CONNECTING and reads nothing. */
static JSValue ws_handshake_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                                    JSValueConst *func_data)
{
    WebSocketData *d = ws_of_for_write(func_data[0]);

    (void)this_val; (void)magic;
    DCHECK(d != NULL, "a WebSocket handshake's reply was delivered to something that is not a WebSocket");
    DCHECK(argc >= 1 && (JS_IsObject(argv[0]) || JS_IsNull(argv[0])),
           "a WebSocket handshake's reply is neither the host's reply record nor null for a network error");
    ws_set(ctx, func_data[0], d, &d->handshake, JS_UNDEFINED);
    if (d->ready_state != WS_CONNECTING) return JS_UNDEFINED;
    if (argc >= 1 && JS_IsObject(argv[0]) && fetch_reply_status(ctx, argv[0]) == 101)
        DFAIL("a host answered a WebSocket handshake with status 101 and the engine has no frame channel to "
              "establish the connection over; build WebSockets §4's open task and the frame requests (the "
              "named residual above)");
    ws_fail(d, func_data[0]);
    return JS_UNDEFINED;
}

/* RFC 6455 §4.1 "Client Requirements" item 10: an element of `Sec-WebSocket-Protocol` is a non-empty string
   of characters U+0021 to U+007E that are not RFC 2616 §2.2 "Basic Rules"' separators. */
static bool ws_protocol_is_token(const char *p, size_t n)
{
    size_t i;

    if (n == 0) return false;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];

        if (c < 0x21 || c > 0x7E || strchr("()<>@,;:\\\"/[]?={}", c) != NULL) return false;
    }
    return true;
}

/* Named residual: unknown arguments.
   Not covered: a URL, subprotocol, close code or reason that is unknown input with no example is validated
   as its display shape (or taken as valid, for the code), and send() counts no bytes for unknown data, so the
   SyntaxError and InvalidAccessError arms of such a call are not forked.
   Next diff builds: a fork of the throwing and succeeding arms at each validation over an unknown.
   Its absence shows as: a page's catch block around `new WebSocket(input)` that no flow enters. */
/* The bytes of a string argument that may be unknown input: an unknown denotes its example where it has a
   string one, else its display shape. Owned: JS_FreeCString. */
static const char *ws_arg_cstring(JSContext *ctx, JSValueConst v, size_t *plen)
{
    JSValue sv;
    const char *r;

    if (!concolic_is(v)) return JS_ToCStringLen(ctx, plen, v);
    sv = concolic_example(ctx, v);
    if (!JS_IsString(sv)) {
        const char *shape = concolic_shape_c(v);

        JS_FreeValue(ctx, sv);
        DCHECK(shape != NULL, "unknown input reached a WebSocket argument with no display shape");
        sv = JS_NewString(ctx, shape);
    }
    r = JS_ToCStringLen(ctx, plen, sv);
    JS_FreeValue(ctx, sv);
    return r;
}

/* WebSockets §3.1's `new WebSocket(url, protocols)` steps 8-9 over the converted `protocols`: the list as
   an Array of strings in `*out`, and the combined `Sec-WebSocket-Protocol` value (WebSockets §2.2 step 8;
   Fetch §2.2.2 "Headers"' combine joins with 0x2C 0x20) in `*combined`, owned, NULL for an empty list.
   False with a SyntaxError thrown. */
static bool ws_protocols(JSContext *ctx, int argc, JSValueConst *argv, JSValue *out, char **combined)
{
    JSValue list = JS_NewArray(ctx);
    uint32_t n = 0, i, j;
    size_t total = 0;
    bool one = argc > 1 && idl_arg_given(argc, argv, 1) && !JS_IsArray(argv[1]);

    *out = JS_UNDEFINED;
    *combined = NULL;
    if (JS_IsException(list)) return false;
    if (one) {
        n = 1;
    } else if (argc > 1 && idl_arg_given(argc, argv, 1)) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");

        if (JS_IsException(lv) || JS_ToUint32(ctx, &n, lv) < 0) { JS_FreeValue(ctx, list); return false; }
        JS_FreeValue(ctx, lv);
    }
    for (i = 0; i < n; i++) {
        JSValue pv = one ? JS_DupValue(ctx, argv[1]) : JS_GetPropertyUint32(ctx, argv[1], i);
        size_t plen = 0;
        const char *p = JS_IsException(pv) ? NULL : ws_arg_cstring(ctx, pv, &plen);
        bool ok = p != NULL && ws_protocol_is_token(p, plen);

        JS_FreeValue(ctx, pv);
        for (j = 0; ok && j < i; j++) {
            JSValue qv = JS_GetPropertyUint32(ctx, list, j);
            const char *q = JS_ToCString(ctx, qv);

            CHECK(q != NULL, "WebSocket: OOM comparing two subprotocol names");
            if (strcmp(p, q) == 0) ok = false;
            JS_FreeCString(ctx, q);
            JS_FreeValue(ctx, qv);
        }
        if (!ok) {
            if (p != NULL) {
                JS_FreeCString(ctx, p);
                JS_ThrowDOMException(ctx, "SyntaxError", "a subprotocol name is repeated or is not a token");
            }
            JS_FreeValue(ctx, list);
            free(*combined);
            *combined = NULL;
            return false;
        }
        JS_SetPropertyUint32(ctx, list, i, JS_NewStringLen(ctx, p, plen));
        *combined = lexbor_realloc(*combined, total + plen + 3);
        CHECK(*combined != NULL, "WebSocket: OOM combining the subprotocol names");
        if (total > 0) { memcpy(*combined + total, ", ", 2); total += 2; }
        memcpy(*combined + total, p, plen);
        total += plen;
        (*combined)[total] = '\0';
        JS_FreeCString(ctx, p);
    }
    *out = list;
    return true;
}

/* WebSockets §2.2 "Opening handshake" steps 1, 2, 8 and 11 for the socket `sock`, whose parsed url is `rec`
   (scheme "ws" or "wss"): the handshake request, recorded on the endpoint surface at the socket's own
   address and parked on the running flow. `address` says whether the URL is bytes; `src` is the page's
   argument, the unknown the address stands for where it is one. */
static void ws_establish(JSContext *ctx, JSValueConst sock, WebSocketData *d, UrlRecord *rec, const char *protocols,
                         FetchAddressClass address, JSValueConst src)
{
    CspRequestMetadata meta = csp_request_metadata_unstated();
    HeaderList hl = { 0 };
    EndpointHeader eh = { "Sec-WebSocket-Protocol", protocols, NULL };
    FetchRequest req = { 0 };
    JSValueConst data[1];
    JSValue deliver;
    char *request_url;
    bool secure = strcmp(rec->scheme, "wss") == 0;

    DCHECK(secure || strcmp(rec->scheme, "ws") == 0,
           "a WebSocket connection was asked for over a scheme WebSockets §3.1's `new WebSocket(url, "
           "protocols)` step 6 refuses");
    /* The endpoint is the socket's own address; the request below is to its http(s) form. */
    endpoint_record(ctx, "GET", concolic_is(src) ? src : d->url, protocols ? &eh : NULL, protocols ? 1 : 0, NULL,
                    engine_prov_of_running_path(), engine_driven_of_running_path(), EPD_WEBSOCKET,
                    FETCH_CREDENTIALS_INCLUDE);
    /* Step 1: "Let requestURL be a copy of url, with its scheme set to "http", if url's scheme is "ws";
       otherwise to "https"." The record is the constructor's own and is not read again. */
    free(rec->scheme);
    rec->scheme = reclaim_strdup(secure ? "https" : "http");
    CHECK(rec->scheme != NULL, "WebSocket: OOM naming the handshake request's scheme");
    request_url = url_serialize(rec, /*exclude_fragment*/ false);
    CHECK(request_url != NULL, "WebSocket: OOM serializing the handshake request's URL");
    if (protocols) header_list_append(&hl, "Sec-WebSocket-Protocol", protocols);
    data[0] = sock;
    deliver = JS_NewCFunctionData(ctx, ws_handshake_deliver, 1, 0, 1, data);
    CHECK(!JS_IsException(deliver), "WebSocket: OOM allocating the handshake's processResponse steps");
    /* Fetch §4.1 "Main fetch" step 4; the park runs steps 6 and 7. */
    fetch_main_report_csp(ctx, request_url, "", meta);
    /* Step 2: the request. Its service-workers mode, cache mode and redirect mode are the zone's to honor:
       it follows no redirect of a handshake and stores none. */
    req.method = "GET";
    req.url = request_url;
    req.destination = "";
    req.headers = hl.n > 0 ? &hl : NULL;
    req.metadata = meta;
    req.credentials = FETCH_CREDENTIALS_INCLUDE;
    req.mode = FETCH_MODE_WEBSOCKET;
    req.referrer = "no-referrer";
    req.referrer_policy = "";
    req.address = address;
    req.address_value = address == FETCH_ADDRESS_UNKNOWN ? src : JS_UNDEFINED;
    req.address_url = JS_UNDEFINED;
    ws_set(ctx, sock, d, &d->handshake, JS_DupValue(ctx, deliver));
    engine_pending_resource_url(ctx, deliver, &req);
    JS_FreeValue(ctx, deliver);
    header_list_free(&hl);
    free(request_url);
}

/* ---- WebSockets §3.1 "Interface definition" ------------------------------------------------------------ */

/* `new WebSocket(url, protocols)`. `new_target` supplies the prototype, so a subclass gets its own. */
static JSValue js_ws_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    FetchAddressClass address;
    UrlRecord rec;
    WebSocketData *d;
    JSValue proto, obj, list, protocols;
    const char *u;
    char *combined = NULL, *ser;
    size_t ulen = 0;
    bool ok;

    (void)magic;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor WebSocket requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WebSocket requires a url");
    /* Steps 1-3: the URL parser over `url` with the API base URL. An unknown with a string example is
       parsed as its example, which is the address a session with that input connects to. */
    address = concolic_is(argv[0]) ? FETCH_ADDRESS_UNKNOWN : FETCH_ADDRESS_CONCRETE;
    if (address == FETCH_ADDRESS_UNKNOWN) {
        JSValue ex = concolic_example(ctx, argv[0]);

        if (JS_IsString(ex)) address = FETCH_ADDRESS_EXAMPLE;
        JS_FreeValue(ctx, ex);
    }
    u = ws_arg_cstring(ctx, argv[0], &ulen);
    if (!u) return JS_EXCEPTION;
    url_record_init(&rec);
    ok = fetch_parse_url(ctx, &rec, u, ulen);
    JS_FreeCString(ctx, u);
    if (!ok)
        return JS_ThrowDOMException(ctx, "SyntaxError", "the WebSocket URL cannot be parsed");
    /* Steps 4-5. */
    if (strcmp(rec.scheme, "http") == 0 || strcmp(rec.scheme, "https") == 0) {
        bool secure = strcmp(rec.scheme, "https") == 0;

        free(rec.scheme);
        rec.scheme = reclaim_strdup(secure ? "wss" : "ws");
        CHECK(rec.scheme != NULL, "WebSocket: OOM naming a socket URL's scheme");
    }
    /* Steps 6-7. */
    if (strcmp(rec.scheme, "ws") != 0 && strcmp(rec.scheme, "wss") != 0) {
        url_record_free(&rec);
        return JS_ThrowDOMException(ctx, "SyntaxError", "the WebSocket URL's scheme is not ws, wss, http or https");
    }
    if (rec.fragment != NULL) {
        url_record_free(&rec);
        return JS_ThrowDOMException(ctx, "SyntaxError", "the WebSocket URL has a fragment");
    }
    /* Steps 8-9. */
    if (!ws_protocols(ctx, argc, argv, &protocols, &combined)) {
        url_record_free(&rec);
        return JS_EXCEPTION;
    }
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsException(proto) && !JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        proto = JS_GetClassProto(ctx, g_ws_class);
        DCHECK(!JS_IsNull(proto), "a WebSocket was constructed in a realm that never ran its install");
    }
    obj = JS_IsException(proto) ? JS_EXCEPTION : JS_NewObjectProtoClass(ctx, proto, g_ws_class);
    JS_FreeValue(ctx, proto);
    ser = url_serialize(&rec, /*exclude_fragment*/ false);
    CHECK(ser != NULL, "WebSocket: OOM serializing a socket's URL");
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, protocols);
        free(combined); free(ser);
        url_record_free(&rec);
        return JS_EXCEPTION;
    }
    d = lexbor_calloc(1, sizeof *d);
    CHECK(d != NULL, "WebSocket: OOM building a WebSocket");
    d->realm = ctx;
    d->url = JS_NewString(ctx, ser);   /* step 10 */
    d->protocols = protocols;
    d->protocol = JS_NewString(ctx, "");
    d->extensions = JS_NewString(ctx, "");
    d->handshake = JS_UNDEFINED;
    d->ready_state = WS_CONNECTING;
    free(ser);
    CHECK(!JS_IsException(d->url) && !JS_IsException(d->protocol) && !JS_IsException(d->extensions),
          "WebSocket: OOM building a WebSocket's strings");
    JS_SetOpaque(obj, d);
    list = ws_list(ctx);
    JS_SetPropertyUint32(ctx, list, ws_list_len(ctx, list), JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, list);
    /* Steps 11-12. */
    ws_establish(ctx, obj, ws_of_for_write(obj), &rec, combined, address, argv[0]);
    free(combined);
    url_record_free(&rec);
    return obj;
}

enum { WS_GET_URL = 0, WS_GET_READY_STATE, WS_GET_BUFFERED_AMOUNT, WS_GET_EXTENSIONS, WS_GET_PROTOCOL,
       WS_GET_BINARY_TYPE };

static JSValue js_ws_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const WebSocketData *d = ws_of(this_val);

    if (!d) return JS_ThrowTypeError(ctx, "not a WebSocket");
    switch (magic) {
    case WS_GET_URL:             return JS_DupValue(ctx, d->url);
    case WS_GET_READY_STATE:     return JS_NewInt32(ctx, d->ready_state);
    case WS_GET_BUFFERED_AMOUNT: return JS_NewFloat64(ctx, d->buffered);
    case WS_GET_EXTENSIONS:      return JS_DupValue(ctx, d->extensions);
    case WS_GET_PROTOCOL:        return JS_DupValue(ctx, d->protocol);
    case WS_GET_BINARY_TYPE:     return JS_NewString(ctx, WS_BINARY_TYPE[d->binary_type]);
    }
    DFAIL("a WebSocket getter was installed with a magic WebSockets §3.1 has no attribute for");
    return JS_UNDEFINED;
}

/* The binaryType setter steps. The declaration converted `val` to one of `enum BinaryType`'s values; Web
   IDL §3.7.6 "Attributes" returns before the steps for any other string. */
static JSValue js_ws_set_binary_type(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    WebSocketData *d = ws_of_for_write(this_val);
    const char *s;
    int i, hit = -1;

    (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a WebSocket");
    s = JS_ToCString(ctx, val);
    if (!s) return JS_EXCEPTION;
    for (i = 0; WS_BINARY_TYPE[i] != NULL; i++)
        if (strcmp(s, WS_BINARY_TYPE[i]) == 0) hit = i;
    JS_FreeCString(ctx, s);
    DCHECK(hit >= 0, "the binaryType setter's steps ran for a value that is not one of enum BinaryType's");
    if (hit >= 0) d->binary_type = (uint8_t)hit;
    return JS_UNDEFINED;
}

/* `close(code, reason)`. */
static JSValue js_ws_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    WebSocketData *d = ws_of_for_write(this_val);

    (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a WebSocket");
    /* Step 1. */
    if (idl_arg_given(argc, argv, 0) && !concolic_is(argv[0])) {
        int32_t code = 0;

        if (JS_ToInt32(ctx, &code, argv[0]) < 0) return JS_EXCEPTION;
        if (code != 1000 && (code < 3000 || code > 4999))
            return JS_ThrowDOMException(ctx, "InvalidAccessError",
                                        "the close code is neither 1000 nor in the range 3000 to 4999");
    }
    /* Step 2: the reason's UTF-8 encoding is at most 123 bytes. */
    if (idl_arg_given(argc, argv, 1)) {
        size_t n = 0;
        const char *reason = ws_arg_cstring(ctx, argv[1], &n);

        if (!reason) return JS_EXCEPTION;
        JS_FreeCString(ctx, reason);
        if (n > 123)
            return JS_ThrowDOMException(ctx, "SyntaxError", "the close reason is longer than 123 bytes");
    }
    /* Step 3. */
    if (d->ready_state == WS_CLOSING || d->ready_state == WS_CLOSED) return JS_UNDEFINED;
    DCHECK(d->ready_state == WS_CONNECTING,
           "close() ran on an OPEN socket; no connection is established while the frame channel is unbuilt");
    /* "If the WebSocket connection is not yet established: Fail the WebSocket connection and set this's
       ready state to CLOSING (2)." The handshake still parked is withdrawn from the host. */
    if (JS_IsFunction(ctx, d->handshake)) {
        engine_pending_resource_cancel(ctx, d->handshake);
        ws_set(ctx, this_val, d, &d->handshake, JS_UNDEFINED);
    }
    ws_fail(d, this_val);
    d->ready_state = WS_CLOSING;
    return JS_UNDEFINED;
}

/* The bytes send() adds to bufferedAmount for `data`: a string's UTF-8 length, a Blob's size, an
   ArrayBuffer's or a view's byte length. -1 with the exception thrown. */
static double ws_data_bytes(JSContext *ctx, JSValueConst data)
{
    size_t n = 0, off = 0;

    if (concolic_is(data)) return 0;
    if (JS_IsString(data)) {
        const char *s = JS_ToCStringLen(ctx, &n, data);

        if (!s) return -1;
        JS_FreeCString(ctx, s);
        return (double)n;
    }
    if (blob_is(data)) {
        const char *type = NULL;

        (void)blob_bytes_of(data, &n, &type);
        return (double)n;
    }
    if (JS_IsArrayBuffer(data)) {
        if (JS_GetArrayBuffer(ctx, &n, data) == NULL) return -1;
        return (double)n;
    }
    {
        JSValue buf = JS_GetArrayBufferView(ctx, data, &off, &n);

        if (JS_IsException(buf)) return -1;
        JS_FreeValue(ctx, buf);
        return (double)n;
    }
}

/* `send(data)`. */
static JSValue js_ws_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    WebSocketData *d = ws_of_for_write(this_val);
    double n;

    (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a WebSocket");
    if (argc < 1) return JS_ThrowTypeError(ctx, "send requires data");
    /* Step 1. */
    if (d->ready_state == WS_CONNECTING)
        return JS_ThrowDOMException(ctx, "InvalidStateError", "send() on a WebSocket that is still CONNECTING");
    DCHECK(d->ready_state != WS_OPEN,
           "send() ran on an OPEN socket; no connection is established while the frame channel is unbuilt");
    /* Step 2, for a connection that is closing or closed: nothing is transmitted, and "any invocation of
       this method ... that does not throw an exception must increase the bufferedAmount attribute". */
    n = ws_data_bytes(ctx, argv[0]);
    if (n < 0) return JS_EXCEPTION;
    d->buffered += n;
    return JS_UNDEFINED;
}

void web_socket_make_disappear_all(JSContext *ctx)
{
    JSValue list = ws_list(ctx);
    uint32_t n = ws_list_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue sock = JS_GetPropertyUint32(ctx, list, i);
        const WebSocketData *d = ws_of(sock);

        DCHECK(d != NULL, "a realm's socket list holds something that is not a WebSocket");
        if (d != NULL && JS_IsFunction(ctx, d->handshake)) {
            WebSocketData *w = ws_of_for_write(sock);

            engine_pending_resource_cancel(ctx, w->handshake);
            ws_set(ctx, sock, w, &w->handshake, JS_UNDEFINED);
            w->failed = 1;
        }
        JS_FreeValue(ctx, sock);
    }
    JS_FreeValue(ctx, list);
}

/* ---- install -------------------------------------------------------------------------------------------------- */

/* WebSockets §3.1's four constants, on the interface object and on its prototype (Web IDL §3.7.5 "Constants"). */
static const JSCFunctionListEntry WS_CONSTANTS[] = {
    JS_PROP_INT32_DEF("CONNECTING", WS_CONNECTING, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("OPEN", WS_OPEN, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("CLOSING", WS_CLOSING, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("CLOSED", WS_CLOSED, JS_PROP_ENUMERABLE),
};

void web_socket_init(JSContext *ctx)
{
    JSClassDef cd = { "WebSocket", .finalizer = ws_finalizer, .gc_mark = ws_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CTOR_ARGS[2] = { IDL_USVSTRING, IDL_DOMSTRING_OR_SEQUENCE };
    static const IdlArgType CLOSE_ARGS[2] = { IDL_UNSIGNED_SHORT_CLAMP, IDL_USVSTRING };
    static const IdlArgType SEND_ARGS[1] = { IDL_BUFFERSOURCE_OR_BLOB_OR_USVSTRING };

    DCHECK(g_ws_rt == NULL, "web_socket_init ran twice; the interface is declared once per agent");
    g_ws_rt = rt;
    JS_NewClassID(rt, &g_ws_class);
    JS_NewClass(rt, g_ws_class, &cd);
    g_list_slot = realm_value_declare(ctx, "WebSockets §7 a realm's WebSocket objects that are not closed");
    g_task_stepid = JS_RegisterStepDef(rt, &js_ws_task_def);
    DCHECK(g_task_stepid >= 0, "WebSockets §4's task machine could not be declared against this runtime");
    g_task_fn = JS_NewCFunction2(ctx, NULL, "", 2, JS_CFUNC_step, g_task_stepid);
    CHECK(JS_IsFunction(ctx, g_task_fn), "WebSockets §4's task callee could not be allocated");
    g_ctor_id = idl_method_id(ctx, CTOR_ARGS, 2, js_ws_ctor, 0);
    idl_optional_from(1);    /* `optional (DOMString or sequence<DOMString>) protocols = []` */
    g_id_close = idl_method_id(ctx, CLOSE_ARGS, 2, js_ws_close, 0);
    idl_optional_from(0);    /* `optional [Clamp] unsigned short code, optional USVString reason` */
    g_id_send = idl_method_id(ctx, SEND_ARGS, 1, js_ws_send, 0);
    g_set_binary_type_id = idl_setter_id(ctx, IDL_ENUM, false, js_ws_set_binary_type, 0);
    idl_arg_enum(0, WS_BINARY_TYPE);
    agent_state_ptr("web_socket", &g_ws_rt, "the runtime WebSocket was declared in, and the latch");
    agent_state_class("web_socket", &g_ws_class, "WebSockets §3.1's WebSocket class, which carries the record");
    agent_state_realm_slot("web_socket", &g_list_slot, "the per-realm slot a realm's open sockets live in");
    agent_state_value("web_socket", &g_task_fn, "WebSockets §4's task callee, one per agent");
    agent_state_id("web_socket", &g_task_stepid, "WebSockets §4's task machine");
    agent_state_id("web_socket", &g_ctor_id, "WebSockets §3.1's `constructor(url, protocols)` pool entry");
    agent_state_id("web_socket", &g_id_close, "WebSockets §3.1's `close(code, reason)` pool entry");
    agent_state_id("web_socket", &g_id_send, "WebSockets §3.1's `send(data)` pool entry");
    agent_state_id("web_socket", &g_set_binary_type_id, "WebSockets §3.1's binaryType setter");
    realm_declare_intrinsic(web_socket_install_realm);
}

void web_socket_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, ctor, list;

    DCHECK(g_ws_class != 0 && g_ctor_id >= 0,
           "a realm asked for the WebSocket interface before web_socket_init declared it");
    prev = JS_GetClassProto(ctx, g_ws_class);
    DCHECK(JS_IsNull(prev), "web_socket_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* Built with the realm, so a flow's sockets are captured by that flow's delta. */
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a realm's socket list could not be allocated");
    realm_value_set(ctx, g_list_slot, list);

    proto = event_target_derived_proto(ctx);   /* `interface WebSocket : EventTarget` */
    idl_interface_tag(ctx, proto, "WebSocket");
    event_target_install_handlers(ctx, proto, EH_WEB_SOCKET);
    JS_SetPropertyFunctionList(ctx, proto, WS_CONSTANTS, (int)(sizeof(WS_CONSTANTS) / sizeof(WS_CONSTANTS[0])));
    idl_install_accessor(ctx, proto, "url", js_ws_get, WS_GET_URL, -1);
    idl_install_accessor(ctx, proto, "readyState", js_ws_get, WS_GET_READY_STATE, -1);
    idl_install_accessor(ctx, proto, "bufferedAmount", js_ws_get, WS_GET_BUFFERED_AMOUNT, -1);
    idl_install_accessor(ctx, proto, "extensions", js_ws_get, WS_GET_EXTENSIONS, -1);
    idl_install_accessor(ctx, proto, "protocol", js_ws_get, WS_GET_PROTOCOL, -1);
    idl_install_accessor(ctx, proto, "binaryType", js_ws_get, WS_GET_BINARY_TYPE, g_set_binary_type_id);
    idl_install_method(ctx, proto, "close", g_id_close);
    idl_install_method(ctx, proto, "send", g_id_send);

    ctor = idl_step_constructor(ctx, "WebSocket", g_ctor_id);
    CHECK(!JS_IsException(ctor), "the WebSocket interface object could not be allocated");
    JS_SetPropertyFunctionList(ctx, ctor, WS_CONSTANTS, (int)(sizeof(WS_CONSTANTS) / sizeof(WS_CONSTANTS[0])));
    JS_SetConstructor(ctx, ctor, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "WebSocket", ctor);
    JS_FreeValue(ctx, global);
    JS_SetClassProto(ctx, g_ws_class, proto);   /* the realm owns it from here */
}

void web_socket_free(JSRuntime *rt)
{
    DCHECK(g_ws_rt != NULL, "WebSocket was released in an agent that never declared it");
    DCHECK(rt == g_ws_rt, "WebSocket was released against a runtime that is not the one it was declared in");
    JS_FreeValueRT(rt, g_task_fn);   /* the prototypes are the realms', released with their contexts */
    agent_state_undo("web_socket");
}
