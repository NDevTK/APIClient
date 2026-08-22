/* INTERNAL SLOTS — Web IDL §3.7.3's "internal slot", as an object this engine can hold in a property.
 *
 * WHY THEY EXIST AT ALL. A platform object's internal state has to live somewhere the page cannot see and the
 * per-flow COW delta CAN: an ordinary property write is already captured by the delta, so a record hung off a
 * private Symbol gives every component isolation, snapshotting and fork-visibility for free, with no new delta
 * kind. That is why the DOM components here store their state this way rather than in C structs.
 *
 * WHY THE RECORD HAS A NULL PROTOTYPE, WHICH IS THE WHOLE POINT OF THIS FILE. Every component reads the RECORD
 * with JS_GetOwnSlot — an own slot, never a lookup — and then reads a FIELD out of it with an ordinary
 * property read, having written that field with an ordinary property write. Both halves consult the prototype
 * chain, and the WRITE is the half that surprises: assigning `slots.type = x` is [[Set]], so a getter-only
 * `type` accessor inherited from Object.prototype swallows it and NO own property is created at all — after
 * which the read finds the page's getter and runs it. WPT patches exactly those names (`type`, `size`, `mode`,
 * `start`, `highWaterMark`), and the engine aborted on its own "JS_GetPropertyInternal reached a getter"
 * assert from inside its event dispatch, which looked nothing like a property that had failed to be written.
 * A null prototype removes both halves of that BY CONSTRUCTION, so no component has to remember a special
 * accessor for its own fields.
 *
 * The sibling rule, for objects that are NOT slot records: a page-facing object the engine populates is
 * creating OWN properties, so it DEFINES them (JS_DefinePropertyValue*) rather than assigning. That is what
 * every spec algorithm that builds one actually says — CreateDataPropertyOrThrow, not Set.
 *
 * There is no state and no interface here: one constructor, for a kind of object with one rule. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_SLOTS_H
#define ENGINE_HOST_BROWSER_CORE_IDL_SLOTS_H

#include "quickjs.h"

/* A fresh internal-slot record: an object with NO prototype, so reading a field out of it can never reach the
   page. Returns JS_EXCEPTION on allocation failure, like every other constructor here. */
static inline JSValue idl_slots_new(JSContext *ctx)
{
    return JS_NewObjectProto(ctx, JS_NULL);
}

#endif
