/* THE STORAGE STANDARD'S MODEL — §4.2 "Storage keys", §4.3 "Storage sheds", §4.4 "Storage shelves",
 * §4.5 "Storage buckets", §4.6 "Storage bottles" and §4.7 "Storage proxy maps". See storage_shed.c.
 *
 * IT IS A SEPARATE COMPONENT FROM THE INTERFACE OVER IT because it is a separate STANDARD: HTML §12.2.1's
 * `Storage` is one endpoint of this model, Indexed Database §2.1 is another, File System §3's bucket file
 * system is a third. What they share is where the data lives, and that is what this file is. */
#ifndef ENGINE_HOST_BROWSER_CORE_STORAGE_STORAGE_SHED_H
#define ENGINE_HOST_BROWSER_CORE_STORAGE_STORAGE_SHED_H

#include "quickjs.h"

/* Storage §4.1: "A storage type is 'local' or 'session'." */
typedef enum {
    STORAGE_TYPE_LOCAL = 0,
    STORAGE_TYPE_SESSION,
} StorageType;

void storage_shed_init(JSContext *ctx);
void storage_shed_free(JSRuntime *rt);

/* Storage §4.6's OBTAIN A STORAGE BOTTLE MAP, given a storage `type`, THIS realm's environment settings object
 * and a storage `identifier`. The result is a §4.7 STORAGE PROXY MAP, freshly minted and appended to the
 * bottle's proxy map reference set exactly as step 7 says.
 *
 * JS_UNDEFINED IS §4.2's FAILURE and is a real answer, not an error: obtain-a-storage-key returns failure for
 * an OPAQUE ORIGIN, and HTML §12.2.2/§12.2.3 turn that into a "SecurityError" DOMException. A caller that
 * treats it as an allocation failure has confused the two. OWNED. */
JSValue storage_shed_obtain_bottle_map(JSContext *ctx, StorageType type, const char *identifier);

/* §4.7: "A storage proxy map is equivalent to a map, except that all operations are instead performed on its
   BACKING MAP." This is that backing map — the object every read and write of the endpoint's data goes to.
   OWNED. */
JSValue storage_shed_backing_map(JSContext *ctx, JSValueConst proxy_map);

/* The §4.6 QUOTA of the bottle this proxy map is over, in bytes, or -1 for §4.1's null (no limit).
   HTML §12.2.1's setItem step 4 ("if value cannot be stored, then throw a QuotaExceededError") is the only
   caller: the quota is what makes "cannot be stored" a computed fact rather than a shrug. */
double storage_shed_quota(JSContext *ctx, JSValueConst proxy_map);

/* HOW MANY OTHER PROXY MAPS ARE IN THIS BOTTLE'S §4.6 PROXY MAP REFERENCE SET.
 * HTML §12.2.1's broadcast step 3 collects "all Storage objects excluding storage whose type is storage's type
 * and whose relevant settings object's origin is same origin with storage's". In an ORIGIN-KEYED AGENT CLUSTER
 * (CLAUDE.md §Security) there is one storage key per instance and therefore one bottle per (type, identifier),
 * so every same-origin Storage of that type holds a proxy map over THIS bottle and the intersection step 3
 * describes IS this set. Zero means the broadcast has nothing to fire at. */
int storage_shed_other_proxy_maps(JSContext *ctx, JSValueConst proxy_map);

#endif
