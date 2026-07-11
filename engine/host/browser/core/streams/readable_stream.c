/* ReadableStream (WHATWG Streams) — see readable_stream.h. The single-chunk model: a reader delivers the
   underlying value once (taint-carrying), then done. Reused by Response.body / Blob.stream. */
#include "core/streams/readable_stream.h"
#include "solver/concolic.h"   /* js_noop */
#include "check.h"             /* DCHECK — assert the component's own invariants */

#include "platform/promise.h"

static JSValue m_prom_undef(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, JS_UNDEFINED); }

/* reader.read(): the [[body]] as one chunk (done=false) the first call, then done=true. The done flag rides the
   reader object (__done) — a reader is per-getReader state, so two readers stream independently. */
static JSValue m_reader_read(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v;
    JSValue res = JS_NewObject(ctx);
    JSValue df = JS_GetPropertyStr(ctx, t, "__done");
    if (JS_ToBool(ctx, df)) { JS_SetPropertyStr(ctx, res, "value", JS_UNDEFINED); JS_SetPropertyStr(ctx, res, "done", JS_TRUE); }
    else { JS_SetPropertyStr(ctx, res, "value", JS_GetPropertyStr(ctx, t, "__body"));   /* one chunk = the whole taint-carrying body */
           JS_SetPropertyStr(ctx, res, "done", JS_FALSE); JS_SetPropertyStr(ctx, t, "__done", JS_TRUE); }
    JS_FreeValue(ctx, df);
    return js_resolved(ctx, res);
}
static JSValue m_get_reader(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v;
    JSValue rd = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rd, "__body", JS_GetPropertyStr(ctx, t, "__body"));   /* the stream's [[body]] */
    JS_SetPropertyStr(ctx, rd, "__done", JS_FALSE);
    JS_SetPropertyStr(ctx, rd, "read", JS_NewCFunction(ctx, m_reader_read, "read", 0));
    JS_SetPropertyStr(ctx, rd, "cancel", JS_NewCFunction(ctx, m_prom_undef, "cancel", 0));
    JS_SetPropertyStr(ctx, rd, "releaseLock", JS_NewCFunction(ctx, js_noop, "releaseLock", 0));
    JS_SetPropertyStr(ctx, rd, "closed", js_resolved(ctx, JS_UNDEFINED));
    return rd;
}
JSValue readable_stream_new(JSContext *ctx, JSValue chunk) {
    JSValue s = JS_NewObject(ctx);
    DCHECK(JS_IsObject(s), "readable_stream_new: JS_NewObject returned non-object (engine invariant)");
    JS_SetPropertyStr(ctx, s, "__body", chunk);   /* consumes chunk */
    JS_SetPropertyStr(ctx, s, "getReader", JS_NewCFunction(ctx, m_get_reader, "getReader", 0));
    JS_SetPropertyStr(ctx, s, "cancel", JS_NewCFunction(ctx, m_prom_undef, "cancel", 0));
    JS_SetPropertyStr(ctx, s, "locked", JS_FALSE);
    return s;   /* unbuilt tee/pipeTo/pipeThrough left for on-demand build (a subagent can extend this component) */
}
