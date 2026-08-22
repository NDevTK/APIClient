/* THE ONE VIRTUAL FILESYSTEM — File System Standard §2.1's data model. See file_system.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* §2.1's TWO FILE SYSTEM ROOTS — "an opaque string whose value is implementation-defined". There are exactly
   two in this engine and they are two because the standards say so: §3's BUCKET FILE SYSTEM is reached with no
   prompt at all (`navigator.storage.getDirectory()`), and File System Access §3's LOCAL FILE SYSTEM is what a
   PICKER chooses from and what the mock device seeds. They are the same MODEL over two roots, never two
   stores.
   THE BUCKET ROOT'S PATH IS « "" », which §2.2 makes load-bearing: "a FileSystemHandle is in a bucket file
   system if the first item of its locator's path is the empty string", and every other path item is a valid
   file name, which the empty string is not. So the local root's path is « "local" » — non-empty, so that one
   test answers correctly for both without a second flag to keep in step with it. */
#define FS_ROOT_BUCKET "bucket"
#define FS_ROOT_LOCAL  "local"

/* Declared once per AGENT. BOTH ROOTS ARE BUILT HERE, at the pre-boot COW baseline, and §3's "if map["root"]
   does not exist" is therefore already satisfied when the first getDirectory() runs — a root built lazily on
   the first read would be built inside whichever flow asked first, making that flow's creation every sibling's
   baseline. Nothing can observe the difference: a root that has just been created and a root created at init
   are both an empty directory entry whose name is the empty string. */
void file_system_init(JSContext *ctx);
void file_system_free(JSRuntime *rt);

/* §2.1's ROOT DIRECTORY ENTRY for one of the two roots. OWNED. */
JSValue file_system_root_entry(JSContext *ctx, const char *root);

/* §2.1's LOCATE AN ENTRY given a file system locator. `path` is the locator's path, whose FIRST item names the
   root (the root entry's own name) and whose remaining items are the walk. Returns the entry (OWNED) or
   JS_NULL, which is the "returns a file entry or null" the algorithm's constraints state.
   IT DOES NOT CHECK THE LOCATOR'S KIND: §2.1 says a file locator locates a file entry or null, and every
   caller of this asserts that where its own algorithm asserts it ("Assert: entry is a file entry"). */
JSValue file_system_locate(JSContext *ctx, const char *root, const char *const *path, int npath);

/* §2.1's TWO KINDS. A file system entry is either a file entry or a directory entry and nothing else, which is
   what the DCHECK inside these asserts rather than what a caller has to test twice. */
bool file_system_is_directory(JSValueConst entry);
bool file_system_is_file(JSValueConst entry);

/* A directory entry's CHILDREN — §2.1's "a set of children, which are themselves file system entries".
   `file_system_child` is the "for each child of entry's children, if child's name equals name" every one of
   §2.4's methods opens with. OWNED, or JS_NULL when no child has that name. */
JSValue file_system_child(JSContext *ctx, JSValueConst dir, const char *name);
/* THE CHILDREN AS A LIST, for the iteration §2.4.1 describes and for the device's own selection. `pname` takes
   the name (OWNED, a JS string) and the answer is the entry (OWNED). §2.4.1: "This is intentionally very vague
   about the iteration order", so the order is the record's own property order. */
int     file_system_child_count(JSContext *ctx, JSValueConst dir);
JSValue file_system_child_at(JSContext *ctx, JSValueConst dir, int i, JSValue *pname);
bool    file_system_children_empty(JSContext *ctx, JSValueConst dir);

/* §2.4.2 and §2.4.3's CREATE branches — "let child be a new file entry / directory entry whose query access and
   request access algorithms are those of entry", named, and appended to entry's children. A file entry is born
   with an empty byte sequence and the current time; a directory entry with an empty set of children. The name
   is NOT validated here: §2.4.2 step 1 validates it before the entry is located, so a caller that reached this
   has already run that step, and this asserts it. OWNED. */
JSValue file_system_create_file(JSContext *ctx, JSValueConst dir, const char *name);
JSValue file_system_create_directory(JSContext *ctx, JSValueConst dir, const char *name);
/* §2.4.4's "remove child from entry's children". The child must be there — §2.4.4 found it by name first. */
void    file_system_remove_child(JSContext *ctx, JSValueConst dir, const char *name);

