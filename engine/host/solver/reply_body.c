/* solver/reply_body.c — a streamed response body's pieces; see reply_body.h.
 *
 * A source is a null-prototype object: `items` (the pieces so far, each `{op, bytes}`), `waiters` (the
 * unanswered FLOW_PENDING_BODY records), `head` (the head record), `url`/`method` (the request), `prov`/
 * `driven` (the reply's grade), `declined` (the zone's reason once it stopped waiting, else null) and `whole`
 * (the assembled reply once the body ended). Every write to it is bracketed by cow_engine_write_begin/end: the
 * pieces are what the server sent, the same on every timeline, and no flow's delta may capture them. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/reply_body.h"
#include "solver/reply_decode.h"
#include "solver/pending.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include "solver/cow.h"
#include "solver/reclaim.h"
#include "core/fetch/fetch.h"

/* The sources whose body has not ended, by the zone's token; each holds a reference to its source. */
typedef struct { char *token; JSValue source; } OpenSource;
static OpenSource *g_open;
static int g_open_n, g_open_cap;

static JSValue rb_get(JSContext *ctx, JSValueConst o, const char *name)
{
    return JS_GetPropertyStr(ctx, o, name);
}

static void rb_put(JSContext *ctx, JSValueConst o, const char *name, JSValue v)
{
    int r;

    cow_engine_write_begin();
    r = JS_DefinePropertyValueStr(ctx, o, name, v, JS_PROP_C_W_E);
    cow_engine_write_end();
    CHECK(r >= 0, "reply_body: OOM writing a body source's field");
}

static uint32_t rb_len(JSContext *ctx, JSValueConst arr)
{
    int64_t n = 0;

    CHECK(JS_GetLength(ctx, arr, &n) == 0 && n >= 0 && n <= UINT32_MAX,
          "reply_body: a body source's list has no length");
    return (uint32_t)n;
}

static void rb_push(JSContext *ctx, JSValueConst arr, JSValue v)
{
    int r;

    cow_engine_write_begin();
    r = JS_SetPropertyUint32(ctx, arr, rb_len(ctx, arr), v);
    cow_engine_write_end();
    CHECK(r >= 0, "reply_body: OOM appending to a body source's list");
}

static JSValue rb_source_of(JSContext *ctx, JSValueConst reply)
{
    JSValue s = rb_get(ctx, reply, "source");

    DCHECK(JS_IsObject(s), "a body source was asked of a reply record that is not a streamed head — "
                           "reply_body_open attaches one to every record carrying a `stream` token");
    return s;
}

static int rb_open_find(const char *token)
{
    int i;

    for (i = 0; i < g_open_n; i++)
        if (!strcmp(g_open[i].token, token)) return i;
    return -1;
}

/* A piece: the op and, for an enqueue, the bytes as an ArrayBuffer every reading flow copies its chunk from. */
static JSValue rb_item(JSContext *ctx, ReplyBodyOp op, const uint8_t *bytes, size_t n)
{
    JSValue it = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(it), "reply_body: OOM building a body piece");
    rb_put(ctx, it, "op", JS_NewInt32(ctx, (int32_t)op));
    if (op == REPLY_BODY_ENQUEUE) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, bytes, n);

        CHECK(!JS_IsException(ab), "reply_body: OOM copying a body piece's bytes");
        rb_put(ctx, it, "bytes", ab);
    }
    return it;
}

/* Answers `e` with piece `item` of `source`, as engine_provide answers a park: the value, the flag and the
   arrival stamp the delivery ladder orders by. */
static void rb_answer(JSContext *ctx, JSValueConst e, JSValueConst items, uint32_t item)
{
    JSValue piece = JS_GetPropertyUint32(ctx, items, item);

    DCHECK(JS_IsObject(piece), "a body park was answered with a piece its source does not hold");
    pending_set(e, PEND_VALUE, piece);
    pending_set(e, PEND_HAVE_VALUE, JS_TRUE);
    pending_set_int(e, PEND_WORK_SEQ, (int64_t)flow_work_seq_next());
}

