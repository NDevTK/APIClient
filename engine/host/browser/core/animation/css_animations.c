/* CSS Animations' CSSAnimation objects. See css_animations.h. */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/animation/animation.h"
#include "core/animation/animation_timeline.h"
#include "core/animation/css_animations.h"
#include "core/css/css_animation_shorthand.h"
#include "core/css/css_author_sheets.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_style_declaration.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/realm.h"

static JSRuntime *g_rt;
static JSClassID g_class;              /* CSSAnimation's prototype slot; instances are Animation's class */
static JSValue g_key = JS_UNDEFINED;   /* the private Symbol an element's CSS animations live under */

static uint32_t ca_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

/* ---- the computed values, as a coordinated value list -------------------------------------------------- */

/* A computed list's items. Items are separated by top-level commas; a `<string>` keeps its commas. */
typedef struct { char **v; int n; } CaList;

static void ca_list_parse(const char *text, CaList *out)
{
    size_t i, start = 0, len = strlen(text);
    char quote = 0;

    out->v = NULL;
    out->n = 0;
    for (i = 0; i <= len; i++) {
        char c = i < len ? text[i] : ',';

        if (quote) {
            if (c == '\\' && i + 1 < len) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c != ',') continue;
        {
            size_t s = start, e = i;
            char *item;

            while (s < e && text[s] == ' ') s++;
            while (e > s && text[e - 1] == ' ') e--;
            item = malloc(e - s + 1);
            CHECK(item != NULL, "CSS Animations: OOM reading a computed list");
            memcpy(item, text + s, e - s);
            item[e - s] = '\0';
            out->v = realloc(out->v, (size_t)(out->n + 1) * sizeof *out->v);
            CHECK(out->v != NULL, "CSS Animations: OOM reading a computed list");
            out->v[out->n++] = item;
            start = i + 1;
        }
    }
}

static void ca_list_free(CaList *l)
{
    int i;

    for (i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

/* The longhand's computed list, read through CSS Values 4 Appendix A "Coordinating List-Valued Properties": item `i` of
   a list shorter than the base's repeats it. */
static void ca_longhand(lxb_dom_element_t *el, const char *name, CaList *out)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "a CSS Animations longhand computed to no value — its row derives one for every element");
    ca_list_parse(v, out);
    free(v);
    DCHECK(out->n > 0, "a CSS Animations longhand computed to an empty list — §4's grammar admits none");
}

static const char *ca_item(const CaList *l, int i) { return l->v[i % l->n]; }

/* A `<keyframes-name>` item as the name it is: a `<string>`'s contents, or the identifier. OWNED. */
static char *ca_name_of(const char *item)
{
    size_t n = strlen(item), i, k = 0;
    char *out = malloc(n + 1);

    CHECK(out != NULL, "CSS Animations: OOM reading an animation name");
    if (n >= 2 && (item[0] == '"' || item[0] == '\'')) {
        for (i = 1; i + 1 < n; i++) {
            if (item[i] == '\\' && i + 2 < n) i++;
            out[k++] = item[i];
        }
        out[k] = '\0';
        return out;
    }
    memcpy(out, item, n + 1);
    return out;
}

/* The associated effect's timing (Web Animations §4.6) for the `i`th animation: CSS Animations Level 2 §4's
   correspondence of each longhand to its Web Animations concept. */
static JSValue ca_effect(JSContext *ctx, JSValueConst target, const CaList *lists, int i)
{
    JSValue fx = JS_NewObjectProto(ctx, JS_NULL);
    double duration = 0, delay = 0, iterations = 1;
    const char *count = ca_item(&lists[3], i);
    bool parsed;

    CHECK(!JS_IsException(fx), "a CSS animation's effect timing could not be allocated");
    parsed = css_animation_time_ms(ca_item(&lists[0], i), &duration);
    DCHECK(parsed, "a computed `animation-duration` item is not a time — its computed value is one by definition");
    parsed = css_animation_time_ms(ca_item(&lists[2], i), &delay);
    DCHECK(parsed, "a computed `animation-delay` item is not a time — its computed value is one by definition");
    iterations = strcmp(count, "infinite") == 0 ? INFINITY : strtod(count, NULL);
    JS_SetPropertyStr(ctx, fx, "duration", JS_NewFloat64(ctx, duration));
    JS_SetPropertyStr(ctx, fx, "delay", JS_NewFloat64(ctx, delay));
    JS_SetPropertyStr(ctx, fx, "iterations", JS_NewFloat64(ctx, iterations));
    JS_SetPropertyStr(ctx, fx, "endDelay", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, fx, "direction", JS_NewString(ctx, ca_item(&lists[4], i)));
    JS_SetPropertyStr(ctx, fx, "fill", JS_NewString(ctx, ca_item(&lists[5], i)));
    /* Web Animations §4.6.7's effect target: the element the keyframes apply to, here the owning element. */
    JS_SetPropertyStr(ctx, fx, "target", JS_DupValue(ctx, target));
    return fx;
}

