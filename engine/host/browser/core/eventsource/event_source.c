/* HTML §9.2.2 "The EventSource interface", HTML §9.2.3 "Processing model" and HTML §9.2.9 "Garbage collection".
 *
 * The connection is a fetch parked on the running flow like every other request, so the trusted zone judges it
 * under the one egress rule. Its processResponse runs at the response's head (the zone streams a request whose
 * destination is the empty string), and the body's pieces arrive through solver/reply_body.h. Every state change
 * a page observes is a task on the remote event task source, run by one machine (es_task_def).
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
#include "core/dom/document.h"
#include "core/eventsource/event_source.h"
#include "core/eventsource/event_source_parser.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/message_event.h"
#include "core/fetch/fetch.h"
#include "core/fetch/headers.h"
#include "core/html/cors_settings_attribute.h"
#include "core/idl_args.h"
#include "core/mime/mime_type.h"
#include "core/realm.h"
#include "core/timing/task_source.h"
#include "core/timing/timer.h"
#include "core/url/url.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/reply_body.h"

/* §9.2.2's ready state values, which are also the interface's three constants. */
enum { ES_CONNECTING = 0, ES_OPEN, ES_CLOSED };
/* §9.2.2's reconnection time starts at "an implementation-defined value, probably in the region of a few
   seconds": Blink's EventSource::kDefaultReconnectDelay. */
#define ES_DEFAULT_RECONNECTION_MS 3000.0

typedef struct {
    JSValue    url;            /* §9.2.2's url, serialized; written once by the constructor */
    JSValue    src;            /* the constructor's url argument where it is unknown input, else undefined */
    JSValue    last_event_id;  /* §9.2.2's last event ID string */
    JSValue    deliver;        /* the parked processResponse; undefined once answered or withdrawn */
    JSValue    body;           /* the response body's bytes received so far (an ArrayBuffer), or undefined */
    JSValue    origin;         /* the serialized origin of the response's final URL, for §9.2.6 step 5 */
    JSContext *realm;          /* the relevant realm; borrowed, the agent owns its realms */
    double     reconnection_ms;
    uint8_t    ready_state;
    uint8_t    with_credentials;
    uint8_t    address;        /* the FetchAddressClass of `url` */
} EventSourceData;

static JSClassID  g_es_class;
static JSClassID  g_set_slot = JS_INVALID_CLASS_ID;   /* the per-realm Array of objects that are not CLOSED */
static JSRuntime *g_es_rt;
static JSValue    g_task_fn = JS_UNDEFINED;            /* the remote event task callee, one per agent */
static int        g_task_stepid = -1, g_response_stepid = -1, g_body_stepid = -1, g_wait_stepid = -1;
static int        g_ctor_id = -1, g_id_close = -1;

#define ESO(f) (uint16_t)offsetof(EventSourceData, f)
static const uint16_t ES_VALS[] = { ESO(url), ESO(src), ESO(last_event_id), ESO(deliver), ESO(body), ESO(origin) };
static const CowRecord ES_REC = { sizeof(EventSourceData), ES_VALS, (int)(sizeof(ES_VALS) / sizeof(ES_VALS[0])) };

/* The record, captured for the running flow before it can be written: an arm that closes the object must not
   close it for its sibling. */
static EventSourceData *es_of(JSValueConst v)
{
    EventSourceData *d = JS_GetOpaque(v, g_es_class);

    if (d) cow_capture_host_record(v, d, &ES_REC);
    return d;
}

#define es_set(ctx_, d_, slot_, v_) cow_record_set_at((ctx_), (d_), &ES_REC, (slot_), (v_), __FILE__, __LINE__)

static void es_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    EventSourceData *d = JS_GetAnyOpaque(val, &id);
    size_t i;

    (void)id;
    if (!d) return;
    for (i = 0; i < sizeof(ES_VALS) / sizeof(ES_VALS[0]); i++)
        JS_FreeValueRT(rt, *(JSValue *)((char *)d + ES_VALS[i]));
    free(d);
}

static void es_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    EventSourceData *d = JS_GetAnyOpaque(val, &id);
    size_t i;

    (void)id;
    if (!d) return;
    for (i = 0; i < sizeof(ES_VALS) / sizeof(ES_VALS[0]); i++)
        JS_MarkValue(rt, *(JSValue *)((char *)d + ES_VALS[i]), mark_func);
}

/* ---- §9.2.9: the realm's objects that are not CLOSED --------------------------------------------------- */

/* §9.2.9 keeps an object alive while it is CONNECTING or OPEN with listeners, or has a task queued; the set
   holds every object until it is CLOSED, which covers all three. §7.5.9's forcible close walks it. */
static JSValue es_set_of(JSContext *ctx)
{
    JSValue set;

    DCHECK(g_es_rt != NULL, "a realm's EventSource objects were reached before event_source_init declared them");
    set = realm_value_get(ctx, g_set_slot);
    DCHECK(JS_IsArray(set), "a realm has no EventSource set; event_source_install_realm is on realm.h's one list, "
                            "so this realm skipped it");
    return set;
}

