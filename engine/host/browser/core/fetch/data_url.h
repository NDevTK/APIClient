/* THE data: URL PROCESSOR — WHATWG Fetch §6. See data_url.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_DATA_URL_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_DATA_URL_H
#include <stdbool.h>
#include <stddef.h>

#include "core/mime/mime_type.h"
#include "core/url/url.h"

/* §6: "A data: URL struct is a struct that consists of a MIME type and a body (a byte sequence)." The body is
   a byte sequence and not a string: `data:application/octet-stream;base64,AAA=` decodes to a NUL, and a
   consumer that took a strlen of it would deliver nothing. */
typedef struct {
    MimeType mime;
    char    *body;
    size_t   body_len;
} DataUrlStruct;

/* §6's "data: URL processor". False is the spec's FAILURE, which §4.3's scheme fetch turns into a NETWORK
   ERROR — there are exactly two of them (no U+002C in the URL at all, and a base64 body that
   forgiving-base64 decode rejects) and everything else the standard recovers from. `out` is left
   initialised-and-empty on failure, so the caller frees it either way. */
bool data_url_process(const UrlRecord *data_url, DataUrlStruct *out);
void data_url_struct_free(DataUrlStruct *s);

#endif