/* ---- the element's list --------------------------------------------------------------------------------- */

static JSValue ca_list_get(JSContext *ctx, JSValueConst wrap)
{
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    JSValue l = JS_UNDEFINED;

    CHECK(k != JS_ATOM_NULL, "the CSS animations key could not be reached");
    if (JS_GetOwnSlot(ctx, &l, wrap, k) <= 0) l = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsArray(l)) {
        JS_FreeValue(ctx, l);
        l = JS_NewArray(ctx);
        CHECK(!JS_IsException(l), "an element's CSS animations could not be allocated");
    }
    return l;
}

static void ca_list_set(JSContext *ctx, JSValueConst wrap, JSValue list)
{
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    CHECK(k != JS_ATOM_NULL, "the CSS animations key could not be reached");
    JS_DefinePropertyValue(ctx, (JSValue)wrap, k, list, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_FreeAtom(ctx, k);
}

static JSContext *ca_realm(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSContext *ctx = document_realm_of(lxb_dom_interface_node(n->owner_document));

    DCHECK(ctx != NULL, "an element whose CSS animations were asked for has no document realm");
    return ctx;
}

JSValue css_animations_of(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue wrap = node_wrap(ctx, lxb_dom_interface_node(el)), l;

    DCHECK(JS_IsObject(wrap), "an element has no wrapper to hold its CSS animations");
    l = ca_list_get(ctx, wrap);
    JS_FreeValue(ctx, wrap);
    return l;
}

/* CSS Animations Level 2 §2.1: an element displayed as `none`, or inside one, runs no animations. */
static bool ca_display_none(lxb_dom_element_t *el)
{
    lxb_dom_element_t *e;

    for (e = el; e != NULL; e = css_parent_element(e))
        if (css_computed_display_is(e, CSS_KEYWORD_NONE)) return true;
    return false;
}

static bool ca_name_is(JSContext *ctx, JSValueConst animation, const char *name)
{
    JSValue slots = animation_slots(ctx, animation), v = JS_GetPropertyStr(ctx, slots, "cssName");
    const char *s = JS_ToCString(ctx, v);
    bool is = s != NULL && strcmp(s, name) == 0;

    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, slots);
    return is;
}

static const char *const LONGHAND_ORDER[] = {
    "animation-duration", "animation-timing-function", "animation-delay", "animation-iteration-count",
    "animation-direction", "animation-fill-mode", "animation-play-state",
};