static uint32_t es_set_len(JSContext *ctx, JSValueConst set)
{
    JSValue lv = JS_GetPropertyStr(ctx, set, "length");
    uint32_t n = 0;

    CHECK(!JS_IsException(lv) && JS_ToUint32(ctx, &n, lv) == 0, "a realm's EventSource set has no readable length");
    JS_FreeValue(ctx, lv);
    return n;
}

static void es_set_remove(JSContext *rctx, JSValueConst ev)
{
    JSValue set = es_set_of(rctx);
    uint32_t n = es_set_len(rctx, set), i;

    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(rctx, set, i);
        bool hit = JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(ev);

        JS_FreeValue(rctx, v);
        if (!hit) continue;
        if (i + 1 < n) JS_SetPropertyUint32(rctx, set, i, JS_GetPropertyUint32(rctx, set, n - 1));
        JS_SetPropertyStr(rctx, set, "length", JS_NewUint32(rctx, n - 1));
        break;
    }
    JS_FreeValue(rctx, set);
}

/* §9.2.2's close() and §9.2.9's forcibly close: abort the fetch started for the object (withdraw the parked
   processResponse; a body delivery reads no further piece of a CLOSED object) and set readyState to CLOSED. */
static void es_close(JSContext *ctx, EventSourceData *d, JSValueConst ev)
{
    if (JS_IsFunction(ctx, d->deliver)) {
        engine_pending_resource_cancel(ctx, d->deliver);
        es_set(ctx, d, &d->deliver, JS_UNDEFINED);
    }
    if (d->ready_state != ES_CLOSED) es_set_remove(d->realm, ev);
    d->ready_state = ES_CLOSED;
}

/* ---- §9.2.3: the remote event task source -------------------------------------------------------------- */

/* Which task a queued call is; its second argument. */
enum {
    ES_TASK_ANNOUNCE = 0,   /* §9.2.3 announce the connection */
    ES_TASK_FAIL,           /* §9.2.3 fail the connection */
    ES_TASK_LOST,           /* §9.2.3 reestablish the connection, step 1's task */
    ES_TASK_RECONNECT,      /* §9.2.3 reestablish the connection, step 5's task */
    ES_TASK_MESSAGE,        /* §9.2.6 dispatch the event, step 8's task; argument 2 is the event */
};

#define ES_TASK_STAGES(X) \
    X(ET_STATE, "HTML §9.2.3 the task's readyState test and change") \
    X(ET_FIRE,  "HTML §9.2.3 the task's fire of an event at the EventSource object")
enum { ES_TASK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ES_TASK_STEPS[] = { ES_TASK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;
    uint8_t     fphase;   /* the fire request's own phase */
    JSValue     ev;       /* the event being fired, held across the suspension (owned) */
    EventFireCb cb;
} EsTask;

static void es_fetch(JSContext *ctx, JSValueConst obj, EventSourceData *d);

static void es_queue(JSContext *rctx, JSValueConst obj, int kind, JSValueConst event)
{
    JSValue args[3];
    int i, n = JS_IsUndefined(event) ? 2 : 3;

    args[0] = JS_DupValue(rctx, obj);
    args[1] = JS_NewInt32(rctx, kind);
    args[2] = JS_DupValue(rctx, event);
    JS_EnqueueCallTask(rctx, g_task_fn, n, (JSValueConst *)args, TASK_SOURCE_REMOTE_EVENT);
    for (i = 0; i < 3; i++) JS_FreeValue(rctx, args[i]);
}

static int js_es_task_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    EsTask *s = st;
    JSValueConst obj = step_arg(&s->hdr, 0);
    EventSourceData *d = es_of(obj);
    JSContext *rctx;
    int r;

