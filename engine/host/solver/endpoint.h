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

/* The @H surface as a malloc'd JSON ARRAY (caller frees) — findings are C data, so the emit is C, never a
   JS-object round-trip. `[ {"method":..,"url":..,"params":[..]}, ... ]`. It is an array and not a document
   because the DOCUMENT is one thing the host reads once (result.h): a surface that wrapped itself could not
   be composed with the others without a host-side splice, which is the host owning structure again. */
char   *endpoint_json_array(void);

int     endpoint_count(void);

#endif
