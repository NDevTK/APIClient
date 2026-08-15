/* THE History INTERFACE — HTML §7.2.5. See history.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_HISTORY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_HISTORY_H

#include "quickjs.h"

/* Declared once per AGENT: the class (which is both the per-realm prototype slot and §3.7.5's brand), the two
   declared members' step ids, and the realm intrinsic that builds this realm's History object.
   DECLARE core/frame/session_history.c FIRST — its record is what every member of this interface reads, and
   realm.h runs the intrinsics in declaration order. */
void history_init(JSContext *ctx);
void history_free(void);

#endif