    STEP_DISPATCH(ES_TASK_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(ET_STATE);
    {
        int32_t kind = -1;
        int k;
        const char *type = NULL;

        JS_FreeValue(ctx, cb_result);
        /* The state is complete before anything below can throw. */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->fphase = 0;
        DCHECK(d != NULL, "an EventSource task was queued for something that is not an EventSource");
        rctx = d->realm;
        CHECK(JS_ToInt32(ctx, &kind, step_arg(&s->hdr, 1)) == 0, "EventSource: a task's kind could not be read");
        switch (kind) {
        case ES_TASK_ANNOUNCE:
            /* "if the readyState attribute is set to a value other than CLOSED, sets the readyState attribute to
               OPEN and fires an event named open". */
            if (d->ready_state == ES_CLOSED) return JS_STEP_DONE;
            d->ready_state = ES_OPEN;
            type = "open";
            break;
        case ES_TASK_FAIL:
            if (d->ready_state == ES_CLOSED) return JS_STEP_DONE;
            es_close(ctx, d, obj);
            type = "error";
            break;
        case ES_TASK_LOST:
            /* Steps 1.1-1.3: "If the readyState attribute is set to CLOSED, abort the task. Set the readyState
               attribute to CONNECTING. Fire an event named error". */
            if (d->ready_state == ES_CLOSED) return JS_STEP_DONE;
            d->ready_state = ES_CONNECTING;
            type = "error";
            break;
        case ES_TASK_RECONNECT:
            /* Steps 5.1-5.4: "If the EventSource object's readyState attribute is not set to CONNECTING, then
               return", then the Last-Event-ID header and the fetch. */
            if (d->ready_state == ES_CONNECTING) es_fetch(ctx, obj, d);
            return JS_STEP_DONE;
        case ES_TASK_MESSAGE:
            /* §9.2.6 step 8: "Queue a task which, if the readyState attribute is set to a value other than CLOSED,
               dispatches the newly created event". */
            DCHECK(s->hdr.argc >= 3 && JS_IsObject(step_arg(&s->hdr, 2)),
                   "a message task was queued with no event; §9.2.6 creates it before queueing the task");
            if (d->ready_state == ES_CLOSED) return JS_STEP_DONE;
            s->ev = JS_DupValue(ctx, step_arg(&s->hdr, 2));
            STEP_GOTO(s->hdr.stage, ET_FIRE, &s->fphase, NULL);
            return JS_STEP_YIELD;
        default:
            DFAIL("an EventSource task was queued with a kind §9.2.3 does not have");
            return JS_STEP_DONE;
        }
        s->ev = event_new(rctx, type, /*bubbles*/ false, /*cancelable*/ false);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(s->hdr.stage, ET_FIRE, &s->fphase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(ET_FIRE);
    DCHECK(d != NULL && JS_IsObject(s->ev), "an EventSource task resumed at its fire with no object or event");
    rctx = d->realm;
    r = event_target_fire_run(rctx, &s->fphase, STEP_CB(s->cb), obj, s->ev, JS_UNDEFINED, cb_result, NULL,
                              out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(rctx, s->ev);
    s->ev = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static void js_es_task_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EsTask *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static const JSTrampStepDef es_task_def = {
    sizeof(EsTask), js_es_task_step, NULL, 0,
    .visit = js_es_task_visit,
    .algorithm = "HTML §9.2.3 a task on the remote event task source",
    .steps = ES_TASK_STEPS
};

/* §9.2.3 reestablish the connection. Step 1 queues its task now; steps 2-4 wait the reconnection time on the
   timer source, whose steps queue step 5's task behind step 1's on the same source, which is step 4's wait. */
static void es_reestablish(JSContext *ctx, JSValueConst obj, EventSourceData *d)
{
    JSValue steps;

    es_queue(d->realm, obj, ES_TASK_LOST, JS_UNDEFINED);
    steps = JS_NewStepClosure(ctx, g_wait_stepid, 0, 1, &obj);
    CHECK(!JS_IsException(steps), "EventSource: OOM allocating the reconnection wait's completion steps");
    timer_after(d->realm, d->reconnection_ms, steps);
    JS_FreeValue(ctx, steps);
}

#define ES_ONE_STAGE(X) X(ES_ONLY, "the algorithm's one step")
enum { ES_ONE_STAGE(JS_STEP_STAGE_ENUM) };

typedef struct { JSStepHdr hdr; } EsPlain;

static void js_es_plain_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int js_es_wait_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    EsPlain *s = st;
    JSValueConst obj = JS_StepClosureData(&s->hdr, 0);
    EventSourceData *d = es_of(obj);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == ES_ONLY, "the reconnection wait resumed into a stage it does not have");
    DCHECK(d != NULL, "a reconnection wait was set for something that is not an EventSource");
    es_queue(d->realm, obj, ES_TASK_RECONNECT, JS_UNDEFINED);
    return JS_STEP_DONE;
}

static const char *const ES_WAIT_STEPS[] = { "HTML §9.2.3 reestablish the connection step 5 (queue a task)", NULL };
static const JSTrampStepDef es_wait_def = {
    sizeof(EsPlain), js_es_wait_step, NULL, 0,
    .visit = js_es_plain_visit,
    .algorithm = "HTML §9.2.3 reestablish the connection steps 2-5",
    .steps = ES_WAIT_STEPS
};

/* ---- §9.2.5-§9.2.6: interpreting the body --------------------------------------------------------------- */

typedef struct { JSContext *ctx; JSValueConst obj; EventSourceData *d; } EsSinkUser;

static void es_sink_last_event_id(void *user, const char *id, size_t id_n)
{
    EsSinkUser *u = user;
    JSValue v = JS_NewStringLen(u->ctx, id, id_n);

    CHECK(!JS_IsException(v), "EventSource: OOM keeping a last event ID string");
    es_set(u->ctx, u->d, &u->d->last_event_id, v);
}

/* §9.2.6 dispatch the event steps 4-5 (the event, in the object's relevant realm) and step 8's task. */
static void es_sink_dispatch(void *user, const char *type, size_t type_n, const char *data, size_t data_n)
{
    EsSinkUser *u = user;
    JSContext *rctx = u->d->realm;
    char *t = lexbor_malloc(type_n + 1);
    JSValue dv, ev;

    CHECK(t != NULL, "EventSource: OOM naming a message event's type");
    memcpy(t, type, type_n);
    t[type_n] = '\0';
    dv = JS_NewStringLen(rctx, data, data_n);
    CHECK(!JS_IsException(dv), "EventSource: OOM keeping a message event's data");
    ev = message_event_new(rctx, t, dv, u->d->origin, u->d->last_event_id, JS_NULL, JS_UNDEFINED);
    lexbor_free(t);
    JS_FreeValue(rctx, dv);
    CHECK(!JS_IsException(ev), "EventSource: OOM creating a message event");
    es_queue(rctx, u->obj, ES_TASK_MESSAGE, ev);
    JS_FreeValue(rctx, ev);
}

static void es_sink_reconnection_time(void *user, uint64_t millis)
{
    EsSinkUser *u = user;

    u->d->reconnection_ms = (double)millis;
}

static const EventSourceParserSink ES_SINK = {
    es_sink_last_event_id, es_sink_dispatch, es_sink_reconnection_time
};

/* Named residual. Not covered: the body is interpreted once its end has arrived, not line by line as it arrives,
   because event_source_parser_interpret takes a whole body. Next diff builds: an incremental parser whose
   buffers ride the object, fed each piece. Absence shows as: an `open` event from a stream the server holds open
   with no `message` event after it. */
static void es_interpret_and_end(JSContext *ctx, JSValueConst obj, EventSourceData *d)
{
    EsSinkUser u = { ctx, obj, d };
    size_t n = 0;
    const uint8_t *b = JS_IsUndefined(d->body) ? NULL : JS_GetArrayBuffer(ctx, &n, d->body);

    event_source_parser_interpret((const char *)b, n, &ES_SINK, &u);
    es_set(ctx, d, &d->body, JS_UNDEFINED);
    /* processEventSourceEndOfBody: "if res is not a network error, then reestablish the connection". */
    if (d->ready_state != ES_CLOSED) es_reestablish(ctx, obj, d);
}

static void es_body_append(JSContext *ctx, EventSourceData *d, const uint8_t *b, size_t n)
{
    size_t have = 0;
    const uint8_t *old = JS_IsUndefined(d->body) ? NULL : JS_GetArrayBuffer(ctx, &have, d->body);
    uint8_t *joined = lexbor_malloc(have + n + 1);
    JSValue ab;

    CHECK(joined != NULL, "EventSource: OOM growing a response body");
    if (have) memcpy(joined, old, have);
    if (n) memcpy(joined + have, b, n);
    ab = JS_NewArrayBufferCopy(ctx, joined, have + n);
    lexbor_free(joined);
    CHECK(!JS_IsException(ab), "EventSource: OOM keeping a response body");
    es_set(ctx, d, &d->body, ab);
}

/* One piece of a streamed body (solver/reply_body.h), or JS_NULL for a decline's network failure. */
static int js_es_body_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    EsPlain *s = st;
    JSValueConst obj = JS_StepClosureData(&s->hdr, 0);
    JSValueConst piece = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
    EventSourceData *d = es_of(obj);
    ReplyBodyOp op;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == ES_ONLY, "a body piece's delivery resumed into a stage it does not have");
    DCHECK(d != NULL, "a body piece was delivered to something that is not an EventSource");
    if (d->ready_state == ES_CLOSED) return JS_STEP_DONE;
    op = JS_IsNull(piece) ? REPLY_BODY_ERROR : reply_body_piece_op(ctx, piece);
    if (op == REPLY_BODY_ENQUEUE) {
        JSValue ab = JS_GetPropertyStr(ctx, piece, "bytes");
        size_t n = 0;
        const uint8_t *b = fetch_body_bytes(ctx, ab, &n);

        es_body_append(ctx, d, b, n);
        JS_FreeValue(ctx, ab);
        return JS_STEP_DONE;
    }
    /* The end of the body, normal or not, is processResponseEndOfBody. */
    es_interpret_and_end(ctx, obj, d);
    return JS_STEP_DONE;
}

static const char *const ES_BODY_STEPS[] = { "HTML §9.2.2 step 15's interpretation of res's body", NULL };
static const JSTrampStepDef es_body_def = {
    sizeof(EsPlain), js_es_body_step, NULL, 0,
    .visit = js_es_plain_visit,
    .algorithm = "HTML §9.2.2 step 15 a piece of res's body",
    .steps = ES_BODY_STEPS
};

/* ---- §9.2.2 step 15's processResponse ----------------------------------------------------------------- */

/* "if res's status is not 200, or if res's `Content-Type` is not `text/event-stream`": HTML §2.5.2's
   Content-Type is MIME Sniffing's parsed type, compared by its essence. */
static bool es_response_is_event_stream(JSContext *ctx, JSValueConst res)
{
    HeaderList hl = { 0 };
    MimeType mt;
    char *ct, *essence;
    bool ok;

    if (fetch_reply_status(ctx, res) != 200) return false;
    fetch_reply_header_list(ctx, res, &hl);
    ct = header_list_get(&hl, "content-type");
    header_list_free(&hl);
    mime_type_init(&mt);
    ok = ct != NULL && mime_type_extract(&mt, ct);
    free(ct);
    if (!ok) { mime_type_free(&mt); return false; }
    essence = mime_type_essence(&mt);
    CHECK(essence != NULL, "EventSource: OOM computing a MIME type's essence");
    ok = strcmp(essence, "text/event-stream") == 0;
    free(essence);
    mime_type_free(&mt);
    return ok;
}

/* §9.2.6 step 5's origin: the origin of the response's final URL, the last of its URL list. */
static JSValue es_final_origin(JSContext *ctx, JSValueConst res)
{
    JSValue ul = JS_GetPropertyStr(ctx, res, "urlList"), last;
    uint32_t n = 0;
    const char *s;
    UrlRecord rec;
    char *origin;
    JSValue r;

    DCHECK(JS_IsArray(ul), "the host delivered an EventSource reply carrying no `urlList`");
    {
        JSValue lv = JS_GetPropertyStr(ctx, ul, "length");

        CHECK(JS_ToUint32(ctx, &n, lv) == 0, "EventSource: a reply's URL list has no readable length");
        JS_FreeValue(ctx, lv);
    }
    DCHECK(n > 0, "the host delivered an EventSource reply whose URL list is empty");
    last = JS_GetPropertyUint32(ctx, ul, n - 1);
    s = JS_ToCString(ctx, last);
    CHECK(s != NULL, "EventSource: OOM reading a reply's final URL");
    url_record_init(&rec);
    CHECK(fetch_parse_url(ctx, &rec, s, strlen(s)), "EventSource: a reply's final URL does not parse");
    origin = url_serialize_origin(&rec);
    CHECK(origin != NULL, "EventSource: OOM serializing a final URL's origin");
    r = JS_NewString(ctx, origin);
    free(origin);
    url_record_free(&rec);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, last);
    JS_FreeValue(ctx, ul);
    return r;
}

