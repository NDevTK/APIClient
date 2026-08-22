/* FILE SYSTEM ACCESS §2.2 AND §2.3 — the "file-system" powerful feature, and the two members of
 * FileSystemHandle that are its doors. See file_system_access.c.
 *
 *   dictionary FileSystemHandlePermissionDescriptor { FileSystemPermissionMode mode = "read"; };
 *   [Exposed=(Window,Worker), SecureContext, Serializable]
 *   partial interface FileSystemHandle {
 *     Promise<PermissionState> queryPermission(optional FileSystemHandlePermissionDescriptor descriptor = {});
 *     Promise<PermissionState> requestPermission(optional FileSystemHandlePermissionDescriptor descriptor = {});
 *   };
 *
 * WHY THIS IS ITS OWN COMPONENT AND NOT PART OF core/file/file_system_handle.c. It is a DIFFERENT STANDARD over
 * the same objects: the File System Standard defines the model and the handles and never mentions permission,
 * and File System Access registers a powerful feature whose descriptor type is written over that model's
 * entries. The two members here are `partial interface` members and install onto the prototype the other file
 * builds; everything else in this file is Permissions' §4 registry data for one feature, which the other file
 * must not learn.
 *
 * THE FEATURE'S FOUR REGISTRY COLUMNS, AND WHERE EACH OF THEM ENDS UP.
 *   PERMISSION DESCRIPTOR TYPE — `FileSystemPermissionDescriptor : PermissionDescriptor { required
 *     FileSystemHandle handle; FileSystemPermissionMode mode = "read"; }`. `handle` is the SUBJECT and `mode`
 *     the ASPECT of core/permissions/permission_store.h's descriptor; the row that says so is in that file's
 *     registry, and the two things only this component can supply — the brand test for `handle`, and the
 *     constraints below — are DECLARED into it from here.
 *   PERMISSION STATE CONSTRAINTS — the four steps §2.2 writes, and only the first of them survives as C. Read
 *     them in order over this engine's model:
 *       1. "If entry represents a file system entry in a bucket file system, this descriptor's permission
 *          state must always be granted." REAL, and it is the one constraint that FIXES a state: a
 *          bucket-file-system handle answers "granted" without reading the store and without asking the user,
 *          which is why `navigator.storage.getDirectory()` needs no prompt.
 *       2. "Otherwise, if entry's parent is not null, this descriptor's permission state must be equal to the
 *          permission state for a descriptor with the same mode, and a handle representing entry's parent."
 *          DISCHARGED BY IDENTITY, exactly as §4's partial order between two aspects is when both are read out
 *          of one source: this engine's local file system is a real directory tree, so every entry a picker
 *          returns is a child of the local root and the walk this constraint describes terminates there — and
 *          §3.2's store entry is keyed by the descriptor WITHOUT its subject, so the descriptor the walk ends
 *          at indexes the very slot the descriptor it started from does. Two descriptors constrained to be
 *          equal that read one slot are equal in every world, which is stronger than evaluating the constraint
 *          and cannot come apart from it. It stops being an identity the moment a store entry is keyed by the
 *          subject, and permission_store.h's own assert on the key is what says so.
 *       3. "Otherwise, if desc['mode'] is readwrite: ... if read state is not granted, this descriptor's
 *          permission state must be equal to read state." That is §4's PARTIAL ORDER with readwrite as the
 *          stronger descriptor, which core/permissions/permission_store.c already discharges for every feature
 *          that declares an aspect — the registry row's STRONG column is where this constraint is written.
 *   PERMISSION REQUEST ALGORITHM — §2.2's nine steps, which are the whole of `requestPermission()` and are
 *     implemented in this file's machine rather than as a function, because six of them are questions and a
 *     machine may hold exactly one outstanding.
 *   Every other column is left at §4's default, which is what File System Access says by not writing one: the
 *     default permission query algorithm, the default origin permission key, and NO permission revocation
 *     algorithm — which is why Permissions §5.4's react to the user revoking permission has no caller here. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_ACCESS_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_ACCESS_H
#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT, AFTER fs_handle_init and AFTER permissions_init. The order is a dependency and not a
   convention: the two members install onto §2.2's prototype (so the component that builds it declares its
   per-realm intrinsic first, and core/realm.h runs them in declaration order), and the feature's brand test and
   constraints are declared into §4's registry (so the registry has to exist). */
void file_system_access_init(JSContext *ctx);
void file_system_access_free(void);

/* FILE SYSTEM ACCESS §3.1's GRANT — "the fact that the user picked the specific files returned by the local
 * file system handle factories in a prompt should be treated by the user agent as the user intending to grant
 * read access to the website for the returned files. As such, at the time the promise returned by one of the
 * local file system handle factories resolves, permission state for a descriptor with handle set to the
 * returned handle, and mode set to 'read' should be 'granted'. Additionally for calls to showSaveFilePicker
 * the permission state ... and mode set to 'readwrite' should be 'granted'."
 *
 * IT IS A STORE WRITE AND THEREFORE CONCRETE, which is the whole point of putting it here rather than leaving
 * the picker's handle to answer out of the unknown. The user's INTENT is what §5.1 step 8 and §5.2 step 3 are
 * ignorant of; this is not that — the user agent has just OBSERVED the user pick these files, and a value the
 * engine watched happen is a value it knows (core/permissions/permission_store.h states the same rule for a
 * store entry, and core/dom/abort.c for a controller's own signal). Forking here would park a sibling flow in
 * which the user picked a file and did not pick it.
 *
 * `readwrite` writes BOTH descriptors, because §4's partial order makes the readwrite grant imply the read one
 * and writing only the strong half would leave the store's own order assert to derive what §3.1 states
 * outright. */
void fs_access_grant(JSContext *ctx, JSValueConst handle, bool readwrite);

#endif
