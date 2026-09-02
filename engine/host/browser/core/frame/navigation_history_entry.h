/* NavigationHistoryEntry — HTML §7.2.6.5. See navigation_history_entry.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_HISTORY_ENTRY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_HISTORY_ENTRY_H

#include <stdint.h>

#include "quickjs.h"

void navigation_history_entry_init(JSContext *ctx);          /* the slot key + the IDL declarations (agent) */
/* §3.7: THIS REALM's prototype and interface object — declared into core/realm.h's one list. */
void navigation_history_entry_install_protos(JSContext *ctx);
/* THE AGENT'S HALF, UNDONE — a row on core/platform.h's release column, so it takes the RUNTIME: §7.2.6.5's
   class id and its `getState()` declaration are registrations there, and the Symbol is freed with
   JS_FreeValueRT. */
void navigation_history_entry_free(JSRuntime *rt);

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

/* WEB IDL §3.7.6 Attributes' BRAND, as the class every instance wears — what
   NavigationCurrentEntryChangeEventInit's `required NavigationHistoryEntry from` is declared against, so the
   member does not cross as itself and no body writes a check of its own. */
JSClassID navigation_history_entry_class(void);

/* §7.2.6.5's `key`, `id` AND `index` GETTER STEPS, REACHABLE FROM C — because §7.2.6.10.3 defines three of
 * NavigationDestination's members AS them: "return this's entry's key", "…'s ID", "…'s index". A second
 * derivation off the session history entry would be a second implementation of a member, and it would differ
 * exactly where it matters: these three carry §7.2.6.5's NOT-FULLY-ACTIVE answers ("", "", −1), which a
 * re-derivation would answer with the real key of a detached frame's entry.
 * The two strings are OWNED. `index` is `long long` because §7.2.6.5 declares it one, and −1 is a real answer
 * for an entry the live list no longer holds. */
JSValue navigation_history_entry_key(JSContext *ctx, JSValueConst nhe);
JSValue navigation_history_entry_id(JSContext *ctx, JSValueConst nhe);
int64_t navigation_history_entry_index(JSContext *ctx, JSValueConst nhe);

#endif
