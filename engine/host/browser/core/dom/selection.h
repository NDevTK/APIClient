/* SELECTION — Selection API §3 Selection interface, §4.1 Extensions to Document interface and §4.2 Extensions
   to Window interface. See selection.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_SELECTION_H
#define ENGINE_HOST_BROWSER_CORE_DOM_SELECTION_H

#include <lexbor/dom/dom.h>
#include "quickjs.h"

void selection_init(JSContext *ctx);
/* §3's INTERFACE PROTOTYPE OBJECT for one realm — declared into core/realm.h's list. */
void selection_install_proto(JSContext *ctx);
/* §3's interface object on the global. */
void selection_install(JSContext *ctx, JSValueConst global);
/* §4.1's `Selection? getSelection()` on Document.prototype, and §4.2's on the Window. Each is installed by the
   component that OWNS the member rather than by the interface's own file, exactly as core/html/focus.c's
   §6.6 members are — the algorithm and the state are here, so the install is here. */
void selection_install_document_members(JSContext *ctx, JSValueConst proto);
void selection_install_window_members(JSContext *ctx, JSValueConst global);

/* §2's UNIQUE SELECTION FOR ONE DOCUMENT — "Every document with a browsing context has a unique selection
   associated with it", built WITH that document so it belongs to the pre-boot BASELINE rather than to whichever
   flow happened to call `getSelection()` first. `doc` is the Document's own wrapper and is BORROWED. Initially
   EMPTY, which is §2's own initial value. OWNED by the caller — the Document record holds it, and it is that
   record's release that gives it back. */
JSValue selection_new(JSContext *ctx, JSValueConst doc);

void selection_free(void);

#endif
