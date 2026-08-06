/* THE FormData INTERFACE — XMLHttpRequest §5. See form_data.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FORM_DATA_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FORM_DATA_H
#include <stddef.h>

#include <stdbool.h>

#include "quickjs.h"
#include "core/url/url.h"

void form_data_init(JSContext *ctx);
void form_data_install(JSContext *ctx, JSValueConst global);
void form_data_free(JSContext *ctx);

/* A FormData over an entry list the caller built — how `.formData()` hands back what it parsed out of a body
   without going through the interface's own members. The list is COPIED. */
JSValue form_data_new(JSContext *ctx, const UrlEncodedList *entries);

/* Fetch §5.2's `multipart/form-data` PARSER, which `.formData()` runs when the Content-Type says so. Returns
   the FormData, or JS_EXCEPTION with the TypeError the spec's FAILURE makes the promise reject with. It builds
   the object rather than filling a list because a part with a `filename` is a FILE entry, and a File is a
   JSValue only this component can put in an entry. The boundary is the Content-Type's own parameter and is the
   caller's to extract. */
JSValue form_data_parse_multipart(JSContext *ctx, const char *body, size_t len,
                                  const char *boundary, size_t blen);

/* IS THIS A FormData — the brand test Fetch §5.1's BodyInit union performs. Its ENTRY LIST is not exposed:
   an entry's value is `(USVString or File)`, which is a JSValue this component owns, and the one thing another
   spec asks of the list is the serializer below. */
bool form_data_is(JSValueConst v);

/* Fetch §5.1's `multipart/form-data` SERIALIZER — the other direction of the parser above, and the body a
   `new Response(formData)` carries. `*out_n` is the length; the BOUNDARY it chose is written to `boundary`,
   which must hold at least FORM_DATA_BOUNDARY_MAX bytes, because the Content-Type has to name it. */
#define FORM_DATA_BOUNDARY_MAX 64
char *form_data_serialize_multipart(JSContext *ctx, JSValueConst fd, char *boundary, size_t *out_n);

#endif
