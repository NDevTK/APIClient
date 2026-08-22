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

/* §2.1's FILE SYSTEM LOCATOR, READ OFF A HANDLE — the kind, the root and the path, exactly the three facts §2.1
   says a locator is. False when `v` is not a handle, which is the same question fs_handle_is answers and is
   answered here too so a caller that needs the locator makes ONE test rather than two that can disagree.
     BORROWED and IMMUTABLE: §2.2 has no step that writes a locator, so the strings live as long as the handle
   and a caller holding the handle holds them. File System Access §2.2's permission state constraints are
   written over the ENTRY a locator names and its PARENT, which is the locator's path minus its last item — so
   the path is what that component walks, and it must not have to re-derive it from a second copy. */
bool fs_handle_locator(JSValueConst v, bool *directory, const char **root, const char *const **path, int *npath);

/* §2.2's INTERFACE PROTOTYPE OBJECT for THIS realm, which File System Access §2.3's partial interface installs
   `queryPermission` and `requestPermission` on. OWNED, or JS_NULL in a realm that is not a secure context —
   where all three interfaces are absent, so the partial has nothing to extend and installs nothing. */
JSValue fs_handle_proto(JSContext *ctx);

#endif
