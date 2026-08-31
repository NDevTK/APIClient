/* CSSOM §6.2 — CSS STYLE SHEET COLLECTIONS: the list a document or shadow root keeps, the §6.2.2 StyleSheetList
 * that exposes it, and §6.2.3's `styleSheets`.
 *
 * THE HOLDER IS THE ROOT, NOT THE DOCUMENT, and that is the mixin talking rather than an extra generality.
 * §6.2's list is named "the document OR SHADOW ROOT CSS style sheets" and §6.2.3 is a `partial interface mixin
 * DocumentOrShadowRoot`, so a `<style>` inside a shadow tree belongs to that tree's list and never to the
 * document's. DOM's `node_root` already answers which of the two an element is in — a shadow-tree node's root
 * IS its shadow root — and `isConnected` is separately shadow-including, which is why such an element reaches
 * HTML §4.2.6 step 6 at all.
 *
 * THE LIST IS A JS ARRAY ON THE ROOT'S WRAPPER, for the reason the style element's association is a slot on
 * ITS wrapper: a property write is captured by the per-flow heap COW delta for free, so an arm that appends a
 * `<style>` has a sheet in its collection that its sibling does not, and the whole thing parks to the IDB cold
 * tier and resumes because it is made of JS values. A malloc'd C list would revert its head and tail pointers
 * on a context switch and leave the nodes reachable from nothing.
 *
 * WHICH LIST A SHEET IS IN IS REMEMBERED, NOT RE-DERIVED. §6.2's remove is invoked from HTML §4.2.6 step 2,
 * which for a disconnection runs AFTER the element has already left the tree — so `node_root(owner)` then
 * answers the detached subtree's top and would look for the sheet in a list that never held it. That is
 * CLAUDE.md's "an operation that becomes a work item takes its inputs with it" at small scale, and the fix is
 * the same: the holder is recorded when the sheet is ADDED. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_STYLE_SHEET_LIST_H
#define ENGINE_HOST_BROWSER_CORE_CSS_STYLE_SHEET_LIST_H

#include <lexbor/dom/dom.h>

#include "quickjs.h"

void style_sheet_list_init(JSContext *ctx);
/* §6.2.2's prototype for ONE realm — declared into core/realm.h's list. */
void style_sheet_list_install_proto(JSContext *ctx);
/* `StyleSheetList` as a global. */
void style_sheet_list_install(JSContext *ctx, JSValueConst global);
/* §6.2.3's `[SameObject] readonly attribute StyleSheetList styleSheets`, on each of the two interfaces whose
   IDL INCLUDES DocumentOrShadowRoot — Document and ShadowRoot. Called once per prototype, by the component
   that owns that prototype, because which interface carries a member is that component's statement. */
void style_sheet_list_install_mixin(JSContext *ctx, JSValueConst proto);
void style_sheet_list_free(JSRuntime *rt);

/* §6.2's "ADD A CSS STYLE SHEET" step 1 — "add the CSS style sheet to the list of document or shadow root CSS
   style sheets AT THE APPROPRIATE LOCATION", which is tree order over the owner nodes. Invoked by §6.2's
   create-a-CSS-style-sheet step 2. `owner_node` is the sheet's own, passed rather than read back off the sheet
   so the create states what it is adding. Steps 2-5 (the script-blocking set, and the disabled flag the
   preferred CSS style sheet set name decides) are NOT here — see the assertion at the call site. */
void style_sheet_list_add(JSContext *ctx, JSValueConst sheet, JSValueConst owner_node);

/* §6.2's "REMOVE A CSS STYLE SHEET" step 1 — "remove the CSS style sheet from the list of document or shadow
   root CSS style sheets". The list it is in is the one the add recorded, never the one its owner node's current
   root names. */
void style_sheet_list_remove(JSContext *ctx, JSValueConst sheet);

/* THE LIST ITSELF, FOR THE CASCADE — `root`'s CSS style sheets in tree order, as the very JS Array §6.2.3's
   collection shares, so a sheet the running flow added is in it and its sibling's is not.
   IT DOES NOT CREATE ONE. The two callers above are placing a sheet and may mint the list; a READ must not,
   because a document that declares no styles would otherwise grow one on the first computed-value read of the
   first flow that took one — a heap write attributed to whichever flow happened to ask first. JS_UNDEFINED is
   the answer for a root with no list, and for a `root` that is neither a Document nor a ShadowRoot: a
   DISCONNECTED subtree's root is a real state (its own top element), and §6.2 gives it no collection at all.
   OWNED. */
JSValue style_sheet_list_of(JSContext *ctx, lxb_dom_node_t *root);

#endif
