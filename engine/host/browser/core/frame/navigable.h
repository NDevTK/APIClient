/* HTML §7.4 — creating a navigable, and where a same-origin about:blank child comes from.
 *
 * `window.open()` with no argument and an `<iframe>` with no `src` both produce a navigable whose initial
 * Document is `about:blank`. That Document has no response to take anything from, which is why HTML gives every
 * Document a §7.2.6 POLICY CONTAINER and has §7.4 CLONE THE CREATOR'S when there is a creator. The child
 * inherits its parent's CSP by the ordinary rule, not by an inheritance rule written for CSP — see
 * policy_container.h.
 *
 * AN INSTANCE IS AN ORIGIN-KEYED AGENT, so WHERE the child lives is decided by its ORIGIN. A SAME-ORIGIN child
 * is a second realm in THIS heap — HTML's similar-origin window agent — and its policy container is cloned by
 * an ordinary in-heap copy. A CROSS-ORIGIN child is another instance, and there the clone travels as the
 * creator's SERIALIZED policy on the create notice, which the container can do precisely because it is a flat
 * parse over one owned string: the clone that crosses an instance and the one that crosses a session are the
 * same operation.
 *
 * CREATION IS SYNCHRONOUS, AND THAT IS NOT A CONVENIENCE — IT IS THE SPEC. §4.8.5's iframe INSERTION STEPS
 * create the child navigable, so `frame.contentWindow` answers on the line after the append, and §7.4's
 * `open()` returns its WindowProxy at its own call site. Neither can round-trip to the host. This used to, and
 * the cost was not subtle: every host answered "not created" rather than host a second document, so
 * contentWindow was null and the whole of html/browsers read members of null.
 *
 * WHAT MAKES IT SYNCHRONOUS IS THE NAME. A document is named, and a document created by this one is named
 * "<my name>.<n>" — unique by induction with no allocator and no round trip (world.h). So the engine MINTS the
 * child and the host is TOLD, as a one-way notice. The host still owns routing, because only the trusted zone
 * knows which instance holds which document; what it no longer owns is the identity, which it never needed to.
 *
 * A HOST THAT WILL NOT HOST THE CHILD does not answer that by withholding a name — it simply never provisions
 * the instance, and every read through the proxy parks. That is a host gap, visible as a parked flow, rather
 * than a page-visible null that a page cannot tell from a popup blocker. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_H

#include <stdbool.h>

#include "quickjs.h"

/* Install §7.4's scriptable entry point — `window.open`. `origin` is this document's, which the initial
   about:blank child inherits along with the policy container. */
void navigable_install(JSContext *ctx, JSValueConst global, const char *origin);
void navigable_free(JSContext *ctx);

/* §7.4's CREATE A NEW NAVIGABLE. `url` is the child's initial address; NULL, "" or "about:blank" all mean the
   initial about:blank Document, which inherits this document's origin and policy container. Returns the child's
   WindowProxy, or JS_UNDEFINED when `url` does not parse — the caller decides what that means, because §7.4
   throws a SyntaxError for it where §4.8.5 does not. */
/* `name` is the browsing context name to give it (an iframe's `name` attribute, §7.4's target), or NULL.
   `is_child` distinguishes §4.8.5's CHILD navigable — nested in this one, so its `parent` is this Window — from
   §7.4's AUXILIARY one, which is its own top and links back through `opener`. */
JSValue navigable_create(JSContext *ctx, const char *url, const char *name, bool is_child);

#endif
