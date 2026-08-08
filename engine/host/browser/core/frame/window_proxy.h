/* WindowProxy — HTML §7.2.5.1. See window_proxy.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* `origin` is THIS DOCUMENT'S, serialized — the ACCESSOR side of §7.2.5.1's same-origin check. Without it the
   check cannot be made at all, which is the state this component was in: it carried the TARGET's origin and
   compared it against nothing. */
void window_proxy_init(JSContext *ctx, const char *origin);
void window_proxy_free(JSContext *ctx);

/* A proxy over a navigable whose active Window is `window` and whose active document's origin is `origin`.
   Both are the binding at this moment; a navigation replaces them, PER FLOW. `doc` names WHICH document that
   Window is — a same-origin child is a second realm in this agent, so "local" is no longer a synonym for "the
   instance root" and the proxy has to say which of this agent's documents it is over. */
/* `url` is the navigable's initial address — what its REALM is built from on the first read that reaches
   through to the active document (see navigable.h). The proxy owns that realm once built; the navigable's own
   members (window/self/frames/parent/top/opener/closed/name) never need it and never build it. */
/* `creator_csp` is §7.4's CLONE of the creating document's policy container, as text — what a navigable created
   with NO address (about:blank) runs its scripts under, since it has no response of its own to carry one. Taken
   HERE, at creation, because such a navigable's realm is materialized lazily and the realm that materializes it
   need not be the one that created it. NULL for an addressed navigable, whose policy comes with its response. */
JSValue window_proxy_new(JSContext *ctx, uint32_t doc, const char *url, const char *origin, const char *name,
                         bool is_popup, const char *creator_csp, JSValueConst parent, JSValueConst opener);

/* That cloned policy text, for the materialization that builds the child's document. BORROWED, NULL for none. */
const char *window_proxy_creator_csp(JSValueConst proxy);

/* §7.4's "popup window is requested" for this navigable — §7.2.5.3's BarProps are the negation of it. */
bool window_proxy_is_popup(JSValueConst proxy);

/* §7.2.5.1's proxy for the realm that is ASKING — `window`, `self`, and the `source` of every message it
   posts. Its realm is this one and is already built, so nothing about it is deferred.
   `name` is the navigable's, and NULL is the host STATING THAT IT DOES NOT KNOW IT — this is the one navigable
   §7.4 did not create, so the browser may have been handed a name by a cross-origin document that set it before
   navigating, and `window.name` then reads as unknown external input. A host that loaded the document itself
   knows the answer is "" and says so. */
JSValue window_proxy_new_self(JSContext *ctx, uint32_t doc, const char *name);

/* A proxy over a navigable whose active document lives in ANOTHER WASM instance. It carries no Window — there
   is no local object to hold — so every read through it is a cross-document operation the flow suspends on. */
/* `name` is the BROWSING CONTEXT's name (the iframe element's `name` attribute, or §7.4's target), NULL for
   none. `parent` is the parent navigable — this instance's own Window for a child navigable, JS_UNDEFINED for a
   top-level one — and `opener` is §7.2.5's, JS_NULL when the navigable was not opened by a script. */
JSValue window_proxy_new_remote(JSContext *ctx, uint32_t doc, const char *origin, const char *name,
                                JSValueConst parent, JSValueConst opener);

/* §4.8.5's DESTROY A CHILD NAVIGABLE, from the side that owns the navigable: the proxy a page is holding stays
   the object it was — the spec files check that it does — and reports `closed`, an empty `name`, and no active
   document from that point on. Captured into the RUNNING FLOW's delta, so a sibling arm that never removed the
   element still sees the frame it knew. */
void window_proxy_close(JSContext *ctx, JSValueConst proxy);

/* §7.2.5.1's shared member surface, so a component that owns one of the cross-origin-accessible members
   (postMessage) installs it where every proxy sees it, and `a.postMessage === b.postMessage` holds. */
JSValueConst window_proxy_proto(void);

/* §7.2.5.1's members on that prototype. A LOCAL proxy answers every one by reading its own Window in this turn.
   A REMOTE one answers the NAVIGABLE's own state — window/self/frames/globalThis/parent/top/opener/closed/name,
   plus close() — in this turn too, because the navigable belongs to the instance that created it; only a member
   that reads through to the ACTIVE DOCUMENT SUSPENDS the flow on a host request carrying (document, world,
   member). Installed separately from init because it needs the IDL declaration machinery, which is not up when
   the class is registered. */
void window_proxy_install_members(JSContext *ctx);

/* §7.2.5's `closed` — a fact about the NAVIGABLE, so the Window's getter and the proxy's read the same byte.
   Per-flow: captured into the running flow's delta, so a sibling arm that never closed it still sees it open. */
bool window_proxy_closed(JSContext *ctx, JSValueConst proxy);
void window_proxy_set_closed(JSContext *ctx, JSValueConst proxy);

/* IS THIS A WindowProxy? MessageEvent's `source` union names one, and §9.4.4's post takes one as its target. */
bool window_proxy_is(JSValueConst v);

/* IS THE NAVIGABLE'S ACTIVE DOCUMENT IN ANOTHER INSTANCE? Asked of the world registry as "does this agent hold
   that document's realm" — never as "is it the document I am", which answers `true` for a same-origin sibling
   sitting in the same runtime. Asserts that the proxy's document and its Window agree, because a proxy where
   they disagree answers a cross-instance read out of the wrong heap. */
