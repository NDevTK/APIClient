/* CSSOM VIEW §6's element scroll position — see element_scroll_position.h for why it is an internal-slot record
   on the element's wrapper and why a never-scrolled element has none. */
#include <math.h>
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_computed_value.h"
#include "core/dom/document.h"
#include "core/dom/element_scroll_position.h"
#include "core/dom/node.h"
#include "core/idl_slots.h"
#include "core/layout/scroll_container.h"
#include "core/layout/used_value.h"

#define ESP_X "x"
#define ESP_Y "y"

static JSValue g_key = JS_UNDEFINED;
static JSAtom  g_atom = JS_ATOM_NULL;

void element_scroll_position_init(JSContext *ctx)
{
    DCHECK(g_atom == JS_ATOM_NULL, "element_scroll_position_init ran twice — a second key would leave every "
                                   "element already scrolled with no position under it");
    g_key = JS_NewSymbol(ctx, "elementScrollPosition", false);
    CHECK(!JS_IsException(g_key), "the element scroll position key could not be allocated");
    g_atom = JS_ValueToAtom(ctx, g_key);
    CHECK(g_atom != JS_ATOM_NULL, "the element scroll position key could not be interned");
}

void element_scroll_position_free(JSRuntime *rt)
{
    if (g_atom != JS_ATOM_NULL) { JS_FreeAtomRT(rt, g_atom); g_atom = JS_ATOM_NULL; }
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
}

double element_scroll_position(lxb_dom_element_t *el, bool vertical)
{
    JSValueConst wrap;
    JSContext *rctx;
    JSValue rec, v;
    double px = 0.0;

    DCHECK(el != NULL, "an element's scroll position was asked of no element");
    DCHECK(g_atom != JS_ATOM_NULL, "an element's scroll position was asked before its key was declared");
    /* A record is written only through the wrapper, so an element with none was never scrolled. */
    wrap = node_wrap_peek(lxb_dom_interface_node(el));
    if (!JS_IsObject(wrap)) return 0.0;
    rctx = document_realm_of(lxb_dom_interface_node(el));
    DCHECK(rctx != NULL, "an element's scroll position was read in a document no realm was installed for");
    if (JS_GetOwnSlot(rctx, &rec, wrap, g_atom) <= 0) return 0.0;
    v = JS_GetPropertyStr(rctx, rec, vertical ? ESP_Y : ESP_X);
    DCHECK(JS_IsNumber(v), "an element's scroll position record holds a non-number — this file is its one "
                           "writer and writes two finite doubles");
    JS_ToFloat64(rctx, &px, v);
    JS_FreeValue(rctx, v);
    JS_FreeValue(rctx, rec);
    return px;
}

void element_scroll_position_set(JSContext *ctx, lxb_dom_element_t *el, double x, double y)
{
    JSValue wrap, rec;

    DCHECK(el != NULL && g_atom != JS_ATOM_NULL, "an element's scroll position was written for no element, or "
                                                 "before its key was declared");
    DCHECK(isfinite(x) && isfinite(y), "an element's scroll position was written non-finite — CSSOM VIEW §3.2's "
                                       "normalize non-finite values and §6.1's clamp run before the write");
    DCHECK(scroll_container_is(el), "an element's scroll position was written for an element that establishes no "
                                    "scrolling box — §6.1's scroll an element is stated over its associated one");
    DCHECK(document_realm_of(lxb_dom_interface_node(el)) == ctx,
           "an element's scroll position was written in a realm other than its node document's, where "
           "element_scroll_position reads it");
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    CHECK(JS_IsObject(wrap), "an element being scrolled could not be wrapped");
    if (JS_GetOwnSlot(ctx, &rec, wrap, g_atom) <= 0) {
        rec = idl_slots_new(ctx);
        CHECK(!JS_IsException(rec), "an element's scroll position record could not be allocated");
        JS_DefinePropertyValue(ctx, wrap, g_atom, JS_DupValue(ctx, rec), JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    }
    JS_SetPropertyStr(ctx, rec, ESP_X, JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, rec, ESP_Y, JS_NewFloat64(ctx, y));
    JS_FreeValue(ctx, rec);
    JS_FreeValue(ctx, wrap);
}

void element_scroll_position_displacement(lxb_dom_element_t *el, double *dx, double *dy)
{
    lxb_dom_element_t *c = el;

    DCHECK(el != NULL && dx != NULL && dy != NULL, "a box's scroll displacement was asked of no element or into "
                                                   "nowhere");
    *dx = 0.0;
    *dy = 0.0;
    /* CSS 2.1 §10.1's chain, link by link. A fixed box's containing block is the viewport, which no element
       scrolls; every other link is an element or the initial containing block (NULL), where the chain ends. */
    while (!css_computed_keyword_is(c, "position", CSS_KEYWORD_FIXED)) {
        UsedValueCbLink link = used_value_containing_block_link(c);

        c = link.element;
        if (c == NULL) return;
        if (scroll_container_is(c)) {
            *dx += element_scroll_position(c, false);
            *dy += element_scroll_position(c, true);
        }
    }
}
