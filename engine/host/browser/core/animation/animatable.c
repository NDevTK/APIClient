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
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/layout/layout_job.h"

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

/* The style change event over `root`'s elements: `root` itself when it is an element and `self`, and every
   element below it when `subtree`. */
static void style_change_event(lxb_dom_node_t *root, bool self, bool subtree)
{
    LayoutJob *job = layout_job_new();   /* this activation's, so each computed value reads its cascade record */
    lxb_dom_node_t *n;

    layout_job_enter(job);
    if (self && root->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        css_animations_update(lxb_dom_interface_element(root));
        g_walk_elements++;
    }
    for (n = subtree ? root->first_child : NULL; n != NULL && n != root;) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            css_animations_update(lxb_dom_interface_element(n));
            g_walk_elements++;
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n != root && n->next == NULL) n = n->parent;
        if (n == root) break;
        n = n->next;
    }
    layout_job_leave(job);
    layout_job_free(job);
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

/* §4.6.7's relevant animations for `root`, sorted by §5.4.2's composite order (an insertion sort, so stable). */
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

/* §6.8's `sequence<Animation> getAnimations(optional GetAnimationsOptions options = {})`. */
static JSValue js_element_get_animations(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    lxb_dom_node_t *target = node_of(this_val);
    JSValue pseudo;
    bool subtree;

    (void)magic;
    DCHECK(target != NULL && target->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "Element.getAnimations ran on a receiver the declaration's brand did not refuse");
    pseudo = idl_dict_get(ctx, options, "pseudoElement");
    if (!JS_IsNull(pseudo) && !JS_IsUndefined(pseudo))
        DFAIL("Web Animations §6.8 getAnimations() step 2 was given a pseudoElement — build its pseudo-element "
              "parsing and the CSS animations a pseudo-element owns, which this build generates for none");
    JS_FreeValue(ctx, pseudo);
    subtree = idl_dict_bool(ctx, options, "subtree");
    style_change_event(target, true, subtree);
    return relevant_animations(ctx, target, true, subtree);
}

/* §6.10's `sequence<Animation> getAnimations()`: the relevant animations for a subtree of the document or
   shadow root, whose descendants (not itself) are the targets. */
static JSValue js_root_get_animations(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *root = node_of(this_val);

    (void)argc;
    (void)argv;
    (void)magic;
    if (root == NULL || (root->type != LXB_DOM_NODE_TYPE_DOCUMENT && root->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT))
        return JS_ThrowTypeError(ctx, "'getAnimations' called on an object that is not a Document or ShadowRoot");
    style_change_event(root, false, true);
    return relevant_animations(ctx, root, false, true);
}

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
    g_id_element = idl_method_id_dict(ctx, ARGS, 1, OPTIONS, 2, js_element_get_animations, 0);
    idl_optional_from(0);   /* `optional GetAnimationsOptions options = {}` */
    idl_this_iface(element_is, "Element");
    g_id_root = idl_method_id(ctx, NULL, 0, js_root_get_animations, 0);
    agent_state_id("animation", &g_id_element, "Web Animations §6.8 Element.getAnimations declaration");
    agent_state_id("animation", &g_id_root, "Web Animations §6.10 DocumentOrShadowRoot.getAnimations declaration");
}

void animatable_free(void)
{
    agent_state_reached("animation");   /* animation_free undoes the row's slots, these included */
}
