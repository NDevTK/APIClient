/* Web Animations Level 1 §6.8 and §6.10's getAnimations(). See animatable.h. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/animation/animatable.h"
#include "core/animation/animation.h"
#include "core/animation/css_animations.h"
#include "core/animation/document_animations.h"
#include "core/dom/element.h"
#include "core/css/css_style_declaration.h"
#include "core/dom/named_walk.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/layout/layout_job.h"
#include "core/timing/thread_work.h"
#include "solver/rest_unit.h"

static int g_id_element = -1, g_id_root = -1;
/* Lifetime counts over the process, read by the result document's census and by nothing that decides. */
static unsigned long long g_walk_elements, g_walk_rests;

unsigned long long animatable_style_change_elements_life(void) { return g_walk_elements; }
unsigned long long animatable_style_change_rests_life(void) { return g_walk_rests; }

static uint32_t an_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

/* The element the walk updates after `n`, or NULL: `root` itself first when it is an element the call targets,
   then, for a subtree, its descendant elements in tree order. */
static lxb_dom_node_t *walk_next(lxb_dom_node_t *n, lxb_dom_node_t *root, bool self, bool subtree)
{
    if (n == NULL) {
        if (self) return root;
        return subtree ? named_walk_successor(root, root) : NULL;
    }
    return subtree ? named_walk_successor(n, root) : NULL;
}

/* Is `el`'s style readable now: its author match and its ancestors', which its `display: none` test reads? */
static bool walk_style_ask(lxb_dom_node_t *el)
{
    lxb_dom_node_t *e;

    for (e = el; e != NULL && e->type == LXB_DOM_NODE_TYPE_ELEMENT; e = e->parent)
        if (!cssom_style_ask(lxb_dom_interface_element(e))) return false;
    return true;
}

/* Is the animation's associated effect's target `root` or below it (an inclusive descendant when `inclusive`,
   §4.6.7's two definitions)? */
static bool targets_within(JSContext *ctx, JSValueConst animation, lxb_dom_node_t *root, bool inclusive,
                           bool subtree)
{
    JSValue slots = animation_slots(ctx, animation), fx = JS_GetPropertyStr(ctx, slots, "effect"), t;
    lxb_dom_node_t *n;
    bool within = false;

    JS_FreeValue(ctx, slots);
    if (!JS_IsObject(fx)) { JS_FreeValue(ctx, fx); return false; }
    t = JS_GetPropertyStr(ctx, fx, "target");
    JS_FreeValue(ctx, fx);
    n = node_of(t);
    JS_FreeValue(ctx, t);
    if (n == NULL) return false;
    if (n == root) within = inclusive;
    else if (subtree)
        for (n = n->parent; n != NULL && !within; n = n->parent) within = n == root;
    return within;
}

/* §4.6.7's relevant animations for `root`, sorted by §5.4.2's composite order (an insertion sort, so stable).
   Named residual: this pass over the Document's animations runs in one activation. The next diff takes it into the
   machine's own stage, an animation per item, with a merge sort over the answer. Its absence shows as a
   getAnimations() on a document holding thousands of animations retiring more than a work quantum at its end. */
static JSValue relevant_animations(JSContext *ctx, lxb_dom_node_t *root, bool inclusive, bool subtree)
{
    JSValue all = document_animations_all(ctx), out = JS_NewArray(ctx);
    uint32_t i, j, n = an_len(ctx, all), m = 0;

    CHECK(!JS_IsException(out), "getAnimations' answer could not be allocated");
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, all, i);

        if (!targets_within(ctx, a, root, inclusive, subtree) || !animation_relevant(ctx, a)) {
            JS_FreeValue(ctx, a);
            continue;
        }
        for (j = m++; j > 0; j--) {
            JSValue prev = JS_GetPropertyUint32(ctx, out, j - 1);

            if (animation_composite_order(ctx, prev, a) <= 0) { JS_FreeValue(ctx, prev); break; }
            JS_SetPropertyUint32(ctx, out, j, prev);
        }
        JS_SetPropertyUint32(ctx, out, j, a);
    }
    JS_FreeValue(ctx, all);
    return out;
}

/* §6.8 and §6.10's getAnimations() as one step machine owning a layout job: the style change event updates one
   element at a time under a resting drive, charged as a DOM walk, and the machine rests where the drive's
   allowance is spent or an element's author match parks; then the relevant animations are answered. A selector
   the cascade cannot decide forks the flow (layout_job_fork). */
#define GA_STAGES(X) \
    X(GA_WALK, "Web Animations §6.8 getAnimations() / §6.10 getAnimations() (the style change event over the " \
               "target's elements under a resting drive, then the relevant animations in composite order)")
