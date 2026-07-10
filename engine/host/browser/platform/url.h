/* URL query-parameter extraction + WHATWG URL canonicalization (url_resolve). Browser URL semantics with no
 * scheduler/flow state; the query splitter just shapes an already-computed query for the @H recorder. */
#ifndef ENGINE_HOST_URL_H
#define ENGINE_HOST_URL_H

#include "quickjs.h"

/* Resolve `input` against `base` with the vendored Lexbor WHATWG URL parser -> serialized absolute href
   (malloc'd; caller frees), or NULL on parse failure (the caller then yields opaque, never an invented value). */
char *url_resolve(const char *input, const char *base);

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
