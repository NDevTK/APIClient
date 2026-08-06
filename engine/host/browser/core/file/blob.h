/* THE Blob AND File INTERFACES — W3C File API §3 and §4. See blob.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_BLOB_H
#define ENGINE_HOST_BROWSER_CORE_FILE_BLOB_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

void blob_init(JSContext *ctx);
void blob_install(JSContext *ctx, JSValueConst global);
void blob_free(JSContext *ctx);

/* BUILD ONE from bytes the host already holds — Fetch's `blob()` reader, and every other spec that answers with
   a Blob it did not receive. `type` is the MIME type as the caller's own rules produced it, "" for none; it is
   stored as given, because the §3.1 lowercasing is the CONSTRUCTOR's step and not the interface's. */
JSValue blob_new(JSContext *ctx, const char *bytes, size_t len, const char *type);
/* The same with the type's LENGTH, for a caller holding it as a JS string: §3.1's check covers NUL, which a C
   string has already truncated at. */
JSValue blob_new_len(JSContext *ctx, const char *bytes, size_t len, const char *type, size_t type_len);

/* THE BLOB A VALUE IS, or NULL. The brand test Web IDL's unions perform — BlobPart's `Blob` arm, BodyInit's,
   and FormData's `append` overload all ask exactly this. `*plen` and `*ptype` may be NULL. */
const char *blob_bytes_of(JSValueConst v, size_t *plen, const char **ptype);
bool        blob_is(JSValueConst v);

#endif