void reply_body_open(JSContext *ctx, JSValueConst reply, const char *token, const char *method, const char *url)
{
    JSValue src, items, body;
    size_t n = 0;
    const uint8_t *bytes;

    DCHECK(token != NULL && *token && method != NULL && url != NULL,
           "a streamed head was opened with no token, method or address — the token keys every later record");
    DCHECK(rb_open_find(token) < 0,
           "the trusted zone opened a second body under a token that names a body still open — the zone mints "
           "one token per streamed send");
    src = JS_NewObjectProto(ctx, JS_NULL);
    items = JS_NewArray(ctx);
    CHECK(!JS_IsException(src) && !JS_IsException(items), "reply_body: OOM opening a body source");
    rb_put(ctx, src, "items", items);
    rb_put(ctx, src, "waiters", JS_NewArray(ctx));
    rb_put(ctx, src, "head", JS_DupValue(ctx, reply));
    rb_put(ctx, src, "url", JS_NewString(ctx, url));
    rb_put(ctx, src, "method", JS_NewString(ctx, method));
    rb_put(ctx, src, "declined", JS_NULL);
    rb_put(ctx, src, "whole", JS_UNDEFINED);
    /* The head's own bytes are the first read's: Fetch §4.7 HTTP-network fetch enqueues each piece the
       transmission yields, and an empty read yields nothing to enqueue. */
    body = fetch_reply_body(ctx, reply);
    bytes = fetch_body_bytes(ctx, body, &n);
    if (n) rb_push(ctx, items, rb_item(ctx, REPLY_BODY_ENQUEUE, bytes, n));
    JS_FreeValue(ctx, body);
    rb_put(ctx, reply, "source", JS_DupValue(ctx, src));
    if (g_open_n == g_open_cap) {
        g_open_cap = g_open_cap ? 2 * g_open_cap : 8;
        g_open = reclaim_realloc(g_open, (size_t)g_open_cap * sizeof *g_open);
        CHECK(g_open != NULL, "reply_body: OOM growing the open bodies");
    }
    g_open[g_open_n].token = reclaim_strdup(token);
    CHECK(g_open[g_open_n].token != NULL, "reply_body: OOM naming an open body");
    g_open[g_open_n++].source = src;
}

int reply_body_streamed(JSContext *ctx, JSValueConst reply)
{
    JSValue s;
    int r;

    if (!JS_IsObject(reply)) return 0;
    s = rb_get(ctx, reply, "source");
    r = JS_IsObject(s);
    JS_FreeValue(ctx, s);
    return r;
}

void reply_body_set_grade(JSContext *ctx, JSValueConst reply, int prov, int driven)
{
    JSValue src = rb_source_of(ctx, reply);

    rb_put(ctx, src, "prov", JS_NewInt32(ctx, prov));
    rb_put(ctx, src, "driven", JS_NewInt32(ctx, driven));
    JS_FreeValue(ctx, src);
}

/* The whole reply at the body's end: the head's fields, `computedType`, and every enqueued piece's bytes. */
static JSValue rb_whole(JSContext *ctx, JSValueConst src, const char *computed_type)
{
    static const char *const FIELDS[] = { "status", "statusText", "headers", "urlList", "type" };
    JSValue head = rb_get(ctx, src, "head"), items = rb_get(ctx, src, "items");
    JSValue whole = JS_NewObject(ctx);
    uint32_t i, n = rb_len(ctx, items);
    size_t total = 0, off = 0;
    uint8_t *all;

    CHECK(!JS_IsException(whole), "reply_body: OOM assembling a body's whole reply");
    for (i = 0; i < sizeof FIELDS / sizeof *FIELDS; i++)
        rb_put(ctx, whole, FIELDS[i], rb_get(ctx, head, FIELDS[i]));
    rb_put(ctx, whole, "computedType", JS_NewString(ctx, computed_type));
    for (i = 0; i < n; i++) {
        JSValue it = JS_GetPropertyUint32(ctx, items, i), b = rb_get(ctx, it, "bytes");
        size_t bn = 0;

        if (JS_IsArrayBuffer(b)) { (void)fetch_body_bytes(ctx, b, &bn); total += bn; }
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, it);
    }
    all = reclaim_malloc(total ? total : 1);
    CHECK(all != NULL, "reply_body: OOM assembling a body's bytes");
    for (i = 0; i < n; i++) {
        JSValue it = JS_GetPropertyUint32(ctx, items, i), b = rb_get(ctx, it, "bytes");
        size_t bn = 0;

        if (JS_IsArrayBuffer(b)) {
            const uint8_t *p = fetch_body_bytes(ctx, b, &bn);

            memcpy(all + off, p, bn);
            off += bn;
        }
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, it);
    }
    cow_engine_write_begin();
    fetch_reply_set_body(ctx, whole, all, total);
    cow_engine_write_end();
    free(all);
    JS_FreeValue(ctx, head);
    JS_FreeValue(ctx, items);
    return whole;
}

