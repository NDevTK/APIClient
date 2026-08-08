/* HTMLIFrameElement's NAVIGABLE — HTML §4.8.5, and the element half of the cross-document machinery.
 *
 * §4.8.5's insertion steps CREATE A CHILD NAVIGABLE when an `<iframe>` is inserted into a document, and they do
 * it THERE — synchronously, in the same turn as the append. `frame.contentWindow` answers on the very next line
 * in every browser, and the spec files say so directly: `const otherW = document.body.appendChild(frame)
 * .contentWindow` then reads `otherW.self`. It is not lazily on the first `contentWindow` either, which is
 * observably different — a page may insert a frame and read `window.length` without ever touching it.
 *
 * IT WAS A QUEUED TASK, AND THAT WAS A WORKAROUND FOR AN IDENTITY PROBLEM, NOT A SCHEDULING ONE. Creating the
 * child meant asking the host to mint its document id, asking suspends, and the insertion-steps drain may not
 * suspend — so the work was pushed into a task that could. The fix is not a better place to suspend: it is not
 * to suspend. A document is NAMED and a child's name is minted locally (world.h), so creation is a mint, a
 * notice and an object allocation, none of which can block. The task is gone and so is the ordering lie its
 * comment recorded.
 *
 * THE NAVIGABLE IS PER-FLOW, and it is kept on the ELEMENT'S WRAPPER rather than in a table beside it. A flow
 * that inserted the frame has one; a sibling that never did must not see it, and a C-side registry would show
 * it to both — silently, because a proxy that exists in the wrong world answers reads perfectly well. A hidden
 * own slot on the wrapper is an ordinary property write on a baseline object, so the heap COW delta isolates it
 * with nothing added here; a forked arm inherits its parent's through the delta and issues no second create,
 * which is also what keeps one child document per WORLD rather than one per flow. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "check.h"
#include "quickjs.h"
#include "core/html/html_iframe.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/document.h"
#include "core/frame/navigable.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"

/* The wrapper slot the navigable lives in. Not a name a page would write, and read through JS_GetOwnSlot so no
   prototype lookup and no page code can intercept it — the same arrangement the custom-element upgrade mark
   uses, and for the same reason. */
static JSAtom g_atom_navigable = JS_ATOM_NULL;

/* This element's navigable IN THIS FLOW, or JS_UNDEFINED. */
static JSValue iframe_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue v;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_navigable) <= 0) return JS_UNDEFINED;
    return v;
}

bool iframe_has_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue v = iframe_navigable(ctx, wrap);
    bool had = !JS_IsUndefined(v);
    JS_FreeValue(ctx, v);
    return had;
}

void iframe_create_navigable(JSContext *ctx, JSValueConst wrap)
{
    char *src, *name;
    JSValue proxy;

    DCHECK(g_atom_navigable != JS_ATOM_NULL, "an iframe's navigable was created before iframe_init ran");
    DCHECK(JS_IsObject(wrap), "something that is not an element wrapper was given a child navigable");
    if (iframe_has_navigable(ctx, wrap)) return;   /* this flow already has one */

    /* §4.8.5's "process the iframe attributes": `src` is the child's initial address, and an absent or empty
       one is the initial about:blank Document. `name` becomes the BROWSING CONTEXT's name — which is why
       renaming the window later leaves the attribute alone, and why removing the frame empties the name while
       the attribute keeps its value. Both are read through the DOM chokepoint so the read stays in the running
       flow's delta — a flow that set `src` and a sibling that did not create different children, which is the
       whole reason the navigable is per-flow. */
    src  = element_attr_get(ctx, wrap, "src");
    name = element_attr_get(ctx, wrap, "name");
    proxy = navigable_create(ctx, src, name, true);
    /* §4.8.5 has no "did not parse" branch the way §7.4 does: an `<iframe src="::">` still has a navigable,
       holding the initial about:blank it was created with. */
    if (JS_IsUndefined(proxy)) proxy = navigable_create(ctx, NULL, name, true);
    free(src);
    free(name);
    DCHECK(!JS_IsUndefined(proxy), "an about:blank child navigable could not be created, which has no failing "
                                   "branch — its address needs no parse and its origin is this document's");
    /* WRITABLE, NOT CONFIGURABLE. The removing steps below CLEAR this slot, and a slot defined with no flags
       at all can be neither rewritten nor deleted — the destroy silently did nothing and a removed frame kept
       answering `contentWindow`. Non-configurable is deliberate: a page that guessed the name still cannot
       delete the navigable out from under the element. */
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_navigable, proxy, JS_PROP_WRITABLE);
}

/* §4.8.5's REMOVING STEPS: an <iframe> that leaves a document DESTROYS its child navigable. Two halves, and
   both are observable. The ELEMENT loses its navigable, so `contentWindow` is null from here on; the PROXY a
   page is still holding stays the same object and reports `closed` and an empty `name`, because a WindowProxy
   outlives the navigable it named — that is what the spec files check, and it is the same sentence that makes a
   WindowProxy a separate object from its Window in the first place. */
