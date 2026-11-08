/* CSSOM VIEW §6's element scroll position — see element_scroll_position.h for why it lives on the node's
   internal-slot record and why a never-scrolled element has none. */
#include <math.h>
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_computed_value.h"
#include "core/dom/document.h"
#include "core/dom/element_scroll_position.h"
#include "core/dom/node.h"
#include "core/layout/scroll_container.h"
#include "core/layout/used_value.h"

/* The two fields' names on the node's internal-slot record (core/dom/node.h), which other components share. */
static JSAtom g_atom_x = JS_ATOM_NULL;
static JSAtom g_atom_y = JS_ATOM_NULL;

void element_scroll_position_init(JSContext *ctx)
{
    DCHECK(g_atom_x == JS_ATOM_NULL, "element_scroll_position_init ran twice");
    g_atom_x = JS_NewAtom(ctx, "scrollPositionX");
    g_atom_y = JS_NewAtom(ctx, "scrollPositionY");
    CHECK(g_atom_x != JS_ATOM_NULL && g_atom_y != JS_ATOM_NULL,
          "the element scroll position's field names could not be interned");
}

void element_scroll_position_free(JSRuntime *rt)
{
    if (g_atom_x != JS_ATOM_NULL) { JS_FreeAtomRT(rt, g_atom_x); g_atom_x = JS_ATOM_NULL; }
    if (g_atom_y != JS_ATOM_NULL) { JS_FreeAtomRT(rt, g_atom_y); g_atom_y = JS_ATOM_NULL; }
}

double element_scroll_position(lxb_dom_element_t *el, bool vertical)
{
    JSValueConst slots;
    JSContext *rctx;
    JSValue v;
    double px = 0.0;
    int r;

    DCHECK(el != NULL, "an element's scroll position was asked of no element");
    DCHECK(g_atom_x != JS_ATOM_NULL, "an element's scroll position was asked before its names were declared");
    /* The fields are written only through the record, so an element with none was never scrolled. */
    slots = node_internal_slots_peek(lxb_dom_interface_node(el));
    if (!JS_IsObject(slots)) return 0.0;
    rctx = document_realm_of(lxb_dom_interface_node(el));
    DCHECK(rctx != NULL, "an element's scroll position was read in a document no realm was installed for");
    r = JS_GetOwnSlot(rctx, &v, slots, vertical ? g_atom_y : g_atom_x);
    DCHECK(r >= 0, "reading an element's scroll position off its internal-slot record threw");
    if (r == 0) return 0.0;
    DCHECK(JS_IsNumber(v), "an element's scroll position record holds a non-number — this file is its one "
                           "writer and writes two finite doubles");
    JS_ToFloat64(rctx, &px, v);
    JS_FreeValue(rctx, v);
    return px;
}

void element_scroll_position_set(JSContext *ctx, lxb_dom_element_t *el, double x, double y)
{
    JSValueConst slots;

    DCHECK(el != NULL && g_atom_x != JS_ATOM_NULL, "an element's scroll position was written for no element, "
                                                   "or before its names were declared");
    DCHECK(isfinite(x) && isfinite(y), "an element's scroll position was written non-finite — CSSOM VIEW §3.2's "
                                       "normalize non-finite values and §6.1's clamp run before the write");
    DCHECK(scroll_container_is(el), "an element's scroll position was written for an element that establishes no "
                                    "scrolling box — §6.1's scroll an element is stated over its associated one");
    DCHECK(document_realm_of(lxb_dom_interface_node(el)) == ctx,
           "an element's scroll position was written in a realm other than its node document's, where "
           "element_scroll_position reads it");
    slots = node_internal_slots(ctx, lxb_dom_interface_node(el));
    CHECK(JS_DefinePropertyValue(ctx, slots, g_atom_x, JS_NewFloat64(ctx, x), JS_PROP_C_W_E) >= 0 &&
          JS_DefinePropertyValue(ctx, slots, g_atom_y, JS_NewFloat64(ctx, y), JS_PROP_C_W_E) >= 0,
          "an element's scroll position could not be written to its internal-slot record, which nothing but "
          "this engine can reach, freeze or give a prototype");
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
