/* SVG 2 §4.4.1 Interface SVGElement (`interface SVGElement : Element`), worn by every SVG-namespace element.
 * Its included mixins' members come from their owners: GlobalEventHandlers (core/events/event_target.c), HTML
 * §3.2.2's HTMLOrSVGOrMathMLElement, SVG 2's HTMLOrSVGElement by its current name (core/html/html_element.c),
 * and the CSSOM's `style`, which §4.4.1 says augments SVGElement. Bundles read `style` and `dataset` right
 * after `instanceof SVGElement`, so both are on the prototype.
 * Named residual. Not covered: SVG 2's per-element interfaces (SVGSVGElement, SVGGraphicsElement and the rest)
 * and SVGElementInstance's members. Next diff builds: an SVG name-to-interface table this file's resolver
 * reads, as core/html/html_element.c's reads HTML §3.2.2's. Absence shows as: the ReferenceError for reading
 * `SVGSVGElement` (browser/platform_names.h), and no `getBBox` on a shape. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/css/css_style_declaration.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/events/event_target.h"
#include "core/html/html_element.h"
#include "core/svg/svg_animated_string.h"
#include "core/svg/svg_element.h"

static JSClassID g_class;
static int       g_ready;
static JSAtom    g_class_name_key = JS_ATOM_NULL;   /* the [SameObject] className cache slot on a wrapper */

/* Web IDL §3.7.6 Attributes' brand check, a throw and not an assert. */
static lxb_dom_element_t *svg_receiver(JSContext *ctx, JSValueConst this_val, const char *member)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->ns == LXB_NS_SVG) return lxb_dom_interface_element(n);
    JS_ThrowTypeError(ctx, "SVGElement.%s was reached on something that is not an SVG element", member);
    return NULL;
}

/* §4.4.1: "The className IDL attribute reflects the 'class' attribute", through an SVGAnimatedString, one per
   element ([SameObject]), cached on the wrapper as `dataset` is. */
static JSValue js_svg_class_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = svg_receiver(ctx, this_val, "className");
    JSValue cached;

    (void)magic;
    if (!el) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &cached, this_val, g_class_name_key) > 0 && JS_IsObject(cached))
        return cached;
    JS_FreeValue(ctx, cached);
    cached = svg_animated_string_new(ctx, el, "class");
    if (JS_IsException(cached)) return cached;
    JS_DefinePropertyValue(ctx, (JSValue)this_val, g_class_name_key, JS_DupValue(ctx, cached), 0);
    return cached;
}

/* The nearest ancestor `svg` element in the SVG namespace, or NULL. */
static lxb_dom_node_t *nearest_svg_ancestor(lxb_dom_node_t *n)
{
    for (n = n->parent; n; n = n->parent) {
        size_t len = 0;
        const lxb_char_t *local;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_SVG) continue;
        local = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
        if (local && len == 3 && memcmp(local, "svg", 3) == 0) return n;
    }
    return NULL;
}

/* §4.4.1: ownerSVGElement is "the nearest ancestor 'svg' element"; viewportElement is "the nearest ancestor
   element that establishes an SVG viewport", which SVG 2 §8.8 Establishing a new SVG viewport says are the
   'svg' element and a 'symbol' element instanced by a 'use' element. This engine builds no 'use' shadow tree,
   so a symbol is never an instanced ancestor and the two members answer the same element. Both are null for the
   outermost svg element, which has no such ancestor. */
enum { SVG_OWNER_SVG_ELEMENT = 0, SVG_VIEWPORT_ELEMENT };

static JSValue js_svg_ancestor(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = svg_receiver(ctx, this_val,
                                         magic == SVG_OWNER_SVG_ELEMENT ? "ownerSVGElement" : "viewportElement");
    lxb_dom_node_t *anc;

    DCHECK(magic == SVG_OWNER_SVG_ELEMENT || magic == SVG_VIEWPORT_ELEMENT,
           "an SVGElement ancestor member was installed with a magic this file does not name");
    if (!el) return JS_EXCEPTION;
    anc = nearest_svg_ancestor(lxb_dom_interface_node(el));
    return anc ? node_wrap(ctx, anc) : JS_NULL;
}

/* The element-interface resolver node.c asks for an SVG-namespace element. OWNED. */
static JSValue svg_proto_for(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue proto;

    DCHECK(lxb_dom_interface_node(el)->ns == LXB_NS_SVG,
           "the SVG element-interface resolver was asked about an element in another namespace — node.c keys "
           "the resolvers by namespace, so this one is registered for LXB_NS_SVG alone");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "SVGElement.prototype was asked for in a realm that never ran its install");
    return proto;
}

void svg_element_init(JSContext *ctx)
{
    JSClassDef d = { "SVGElement" };

    DCHECK(!g_ready, "svg_element_init ran twice — the interface is declared once per agent");
    g_class = 0;
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class, &d) == 0, "the SVGElement class could not be registered");
    /* `element`, the row whose release gives this slot back (core/agent_state.h): this component is declared
       from element_init and released from element_free. */
    agent_state_class("element", &g_class, "SVG 2 §4.4.1's SVGElement class");
    g_class_name_key = JS_NewAtom(ctx, "__svgClassNameSlot");
    CHECK(g_class_name_key != JS_ATOM_NULL, "the className slot key could not be interned");
    svg_animated_string_init(ctx);
    g_ready = 1;
    realm_declare_intrinsic(svg_element_install_protos);
    node_set_element_resolver(LXB_NS_SVG, svg_proto_for);
}

void svg_element_install_protos(JSContext *ctx)
{
    JSValue base, proto, prev;

    DCHECK(g_ready, "a realm asked for SVGElement.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "svg_element_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    base = element_proto(ctx);   /* `interface SVGElement : Element` */
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "SVGElement.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "SVGElement");
    idl_install_accessor(ctx, proto, "className", js_svg_class_name, 0, -1);
    idl_install_accessor(ctx, proto, "ownerSVGElement", js_svg_ancestor, SVG_OWNER_SVG_ELEMENT, -1);
    idl_install_accessor(ctx, proto, "viewportElement", js_svg_ancestor, SVG_VIEWPORT_ELEMENT, -1);
    event_target_install_handlers(ctx, proto, EH_GLOBAL);       /* includes GlobalEventHandlers */
    html_or_svg_or_mathml_element_install(ctx, proto);         /* includes HTMLOrSVGElement */
    cssom_install_style_attribute(ctx, proto);                 /* CSSOM's ElementCSSInlineStyle */
    JS_SetClassProto(ctx, g_class, proto);
}

void svg_element_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto;

    DCHECK(g_ready, "the SVGElement interface was installed before its prototype was built");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "SVGElement was installed into a realm that never ran its proto build");
    node_install_interface(ctx, global, "SVGElement", proto);
    JS_FreeValue(ctx, proto);
    svg_animated_string_install(ctx, global);
}

void svg_element_free(JSRuntime *rt)
{
    if (!g_ready) return;
    node_set_element_resolver(LXB_NS_SVG, NULL);
    svg_animated_string_free();
    if (g_class_name_key != JS_ATOM_NULL) {
        JS_FreeAtomRT(rt, g_class_name_key);
        g_class_name_key = JS_ATOM_NULL;
    }
    g_ready = 0;
    agent_state_reached("element");
}
