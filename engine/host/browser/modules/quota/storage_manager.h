/* StorageManager (navigator.storage) — the Storage API as a real Blink-style module (modules/quota/).
 * estimate() resolves to concolic usage/quota numbers, persist()/persisted() to a concolic bool, so
 * `storage.persisted().then(p => p ? offline : online)` and quota-gated feature checks explore BOTH arms (a PWA's
 * offline/caching surface reached without the real grant). getDirectory (OPFS) DFAILs — the forcing function to
 * build the FileSystemDirectoryHandle root at the root. Split out of navigator.c so the concern owns its file +
 * DCHECKs + a subagent behind one contract (storage_manager_make); unbuilt members DFAIL via idl_dfail_wrap. */
#ifndef ENGINE_HOST_BROWSER_MODULES_QUOTA_STORAGE_MANAGER_H
#define ENGINE_HOST_BROWSER_MODULES_QUOTA_STORAGE_MANAGER_H
#include "quickjs.h"

/* The navigator.storage object (a StorageManager instance). */
JSValue storage_manager_make(JSContext *ctx);

#endif