void css_animations_update(lxb_dom_element_t *el)
{
    JSContext *ctx = ca_realm(el);
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    JSValue wrap = node_wrap(ctx, node), existing = ca_list_get(ctx, wrap), fresh, out, proto, tl;
    CaList names = { NULL, 0 }, lists[7];
    struct CssAuthorSheets *sheets;
    int i, j, nl = (int)(sizeof LONGHAND_ORDER / sizeof LONGHAND_ORDER[0]);
    uint32_t k, m;

    DCHECK(g_rt != NULL, "an element's CSS animations were updated before CSSAnimation was declared");
    if (!ca_display_none(el)) ca_longhand(el, "animation-name", &names);
    for (j = 0; j < nl; j++) {
        lists[j].v = NULL;
        lists[j].n = 0;
        if (names.n) ca_longhand(el, LONGHAND_ORDER[j], &lists[j]);
    }
    sheets = cssom_author_sheets(lxb_dom_interface_node(node->owner_document));
    proto = JS_GetClassProto(ctx, g_class);
    tl = animation_timeline_default(ctx);
    fresh = JS_NewArray(ctx);
    CHECK(!JS_IsException(fresh), "an element's new CSS animations could not be allocated");
    /* Level 1 §2: "iterating over the new list of animations from last to first, and, for each animation,
       finding the last matching animation in the list of existing animations". */
    for (i = names.n - 1; i >= 0; i--) {
        char *name = ca_name_of(names.v[i]);
        JSValue anim = JS_UNDEFINED, slots, fx;

        if (strcmp(names.v[i], "none") == 0 || !css_author_sheets_keyframes(sheets, name)) { free(name); continue; }
        for (k = ca_len(ctx, existing); k-- > 0;) {
            JSValue a = JS_GetPropertyUint32(ctx, existing, k);

            if (!JS_IsUndefined(a) && ca_name_is(ctx, a, name)) {
                anim = a;
                JS_SetPropertyUint32(ctx, existing, k, JS_UNDEFINED);   /* "removed from the existing list" */
                break;
            }
            JS_FreeValue(ctx, a);
        }
        fx = ca_effect(ctx, wrap, lists, i);
        if (JS_IsUndefined(anim)) {
            anim = animation_create(ctx, proto, tl, fx, false);
            slots = animation_slots(ctx, anim);
            JS_SetPropertyStr(ctx, slots, "owner", JS_DupValue(ctx, wrap));
            JS_SetPropertyStr(ctx, slots, "cssName", JS_NewString(ctx, name));
        } else {
            slots = animation_slots(ctx, anim);
            JS_SetPropertyStr(ctx, slots, "effect", JS_DupValue(ctx, fx));
        }
        JS_SetPropertyStr(ctx, slots, "cssIndex", JS_NewInt32(ctx, i));
        JS_FreeValue(ctx, fx);
        JS_FreeValue(ctx, slots);
        JS_SetPropertyUint32(ctx, fresh, (uint32_t)i, anim);
        free(name);
    }
    /* An existing animation nothing matched is canceled and disassociated (Level 2 §2.1); its owning element at
       that moment is what CSS Animations Level 2 §2.2 orders its cancel event by. */
    for (k = 0, m = ca_len(ctx, existing); k < m; k++) {
        JSValue a = JS_GetPropertyUint32(ctx, existing, k), slots;

        if (JS_IsUndefined(a)) continue;
        animation_cancel(ctx, a);
        slots = animation_slots(ctx, a);
        JS_SetPropertyStr(ctx, slots, "formerOwner", JS_GetPropertyStr(ctx, slots, "owner"));
        JS_SetPropertyStr(ctx, slots, "owner", JS_NULL);
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, a);
    }
    /* The new list without the names that generated nothing, in `animation-name` order; then Level 2 §4.5's
       play state, applied where it is newly running or newly paused. */
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "an element's CSS animations could not be allocated");
    for (i = 0, m = 0; i < names.n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, fresh, (uint32_t)i), slots, prev;
        const char *state = ca_item(&lists[6], i), *p;
        bool newly;

        if (JS_IsUndefined(a)) continue;
        slots = animation_slots(ctx, a);
        prev = JS_GetPropertyStr(ctx, slots, "cssPlayState");
        p = JS_IsString(prev) ? JS_ToCString(ctx, prev) : NULL;
        newly = p == NULL || strcmp(p, state) != 0;
        JS_FreeCString(ctx, p);
        JS_FreeValue(ctx, prev);
        if (newly) {
            bool ok = strcmp(state, "paused") == 0 ? animation_pause(ctx, a) : animation_play(ctx, a, false);

            /* Web Animations §4.5.9 throws for a page-reversed playback rate over an infinite effect; the style
               change event that ran the procedure has no caller to hand the exception to. */
            if (!ok) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_SetPropertyStr(ctx, slots, "cssPlayState", JS_NewString(ctx, state));
        }
        JS_FreeValue(ctx, slots);
        JS_SetPropertyUint32(ctx, out, m++, a);
    }
    ca_list_set(ctx, wrap, out);
    JS_FreeValue(ctx, fresh);
    JS_FreeValue(ctx, tl);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, existing);
    JS_FreeValue(ctx, wrap);
    for (j = 0; j < nl; j++) ca_list_free(&lists[j]);
    ca_list_free(&names);
}

/* ---- CSS Animations Level 2 §2.2 composite order ------------------------------------------------------------- */

static JSValue ca_slot(JSContext *ctx, JSValueConst animation, const char *k)
{
    JSValue slots = animation_slots(ctx, animation), v = JS_GetPropertyStr(ctx, slots, k);

    JS_FreeValue(ctx, slots);
    return v;
}

static int ca_depth(lxb_dom_node_t *n)
{
    int d = 0;

    for (; n->parent; n = n->parent) d++;
    return d;
}

/* Tree order of two nodes: negative when `a` precedes `b`. */
static int ca_tree_order(lxb_dom_node_t *a, lxb_dom_node_t *b)
{
    int da = ca_depth(a), db = ca_depth(b);
    lxb_dom_node_t *x;

    if (a == b) return 0;
    for (; da > db; da--) { a = a->parent; if (a == b) return 1; }    /* `b` is an ancestor of `a` */
    for (; db > da; db--) { b = b->parent; if (b == a) return -1; }   /* `a` is an ancestor of `b` */
    while (a->parent != b->parent) { a = a->parent; b = b->parent; }
    for (x = a; x; x = x->next)
        if (x == b) return -1;
    return 1;
}

