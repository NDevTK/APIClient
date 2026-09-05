/* THE FileReader INTERFACE — W3C File API §6 Reading Data. See file_reader.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_READER_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_READER_H
#include <stddef.h>

#include "quickjs.h"

void file_reader_init(JSContext *ctx);
/* File API §6.2 "The FileReader API"' PROTOTYPE AND ITS INTERFACE OBJECT, FOR ONE REALM — run where a realm's
   other intrinsics are added, exactly once per realm. Web IDL §3.8 "Platform objects implementing interfaces"
   is given a realm and names no Document, and §6.2 is `[Exposed=(Window,Worker)]`, so the name is owed by a
   realm that reaches no per-document install. */
void file_reader_install_proto(JSContext *ctx);
void file_reader_free(JSRuntime *rt);

/* §6.3 Packaging data's FOUR RESULT KINDS, which are the `type` operand of both the read operation (§6.2) and
   every synchronous read method (§6.5.1). The order is §6.3's own switch order. */
typedef enum {
    FILE_READ_DATA_URL = 0,
    FILE_READ_TEXT,
    FILE_READ_ARRAY_BUFFER,
    FILE_READ_BINARY_STRING,
} FileReadType;

/* §6.3 Packaging data — "A Blob has an associated package data algorithm, given bytes, a type, a mimeType, and
   an optional encodingLabel". It is DECLARED as a Blob's algorithm and takes no Blob: its operands are a byte
   sequence, a result kind, the blob's `type` STRING and an optional encoding label, so it is a pure function of
   four values and belongs beside the two algorithms that run it — §6.2's read operation here, and §6.5.1's
   synchronous read methods on the day a worker global exists to expose them.

   `mime_type` is the Blob's own §3.1-normalised type ("" for none) and `encoding_label` is NULL for §6.3's
   "the encodingLabel is present" being false — a POSITIVE statement of absence rather than a hole, because the
   Text arm's step order distinguishes an absent label from one that fails to name an encoding.

   Returns a new owned value — a DOMString for DataURL/Text/BinaryString and an ArrayBuffer for ArrayBuffer —
   or JS_EXCEPTION with the throw live, which §6.2 step 10.5.3 makes the FileReader's `error`. */
JSValue file_reader_package_data(JSContext *ctx, const char *bytes, size_t len, FileReadType type,
                                 const char *mime_type, const char *encoding_label);

#endif
