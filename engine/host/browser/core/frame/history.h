/* THE History INTERFACE — HTML §7.2.5 "The History interface". See history.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_HISTORY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_HISTORY_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared once per AGENT: the class (which is both the per-realm prototype slot and §3.7.6 Attributes' and
   §3.7.7 Operations' brand), the two
   declared members' step ids, and the realm intrinsic that builds this realm's History object.
   DECLARE core/frame/session_history.c FIRST — its record is what every member of this interface reads, and
   realm.h runs the intrinsics in declaration order. */
void history_init(JSContext *ctx);
void history_free(void);

/* §7.2.5's "a Document document can have its URL rewritten to a URL targetURL", asked of THIS realm's
   Document.
 *
 * IT LEAVES THIS FILE BECAUSE §7.2.6.10.4 ASKS IT TOO — the inner navigate event firing algorithm's step 7
 * computes `canIntercept` from exactly this predicate — and the rule is a table of per-scheme loosenings
 * (origin-bearing components always, then path and query by scheme) that a second derivation would get wrong in
 * the `file:` row before anyone noticed. `target_url` is a SERIALIZED absolute URL. */
bool history_document_can_have_url_rewritten(JSContext *ctx, const char *target_url);

#endif