/* The element CSS Animations Level 2 §2.2 orders `animation` by: its owning element, or, for one canceled off it
   and not yet back on the global animation list, the owning element at the moment it was canceled. JS_NULL for
   neither. */
static JSValue ca_order_owner(JSContext *ctx, JSValueConst animation, bool *canceled)
{
    JSValue o = ca_slot(ctx, animation, "owner"), listed;
    int32_t pos = 0;

    *canceled = false;
    if (JS_IsObject(o)) return o;
    JS_FreeValue(ctx, o);
    listed = ca_slot(ctx, animation, "listed");
    JS_ToInt32(ctx, &pos, listed);
    if (pos != 0) return JS_NULL;
    o = ca_slot(ctx, animation, "formerOwner");
    if (!JS_IsObject(o)) { JS_FreeValue(ctx, o); return JS_NULL; }
    *canceled = true;
    return o;
}

bool css_animations_owned(JSContext *ctx, JSValueConst animation)
{
    bool canceled;
    JSValue o = ca_order_owner(ctx, animation, &canceled);
    bool owned = JS_IsObject(o);

    JS_FreeValue(ctx, o);
    return owned;
}

int css_animations_order(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    bool ka, kb;
    JSValue oa = ca_order_owner(ctx, a, &ka), ob = ca_order_owner(ctx, b, &kb), ia, ib;
    int r;
    int32_t xa = 0, xb = 0;

    DCHECK(JS_IsObject(oa) && JS_IsObject(ob), "CSS Animations §2.2 ordered an animation with no owning element");
    r = ca_tree_order(node_of(oa), node_of(ob));
    /* Within one element, a canceled animation's position sorts before every position not canceled. */
    if (r == 0 && ka != kb) r = ka ? -1 : 1;
    if (r == 0) {
        ia = ca_slot(ctx, a, "cssIndex");
        ib = ca_slot(ctx, b, "cssIndex");
        JS_ToInt32(ctx, &xa, ia);
        JS_ToInt32(ctx, &xb, ib);
        r = xa < xb ? -1 : xa > xb;
    }
    JS_FreeValue(ctx, oa);
    JS_FreeValue(ctx, ob);
    return r;
}

/* ---- CSS Animations Level 2 §6.1 The CSSAnimation interface -------------------------------------------------- */

/* `readonly attribute CSSOMString animationName`: "the value of the animation-name property that caused this
   object to be generated". */
static JSValue js_css_animation_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue v;

    (void)magic;
    if (!animation_is(ctx, this_val)) return JS_ThrowTypeError(ctx, "'animationName' getter called on a non-CSSAnimation");
    v = ca_slot(ctx, this_val, "cssName");
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowTypeError(ctx, "'animationName' getter called on a non-CSSAnimation");
    }
    return v;
}

static void css_animations_install_realm(JSContext *ctx)
{
    JSValue base = animation_proto(ctx), proto, prev;

    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "css_animations_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObjectProto(ctx, base);   /* Level 2 §6.1: `interface CSSAnimation : Animation` */
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "CSSAnimation.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CSSAnimation");
    idl_install_accessor(ctx, proto, "animationName", js_css_animation_name, 0, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

void css_animations_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_class), ctor;

    DCHECK(!JS_IsNull(proto), "CSSAnimation was installed in a realm with no prototype for it");
    ctor = idl_interface_object(ctx, "CSSAnimation", proto);
    CHECK(!JS_IsException(ctor), "the CSSAnimation interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "CSSAnimation", ctor);
}

void css_animations_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_rt == NULL, "css_animations_init ran twice — CSSAnimation is declared once per agent");
    g_rt = rt;
    {
        JSClassDef def = { "CSSAnimation" };

        JS_NewClassID(rt, &g_class);
        CHECK(JS_NewClass(rt, g_class, &def) == 0, "CSSAnimation's prototype slot could not be declared");
    }
    g_key = JS_NewSymbol(ctx, "cssAnimations", false);
    CHECK(!JS_IsException(g_key), "the CSS animations key could not be allocated");
    agent_state_ptr("animation", &g_rt, "the runtime CSS Animations Level 2 §6.1 was declared in");
    agent_state_class("animation", &g_class, "CSS Animations Level 2 §6.1 CSSAnimation's prototype slot");
    agent_state_value("animation", &g_key, "the private key an element's CSS animations live under");
    realm_declare_intrinsic(css_animations_install_realm);
}

void css_animations_free(JSRuntime *rt)
{
    DCHECK(g_rt == rt, "CSSAnimation was released against a runtime other than the one it was declared in");
    JS_FreeValueRT(rt, g_key);
    agent_state_reached("animation");   /* animation_free undoes the row's slots, these included */
}
