/* NavigationHistoryEntry — HTML §7.2.6.5. See navigation_history_entry.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_HISTORY_ENTRY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_HISTORY_ENTRY_H

#include "quickjs.h"

void navigation_history_entry_init(JSContext *ctx);          /* the slot key + the IDL declarations (agent) */
/* §3.7: THIS REALM's prototype and interface object — declared into core/realm.h's one list. */
void navigation_history_entry_install_protos(JSContext *ctx);
void navigation_history_entry_free(JSContext *ctx);

/* §7.2.6.5: "Each NavigationHistoryEntry has an associated SESSION HISTORY ENTRY, which is a session history
   entry." Every member of the interface is a question about that entry, so the whole of the object's state is
   this one field — held as an own slot under a private Symbol, which is what makes it per-flow (a property
   write the COW delta captures) and unforgeable.
   `she` is BORROWED and DUPPED. The answer is OWNED. */
JSValue navigation_history_entry_new(JSContext *ctx, JSValueConst she);

/* THE SESSION HISTORY ENTRY this NavigationHistoryEntry names. OWNED. §7.2.6.4's entry-list maintenance and
   §7.2.6.3's get-the-navigation-API-entry-index both compare entries by it, which is the one operation the
   Navigation needs and the one thing this component will not answer with a raw property read from outside. */
JSValue navigation_history_entry_she(JSContext *ctx, JSValueConst nhe);

/* WEB IDL §3.7.5's BRAND, as the class every instance wears — what
   NavigationCurrentEntryChangeEventInit's `required NavigationHistoryEntry from` is declared against, so the
   member does not cross as itself and no body writes a check of its own. */
JSClassID navigation_history_entry_class(void);

#endif