static int js_es_response_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    EsPlain *s = st;
    JSValueConst obj = JS_StepClosureData(&s->hdr, 0);
    JSValueConst res = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
    EventSourceData *d = es_of(obj);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == ES_ONLY, "an EventSource's processResponse resumed into a stage it does not have");
    DCHECK(d != NULL, "an EventSource's processResponse was delivered to something that is not an EventSource");
    DCHECK(JS_IsObject(res) || JS_IsNull(res),
           "an EventSource's reply is neither the host's reply record nor null for a network error");
    es_set(ctx, d, &d->deliver, JS_UNDEFINED);
    /* close() aborted the fetch: its response is an aborted network error, and failing the connection then
       changes nothing, since the object is CLOSED. */
    if (d->ready_state == ES_CLOSED) return JS_STEP_DONE;
    if (JS_IsNull(res)) {
        es_reestablish(ctx, obj, d);
        return JS_STEP_DONE;
    }
    if (fetch_reply_unobserved(ctx, res)) {
        DFAIL("an EventSource's request was answered with a reply no server sent; its status, its type and its "
              "event sequence are unknown input. Build the fork over §9.2.2 step 15's status and type test and "
              "over the stream's events");
        es_queue(d->realm, obj, ES_TASK_FAIL, JS_UNDEFINED);
        return JS_STEP_DONE;
    }
    if (!es_response_is_event_stream(ctx, res)) {
        es_queue(d->realm, obj, ES_TASK_FAIL, JS_UNDEFINED);
        return JS_STEP_DONE;
    }
    /* "Otherwise, announce the connection and interpret res's body line by line." */
    es_set(ctx, d, &d->origin, es_final_origin(ctx, res));
    es_set(ctx, d, &d->body, JS_UNDEFINED);
    es_queue(d->realm, obj, ES_TASK_ANNOUNCE, JS_UNDEFINED);
    if (reply_body_streamed(ctx, res)) {
        JSValue body_deliver = JS_NewStepClosure(ctx, g_body_stepid, 1, 1, &obj);

        CHECK(!JS_IsException(body_deliver), "EventSource: OOM allocating a body's delivery");
        reply_body_park(ctx, res, body_deliver, 0, document_environment_doc(ctx));
        JS_FreeValue(ctx, body_deliver);
        return JS_STEP_DONE;
    }
    {
        JSValue bd = JS_GetPropertyStr(ctx, res, "body");
        size_t n = 0;
        const uint8_t *b = fetch_body_bytes(ctx, bd, &n);

        es_body_append(ctx, d, b, n);
        JS_FreeValue(ctx, bd);
    }
    es_interpret_and_end(ctx, obj, d);
    return JS_STEP_DONE;
}

