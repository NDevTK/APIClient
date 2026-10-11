/* HTML §8.1.5.3 "Import map processing model" — Blink core/script/import_map.cc. A global's import map and
 * resolved module set, the parse that builds an import map from an `importmap` script's text, the merge that
 * registers one, and the two lookups module specifier resolution and the descendant fetch options make.
 *
 * Both concepts are per global (HTML §8.1.3.3 "Realms, settings objects, and global objects": "A global object
 * has an import map, initially an empty import map"), so they are a per-realm record built at realm creation
 * and held as JS values: a flow that registers a map or resolves a specifier writes the record's properties,
 * and the heap COW delta gives each flow its own. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_IMPORT_MAP_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_IMPORT_MAP_H
#include <stddef.h>
#include "quickjs.h"
#include "core/url/url.h"

/* The agent's half: the per-realm slot and the intrinsic that gives every realm its empty record. */
void import_map_init(JSContext *ctx);
void import_map_free(JSRuntime *rt);

/* HTML §8.1.4.8 "Import map parse results"' create an import map parse result given `input` and `base_url`,
   then register an import map given this realm's global — the two steps an inline `importmap` script takes
   from "prepare the script element" to "execute the script element". The global is a Window (only a Document
   has `script` elements). Returns 0, or -1 with the parse result's error to rethrow pending on `ctx`, which
   the caller reports (HTML §8.1.4.6) because reporting fires `error` and runs the page's code. */
int import_map_create_and_register(JSContext *ctx, const char *input, size_t input_len, const char *base_url);

/* HTML §8.1.5.1 "The resolution algorithm" steps 4-5 and 9-11: the import map (this realm's global's when the
   global is a Window, otherwise an empty one) matched for `normalized_specifier` from `serialized_base_url`.
   `as_url` is step 7's asURL or NULL. Returns 1 with `out` (initialised by the caller) holding the result, 0
   for a null result, or -1 with resolve an imports match's TypeError thrown. */
int import_map_resolve(JSContext *ctx, const char *serialized_base_url, const char *normalized_specifier,
                       const UrlRecord *as_url, UrlRecord *out);

/* HTML §8.1.5.3's add module to resolved module set, which §8.1.5.1 step 13 performs. */
void import_map_add_resolved(JSContext *ctx, const char *serialized_base_url, const char *normalized_specifier,
                             const UrlRecord *as_url);

/* HTML §8.1.4.2 "Fetching scripts"' resolve a module integrity metadata given `url` (serialized) and this
   realm: the import map's integrity[url], or the empty string. Owned; `free` it. */
char *import_map_module_integrity(JSContext *ctx, const char *url);

#endif
