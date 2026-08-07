/* HTML §7.4 — creating a navigable, and where a same-origin about:blank child comes from.
 *
 * `window.open()` with no argument and an `<iframe>` with no `src` both produce a navigable whose initial
 * Document is `about:blank`. That Document has no response to take anything from, which is why HTML gives every
 * Document a §7.2.6 POLICY CONTAINER and has §7.4 CLONE THE CREATOR'S when there is a creator. The child
 * inherits its parent's CSP by the ordinary rule, not by an inheritance rule written for CSP — see
 * policy_container.h.
 *
 * ONE INSTANCE IS ONE DOCUMENT, so the child is not in this heap and the clone is a CROSS-INSTANCE operation:
 * the creator's serialized policy travels with the request, and only the host can mint the child's document id
 * because only the host knows what ids exist. The engine does NOT mint one locally — a local counter would
 * collide with the host's namespace, and partitioning the id space to avoid that is a CAP on how many
 * documents there can be, which this project does not do.
 *
 * SO CREATION IS A SUSPEND, and that is the whole reason the host-request register exists. The flow parks at
 * the `open()` call exactly as it parks at an await, siblings run, the host answers with the child's id, and
 * the flow resumes holding a WindowProxy for it. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_H

#include "quickjs.h"

/* Install §7.4's scriptable entry point — `window.open`. `origin` is this document's, which the initial
   about:blank child inherits along with the policy container. */
void navigable_install(JSContext *ctx, JSValueConst global, const char *origin);
void navigable_free(JSContext *ctx);

#endif
