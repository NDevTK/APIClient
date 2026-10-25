/* core/frame/open_again.h — the attacker opening this page's address again in a new window.
 *
 * HTML §7.2.2.1's window open steps, run by an attacker page with this page's address as `url` and no handle on
 * the page's own window: a fresh top-level traversable whose document is a second load of the same address. It
 * needs no handle and survives COOP, so it is the attacker's most general way to load the page again, and it is
 * how a value the first load stored reaches code the second load runs.
 *
 * What the second document sees is what a fresh open gives it and not a reload's leftovers: the user agent's
 * local shed, cookie jar and IndexedDB are shared (Storage §4.3, one top-level-site partition), while the session
 * shed is a new traversable's and empty, the navigable's name is the opener's target (unknown, an attacker
 * source), the referrer is Referrer Policy's derivation from the attacker page (the referrer source already
 * answers it), and the session history is a fresh one of length 1. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_OPEN_AGAIN_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_OPEN_AGAIN_H

#include <stdbool.h>
#include "quickjs.h"

/* Can an attacker open this realm's page again: its document is the top-level page of this instance and its
   address is an HTTP(S) URL, which another site's window.open can load. */
bool open_again_available(JSContext *ctx);

/* A frame performing the open on the running flow: the second document is enqueued as a document-load job of
   this navigable, with the session shed emptied and the name forgotten first. Owned by the caller's flow. */
JSValue *open_again_flow(JSContext *ctx);

#endif
