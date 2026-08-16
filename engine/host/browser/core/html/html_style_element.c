/* HTML §4.2.6 — the `style` element's associated CSS style sheet. See html_style_element.h.
 *
 * THE ASSOCIATION IS A SLOT ON THE ELEMENT'S WRAPPER, and that is what makes it per-flow. A flow that appends a
 * `<style>` gets a sheet its siblings do not have, and a flow that sets `sheet.disabled` disables it only for
 * itself — the slot write is an ordinary property write the heap COW delta already captures, and the sheet's
 * own record time-travels through core/css/css_style_sheet.c's accessor. Nothing here needs a registry of
 * sheets, which is the point: a registry is shared state the flow machinery does not swap, and the document's
 * tree already IS the list.
 *
 * `disabled` IS NOT A REFLECTION, and it used to be one. HTML §4's element-interface table carried
 * `{ "disabled", "disabled", REFLECT_BOOL }` for `<style>`, which is a member HTML does not define: there is no
 * `disabled` content attribute on a style element, and §4.2.6's IDL attribute is a forwarding to the SHEET's
 * disabled flag. The reflection answered `<style disabled>` (markup no browser honours) and ignored the flag
 * every real page sets, so `styleEl.disabled = true` disabled nothing and `styleEl.disabled` read back false
 * for a sheet that was disabled. Both directions were wrong and neither threw.
 *
 * THE THREE TRIGGERS ARE THREE SEAMS, and the standard lists them as three because no one of them subsumes the
 * others: the element is popped off the parser's stack (this engine's Lexbor parse has no such seam, so
 * html_style_element_parsed stands where it would be), it becomes connected or disconnected (§4.2.3's
 * insertion/removing steps, run from the tree-steps drain in the INSERTED NODE'S realm), and its children
 * changed steps run (§4.2.3's third family, which is the only one that can see `styleEl.textContent = '…'`). */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_style_sheet.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/frame/policy_container.h"
#include "core/html/html_style_element.h"
#include "core/idl_args.h"

/* The private key the association hangs off the element's wrapper — a Symbol, so a page can neither see it nor
   forge one, and an own SLOT rather than a lookup, so nothing on a prototype can answer for it. */
static JSValue g_key = JS_UNDEFINED;
static int     g_id_set_disabled = -1;
static bool    g_ready;

/* Is this node an HTML `style` element? SVG has a `<style>` too and it is a DIFFERENT interface with its own
   standard (SVGStyleElement), so the namespace is part of the question rather than an afterthought. */
static bool style_element_is(lxb_dom_node_t *n)
{
    size_t len = 0;
    const lxb_char_t *name;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    name = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
    /* The LOCAL name, which is already lowercase for an HTML-namespace element however the markup spelled it —
       the qualified name would carry a prefix an XML-parsed document can give it. */
    return name && len == 5 && memcmp(name, "style", 5) == 0;
}

/* ---- the association ------------------------------------------------------------------------------------ */

static JSAtom style_key(JSContext *ctx)
{
    JSAtom k;

    DCHECK(g_ready, "a style element's associated sheet was asked for before html_style_element_init ran");
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the style-element association key could not be interned");
    return k;
}

/* "this's associated CSS style sheet", or JS_NULL. BORROWED-then-owned: a DUP, like every other slot read. */
static JSValue style_sheet_of(JSContext *ctx, JSValueConst wrap)
{
    JSAtom k = style_key(ctx);
    JSValue cur;

    if (JS_GetOwnSlot(ctx, &cur, wrap, k) <= 0) cur = JS_NULL;
    JS_FreeAtom(ctx, k);
    if (!css_style_sheet_is(cur)) {
        DCHECK(JS_IsNull(cur) || JS_IsUndefined(cur),
               "a style element's association slot held something that is not a CSS style sheet — this file is "
               "the only writer and it writes a sheet or nothing");
        JS_FreeValue(ctx, cur);
        return JS_NULL;
    }
    return cur;
}

static void style_set_sheet(JSContext *ctx, JSValueConst wrap, JSValue sheet)   /* CONSUMES sheet */
{
    JSAtom k = style_key(ctx);

    JS_SetProperty(ctx, (JSValue)wrap, k, sheet);
    JS_FreeAtom(ctx, k);
}

/* ---- §4.2.6's "UPDATE A STYLE BLOCK" --------------------------------------------------------------------- */

