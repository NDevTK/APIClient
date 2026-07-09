/* URL query-parameter extraction — parse a COMPUTED url's query string into endpoint param objects.
 * Pure: string parsing + percent-decoding + the JS API, no engine state. The engine owns URL canonicalization
 * (Lexbor); this only splits the already-computed query into the shape the @H recorder attaches. */
#ifndef ENGINE_HOST_URL_H
#define ENGINE_HOST_URL_H

#include "quickjs.h"

/* A URL/shape carries an opaque HOLE — "{}" (generic) or "{tag}" (source-tagged: {hash}/{search}) — iff it
   has a '{' followed by only lowercase letters then '}'. Such a URL is not concretely fetchable. */
char *url_solve_holes(JSContext *ctx, const char *url);   /* substitute {src} holes an == gate PINNED with the concrete value (malloc'd, NULL if none) */
int has_hole(const char *s);

/* The concrete URL a network web API (fetch/WebSocket/Worker/sendBeacon/serviceWorker.register) was invoked
   with: the opaque EXAMPLE if the arg carries one (a computed/config URL), else its ToString. malloc'd
   (caller frees), NULL if empty — the ONE idiom every network host-edge shares to read its URL argument. */
char *url_from_arg(JSContext *ctx, JSValueConst arg);

/* Build {name, location:"query", validValues:[value?]} objects from `url`'s query string onto `params`
   (a JS array). A hole value ({search}) passes through literally (opacity marker, decoded downstream). */
void build_query_params(JSContext *ctx, const char *url, JSValueConst params);

#endif
