/* INDEXED DATABASE §4.2's IDBVersionChangeEvent and its FIRE algorithm. See idb_version_change_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_VERSION_CHANGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_VERSION_CHANGE_EVENT_H

#include <stdbool.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* Declared once per AGENT: the class §4.2's brand check asks, the constructor's declaration, and the per-realm
   install that builds this realm's IDBVersionChangeEvent.prototype and interface object. The component holds
   one agent-lifetime JS value — the private Symbol its two slots hang off — so it has a release. */
void idb_version_change_event_init(JSContext *ctx);
void idb_version_change_event_free(JSRuntime *rt);

/* §4.2's "FIRE A VERSION CHANGE EVENT named e at target given oldVersion and newVersion", as a REQUEST — the
 * shape event_target_fire_run has, for the same reason: the dispatch runs the page's listeners, so the caller
 * is a step machine that RESTS at it, and every one of this algorithm's three callers (§5.1's `versionchange`
 * and `blocked`, §5.7's `upgradeneeded`) is one.
 *
 * `newVersion` is the IDL's `unsigned long long?` — `has_new` false is the null a delete reports, which is the
 * whole of what tells `deleteDatabase`'s success event from `open`'s.
 *
 * `pev` is the caller's EVENT SLOT: this algorithm's step 1 creates the event and steps 2-5 initialise it, and
 * the caller has to hold that object across the suspension the dispatch is — so the event is minted into the
 * slot on the first entry and re-used on every resume, exactly as the two fire machines in idb_request.c hold
 * theirs. It is JS_UNDEFINED on the first entry and OWNED by the caller afterwards.
 *
 * `pdid_throw` answers step 7's return value — §4.2's legacyOutputDidListenersThrowFlag, which §5.7 step 9.6
 * reads as `didThrow` and which the other two callers discard. NULL when the caller does not want it.
 *
 * Returns JS_STEP_CALL (return it) or 0 when it has answered, or -1 with a throw live. */
int idb_fire_version_change_event_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap,
                                      JSValueConst target, const char *type, double old_version,
                                      double new_version, bool has_new, JSValue *pev, JSValue in,
                                      bool *pdid_throw, JSValue **out_cb, int *out_argc);

#endif