bool window_proxy_is_remote(JSValueConst proxy);

/* WHICH DOCUMENT the navigable's active document is — what the host routes a cross-document request by. */
uint32_t window_proxy_doc(JSValueConst proxy);

/* The navigable's CURRENT active Window, as this flow sees it (owned). Crashes for a proxy whose navigable is
   in another WASM instance — that resolve is a host round trip and is not built; see window_proxy.c. */
JSValue window_proxy_window(JSContext *ctx, JSValueConst proxy);

/* THE ACTIVE DOCUMENT'S REALM, MATERIALIZED IF IT IS NOT YET (navigable.h). Only for a navigable this agent
   HOLDS — it crashes for a peer's, which is a suspend and not a realm. A same-origin document is in this heap
   and every read of it is answered in the asking turn, because the spec is synchronous there and a suspend
   would be OBSERVABLE. */
JSContext *window_proxy_realm(JSContext *ctx, JSValueConst proxy);

/* HAS THIS NAVIGABLE'S ACTIVE DOCUMENT BEEN MATERIALIZED YET? Asked by a walk over the navigable TREE, which
   must not build a realm just to look at it: an unmaterialized navigable holds the initial about:blank Document
   §7.4 created it with, and that Document has NO child navigables by construction — so "not materialized" and
   "no children" are the same answer, and materializing every navigable a forced-execution frontier ever created
   in order to ask is the heap exhaustion navigable.c's deferral exists to avoid. */
bool window_proxy_materialized(JSValueConst proxy);

/* §7.2.5's `parent`, `top` and `opener` — the NAVIGABLE's, so a Window answers them from the same record its
   own WindowProxy does. One navigable, one answer, whether a page reads `parent` or `otherW.parent`. Owned. */
JSValue window_proxy_parent(JSContext *ctx, JSValueConst proxy);
JSValue window_proxy_top_of(JSContext *ctx, JSValueConst proxy);
JSValue window_proxy_opener(JSContext *ctx, JSValueConst proxy);
/* §7.2.5's `opener` SETTER, null branch: DISOWN the opener — the link is severed on the NAVIGABLE, so every
   later read of it (this Window's `opener`, the proxy's) answers null and no own property is defined. It is
   the half of that setter that is not Web IDL's replace-with-a-value, and it is a real state change rather
   than a value assignment, which is why it lives on the navigable. Per-flow: captured into the running flow's
   delta, so a sibling arm that did not disown still has its opener. */
void window_proxy_disown_opener(JSContext *ctx, JSValueConst proxy);

/* IS THE NAVIGABLE'S ACTIVE DOCUMENT SAME-ORIGIN WITH THIS ONE? §7.2.5.1's check, exported because §4.8.5's
   `contentDocument` makes the same decision one layer up — and must make it BEFORE asking the peer, since a
   cross-origin answer is null and asking for it would both leak and suspend a flow on a settled question.
   An OPAQUE origin is same-origin with NOTHING, including another opaque one. */
bool window_proxy_same_origin_of(JSValueConst proxy);

/* THE BROWSING CONTEXT'S NAME, as this flow sees it — "" when it has none. §7.3.3's named access on the Window
   matches against it, so the walk that answers `window.myFrameName` needs to read it. BORROWED. */
const char *window_proxy_name(JSValueConst proxy);

/* §7.11's `name`, READ AND WRITTEN THROUGH THE ONE PLACE IT LIVES. A Window and its WindowProxy are two
   spellings of one navigable, so `window.name` inside a document and `w.name` from its opener are one attribute
   of one record — window.c answers the global's accessor from here rather than from a second source. The value
   is CONCRETE where the navigable's name was stated (§7.4 states it) and CONCOLIC where it was not, which is
   the navigable the instance started in. */
JSValue window_proxy_name_value(JSContext *ctx, JSValueConst proxy);
JSValue window_proxy_name_assign(JSContext *ctx, JSValueConst proxy, JSValueConst v);

/* The active document's origin, as this flow sees it — what §7.2.5.1's same-origin check reads. BORROWED. */
const char *window_proxy_origin(JSValueConst proxy);

/* NAVIGATE — REPLACE THE NAVIGABLE'S ACTIVE DOCUMENT while the proxy object stays the same, which is the whole
   reason a WindowProxy exists: a page holding `iframe.contentWindow` across a navigation holds the same object
   and reaches the NEW document through it.
   ALL FIVE FACTS MOVE AT ONCE — realm, Window, document id, address, origin — because they are one binding.
   An earlier attempt replaced the Window and the origin and left the REALM behind, so the two halves of one
   navigable named different documents; that is why they are one call and not five setters.
   PER FLOW: the whole record is captured into the running flow's delta at the accessor, so a sibling arm that
   never navigated still resolves this proxy to the document it knew, and a parked flow resumes into its own.
   `realm` is BORROWED — the agent owns every realm it built (navigable.c) — and the superseded one is NOT torn
   down here: a flow parked inside it resumes there, which is what makes it a time-travel entity rather than a
   page a browser could throw away. */
void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSContext *realm, uint32_t doc,
                           const char *url, const char *origin);

#endif