void html_style_element_update(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSContext *realm;
    JSValue wrap, had, sheet;
    size_t tlen = 0;
    const lxb_char_t *type;

    if (!style_element_is(n)) return;
    DCHECK(g_ready, "§4.2.6's update a style block ran before html_style_element_init declared its members");

    /* §4.2.3's STEPS BELONG TO THE NODE'S DOCUMENT, NOT TO WHOEVER PERFORMED THE WRITE — two same-origin
       documents are one agent, so `frame.contentDocument.head.appendChild(styleEl)` is a mutation this flow may
       make in another document's tree, and the sheet it creates is an object of THAT document's realm. The
       mutating ctx would build it in the wrong one and every member on it would then answer from the wrong
       realm forever (§3.7). */
    realm = document_realm_of(n);
    DCHECK(realm != NULL,
           "§4.2.6's update a style block reached a style element in a document no realm was installed for — "
           "every Document record names its realm at the line that builds it, §4.5.1's three factories and "
           "DOMParser included, so a node whose owner document cannot answer is a node in a document nobody "
           "registered rather than a document that legitimately has none");
    wrap = node_wrap(realm, n);
    DCHECK(JS_IsObject(wrap),
           "§4.2.6's update a style block could not reach the style element's wrapper — the association is an "
           "own slot ON that object, so without one there is nowhere for the sheet to live and the element "
           "would silently keep answering `sheet` as null");

    /* STEP 2 — "if element has an associated CSS style sheet, remove the CSS style sheet in question." */
    had = style_sheet_of(realm, wrap);
    if (!JS_IsNull(had)) {
        css_style_sheet_remove(realm, had);
        style_set_sheet(realm, wrap, JS_NULL);
    }
    JS_FreeValue(realm, had);

    /* STEP 3 — "if element is not connected, then return." */
    if (!node_is_connected(n)) { JS_FreeValue(realm, wrap); return; }

    /* STEP 4 — "if element's type attribute is present and its value is neither the empty string nor an ASCII
       case-insensitive match for text/css, then return." A value with parameters ("text/css; charset=utf-8")
       returns early, which is why this is a whole-value compare and not a MIME parse. */
    type = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tlen);
    if (type && tlen != 0) {
        static const char CSS[] = "text/css";
        size_t i;

        if (tlen != sizeof(CSS) - 1) { JS_FreeValue(realm, wrap); return; }
        for (i = 0; i < tlen; i++) {
            char c = (char)type[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != CSS[i]) { JS_FreeValue(realm, wrap); return; }
        }
    }

    /* STEP 5 — "if the Should element's inline behavior be blocked by Content Security Policy? algorithm
       returns Blocked when executed upon the style element, style, and the style element's child text content,
       then return."
       core/frame/policy_container.c parses the SCRIPT-governing directives only, so there is no `style-src` /
       `style-src-elem` / `default-src` question to ask. A document with no policy at all provably answers
       Allowed, so the gap can only produce a wrong answer for a document that HAS one — which is exactly what
       this asserts, at the step that would then be skipping a block real Chrome performs. */
    DCHECK(policy_container_csp(document_policy(realm)) == NULL,
           "§4.2.6's update a style block reached step 5 for a document that CARRIES a Content Security "
           "Policy, and this engine cannot ask whether the policy blocks an inline style: "
           "core/frame/policy_container.c's PolicyScriptKind models POLICY_INLINE_SCRIPT, "
           "POLICY_INLINE_HANDLER, POLICY_JAVASCRIPT_URL and POLICY_EVAL and nothing for `style-src`. So this "
           "creates a style sheet a real browser would refuse, and every value the cascade resolves from it is "
           "wrong. Add the style directives to that parser and ask policy_allows here");

    /* STEP 6 — "create a CSS style sheet with the following properties". Every one of them is §4.2.6's own
       table: the owner node is the element, the location, parent CSS style sheet and owner CSS rule are null
       (an embedded sheet has no first request, no parent and no @import that pulled it in), and the disabled
       flag is left at its default. The media and the title are references to the element's ATTRIBUTES rather
       than copies of them, which is why neither is passed: the sheet reads them off the owner node. */
    sheet = css_style_sheet_create(realm, wrap, JS_NULL, JS_NULL, JS_NULL);
    if (JS_IsException(sheet)) {
        JS_FreeValue(realm, sheet);
        JS_FreeValue(realm, wrap);
        return;
    }
    style_set_sheet(realm, wrap, sheet);
    JS_FreeValue(realm, wrap);

    /* STEPS 7-8 are the RENDER-BLOCKING half — "if element contributes a script-blocking style sheet, append
       element to its node document's script-blocking style sheet set", and the render-blocking of a matching
       `media`. Both are about when the document is allowed to paint, and this engine paints nothing; the sheet
       they would delay is already the one built above, so nothing observable is skipped. */
}

