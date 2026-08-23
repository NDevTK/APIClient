/* THE TWO Window GETTERS — HTML §12.2.2 "The sessionStorage getter" and §12.2.3 "The localStorage getter".
 * See window_storage.c.
 *
 *     interface mixin WindowSessionStorage { readonly attribute Storage sessionStorage; };
 *     Window includes WindowSessionStorage;
 *     interface mixin WindowLocalStorage  { readonly attribute Storage localStorage; };
 *     Window includes WindowLocalStorage;
 *
 * They are their own component and not a pair of lines inside storage.c because they are their own SECTIONS
 * with their own algorithm: each holds a per-DOCUMENT holder ("A Document object has an associated local
 * storage holder, which is null or a Storage object. It is initially null"), which is what makes
 * `localStorage === localStorage` true, and each turns Storage §4.2's storage-key failure into a
 * "SecurityError" DOMException. §12.2.1's interface knows nothing about either. */
#ifndef ENGINE_HOST_BROWSER_CORE_STORAGE_WINDOW_STORAGE_H
#define ENGINE_HOST_BROWSER_CORE_STORAGE_WINDOW_STORAGE_H

#include "quickjs.h"

void window_storage_init(JSContext *ctx);
void window_storage_free(JSRuntime *rt);

#endif
