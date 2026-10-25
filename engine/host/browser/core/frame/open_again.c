/* core/frame/open_again.c — the attacker opening this page's address again (see open_again.h). */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/document.h"
#include "core/frame/navigable.h"
#include "core/frame/open_again.h"
#include "core/frame/session_history.h"
#include "core/frame/window_proxy.h"
#include "core/storage/storage_shed.h"

#define OAG_STAGES(X)                                                                                        \
    X(OAG_OPEN, "HTML §7.2.2.1 window open steps, performed by an attacker page with this page's address as " \
                "url: the new top-level traversable's state, then its document's load")
enum { OAG_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OAG_STEPS[] = { OAG_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;   /* first — the driver writes the def and the operand bounds through it */
} OpenAgainFlow;

/* The active entry's address, owned, or NULL when the realm has no top-level page to open again. */
static char *page_address(JSContext *ctx)
{
    JSValueConst proxy = document_window_proxy(ctx);
    JSValue url;
    const char *s;
    char *out = NULL;

    if (!window_proxy_is(proxy) || window_proxy_is_remote(proxy)) return NULL;
    url = session_history_active_entry_url(ctx);
    s = JS_ToCString(ctx, url);
    CHECK(s != NULL, "open again: OOM reading the page's address");
    if (!strncmp(s, "https:", 6) || !strncmp(s, "http:", 5)) {
        out = strdup(s);
        CHECK(out != NULL, "open again: OOM copying the page's address");
    }
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, url);
    return out;
}

bool open_again_available(JSContext *ctx)
{
    char *addr = page_address(ctx);
    bool ok = addr != NULL;

    free(addr);
    return ok;
}

static int open_again_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    OpenAgainFlow *s = st;
    char *addr;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    STEP_DISPATCH(OAG_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(OAG_OPEN);
    addr = page_address(ctx);
    DCHECK(addr != NULL, "the page was opened again in a realm open_again_available refuses");
    if (!addr) return JS_STEP_DONE;
    /* The new traversable's own state before its document exists: an empty session shed and a name the
       opener's target gives it. */
    storage_shed_session_forget(ctx);
    window_proxy_name_forget(ctx, document_window_proxy(ctx));
    navigable_open_again_load(ctx, addr);
    free(addr);
    return JS_STEP_DONE;
}

static const JSTrampStepDef open_again_def = {
    sizeof(OpenAgainFlow), open_again_step, NULL, 0,
    .algorithm = "the attacker opening this page's address again, as HTML §7.2.2.1's window open steps",
    .steps = OAG_STEPS
};

/* Registered on first use, as core/dom/user_scroll.c's machine is; the definition outlives the runtime. */
static int g_open_again_stepid = -1;

JSValue *open_again_flow(JSContext *ctx)
{
    JSValue fn;
    JSValue *base;

    if (g_open_again_stepid < 0) {
        g_open_again_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &open_again_def);
        CHECK(g_open_again_stepid >= 0, "the open-again machine could not be registered with this runtime");
    }
    fn = JS_NewCFunction2(ctx, NULL, "openAgain", 0, JS_CFUNC_step, g_open_again_stepid);
    CHECK(!JS_IsException(fn), "the open-again task callee could not be allocated");
    base = JS_FlowNewCall(ctx, fn, JS_UNDEFINED, 0, NULL, false);
    JS_FreeValue(ctx, fn);
    CHECK(base != NULL, "the open-again task frame could not be allocated");
    return base;
}