/* §4.2.3's children changed steps for a `<style>` — the standard's third trigger, and the only one that sees a
   rewritten body. Registered rather than called, because "whose children changed" is a DOM question. */
static void style_children_changed(JSContext *ctx, lxb_dom_node_t *parent)
{
    (void)ctx;   /* the MUTATING realm; §4.2.6's steps belong to the element's own, which it resolves itself */
    if (!style_element_is(parent)) return;
    html_style_element_update(lxb_dom_interface_element(parent));
}

void html_style_element_parsed(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n;

    DCHECK(root != NULL, "§4.2.6's parsed-tree walk was given no tree to walk");
    DCHECK(g_ready, "a parsed tree reached §4.2.6 before html_style_element_init ran");
    /* SHADOW-INCLUDING, for the reason media_element_parsed's walk is: a `<template shadowrootmode>` has by now
       moved its contents into a shadow root, and a `<style>` among them is a style element in a tree. */
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root))
        if (style_element_is(n)) html_style_element_update(lxb_dom_interface_element(n));
}

/* ---- the members ----------------------------------------------------------------------------------------- */

/* WEB IDL §3.7.5's BRAND CHECK, and it is a THROW rather than an assert: all three members below live on
   HTMLStyleElement.prototype, and a page reaches an accessor off a prototype with `.call` on anything at all —
   `Object.create(HTMLStyleElement.prototype).sheet` included — so the receiver is the PAGE'S input. */
static bool style_receiver_ok(JSContext *ctx, JSValueConst this_val, const char *member)
{
    if (style_element_is(node_of(this_val))) return true;
    JS_ThrowTypeError(ctx, "HTMLStyleElement.%s was reached on something that is not a <style> element", member);
    return false;
}

/* CSSOM §6.3.2's LinkStyle: "The sheet attribute must return the associated CSS style sheet ... or null if
   there is none." HTMLStyleElement includes LinkStyle (HTML §4.2.6's IDL). */
static JSValue js_style_sheet(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!style_receiver_ok(ctx, this_val, "sheet")) return JS_EXCEPTION;
    return style_sheet_of(ctx, this_val);
}

/* §4.2.6: "The disabled getter steps are: 1. If this does not have an associated CSS style sheet, return false.
   2. If this's associated CSS style sheet's disabled flag is set, return true. 3. Return false." */
static JSValue js_style_disabled(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue sheet;
    bool on;

    (void)magic;
    if (!style_receiver_ok(ctx, this_val, "disabled")) return JS_EXCEPTION;
    sheet = style_sheet_of(ctx, this_val);
    if (JS_IsNull(sheet)) return JS_NewBool(ctx, false);   /* step 1 */
    on = css_style_sheet_disabled(sheet);
    JS_FreeValue(ctx, sheet);
    return JS_NewBool(ctx, on);
}

/* §4.2.6: "The disabled setter steps are: 1. If this does not have an associated CSS style sheet, return. 2. If
   the given value is true, set this's associated CSS style sheet's disabled flag. Otherwise, unset it."
   The early return in step 1 is the standard's, and it is what its own example turns on: a `<style>` that is
   not yet in the document swallows the assignment entirely and reads back false afterwards. */
static JSValue js_style_set_disabled(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue sheet;

    (void)magic;
    if (!style_receiver_ok(ctx, this_val, "disabled")) return JS_EXCEPTION;
    sheet = style_sheet_of(ctx, this_val);
    if (JS_IsNull(sheet)) return JS_UNDEFINED;   /* step 1 */
    css_style_sheet_set_disabled(sheet, JS_ToBool(ctx, val) != 0);
    JS_FreeValue(ctx, sheet);
    return JS_UNDEFINED;
}

void html_style_element_init(JSContext *ctx)
{
    if (g_ready) return;   /* one AGENT, one key and one pool entry */
    g_key = JS_NewSymbol(ctx, "htmlStyleElementSheet", false);
    CHECK(!JS_IsException(g_key), "the style-element association key allocation failed");
    g_id_set_disabled = idl_setter_id(ctx, IDL_BOOLEAN, false, js_style_set_disabled, 0);
    g_ready = true;
    node_add_children_changed_hook(style_children_changed);
}

void html_style_element_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "§4.2.6's members were installed before html_style_element_init ran");
    idl_install_accessor(ctx, proto, "sheet", js_style_sheet, 0, -1);
    idl_install_accessor(ctx, proto, "disabled", js_style_disabled, 0, g_id_set_disabled);
}

void html_style_element_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;
    g_id_set_disabled = -1;
    g_ready = false;
}
