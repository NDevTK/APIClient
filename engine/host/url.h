/* URL query-parameter extraction — parse a COMPUTED url's query string into endpoint param objects.
 * Pure: string parsing + percent-decoding + the JS API, no engine state. The engine owns URL canonicalization
 * (Lexbor); this only splits the already-computed query into the shape the @H recorder attaches. */
#ifndef ENGINE_HOST_URL_H
#define ENGINE_HOST_URL_H

#include "quickjs.h"

/* Build {name, location:"query", validValues:[value?]} objects from `url`'s query string onto `params`
   (a JS array). A hole value ({search}) passes through literally (opacity marker, decoded downstream). */
void build_query_params(JSContext *ctx, const char *url, JSValueConst params);

#endif
