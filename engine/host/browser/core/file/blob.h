/* THE Blob AND File INTERFACES — W3C File API §3 and §4. See blob.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_BLOB_H
#define ENGINE_HOST_BROWSER_CORE_FILE_BLOB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

void blob_init(JSContext *ctx);
void blob_install_protos(JSContext *ctx);   /* §3.1's and §4's prototypes, for ONE realm */
void blob_install(JSContext *ctx, JSValueConst global);
void blob_free(JSContext *ctx);

/* BUILD ONE from bytes the host already holds — Fetch's `blob()` reader, and every other spec that answers with
   a Blob it did not receive. `type` is the MIME type as the caller's own rules produced it, "" for none; it is
   stored as given, because the §3.1 lowercasing is the CONSTRUCTOR's step and not the interface's. */
JSValue blob_new(JSContext *ctx, const char *bytes, size_t len, const char *type);
/* The same with the type's LENGTH, for a caller holding it as a JS string: §3.1's check covers NUL, which a C
   string has already truncated at. */
JSValue blob_new_len(JSContext *ctx, const char *bytes, size_t len, const char *type, size_t type_len);

/* §4.1's File: a Blob wearing File.prototype and carrying a name. Fetch's multipart parser builds one for a
   part that names a filename, which is what a form's file control submits. */
JSValue file_new(JSContext *ctx, const char *bytes, size_t len, const char *type, size_t type_len,
                 const char *name, size_t name_len, int64_t last_modified);

/* THE BLOB A VALUE IS, or NULL. The brand test Web IDL's unions perform — BlobPart's `Blob` arm, BodyInit's,
   and FormData's `append` overload all ask exactly this. `*plen` and `*ptype` may be NULL. */
const char *blob_bytes_of(JSValueConst v, size_t *plen, const char **ptype);
bool        blob_is(JSValueConst v);
/* THE FILE NAME one carries, or NULL when it is a plain Blob — which is the whole of what distinguishes the
   two, so this is also the "is it a File" test. */
const char *blob_file_name_of(JSValueConst v);
int64_t     blob_last_modified_of(JSValueConst v);

/* RECORD WHERE THIS BLOB'S BYTE SEQUENCE CAME FROM — the source identity of external input this engine did not
   compute, which for a File read off the virtual filesystem is the file itself. `shape` is the @H/@S display
   form and `src` is what a flow's path constraint and an @S candidate delivery are keyed by, exactly as
   core/frame/location.c declares them for the two URL sources.
   IT CHANGES NO BYTE. §3.3's `text()` mints the concolic AT THE READ (per read, so a candidate substitution can
   reach it), and every other reader answers with the real bytes. Called once, at the mint, by the component
   that knows the provenance; a second call is a byte sequence claiming two origins. */
void blob_set_source(JSContext *ctx, JSValueConst v, const char *shape, const char *src);

/* ---- File API §8's BLOB URL STORE ------------------------------------------------------------------------
 *
 * `URL.createObjectURL` and `URL.revokeObjectURL` are declared on the URL interface and defined by File API, so
 * url.c installs them and this owns them: the store is a map from a `blob:` URL to the Blob it names, and only
 * the component that knows what a Blob is can hold one alive.
 * `create` returns a malloc'd URL string, or NULL with a TypeError live when the argument is not a Blob. */
char *blob_url_create(JSContext *ctx, JSValueConst obj);
void  blob_url_revoke(JSContext *ctx, const char *url, size_t len);
/* THE BLOB A `blob:` URL NAMES, or JS_UNDEFINED — what a fetch of one resolves through. Borrowed. */
JSValueConst blob_url_lookup(const char *url, size_t len);

#endif
