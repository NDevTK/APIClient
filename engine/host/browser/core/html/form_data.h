/* THE FormData INTERFACE — XMLHttpRequest §5. See form_data.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FORM_DATA_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FORM_DATA_H
#include <stddef.h>

#include "quickjs.h"
#include "core/url/url.h"

void form_data_init(JSContext *ctx);
void form_data_install(JSContext *ctx, JSValueConst global);
void form_data_free(JSContext *ctx);

/* A FormData over an entry list the caller built — how `.formData()` hands back what it parsed out of a body
   without going through the interface's own members. The list is COPIED. */
JSValue form_data_new(JSContext *ctx, const UrlEncodedList *entries);

/* Fetch §5.2's `multipart/form-data` PARSER, which `.formData()` runs when the Content-Type says so. `entries`
   is filled with the parts; returns -1 for the spec's FAILURE, which is what makes the promise reject with a
   TypeError. The boundary is the Content-Type's own parameter and is the caller's to extract. */
int form_data_parse_multipart(const char *body, size_t len, const char *boundary, size_t blen,
                              UrlEncodedList *entries);

/* THE ENTRY LIST a FormData holds, or NULL when the value is not one. The brand test Fetch §5.1's BodyInit
   union performs, and what its `multipart/form-data` SERIALIZER reads. */
const UrlEncodedList *form_data_list_of(JSValueConst v);

/* Fetch §5.1's `multipart/form-data` SERIALIZER — the other direction of the parser above, and the body a
   `new Response(formData)` carries. `*out_n` is the length; the BOUNDARY it chose is written to `boundary`,
   which must hold at least FORM_DATA_BOUNDARY_MAX bytes, because the Content-Type has to name it. */
#define FORM_DATA_BOUNDARY_MAX 64
char *form_data_serialize_multipart(const UrlEncodedList *l, char *boundary, size_t *out_n);

#endif