int reply_body_append(JSContext *ctx, const char *token, const char *state, const uint8_t *bytes, size_t n,
                      const char *meta)
{
    int at = rb_open_find(token), terminal, declined = 0, nwake = 0;
    JSValue src, items, waiters, kept, *wake;
    uint32_t i, wn;

    if (at < 0) return 0;
    DCHECK(!strcmp(state, "more") || !strcmp(state, "end") || !strcmp(state, "network") ||
           !strcmp(state, "decline"),
           "a body record states none of `more`, `end`, `network` or `decline` — the zone writes one of the four");
    DCHECK(meta != NULL && (strcmp(state, "more") || !*meta),
           "a body record's metadata does not match its state — the end carries its computed type and a failure "
           "its reason, and a piece in the middle carries the empty string");
    src = JS_DupValue(ctx, g_open[at].source);
    items = rb_get(ctx, src, "items");
    if (n) rb_push(ctx, items, rb_item(ctx, REPLY_BODY_ENQUEUE, bytes, n));
    terminal = strcmp(state, "more") != 0;
    if (!strcmp(state, "end")) {
        JSValue whole = rb_whole(ctx, src, meta), mv = rb_get(ctx, src, "method"), uv = rb_get(ctx, src, "url");
        JSValue pv = rb_get(ctx, src, "prov"), dv = rb_get(ctx, src, "driven");
        const char *m = JS_ToCString(ctx, mv), *u = JS_ToCString(ctx, uv);

        rb_push(ctx, items, rb_item(ctx, REPLY_BODY_CLOSE, NULL, 0));
        rb_put(ctx, src, "whole", JS_DupValue(ctx, whole));
        /* Learning from the reply happens once, where the whole body exists, as engine_provide does for a
           reply delivered whole; a head no park took (a paged-out flow's sale) carries no grade and, as in
           engine_provide, teaches nothing. */
        if (JS_IsNumber(pv))
            reply_decode_learn(ctx, m, u, whole, JS_VALUE_GET_INT(pv), JS_VALUE_GET_INT(dv));
        JS_FreeCString(ctx, m);
        JS_FreeCString(ctx, u);
        JS_FreeValue(ctx, mv);
        JS_FreeValue(ctx, uv);
        JS_FreeValue(ctx, pv);
        JS_FreeValue(ctx, dv);
        JS_FreeValue(ctx, whole);
    } else if (!strcmp(state, "network")) {
        /* Fetch §4.7 HTTP-network fetch: a transmission that ends abnormally errors a readable stream with a
           TypeError. */
        rb_push(ctx, items, rb_item(ctx, REPLY_BODY_ERROR, NULL, 0));
    } else if (!strcmp(state, "decline")) {
        rb_put(ctx, src, "declined", JS_NewString(ctx, meta));
        declined = 1;
    }
    /* Every park waiting on a piece the source now holds is answered; one past the end stays, refused when the
       zone declined to wait longer. */
    waiters = rb_get(ctx, src, "waiters");
    kept = JS_NewArray(ctx);
    CHECK(!JS_IsException(kept), "reply_body: OOM listing a body's waiting parks");
    wn = rb_len(ctx, waiters);
    wake = malloc((wn ? wn : 1) * sizeof *wake);
    CHECK(wake != NULL, "reply_body: OOM listing the parks a body record answers");
    for (i = 0; i < wn; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, waiters, i);
        uint32_t item = (uint32_t)pending_get_int(e, PEND_ITEM);

        if (item < rb_len(ctx, items)) {
            rb_answer(ctx, e, items, item);
            wake[nwake++] = JS_DupValue(ctx, e);
        } else {
            if (declined) {
                pending_set(e, PEND_DECLINED, JS_NewString(ctx, meta));
                wake[nwake++] = JS_DupValue(ctx, e);
            }
            rb_push(ctx, kept, JS_DupValue(ctx, e));
        }
        JS_FreeValue(ctx, e);
    }
    rb_put(ctx, src, "waiters", kept);
    /* The members whose registers hold an answered or refused park, and no other, are askable again. */
    if (nwake) flow_wake_owed_holding(wake, nwake);
    for (i = 0; i < (uint32_t)nwake; i++) JS_FreeValue(ctx, wake[i]);
    free(wake);
    if (terminal) {
        free(g_open[at].token);
        JS_FreeValue(ctx, g_open[at].source);
        g_open[at] = g_open[--g_open_n];
    }
    JS_FreeValue(ctx, waiters);
    JS_FreeValue(ctx, items);
    JS_FreeValue(ctx, src);
    return 1;
}

