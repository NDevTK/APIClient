/* A REFERENCE TO AN OBJECT IN ANOTHER AGENT — see remote_object.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_OBJECT_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_OBJECT_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

void remote_object_init(JSContext *ctx);
void remote_object_free(JSContext *ctx);

/* THIS AGENT LENDING ONE OF ITS OBJECTS to a peer: a stable id, minted ONCE per object. Stable because the
   peer's reference has to have the identity a page compares — `w.document === w.document` is false the moment
   two asks mint two ids. The table holds a reference, so an exported object outlives whatever else drops it. */
uint32_t     remote_object_export(JSContext *ctx, JSValueConst v);
/* The object an id names, BORROWED. JS_UNDEFINED for an id this agent never exported, which is a peer naming
   something that was never lent — a protocol error, not a missing property. */
JSValueConst remote_object_by_id(uint32_t id);

/* A REFERENCE to an object the agent holding `doc` exported. One per (doc, id), so identity holds on this side
   too. Reads through it SUSPEND: it is a Proxy whose traps are step machines, which is the one read shape this
   interpreter resolves at the operator site and drives on the trampoline. */
JSValue  remote_object_ref(JSContext *ctx, uint32_t doc, uint32_t id);
bool     remote_object_is(JSValueConst v);
uint32_t remote_object_doc(JSValueConst v);
uint32_t remote_object_id(JSValueConst v);

#endif
