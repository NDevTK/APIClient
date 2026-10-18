/* SVG 2 §4.6.7 Interface SVGAnimatedString: an object reflecting one content attribute of one element.
 *
 *     [Exposed=Window]
 *     interface SVGAnimatedString {
 *                attribute (DOMString or TrustedScriptURL) baseVal;
 *       readonly attribute DOMString animVal;
 *     };
 *
 * Both getters answer the attribute's value, or the empty string when it is absent; the setter sets the
 * attribute. No attribute this engine reflects through it has an initial value or a second, deprecated
 * attribute, and its one reflector (SVGElement's `className`, core/svg/svg_element.c) is not an SVGScriptElement
 * href, so the setter's Trusted Types step has no element to apply to. The read and the write go through
 * core/dom/element.h's value pair, the chokepoint the reflections use, so a write is captured in the running
 * flow's DOM delta and an attacker string keeps its provenance. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/svg/svg_animated_string.h"

/* What one object reflects: the element (a Lexbor node, which outlives every flow, so the object holds no
   reference to the wrapper that caches it) and the attribute's name (static storage, the reflector's own). Set
   once at creation and never written, so it has no state to time-travel. */
typedef struct {
    lxb_dom_node_t *node;
    const char *attr;
} SvgAnimatedString;

static JSClassID g_class;
static int       g_ready;
static int       g_id_set_base_val = -1;

static void sas_finalizer(JSRuntime *rt, JSValue obj)
{
    js_free_rt(rt, JS_GetOpaque(obj, g_class));
}

static JSClassDef g_class_def = { "SVGAnimatedString", .finalizer = sas_finalizer };

/* Web IDL §3.7.6 Attributes' brand check, a throw and not an assert: the accessors sit on a prototype and a page
   reaches one off it with `.call` on anything at all. */
static SvgAnimatedString *sas_receiver(JSContext *ctx, JSValueConst this_val)
{
    SvgAnimatedString *s = JS_GetOpaque(this_val, g_class);

    if (!s) JS_ThrowTypeError(ctx, "the receiver is not an SVGAnimatedString");
    return s;
}

/* §4.6.7's getter steps, for baseVal and animVal alike: the attribute's value when present, else the empty
   string (no initial value and no deprecated second attribute here). */
static JSValue js_sas_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    SvgAnimatedString *s = sas_receiver(ctx, this_val);
    JSValue el, v;

    (void)magic;
    if (!s) return JS_EXCEPTION;
    el = node_wrap(ctx, s->node);
    if (JS_IsException(el)) return el;
    v = element_attr_get_value(ctx, el, s->attr);
    JS_FreeValue(ctx, el);
    if (JS_IsNull(v)) return JS_NewStringLen(ctx, "", 0);
    return v;
}

/* §4.6.7's setter steps: "Otherwise, set the reflected attribute to value." The value has crossed DOMString
   already (the setter is declared IDL_DOMSTRING), and unknown external input crosses as itself. */
static JSValue js_sas_set_base_val(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    SvgAnimatedString *s = sas_receiver(ctx, this_val);
    JSValue el;

    (void)magic;
    if (!s) return JS_EXCEPTION;
    el = node_wrap(ctx, s->node);
    if (JS_IsException(el)) return el;
    element_attr_set_value(ctx, el, s->attr, val);
    JS_FreeValue(ctx, el);
    return JS_UNDEFINED;
}

void svg_animated_string_init(JSContext *ctx)
{
    DCHECK(!g_ready, "svg_animated_string_init ran twice — the class is declared once per agent");
    g_class = 0;
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class, &g_class_def) == 0,
          "the SVGAnimatedString class could not be registered");
    /* `element`, the row whose release gives this slot back: this component is declared from element_init and
       released from element_free, through core/svg/svg_element.c (core/agent_state.h). */
    agent_state_class("element", &g_class, "SVG 2 §4.6.7's SVGAnimatedString class");
    g_id_set_base_val = idl_setter_id(ctx, IDL_DOMSTRING, false, js_sas_set_base_val, 0);
    g_ready = 1;
    realm_declare_intrinsic(svg_animated_string_install_proto);
}

void svg_animated_string_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for SVGAnimatedString.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "svg_animated_string_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "SVGAnimatedString.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "SVGAnimatedString");
    idl_install_accessor(ctx, proto, "baseVal", js_sas_get, 0, g_id_set_base_val);
    idl_install_accessor(ctx, proto, "animVal", js_sas_get, 0, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

JSValue svg_animated_string_new(JSContext *ctx, lxb_dom_element_t *el, const char *attr)
{
    JSValue obj, proto;
    SvgAnimatedString *s;

    DCHECK(g_ready, "an SVGAnimatedString was asked for before svg_animated_string_init ran");
    DCHECK(el != NULL && attr != NULL && *attr, "an SVGAnimatedString was minted with nothing to reflect");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "an SVGAnimatedString was minted in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    s = js_malloc(ctx, sizeof *s);
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    s->node = lxb_dom_interface_node(el);
    s->attr = attr;
    JS_SetOpaque(obj, s);
    return obj;
}

void svg_animated_string_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto;

    DCHECK(g_ready, "the SVGAnimatedString interface was installed before its prototype was built");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "SVGAnimatedString was installed into a realm that never ran its proto build");
    node_install_interface(ctx, global, "SVGAnimatedString", proto);
    JS_FreeValue(ctx, proto);
}

void svg_animated_string_free(void)
{
    if (!g_ready) return;
    g_ready = 0;   /* the prototypes are the realms', released with their contexts */
    g_id_set_base_val = -1;
    agent_state_reached("element");
}