void reply_body_park(JSContext *ctx, JSValueConst reply, JSValueConst deliver, int item, uint32_t doc)
{
    Flow *f = flow_running();
    JSValue src = rb_source_of(ctx, reply), items = rb_get(ctx, src, "items");
    JSValue declined = rb_get(ctx, src, "declined"), e;

    DCHECK(f != NULL, "a body park was made outside a running flow — the stream it feeds belongs to one");
    DCHECK(item >= 0, "a body park waits for a piece before the first");
    e = pending_push(&f->pending, FLOW_PENDING_BODY, flow_path_forced(f), flow_path_pinned(f),
                     flow_path_driven(f), /*parser_inserted*/0, flow_work_seq_next());
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, deliver));
    pending_set(e, PEND_SOURCE, JS_DupValue(ctx, src));
    pending_set_int(e, PEND_ITEM, item);
    pending_set_int(e, PEND_DOC, (int64_t)doc);
    /* Each piece is a network answer of its own, due a latency along this flow's path after the read that asks
       for it, whether or not the zone has already written it. */
    pending_set_int(e, PEND_DUE, engine_reply_due(f));
    if ((uint32_t)item < rb_len(ctx, items)) {
        rb_answer(ctx, e, items, (uint32_t)item);
    } else {
        JSValue waiters = rb_get(ctx, src, "waiters");

        if (JS_IsString(declined)) pending_set(e, PEND_DECLINED, JS_DupValue(ctx, declined));
        rb_push(ctx, waiters, JS_DupValue(ctx, e));
        JS_FreeValue(ctx, waiters);
    }
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, declined);
    JS_FreeValue(ctx, items);
    JS_FreeValue(ctx, src);
}

ReplyBodyOp reply_body_piece_op(JSContext *ctx, JSValueConst piece)
{
    JSValue op = rb_get(ctx, piece, "op");
    int v;

    DCHECK(JS_VALUE_GET_TAG(op) == JS_TAG_INT, "a body piece carries no op — rb_item writes one on every piece");
    v = JS_VALUE_GET_INT(op);
    DCHECK(v >= REPLY_BODY_ENQUEUE && v <= REPLY_BODY_ERROR, "a body piece carries an op this file does not name");
    return (ReplyBodyOp)v;
}

void reply_body_next(JSContext *ctx, JSValueConst e, JSValueConst piece, int open)
{
    JSValue src, head, deliver;

    DCHECK(pending_get_int(e, PEND_KIND) == FLOW_PENDING_BODY, "a body park's successor was asked of another kind");
    if (reply_body_piece_op(ctx, piece) != REPLY_BODY_ENQUEUE || !open) return;
    src = pending_get(e, PEND_SOURCE);
    head = rb_get(ctx, src, "head");
    deliver = pending_get(e, PEND_RESOLVE);
    reply_body_park(ctx, head, deliver, (int)pending_get_int(e, PEND_ITEM) + 1,
                    (uint32_t)pending_get_int(e, PEND_DOC));
    JS_FreeValue(ctx, deliver);
    JS_FreeValue(ctx, head);
    JS_FreeValue(ctx, src);
}

JSValue reply_body_whole(JSContext *ctx, JSValueConst e)
{
    JSValue src = pending_get(e, PEND_SOURCE), w = rb_get(ctx, src, "whole");

    JS_FreeValue(ctx, src);
    return w;
}

JSValue reply_body_url(JSContext *ctx, JSValueConst e)
{
    JSValue src = pending_get(e, PEND_SOURCE), u = rb_get(ctx, src, "url");

    JS_FreeValue(ctx, src);
    return u;
}

int reply_body_open_count(void)
{
    return g_open_n;
}

int reply_body_entry_open(JSContext *ctx, JSValueConst e)
{
    JSValue src = pending_get(e, PEND_SOURCE);
    int i, hit = 0;

    for (i = 0; i < g_open_n && !hit; i++) hit = JS_VALUE_GET_PTR(g_open[i].source) == JS_VALUE_GET_PTR(src);
    JS_FreeValue(ctx, src);
    return hit;
}

void reply_body_reset(JSContext *ctx)
{
    int i;

    for (i = 0; i < g_open_n; i++) {
        free(g_open[i].token);
        JS_FreeValue(ctx, g_open[i].source);
    }
    free(g_open);
    g_open = NULL;
    g_open_n = g_open_cap = 0;
}
