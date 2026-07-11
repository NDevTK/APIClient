/* Storage API module (StorageManager) — see storage_manager.h. */
#include "modules/quota/storage_manager.h"
#include "bindings/idl.h"        /* idl_dfail_wrap — unbuilt StorageManager members (getDirectory/OPFS) DFAIL */
#include "solver/concolic.h"     /* js_concolic — quota numbers / persist bool fork their gates */

#include "platform/promise.h"

/* estimate(): StorageEstimate { usage, quota } — concolic numbers so a quota-gated feature check forks. */
static JSValue storage_estimate(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "usage", js_concolic(ctx, "{storageUsage}", JS_NewInt64(ctx, 1048576)));         /* ~1MB */
    JS_SetPropertyStr(ctx, o, "quota", js_concolic(ctx, "{storageQuota}", JS_NewFloat64(ctx, 1073741824.0)));  /* ~1GB */
    return js_resolved(ctx, o);
}
/* persist()/persisted(): Promise<boolean> — concolic so the persistent-storage gate explores both arms. */
static JSValue storage_persist(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return js_resolved(ctx, js_concolic(ctx, "{storagePersisted}", JS_TRUE));
}

JSValue storage_manager_make(JSContext *ctx) {
    JSValue s = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, s, "estimate", JS_NewCFunction(ctx, storage_estimate, "estimate", 0));
    JS_SetPropertyStr(ctx, s, "persist", JS_NewCFunction(ctx, storage_persist, "persist", 0));
    JS_SetPropertyStr(ctx, s, "persisted", JS_NewCFunction(ctx, storage_persist, "persisted", 0));
    return idl_dfail_wrap(ctx, s, "StorageManager");   /* getDirectory (OPFS) DFAILs — build it at the root */
}