/* A FILE ENTRY'S BINARY DATA AS A JS VALUE, which is the whole reason the model is built out of JS values.
 *
 * Two things ride on it. It is MUTABLE STATE A FLOW WRITES, so it must be a property write the per-flow COW
 * delta captures and a value the snapshot machinery can park to the cold tier — a malloc'd byte buffer hung off
 * a C struct is neither. And a file's CONTENTS ARE EXTERNAL INPUT in the same sense `location.hash` is, so the
 * value may be a CONCOLIC carrying the real bytes as its example; a `char *` could not hold one, and the XSS
 * path a file's bytes reach a sink by would not exist.
 *
 * The byte sequence itself is a JS STRING WITH ONE CODE UNIT PER BYTE — §2.1's byte sequence, exactly, with no
 * encoding applied. A string is immutable, so replacing it is a property write and there is no in-place
 * mutation for the delta to miss; an ArrayBuffer's contents are mutated where no property write happens, which
 * is the one shape that would silently escape the delta. */
JSValue file_system_data(JSContext *ctx, JSValueConst file);              /* OWNED */
void    file_system_set_data(JSContext *ctx, JSValueConst file, JSValue data);   /* CONSUMES `data` */
double  file_system_modified(JSContext *ctx, JSValueConst file);
/* §2.1's modification timestamp, set to the current time. Called by every write that lands on the entry. */
void    file_system_touch(JSContext *ctx, JSValueConst file);

/* THE BYTE SEQUENCE <-> JS VALUE PAIR. `file_system_bytes_value` builds the one-code-unit-per-byte string;
   `file_system_value_bytes` reads one back, taking a CONCOLIC's EXAMPLE where the value is one (which is what
   makes an attacker-controlled file still produce a real File a page can read). The answer is malloc'd and NUL
   terminated for a caller that wants a C string; `*plen` is the true length. */
JSValue file_system_bytes_value(JSContext *ctx, const char *bytes, size_t len);
char   *file_system_value_bytes(JSContext *ctx, JSValueConst v, size_t *plen);

/* §2.1's VALID FILE NAME — "a string that is not an empty string, is not equal to '.' or '..', and does not
   contain '/' or any other character used as path separator on the underlying platform". The underlying
   platform here is this model, whose only separator is the path LIST, so '/' is the one separator character —
   and U+005C is excluded with it, because the standard's own note says it "is not allowed in names on
   Windows", and a name this engine accepts is a name a real user agent may not. */
bool file_system_valid_name(const char *name, size_t len);

/* §2.1's TAKE A LOCK / RELEASE A LOCK on a file entry. `exclusive` picks the "exclusive" or "shared" value.
   Returns the algorithm's own "success" / "failure". */
bool file_system_take_lock(JSContext *ctx, JSValueConst file, bool exclusive);
void file_system_release_lock(JSContext *ctx, JSValueConst file);

/* THE MOCK DEVICE'S ONE EDGE — put a file on the LOCAL FILE SYSTEM, which is the storage a picker chooses
   from. `name` has its PATH COMPONENTS stripped here (HTML §4.10.5.1.17: "Filenames must not contain path
   components ... those parts of filenames that are separated by U+005C"), so nothing downstream has to
   remember that rule and `C:\fakepath\` can never be mistaken for one. `type` is the MIME type this storage
   records for the file, "" for none; it is a fact about the file rather than about its bytes, so the entry
   carries it beside them even though §2.1 does not name it — §2.3.1 says a File's type is "an
   implementation-defined value, based on for example entry's name or its file extension", and a device that
   was told the type answers with what it was told.
   THE BYTES BECOME AN ATTACKER SOURCE. A file on this device is content this engine did not compute — it is
   the same kind of input as a fragment, and every path a bundle takes over it is a path forced execution must
   be able to explore both arms of. */
void file_system_local_add(JSContext *ctx, const char *name, const char *type,
                           const char *bytes, size_t len, double last_modified);
/* §2.3.1's File OVER A FILE ENTRY — "let f be a new File; set f's underlying byte sequence to a copy of
   entry's binary data; set f's name to entry's name; set f's lastModified to entry's modification timestamp;
   set f's type to an implementation-defined value".
   ONE IMPLEMENTATION, because two doors reach it: §2.3.1's `getFile()` and HTML §4.10.5.1.17's file-control
   selection, and a second copy would be a second chance for the two to disagree about the type or — the half
   that matters — about the SOURCE. Where the entry's binary data carries a source identity, the File carries
   it too, so `f.text()` answers with the attacker's bytes AND with the fact that an attacker chose them.
   Minted in `ctx`'s realm, because a File wears that realm's File.prototype. OWNED. */
JSValue file_system_file_new(JSContext *ctx, JSValueConst entry);

/* The MIME type the device recorded for a file entry, or "" — OWNED (JS_FreeCString). */
const char *file_system_type_cstr(JSContext *ctx, JSValueConst file);
/* The entry's §2.1 NAME. OWNED (JS_FreeCString). */
const char *file_system_name_cstr(JSContext *ctx, JSValueConst entry);

#endif
