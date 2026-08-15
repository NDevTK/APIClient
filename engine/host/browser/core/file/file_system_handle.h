/* FileSystemHandle, FileSystemFileHandle and FileSystemDirectoryHandle — File System Standard §2.2-§2.4.
   See file_system_handle.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_HANDLE_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_HANDLE_H
#include <stdbool.h>

#include "quickjs.h"

void fs_handle_init(JSContext *ctx);
void fs_handle_free(void);

/* §2.3 and §2.4's "CREATE A NEW FileSystemFileHandle / FileSystemDirectoryHandle given a file system root and a
   file system path in a Realm" — the constructor every entry point uses. §3's getDirectory calls it with the
   bucket root and « "" »; a picker calls it with the local root and the path of what the user chose.
   `path` is COPIED. Minted in `ctx`'s realm, because a handle wears that realm's prototype. OWNED. */
JSValue fs_handle_new(JSContext *ctx, bool directory, const char *root, const char *const *path, int npath);

/* Is this value a FileSystemHandle at all — §3.7.5's brand check, and the one an IDL_INTERFACE argument
   position brands against. */
bool fs_handle_is(JSValueConst v);

#endif
