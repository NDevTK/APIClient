/* PAGE VISIBILITY — HTML §6.6. See page_visibility.h for why this is ONE source and one derived comparison. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/dom/page_visibility.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "solver/concolic.h"
#include "solver/decide.h"

/* THE SOURCE IDENTITY. One string, used by both members and by the engine's own read, because it is the key
   the path constraint is built from — a second spelling would be a second fact. */
#define VIS_SRC "{visibilityState}"

/* §6.6's states. This user agent PRESENTS every document it holds — it computes every step of
   update-the-rendering and omits only the paint, which is the one step with no headless equivalent — so the
   INITIAL answer is "visible". That is a UA decision the spec asks the UA to make (§6.6's "set the initial
   visibility state"), not a value invented for want of a display. */
#define VIS_VISIBLE "visible"
#define VIS_HIDDEN  "hidden"

/* §6.6's VISIBILITY STATE IS DOCUMENT STATE, and it was a constant. `visibilityState` is not a fact about the
 * user agent that never changes — §6.6 defines "update the visibility state", HTML §7.5.9 CALLS it (unloading a
 * document sets it to "hidden" between pagehide and unload), and a page that listens for `visibilitychange`
 * gets nothing from a getter that answers the same string for ever. A constant makes the read honest and the
 * WRITE impossible, and the write is half the feature.
 *
 * IT LIVES IN THIS REALM'S OWN BASELINE RECORD, the shape document.c's readiness and §8.9's animation-frame map
 * already use, for the two reasons stated there: the record is unreachable from the page, so nothing but this
 * component can write the state; and `state` is an ordinary property write, so the heap COW captures it and one
 * arm of a fork can background its document without touching its sibling's.
 *
 * THE CONCOLIC IS UNCHANGED BY THIS, and that is the point. The DOMAIN is still {hidden|visible} — a real user
 * can background the tab at any moment, so a page's `if (document.hidden)` must still fork both worlds — and
 * what the stored state supplies is the EXAMPLE. Collapsing the read to the stored concrete would delete the
 * arm and everything it reaches, which is what CLAUDE.md means by never collapsing a modelable value to
 * bare-concrete. */
static int g_vis_slot = -1;

static const char *vis_state_of(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_vis_slot), v;
    int32_t hidden = 0;

    DCHECK(JS_IsObject(rec), "a realm answered for its document's visibility state with no record");
    v = JS_GetPropertyStr(ctx, rec, "hidden");
    JS_ToInt32(ctx, &hidden, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    return hidden ? VIS_HIDDEN : VIS_VISIBLE;
}

/* `readonly attribute DocumentVisibilityState visibilityState` — the SOURCE, whose example is the state. */
static JSValue js_vis_state(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return concolic_source_wrap(ctx, "{" VIS_HIDDEN "|" VIS_VISIBLE "}", VIS_SRC,
                                JS_NewString(ctx, vis_state_of(ctx)));
}

void page_visibility_update(JSContext *ctx, bool hidden)
{
    JSValue rec;

    /* STEP 1: a state that is already the new one changes nothing and fires nothing. Without it, unloading a
       document that was already hidden would fire a second `visibilitychange` a browser never fires. */
    if ((strcmp(vis_state_of(ctx), VIS_HIDDEN) == 0) == hidden) return;
    rec = realm_value_get(ctx, g_vis_slot);
    DCHECK(JS_IsObject(rec), "a realm was asked to update a visibility state it has no record for");
    JS_SetPropertyStr(ctx, rec, "hidden", JS_NewInt32(ctx, hidden ? 1 : 0));   /* step 2 */
    JS_FreeValue(ctx, rec);
    /* THE PAGE VISIBILITY CHANGE STEPS: `visibilitychange` at the DOCUMENT, bubbling. Queued rather than
       dispatched inline, because §6.6's caller is an algorithm step and the listeners are the page's code —
       event_target_fire is the queued reach, which is a first-class flow like every other job. */
    event_target_fire(ctx, document_object(ctx), event_new(ctx, "visibilitychange", true, false), JS_UNDEFINED);
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
    /* The flow has committed to neither arm, so the example is the answer — this document's actual stored
       state, which is "visible" for a UA that presents every document it holds until something hides it. */
    {
        JSValue ex = concolic_example(ctx, v);
        hidden = JS_ToBool(ctx, ex);
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, v);
    return hidden;
}

void page_visibility_init(JSContext *ctx)
{
    /* DECLARED ONCE PER AGENT, like every other realm value. The initial state is §6.6's "set the initial
       visibility state", which for this user agent is "visible". */
    if (g_vis_slot < 0)
        g_vis_slot = realm_value_declare(ctx, "the document's §6.6 visibility state");
}

void page_visibility_install(JSContext *ctx, JSValueConst proto)
{
    /* §6.6's SET THE INITIAL VISIBILITY STATE, run where a realm gets its Document members — one record per
       realm, because the state is one Document's. It is written HERE rather than lazily on the first read: a
       record built on first touch would be built inside whichever flow happened to read first, which is the
       defect CLAUDE.md names for per-realm intrinsics, and it would make one flow's document the baseline for
       every other. */
    JSValue rec = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(rec), "page visibility: OOM building a realm's §6.6 visibility state record");
    JS_SetPropertyStr(ctx, rec, "hidden", JS_NewInt32(ctx, 0));
    realm_value_set(ctx, g_vis_slot, rec);

    idl_install_accessor(ctx, proto, "visibilityState", js_vis_state, 0, -1);
    idl_install_accessor(ctx, proto, "hidden", js_vis_hidden, 0, -1);
}
