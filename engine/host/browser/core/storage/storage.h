/* THE Storage INTERFACE — HTML §12.2.1 "The Storage interface". See storage.c.
 *
 *     [Exposed=Window]
 *     interface Storage {
 *       readonly attribute unsigned long length;
 *       DOMString? key(unsigned long index);
 *       getter DOMString? getItem(DOMString key);
 *       setter undefined setItem(DOMString key, DOMString value);
 *       deleter undefined removeItem(DOMString key);
 *       undefined clear();
 *     };
 *
 * WHAT ITS ABSENCE COST, measured over 29 real-site bundles: `localStorage`, `sessionStorage` and `Storage`
 * were reached by 3 bundles across 37 references, and the failure was SILENT rather than loud. A bare
 * `localStorage` throws a ReferenceError, which is the forcing function working; but `window.localStorage`
 * reaches solver/absent.c's hook, finds the name on browser/platform_names.h, declines to mint a concolic for
 * it, and JS_GetPropertyInternal then answers JS_UNDEFINED. vuejs.org's `window.localStorage && …` therefore
 * took the ELSE arm with nothing to say so, and a whole branch of the real site was never explored. */
#ifndef ENGINE_HOST_BROWSER_CORE_STORAGE_STORAGE_H
#define ENGINE_HOST_BROWSER_CORE_STORAGE_STORAGE_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/storage/storage_shed.h"

void storage_init(JSContext *ctx);
void storage_free(JSRuntime *rt);

/* §12.2.1: "Let storage be a new Storage object whose map is map" — `proxy_map` is Storage §4.7's proxy map
   obtained for this environment, CONSUMED. `type` is §12.2.1's associated type, which broadcast reads. */
JSValue storage_new(JSContext *ctx, JSValue proxy_map, StorageType type);

/* Is `v` a Storage of this agent? The brand Web IDL §3.7.6 "Attributes" and §3.7.7 "Operations" check before
   running a member, for anything that has to tell one from a page object wearing the same shape. */
bool storage_is(JSValueConst v);

/* THE CLASS §3.2.15 BRANDS A `Storage`-TYPED IDL POSITION AGAINST — HTML §12.2.4's `Storage? storageArea`, on
   both StorageEventInit and initStorageEvent. It is the class id and not a predicate because that is what
   idl_iface_brand takes; storage_is above is the same fact for a caller holding no declaration. Zero before
   storage_init has run, which every caller asserts rather than branding against nothing. */
JSClassID storage_class_id(void);

#endif
