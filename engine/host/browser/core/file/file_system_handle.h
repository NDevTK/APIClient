/* FileSystemHandle, FileSystemFileHandle and FileSystemDirectoryHandle — File System Standard §2.2-§2.4.
   See file_system_handle.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_HANDLE_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_HANDLE_H
#include <stdbool.h>

#include "quickjs.h"

void fs_handle_init(JSContext *ctx);
void fs_handle_free(void);

/* File System Standard §2.3 and §2.4's "CREATE A NEW FileSystemFileHandle / FileSystemDirectoryHandle given a
   file system root and a file system path in a Realm" — the constructor every entry point uses. The same
   standard's §3 Accessing the Bucket File System is where getDirectory is defined, and it calls this with the
   bucket root and « "" »; a picker calls it with the local root and the path of what the user chose.
   `path` is COPIED. Minted in `ctx`'s realm, because a handle wears that realm's prototype. OWNED. */
JSValue fs_handle_new(JSContext *ctx, bool directory, const char *root, const char *const *path, int npath);

/* Is this value a FileSystemHandle at all — the brand check Web IDL §3.2.15 Interface types performs ("If V
   implements I, then return the IDL interface type value … Throw a TypeError"), and the one an IDL_INTERFACE
   argument position brands against. This read §3.7.5, which is Web IDL's Constants and has no brand check in
   it; the only other place that standard writes the words is §2.13.36, which is ObservableArray. */
bool fs_handle_is(JSValueConst v);

/* File System Standard §2.1's FILE SYSTEM LOCATOR, READ OFF A HANDLE — the kind, the root and the path,
   exactly the three facts that section says a locator is. False when `v` is not a handle, which is the same
   question fs_handle_is answers and is answered here too so a caller that needs the locator makes ONE test
   rather than two that can disagree.
     BORROWED and IMMUTABLE: File System Standard §2.2 has no step that writes a locator, so the strings live
   as long as the handle
   and a caller holding the handle holds them. File System Access §2.2's permission state constraints are
   written over the ENTRY a locator names and its PARENT, which is the locator's path minus its last item — so
   the path is what that component walks, and it must not have to re-derive it from a second copy. */
bool fs_handle_locator(JSValueConst v, bool *directory, const char **root, const char *const **path, int *npath);

/* File System Standard §2.2's INTERFACE PROTOTYPE OBJECT for THIS realm, which File System Access §2.3's
   partial interface installs `queryPermission` and `requestPermission` on. THE TWO STANDARDS' NUMBERS COLLIDE
   AND BOTH ARE NAMED FOR THAT REASON: FS §2.2 is "The FileSystemHandle interface" while FSA §2.2 is
   "Permissions", and FS §2.3 is "The FileSystemFileHandle interface" while FSA §2.3 is "The FileSystemHandle
   interface" — so a bare §2.2 or §2.3 anywhere in this file names two different sections of two different
   documents. OWNED, or JS_NULL in a realm that is not a secure context —
   where all three interfaces are absent, so the partial has nothing to extend and installs nothing. */
JSValue fs_handle_proto(JSContext *ctx);

#endif