static const char *const ES_RESPONSE_STEPS[] = { "HTML §9.2.2 step 15's processResponse", NULL };
static const JSTrampStepDef es_response_def = {
    sizeof(EsPlain), js_es_response_step, NULL, 0,
    .visit = js_es_plain_visit,
    .algorithm = "HTML §9.2.2 step 15 processResponse",
    .steps = ES_RESPONSE_STEPS
};

/* ---- §9.2.2 steps 8-15 and §9.2.3 step 5's fetch ------------------------------------------------------- */

/* Named residual. Not covered: step 11's cache mode "no-store", which FetchRequest does not carry, so the
   trusted zone's cache may answer a reconnection. Next diff builds: Fetch §2.2.5's cache mode on FetchRequest
   and the pending line. Absence shows as: a reconnection answered from the zone's cache rather than sent. */
/* The request §9.2.2 steps 8-13 create, with §9.2.3 step 5.3's Last-Event-ID, fetched with step 15's
   processResponse parked on the running flow. */
static void es_fetch(JSContext *ctx, JSValueConst obj, EventSourceData *d)
{
    int state = d->with_credentials ? CORS_USE_CREDENTIALS : CORS_ANONYMOUS;
    CspRequestMetadata meta = csp_request_metadata_unstated();
    HeaderList hl = { 0 };
    FetchRequest req = { 0 };
    JSValue deliver;
    const char *url = JS_ToCString(ctx, d->url), *leid = JS_ToCString(ctx, d->last_event_id);

    CHECK(url != NULL && leid != NULL, "EventSource: OOM reading the request's URL or last event ID string");
    /* Step 10: "User agents may set (`Accept`, `text/event-stream`) in request's header list." */
    header_list_append(&hl, "Accept", "text/event-stream");
    if (*leid) header_list_append(&hl, "Last-Event-ID", leid);
    deliver = JS_NewStepClosure(ctx, g_response_stepid, 1, 1, &obj);
    CHECK(!JS_IsException(deliver), "EventSource: OOM allocating processResponse");
    fetch_main_report_csp(ctx, url, "", meta);
    req.method = "GET";
    req.url = url;
    req.destination = "";
    req.headers = &hl;
    req.metadata = meta;
    req.credentials = cors_potential_request_credentials(state);
    req.mode = cors_potential_request_mode(state);
    req.referrer = "client";
    req.referrer_policy = "";
    req.address = (FetchAddressClass)d->address;
    req.address_value = d->address == FETCH_ADDRESS_UNKNOWN ? d->src : JS_UNDEFINED;
    req.address_url = JS_UNDEFINED;
    es_set(ctx, d, &d->deliver, JS_DupValue(ctx, deliver));
    engine_pending_resource_url(ctx, deliver, &req);
    JS_FreeValue(ctx, deliver);
    header_list_free(&hl);
    JS_FreeCString(ctx, leid);
    JS_FreeCString(ctx, url);
}