void iframe_destroy_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue proxy = iframe_navigable(ctx, wrap);

    if (JS_IsUndefined(proxy)) return;   /* this flow never had one */
    window_proxy_close(ctx, proxy);
    JS_FreeValue(ctx, proxy);
    /* CLEARED, not deleted: the slot is non-configurable so it cannot be deleted, and it does not need to be —
       an empty slot is what "this element has no navigable" means everywhere it is read. It is an ordinary
       property write on the wrapper, so the heap COW delta isolates it: a sibling arm that never removed the
       element still sees the frame it knew. */
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_navigable, JS_UNDEFINED, JS_PROP_WRITABLE);
}

/* §4.8.5 FOR THE ELEMENTS THE PARSER INSERTED. A browser runs the insertion steps during tree construction, so
 * an `<iframe>` in the page's own markup has a navigable before the first script runs — `window.length` is 1 on
 * a document that never scripted anything. This engine's tree comes from a Lexbor parse that does not pass
 * through the DOM chokepoint, so the parsed tree's iframes get their step here, once, when the document is
 * installed. Everything a script appends afterwards goes through the chokepoint and needs nothing from this. */
void iframe_document_parsed(JSContext *ctx)
{
    lxb_dom_node_t *root = document_root_node(), *n = root;

    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t qn = 0;
            const lxb_char_t *q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
            if (q && qn == 6 && !strncasecmp((const char *)q, "iframe", 6)) {
                JSValue w = node_wrap(ctx, n);
                iframe_create_navigable(ctx, w);
                JS_FreeValue(ctx, w);
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
}

/* §7.2.5's DOCUMENT-TREE CHILD NAVIGABLES, in TREE ORDER — the set `window.length` counts and `window[i]`
 * indexes. It is a WALK, never a counter kept beside the tree: the set changes on every insertion, every
 * removal and every reparent, and a page reads `length` after doing all three. A count that a mutation forgot
 * to adjust is wrong in exactly the case the spec files test.
 *
 * IT IS PER-FLOW TWICE OVER, and both halves come for free. The TREE is per-flow (the DOM COW delta), so a
 * flow that appended a frame walks a document containing it and its sibling does not; and the NAVIGABLE is
 * per-flow (the wrapper slot), so an element that is in both flows' trees still only counts for the flow that
 * gave it one. That is why this asks iframe_has_navigable rather than counting <iframe> ELEMENTS: an element
 * whose navigable was destroyed is still in the tree until it is removed, and it is not a child navigable.
 *
 * `want` < 0 counts them all and returns JS_UNDEFINED; otherwise the nth is returned, or JS_UNDEFINED. */
static JSValue child_navigables(JSContext *ctx, int want, int *out_n)
{
    lxb_dom_node_t *root = document_root_node();
    lxb_dom_node_t *n = root;
    int seen = 0;

    if (out_n) *out_n = 0;
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t qn = 0;
            const lxb_char_t *q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
            if (q && qn == 6 && !strncasecmp((const char *)q, "iframe", 6)) {
                JSValue w = node_wrap(ctx, n);
                JSValue nav = iframe_navigable(ctx, w);
                JS_FreeValue(ctx, w);
                if (!JS_IsUndefined(nav)) {
                    if (want == seen) { if (out_n) *out_n = seen + 1; return nav; }
                    seen++;
                }
                JS_FreeValue(ctx, nav);
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
    if (out_n) *out_n = seen;
    return JS_UNDEFINED;
}

int iframe_child_navigable_count(JSContext *ctx)
{
    int n = 0;
    JSValue v = child_navigables(ctx, -1, &n);
    JS_FreeValue(ctx, v);
    return n;
}

JSValue iframe_child_navigable(JSContext *ctx, int index)
{
    return index < 0 ? JS_UNDEFINED : child_navigables(ctx, index, NULL);
}

/* §4.8.5 `contentWindow`: this flow's child navigable, or null when there is none. Reading THROUGH it is what
   suspends; this read does not, because the proxy is a local object naming a remote document. */
static JSValue js_iframe_content_window(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue v = iframe_navigable(ctx, this_val);
    (void)magic;
    if (JS_IsUndefined(v)) return JS_NULL;
    return v;
}

void iframe_init(JSContext *ctx)
{
    DCHECK(g_atom_navigable == JS_ATOM_NULL, "iframe_init ran twice — one instance is one document");
    g_atom_navigable = JS_NewAtom(ctx, "apiclientNavigable");
    CHECK(g_atom_navigable != JS_ATOM_NULL, "the iframe navigable slot could not be interned");
}

void iframe_install(JSContext *ctx, JSValueConst proto)
{
    idl_install_accessor(ctx, proto, "contentWindow", js_iframe_content_window, 0, -1);
}

void iframe_free(JSContext *ctx)
{
    (void)ctx;
    JS_FreeAtom(ctx, g_atom_navigable);
    g_atom_navigable = JS_ATOM_NULL;
}