enum { IDL_STEP_STAGE_BASE(GA_STAGES) GA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const GA_STEPS[] = { GA_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    LayoutJob      *job;      /* owned, declared through layout_job_step_ops; NULL before the first activation */
    lxb_dom_node_t *cursor;   /* the element the walk updates next, the running flow's node shared by address */
    uint8_t         started;  /* the walk has taken its first element (a NULL cursor after it is the end) */
} GaState;

static void ga_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    GaState *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

enum { GA_ELEMENT = 0, GA_ROOT = 1 };

static int js_get_animations_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                  JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    GaState *s = st;
    bool root_form = idl_step_magic(hdr) == GA_ROOT, self = !root_form, subtree = root_form, done = false;
    lxb_dom_node_t *target = node_of(hdr->this_val);
    LayoutJobDrive d;
    int rc;

    (void)cb_result; (void)out_cb; (void)out_argc;
    STEP_DISPATCH(GA_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(GA_WALK);
    if (root_form) {
        if (target == NULL ||
            (target->type != LXB_DOM_NODE_TYPE_DOCUMENT && target->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT)) {
            JS_ThrowTypeError(ctx, "'getAnimations' called on an object that is not a Document or ShadowRoot");
            return JS_STEP_ABRUPT;
        }
    } else {
        JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
        JSValue pseudo = idl_dict_get(ctx, options, "pseudoElement");

        DCHECK(target != NULL && target->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "Element.getAnimations ran on a receiver the declaration's brand did not refuse");
        if (!JS_IsNull(pseudo) && !JS_IsUndefined(pseudo))
            DFAIL("Web Animations §6.8 getAnimations() step 2 was given a pseudoElement — build its pseudo-element "
                  "parsing and the CSS animations a pseudo-element owns, which this build generates for none");
        JS_FreeValue(ctx, pseudo);
        subtree = idl_dict_bool(ctx, options, "subtree");
    }
    if (s->job == NULL) s->job = layout_job_new();
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    if (!s->started) {
        s->cursor = walk_next(NULL, target, self, subtree);
        s->started = 1;
    }
    rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
    layout_job_enter(s->job);
    layout_job_drive_begin_resting(&d);
    for (;;) {
        if (s->cursor == NULL) { done = true; break; }
        if (!walk_style_ask(s->cursor)) {
            DCHECK(layout_job_drive_spent(&d), "getAnimations' style change event parked an author match with the "
                                               "drive's allowance unspent, so its next activation would ask the "
                                               "same thing over the same state for ever");
            break;
        }
        thread_work_charge(THREAD_WORK_DOM_WALK, 1);
        css_animations_update(lxb_dom_interface_element(s->cursor));
        g_walk_elements++;
        s->cursor = walk_next(s->cursor, target, self, subtree);
        if (layout_job_drive_spent(&d)) break;
    }
    layout_job_drive_end(&d);
    layout_job_leave(s->job);
    rest_unit_end();
    if (layout_job_declined(s->job)) {
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (!done) {
        layout_job_rested(s->job);
        g_walk_rests++;
        return JS_STEP_YIELD;
    }
    *presult = relevant_animations(ctx, target, self, subtree);
    return JS_STEP_DONE;
}

static const IdlStepDecl GA_DECL = {
    js_get_animations_step, sizeof(GaState), ga_visit, NULL,
    "Web Animations §6.8 getAnimations() / §6.10 getAnimations()", GA_STEPS
};

void animatable_install_element_members(JSContext *ctx, JSValueConst element_proto)
{
    DCHECK(g_id_element >= 0, "Element.getAnimations was installed before Web Animations was declared");
    idl_install_method(ctx, element_proto, "getAnimations", g_id_element);
}

void animatable_install_document_or_shadow_root_members(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_root >= 0, "DocumentOrShadowRoot.getAnimations was installed before Web Animations was declared");
    idl_install_method(ctx, proto, "getAnimations", g_id_root);
}

void animatable_init(JSContext *ctx)
{
    static const IdlArgType ARGS[1] = { IDL_DICT };
    /* GetAnimationsOptions: `CSSOMString? pseudoElement = null`, `boolean subtree = false`, in sorted order. */
    static const IdlDictMember OPTIONS[] = {
        { "pseudoElement", IDL_DOMSTRING_NULLABLE, false, NULL, 0, NULL, IDL_DEFAULT_NULL },
        { "subtree", IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
    };

    DCHECK(g_id_element < 0, "animatable_init ran twice — §6.8's member is declared once per agent");
    g_id_element = idl_method_id_step(ctx, ARGS, 1, OPTIONS, 2, &GA_DECL, GA_ELEMENT);
    idl_optional_from(0);   /* `optional GetAnimationsOptions options = {}` */
    idl_this_iface(element_is, "Element");
    g_id_root = idl_method_id_step(ctx, NULL, 0, NULL, 0, &GA_DECL, GA_ROOT);
    agent_state_id("animation", &g_id_element, "Web Animations §6.8 Element.getAnimations declaration");
    agent_state_id("animation", &g_id_root, "Web Animations §6.10 DocumentOrShadowRoot.getAnimations declaration");
}

void animatable_free(void)
{
    agent_state_reached("animation");   /* animation_free undoes the row's slots, these included */
}
