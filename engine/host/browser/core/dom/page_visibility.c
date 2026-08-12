/* PAGE VISIBILITY — HTML §6.6. See page_visibility.h for why this is ONE source and one derived comparison. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/dom/page_visibility.h"
#include "solver/concolic.h"
#include "solver/decide.h"

/* THE SOURCE IDENTITY. One string, used by both members and by the engine's own read, because it is the key
   the path constraint is built from — a second spelling would be a second fact. */
#define VIS_SRC "{visibilityState}"

/* §6.6's states. This user agent PRESENTS every document it holds — it computes every step of
   update-the-rendering and omits only the paint, which is the one step with no headless equivalent — so the
   modelled answer is "visible". That is a UA decision the spec asks the UA to make, not a value invented for
   want of a display. */
#define VIS_VISIBLE "visible"
#define VIS_HIDDEN  "hidden"

/* `readonly attribute DocumentVisibilityState visibilityState` — the SOURCE. */
static JSValue js_vis_state(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return concolic_source_wrap(ctx, "{" VIS_HIDDEN "|" VIS_VISIBLE "}", VIS_SRC,
                                JS_NewString(ctx, VIS_VISIBLE));
}

/* `readonly attribute boolean hidden` — §6.6 defines it as `visibilityState === "hidden"`, so that is what it
   IS here: a comparison over the one source, which keys the same constraint entry a page's own
   `visibilityState === "hidden"` would. The example is the comparison's real answer over the example. */
static JSValue vis_hidden_value(JSContext *ctx)
{
    JSValue state = js_vis_state(ctx, JS_UNDEFINED, 0), r;

    if (!concolic_is(state)) {
        /* No source overlay (a conformance host): the state is the plain modelled string, so the comparison is
           the plain modelled boolean. The spec's answer, computed rather than asserted. */
        const char *s = JS_ToCString(ctx, state);
        bool hidden = s && !strcmp(s, VIS_HIDDEN);
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, state);
        return JS_NewBool(ctx, hidden);
    }
    r = concolic_new_cmp(ctx, VIS_SRC, OPCMP_EQ, VIS_HIDDEN);
    concolic_set_example(ctx, r, JS_FALSE);   /* the comparison over the example: "visible" is not "hidden" */
    JS_FreeValue(ctx, state);
    return r;
}

static JSValue js_vis_hidden(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return vis_hidden_value(ctx);
}

bool page_visibility_hidden(JSContext *ctx)
{
    JSValue v = vis_hidden_value(ctx);
    int arm;
    bool hidden;

    if (!concolic_is(v)) {                    /* a conformance host: the plain modelled boolean */
        hidden = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        return hidden;
    }
    /* CONCRETIZE ON THE PIN. The page's `if (document.hidden)` forked and its taken arm decided this exact
       predicate for this flow; a C read cannot fork, so it takes that decision rather than answering with a
       default the forked arm contradicts. Asked BY VALUE so decide.c stays the only speller of the key. */
    arm = decide_value_arm(v);
    if (arm >= 0) {
        JS_FreeValue(ctx, v);
        return arm == 1;
    }
    /* The flow has committed to neither arm, so the modelled example is the answer — which for a UA that
       presents every document it holds is "not hidden". */
    {
        JSValue ex = concolic_example(ctx, v);
        hidden = JS_ToBool(ctx, ex);
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, v);
    return hidden;
}

void page_visibility_install(JSContext *ctx, JSValueConst proto)
{
    idl_install_accessor(ctx, proto, "visibilityState", js_vis_state, 0, -1);
    idl_install_accessor(ctx, proto, "hidden", js_vis_hidden, 0, -1);
}