/* ---- §9.2.2 "The EventSource interface" ----------------------------------------------------------------- */

/* The bytes of a url argument that may be unknown input: its example where it has a string one, else its
   display shape. Owned: JS_FreeCString. */
static const char *es_arg_cstring(JSContext *ctx, JSValueConst v, size_t *plen)
{
    JSValue sv;
    const char *r;

    if (!concolic_is(v)) return JS_ToCStringLen(ctx, plen, v);
    sv = concolic_example(ctx, v);
    if (!JS_IsString(sv)) {
        const char *shape = concolic_shape_c(v);

        JS_FreeValue(ctx, sv);
        DCHECK(shape != NULL, "unknown input reached an EventSource url with no display shape");
        sv = JS_NewString(ctx, shape);
    }
    r = JS_ToCStringLen(ctx, plen, sv);
    JS_FreeValue(ctx, sv);
    return r;
}

/* `new EventSource(url, eventSourceInitDict)`. `new_target` supplies the prototype, so a subclass gets its own. */
static JSValue js_es_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    FetchAddressClass address;
    UrlRecord rec;
    EventSourceData *d;
    JSValue proto, obj, set;
    const char *u;
    char *ser;
    size_t ulen = 0;
    bool ok, creds;

    (void)magic;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor EventSource requires 'new'");
    DCHECK(argc >= 2 && JS_IsObject(argv[1]),
           "the EventSource constructor ran without its converted EventSourceInit; the declaration defaults it");
    /* Steps 3-4: encoding-parse url relative to settings. An unknown with a string example is parsed as its
       example, which is the address a session with that input connects to. */
    address = concolic_is(argv[0]) ? FETCH_ADDRESS_UNKNOWN : FETCH_ADDRESS_CONCRETE;
    if (address == FETCH_ADDRESS_UNKNOWN) {
        JSValue ex = concolic_example(ctx, argv[0]);

        if (JS_IsString(ex)) address = FETCH_ADDRESS_EXAMPLE;
        JS_FreeValue(ctx, ex);
    }
    u = es_arg_cstring(ctx, argv[0], &ulen);
    if (!u) return JS_EXCEPTION;
    url_record_init(&rec);
    ok = fetch_parse_url(ctx, &rec, u, ulen);
    JS_FreeCString(ctx, u);
    if (!ok)
        return JS_ThrowDOMException(ctx, "SyntaxError", "the EventSource URL cannot be parsed");
    /* Steps 6-7. */
    creds = idl_dict_bool(ctx, argv[1], "withCredentials");
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsException(proto) && !JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        proto = JS_GetClassProto(ctx, g_es_class);
        DCHECK(!JS_IsNull(proto), "an EventSource was constructed in a realm that never ran its install");
    }
    obj = JS_IsException(proto) ? JS_EXCEPTION : JS_NewObjectProtoClass(ctx, proto, g_es_class);
    JS_FreeValue(ctx, proto);
    ser = url_serialize(&rec, /*exclude_fragment*/ false);
    url_record_free(&rec);
    CHECK(ser != NULL, "EventSource: OOM serializing a url");
    if (JS_IsException(obj)) { free(ser); return JS_EXCEPTION; }
    /* Step 1's object, and step 5's url. */
    d = lexbor_calloc(1, sizeof *d);
    CHECK(d != NULL, "EventSource: OOM building an EventSource");
    d->realm = ctx;
    d->url = JS_NewString(ctx, ser);
    d->src = address == FETCH_ADDRESS_UNKNOWN ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    d->last_event_id = JS_NewString(ctx, "");
    d->deliver = JS_UNDEFINED;
    d->body = JS_UNDEFINED;
    d->origin = JS_UNDEFINED;
    d->reconnection_ms = ES_DEFAULT_RECONNECTION_MS;
    d->ready_state = ES_CONNECTING;
    d->with_credentials = creds;
    d->address = (uint8_t)address;
    free(ser);
    CHECK(!JS_IsException(d->url) && !JS_IsException(d->last_event_id), "EventSource: OOM building its strings");
    JS_SetOpaque(obj, d);
    set = es_set_of(ctx);
    JS_SetPropertyUint32(ctx, set, es_set_len(ctx, set), JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, set);
    /* Step 8's request is an endpoint at the object's own address. */
    {
        EndpointHeader eh = { "Accept", "text/event-stream", NULL };

        endpoint_record(ctx, "GET", concolic_is(argv[0]) ? argv[0] : d->url, &eh, 1, NULL,
                        engine_prov_of_running_path(), engine_driven_of_running_path(), EPD_EVENT_SOURCE,
                        cors_potential_request_credentials(creds ? CORS_USE_CREDENTIALS : CORS_ANONYMOUS));
    }
    /* Steps 8-15. */
    es_fetch(ctx, obj, es_of(obj));
    return obj;
}

