/* @H ENDPOINT SURFACE — the deduped set of endpoints the forced execution learned, rebuilt clean.
 *
 * Every request host-edge (fetch/XHR/...) funnels one endpoint into record_endpoint; identical (method, url)
 * pairs dedup on the way in. A concolic URL contributes its SHAPE (`/api/region/{state}.region`), a concrete
 * one its literal. endpoint_result assembles the harness `@H` structure (fetchCallSites) for JSON emit. */
#ifndef ENGINE_HOST_SOLVER_ENDPOINT_H
#define ENGINE_HOST_SOLVER_ENDPOINT_H

#include "quickjs.h"

void    endpoint_init(void);
void    endpoint_free(void);
void    endpoint_suppress(int on);   /* 1 during a candidate/verify re-run: its requests are @S artifacts, not @H */

/* Record one learned endpoint (deduped by method+url). `url` may be concolic (shape) or concrete. */
void    endpoint_record(JSContext *ctx, const char *method, JSValueConst url);

/* Serialize the @H surface directly to a malloc'd JSON string (caller frees) — findings are C data, so the
   emit is C, never a JS-object round-trip. { "fetchCallSites":[ {"method":..,"url":..}, ... ] }. */
char   *endpoint_json(void);

int     endpoint_count(void);

#endif
