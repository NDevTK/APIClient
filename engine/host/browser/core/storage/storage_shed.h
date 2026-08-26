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

/* WHICH Storage OBJECT THIS PROXY MAP STANDS FOR. §4.6 mints one proxy map per obtain and HTML §12.2.2/§12.2.3
   wrap exactly one Storage around each, so the reference set and the set of Storage objects are the same set
   counted from two ends — and this is the link between them. Written once, by the mint, before the object is
   reachable; asserted to be written once, because two Storages over one proxy map would make §12.2.1's
   broadcast fire twice at one document. */
void storage_shed_proxy_map_bind(JSContext *ctx, JSValueConst proxy_map, JSValueConst storage);

/* HTML §12.2.1's broadcast STEP 3 — "let remoteStorages be all Storage objects EXCLUDING storage whose type is
 * storage's type and whose relevant settings object's origin is same origin with storage's" (and, for
 * "session", whose traversable is this one) — as a JS Array of those Storage objects, in reference-set order.
 *
 * IT IS THIS BOTTLE'S §4.6 PROXY MAP REFERENCE SET MINUS THIS MAP, because in an ORIGIN-KEYED AGENT CLUSTER
 * (CLAUDE.md §Security) there is one storage key per instance and therefore one bottle per (type, identifier):
 * every same-origin Storage of that type holds a proxy map over THIS bottle, so the intersection step 3
 * describes IS this set and "excluding storage" is this object's own map, excluded here by identity. An EMPTY
 * array is the ordinary case — one document, one holder, one proxy map — and step 4 over it is a step with
 * nothing to do rather than an absence to guard against. OWNED. */
JSValue storage_shed_other_storages(JSContext *ctx, JSValueConst proxy_map);

#endif
