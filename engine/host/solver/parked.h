/* Parked async delivery — the ONE async-as-flow delivery-park mechanism, shared by dynamic import() and fetch
 * reply consumption. A consumer whose resource (an ES-module chunk / a reply body) is not yet fetched PARKS
 * holding its promise's resolve, keyed by URL; when qjs_provide supplies the resource, park_resolve_url fires
 * each parked resolve with a caller-COMPUTED value (browser-faithful in-place promise resolution — no boot
 * re-run, no opaque settle). A resource that never arrives is resolved via park_drain at finalize (an opaque
 * fallback), so the continuation still runs. ONE mechanism, so the eventual move of the pending promise INTO the
 * per-flow COW delta (async-as-flow) happens in ONE place. See parked.c. */
#ifndef ENGINE_HOST_SOLVER_PARKED_H
#define ENGINE_HOST_SOLVER_PARKED_H
#include "quickjs.h"

typedef struct ParkTable ParkTable;
ParkTable *park_new(void);
/* Hold a promise's `resolve` keyed by `url` (dups resolve). `tag` is caller-defined per-entry state (e.g. a
   reply's is_json) passed back to the compute callback. */
void park_add(JSContext *ctx, ParkTable *t, const char *url, JSValueConst resolve, int tag);
/* The value to resolve a parked entry with (CONSUMED by the park). Called once per fired entry. */
typedef JSValue (*ParkCompute)(JSContext *ctx, const char *url, int tag, void *ud);
void park_resolve_url(JSContext *ctx, ParkTable *t, const char *url, ParkCompute f, void *ud);   /* resolve every parked entry for url */
void park_drain(JSContext *ctx, ParkTable *t, ParkCompute f, void *ud);   /* finalize: resolve every remaining parked entry (opaque fallback) */
void park_free(JSContext *ctx, ParkTable *t);

#endif