enum { ES_GET_URL = 0, ES_GET_WITH_CREDENTIALS, ES_GET_READY_STATE };

static JSValue js_es_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    EventSourceData *d = es_of(this_val);

    if (!d) return JS_ThrowTypeError(ctx, "not an EventSource");
    switch (magic) {
    case ES_GET_URL:              return JS_DupValue(ctx, d->url);
    case ES_GET_WITH_CREDENTIALS: return JS_NewBool(ctx, d->with_credentials);
    case ES_GET_READY_STATE:      return JS_NewInt32(ctx, d->ready_state);
    }
    DFAIL("an EventSource getter was installed with a magic §9.2.2 has no attribute for");
    return JS_UNDEFINED;
}

/* `close()`. */
static JSValue js_es_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    EventSourceData *d = es_of(this_val);

    (void)argc; (void)argv; (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not an EventSource");
    es_close(ctx, d, this_val);
    return JS_UNDEFINED;
}

void event_source_forcibly_close_all(JSContext *ctx)
{
    JSValue set = es_set_of(ctx);
    uint32_t n = es_set_len(ctx, set);

    /* es_close takes each object off the set, so the walk reads its head until the set is empty. */
    while (n > 0) {
        JSValue ev = JS_GetPropertyUint32(ctx, set, 0);
        EventSourceData *d = es_of(ev);

        DCHECK(d != NULL && d->ready_state != ES_CLOSED,
               "a realm's EventSource set holds something that is not an EventSource, or a CLOSED one");
        es_close(ctx, d, ev);
        JS_FreeValue(ctx, ev);
        n = es_set_len(ctx, set);
    }
    JS_FreeValue(ctx, set);
}

bool event_source_is_response_delivery(JSContext *ctx, JSValueConst deliver)
{
    return g_response_stepid >= 0 && JS_IsStepClosureOf(ctx, deliver, g_response_stepid);
}

bool event_source_is_body_delivery(JSContext *ctx, JSValueConst deliver)
{
    return g_body_stepid >= 0 && JS_IsStepClosureOf(ctx, deliver, g_body_stepid);
}

bool event_source_body_delivery_open(JSContext *ctx, JSValueConst deliver)
{
    EventSourceData *d = es_of(JS_StepClosureDataOf(ctx, deliver, g_body_stepid, 0));

    DCHECK(d != NULL, "a body delivery's closure names something that is not an EventSource");
    return d->ready_state != ES_CLOSED;
}

/* ---- install ---------------------------------------------------------------------------------------------- */

