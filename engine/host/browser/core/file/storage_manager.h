/* StorageManager — Storage §2's interface, and File System §3's partial that puts getDirectory() on it.
   See storage_manager.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_STORAGE_MANAGER_H
#define ENGINE_HOST_BROWSER_CORE_FILE_STORAGE_MANAGER_H

#include "quickjs.h"

/* Declared once per AGENT, and AFTER navigator_init: the per-realm install puts `storage` on the Navigator
   that component's own intrinsic built, and core/realm.h runs the intrinsics in declaration order. */
void storage_manager_init(JSContext *ctx);
void storage_manager_free(void);

#endif
