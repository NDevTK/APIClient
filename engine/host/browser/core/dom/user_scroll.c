/* The person scrolls a target into view.
 *
 * A user who scrolls until an element shows performs what CSSOM VIEW §6.1 "Element Scrolling Members"' scroll a
 * target into view computes: each scrolling box from the target outwards is moved so the target is inside it.
 * block and inline "nearest" are the alignment a person scrolling until the element appears stops at, and the
 * scroll is instant (core/dom/element_scrolling.h). Every effect a real scroll has follows from the boxes'
 * positions: §3.1's perform a scroll marks the scroll event pending, and the next update the rendering runs
 * Intersection Observer §3.2.10 over the moved geometry.
 *
 * The walk is the one core/dom/element_view.c's `scrollIntoView` step 8 drives, behind one layout job, one unit
 * per step, so the act rests and parks like any other work. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/document.h"
#include "core/dom/element_scrolling.h"
#include "core/dom/element_view.h"
#include "core/dom/node.h"
#include "core/dom/user_scroll.h"
#include "core/layout/layout_job.h"

#define USF_STAGES(X)                                                                                         \
    X(USF_BEGIN, "CSSOM VIEW §6.1 \"Element Scrolling Members\" scroll a target into view, for the person's "  \
                 "scroll (the target, block and inline \"nearest\")")                                          \
    X(USF_BOX,   "CSSOM VIEW §6.1 \"Element Scrolling Members\" scroll a target into view step 2, one unit of " \
                 "its ancestor walk")
enum { USF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const USF_STEPS[] = { USF_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;                   /* first — the driver writes the def and the operand bounds through it */
    ElementScrollingIntoView walk;   /* DOM nodes of the running flow, which a fork shares by address */
    LayoutJob *job;                  /* owned, declared through layout_job_step_ops; NULL until USF_BOX */
} UserScrollFlow;

static void user_scroll_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    UserScrollFlow *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

static int user_scroll_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    UserScrollFlow *s = st;
    bool more;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    STEP_DISPATCH(USF_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(USF_BEGIN);
    {
        /* The target is the frame's own argument, its wrapper, so a fork and a park carry it. */
        lxb_dom_node_t *n = node_of(step_arg(&s->hdr, 0));

        DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "the person's scroll was built over an argument that is not an element");
        s->job = NULL;
        element_scrolling_into_view_begin(&s->walk, lxb_dom_interface_element(n), SCROLL_LOGICAL_NEAREST,
                                          SCROLL_LOGICAL_NEAREST, NULL);
        s->job = layout_job_new(LAYOUT_JOB_STEP);
        s->hdr.stage = USF_BOX;
    }

    STEP_ARM(USF_BOX);
    layout_job_enter(s->job);
    more = element_scrolling_into_view_next(&s->walk, "auto");
    layout_job_leave(s->job);
    if (more) {
        layout_job_rested(s->job);
        return JS_STEP_YIELD;
    }
    return JS_STEP_DONE;
}

static const JSTrampStepDef user_scroll_def = {
    sizeof(UserScrollFlow), user_scroll_step, NULL, 0,
    .visit = user_scroll_visit,
    .algorithm = "the person's scroll, as CSSOM VIEW §6.1 \"Element Scrolling Members\" scroll a target into view",
    .steps = USF_STEPS
};

/* Registered on first use, as core/html/close_request.c's task machine is; the definition outlives the runtime. */
static int g_user_scroll_stepid = -1;

JSValue *user_scroll_flow(JSContext *ctx, lxb_dom_element_t *target)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(target);
    JSContext *dctx = document_realm_of(lxb_dom_interface_node(n->owner_document));
    JSValue fn, arg;
    JSValue *base;

    DCHECK(dctx != NULL, "the person scrolled an element whose document has no realm");
    DCHECK(element_view_has_box(n), "the person scrolled an element with no associated box — "
                                         "scroll a target into view has no position to reach for it");
    if (g_user_scroll_stepid < 0) {
        g_user_scroll_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &user_scroll_def);
        CHECK(g_user_scroll_stepid >= 0, "the person's scroll machine could not be registered with this runtime");
    }
    fn = JS_NewCFunction2(dctx, NULL, "userScroll", 1, JS_CFUNC_step, g_user_scroll_stepid);
    CHECK(!JS_IsException(fn), "the person's scroll task callee could not be allocated");
    arg = node_wrap(dctx, n);
    CHECK(!JS_IsException(arg), "the person's scroll target could not be wrapped");
    base = JS_FlowNewCall(dctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&arg, false);
    JS_FreeValue(dctx, arg);
    JS_FreeValue(dctx, fn);
    CHECK(base != NULL, "the person's scroll task frame could not be allocated");
    return base;
}