/* §9.2.2's three constants, on the interface object and on its prototype (Web IDL §3.7.5 "Constants"). */
static const JSCFunctionListEntry ES_CONSTANTS[] = {
    JS_PROP_INT32_DEF("CONNECTING", ES_CONNECTING, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("OPEN", ES_OPEN, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("CLOSED", ES_CLOSED, JS_PROP_ENUMERABLE),
};

/* `dictionary EventSourceInit { boolean withCredentials = false; }`: an absent member reads as false through
   idl_dict_bool. */
static const IdlDictMember ES_INIT[] = { { "withCredentials", IDL_BOOLEAN, false, NULL, 0, NULL,
                                           IDL_DEFAULT_NONE, NULL } };

void event_source_init(JSContext *ctx)
{
    JSClassDef cd = { "EventSource", .finalizer = es_finalizer, .gc_mark = es_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CTOR_ARGS[2] = { IDL_USVSTRING, IDL_DICT };

    DCHECK(g_es_rt == NULL, "event_source_init ran twice; the interface is declared once per agent");
    g_es_rt = rt;
    JS_NewClassID(rt, &g_es_class);
    JS_NewClass(rt, g_es_class, &cd);
    g_set_slot = realm_value_declare(ctx, "HTML §9.2.9 a realm's EventSource objects that are not CLOSED");
    g_task_stepid = JS_RegisterStepDef(rt, &es_task_def);
    g_response_stepid = JS_RegisterStepDef(rt, &es_response_def);
    g_body_stepid = JS_RegisterStepDef(rt, &es_body_def);
    g_wait_stepid = JS_RegisterStepDef(rt, &es_wait_def);
    DCHECK(g_task_stepid >= 0 && g_response_stepid >= 0 && g_body_stepid >= 0 && g_wait_stepid >= 0,
           "an EventSource machine could not be declared against this runtime");
    g_task_fn = JS_NewCFunction2(ctx, NULL, "", 2, JS_CFUNC_step, g_task_stepid);
    CHECK(JS_IsFunction(ctx, g_task_fn), "the remote event task callee could not be allocated");
    g_ctor_id = idl_method_id_dict(ctx, CTOR_ARGS, 2, ES_INIT, 1, js_es_ctor, 0);
    idl_optional_from(1);    /* `optional EventSourceInit eventSourceInitDict = {}` */
    g_id_close = idl_method_id(ctx, NULL, 0, js_es_close, 0);
    agent_state_ptr("event_source", &g_es_rt, "the runtime EventSource was declared in, and the latch");
    agent_state_class("event_source", &g_es_class, "HTML §9.2.2's EventSource class, which carries the record");
    agent_state_realm_slot("event_source", &g_set_slot, "the per-realm slot a realm's EventSource set lives in");
    agent_state_value("event_source", &g_task_fn, "the remote event task callee, one per agent");
    agent_state_id("event_source", &g_task_stepid, "HTML §9.2.3's task machine");
    agent_state_id("event_source", &g_response_stepid, "HTML §9.2.2 step 15's processResponse machine");
    agent_state_id("event_source", &g_body_stepid, "HTML §9.2.2 step 15's body machine");
    agent_state_id("event_source", &g_wait_stepid, "HTML §9.2.3's reconnection wait machine");
    agent_state_id("event_source", &g_ctor_id, "HTML §9.2.2's `constructor(url, eventSourceInitDict)` pool entry");
    agent_state_id("event_source", &g_id_close, "HTML §9.2.2's `close()` pool entry");
    realm_declare_intrinsic(event_source_install_realm);
}

void event_source_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, ctor, set;

    DCHECK(g_es_class != 0 && g_ctor_id >= 0,
           "a realm asked for the EventSource interface before event_source_init declared it");
    prev = JS_GetClassProto(ctx, g_es_class);
    DCHECK(JS_IsNull(prev), "event_source_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* Built with the realm, so a flow's objects are captured by that flow's delta. */
    set = JS_NewArray(ctx);
    CHECK(!JS_IsException(set), "a realm's EventSource set could not be allocated");
    realm_value_set(ctx, g_set_slot, set);

    proto = event_target_derived_proto(ctx);   /* `interface EventSource : EventTarget` */
    idl_interface_tag(ctx, proto, "EventSource");
    event_target_install_handlers(ctx, proto, EH_EVENT_SOURCE);
    JS_SetPropertyFunctionList(ctx, proto, ES_CONSTANTS, (int)(sizeof(ES_CONSTANTS) / sizeof(ES_CONSTANTS[0])));
    idl_install_accessor(ctx, proto, "url", js_es_get, ES_GET_URL, -1);
    idl_install_accessor(ctx, proto, "withCredentials", js_es_get, ES_GET_WITH_CREDENTIALS, -1);
    idl_install_accessor(ctx, proto, "readyState", js_es_get, ES_GET_READY_STATE, -1);
    idl_install_method(ctx, proto, "close", g_id_close);

    ctor = idl_step_constructor(ctx, "EventSource", g_ctor_id);
    CHECK(!JS_IsException(ctor), "the EventSource interface object could not be allocated");
    JS_SetPropertyFunctionList(ctx, ctor, ES_CONSTANTS, (int)(sizeof(ES_CONSTANTS) / sizeof(ES_CONSTANTS[0])));
    JS_SetConstructor(ctx, ctor, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "EventSource", ctor);
    JS_FreeValue(ctx, global);
    JS_SetClassProto(ctx, g_es_class, proto);   /* the realm owns it from here */
}

void event_source_free(JSRuntime *rt)
{
    DCHECK(g_es_rt != NULL, "EventSource was released in an agent that never declared it");
    DCHECK(rt == g_es_rt, "EventSource was released against a runtime that is not the one it was declared in");
    JS_FreeValueRT(rt, g_task_fn);   /* the prototypes are the realms', released with their contexts */
    agent_state_undo("event_source");
}
