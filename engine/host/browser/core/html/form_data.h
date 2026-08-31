/* THE FormData INTERFACE — XMLHttpRequest §5. See form_data.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FORM_DATA_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FORM_DATA_H
#include <stddef.h>

#include <stdbool.h>

#include "quickjs.h"
#include "core/url/url.h"

void form_data_init(JSContext *ctx);
void form_data_install_proto(JSContext *ctx);   /* §5's prototype, for ONE realm */
void form_data_install(JSContext *ctx, JSValueConst global);
void form_data_free(JSContext *ctx);

/* A FormData over an entry list the caller built — how `.formData()` hands back what it parsed out of a body
   without going through the interface's own members. The list is COPIED. */
JSValue form_data_new(JSContext *ctx, const UrlEncodedList *entries);

/* Fetch §5.3 "Body mixin"'s `multipart/form-data` PARSER, which `.formData()` runs when the Content-Type says
   so — the parse is §5.3's own `formData()` method steps, delegating the grammar to RFC 7578. (§5.2 "BodyInit
   unions" stood here and names multipart only as the ENCODING an extract runs, the inverse of this.) Returns
   the FormData, or JS_EXCEPTION with the TypeError the spec's FAILURE makes the promise reject with. It builds
   the object rather than filling a list because a part with a `filename` is a FILE entry, and a File is a
   JSValue only this component can put in an entry. The boundary is the Content-Type's own parameter and is the
   caller's to extract. */
JSValue form_data_parse_multipart(JSContext *ctx, const char *body, size_t len,
                                  const char *boundary, size_t blen);

/* IS THIS A FormData — the brand test Fetch §5.2 "BodyInit unions" performs, FormData being one arm of
   `XMLHttpRequestBodyInit`. (§5.1 stood here and is "Headers class", which declares no union.) */
bool form_data_is(JSValueConst v);
/* The CLASS a `FormData`-typed Web IDL position brands against — §4.10.22.1's `required FormData formData`
   member is the first, and the brand is part of the TYPE rather than something a body re-tests. */
JSClassID form_data_class_id(void);

/* ---- THE ENTRY LIST, AS THE THING HTML BUILDS INTO ---------------------------------------------------------
 *
 * HTML §4.10.22.4 step 6 makes "a new FormData object ASSOCIATED WITH entry list" — the same list, not a copy —
 * so this engine builds the entry list AS a FormData from step 4 onward. That is not a shortcut past the
 * spec's `entry list`: nothing observes the list between steps 4 and 6, and by step 6 the two are one object by
 * definition. It is also what §State-isolation requires of it — the list is a JS value on the flow's step
 * state, so it forks per flow and parks to the cold tier with the flow that is building it, which a malloc'd C
 * list on the side could not do.
 *
 * `value` may be a CONCOLIC. §4.10.18.1's value state holds a JSValue rather than bytes exactly so that
 * `input.value = location.hash` reaches the submission as the source it came from; an entry carries it
 * unchanged, and the serialisers below render its shape. */

/* HTML §4.10.22.4's "create an entry ... and append it to entry list", for the ENGINE's own construction.
   `value` is CONSUMED. `name` is borrowed. */
void form_data_append_entry(JSContext *ctx, JSValueConst fd, const char *name, size_t nlen, JSValue value);
/* §4.13.7.3's entry-construction step 1: "append each item of element's submission value to entry list" —
   every entry of `src`, in order, onto `dst`. */
void form_data_append_all(JSContext *ctx, JSValueConst dst, JSValueConst src);

/* THE ENTRY LIST, READ BACK. §4.10.21.3's request derivation walks it; the name is BORROWED from the list and
   the value is BORROWED too, both valid until the FormData is mutated or released. */
int          form_data_entry_count(JSValueConst fd);
const char  *form_data_entry_name(JSValueConst fd, int i, size_t *plen);
JSValueConst form_data_entry_value(JSValueConst fd, int i);

/* INFRA's "clone" of a FormData's ENTRY LIST, as a fresh FormData — HTML §4.13.7.3's `setFormValue` stores
   one rather than the object it was handed, so a later `append` to that object changes nothing about what the
   element submits. OWNED. */
JSValue form_data_clone(JSContext *ctx, JSValueConst src);

/* HTML §4.10.22.8 "Multipart form data"'s `multipart/form-data` encoding algorithm — the SERIALIZER, the other
   direction of the parser above, and the body a `new Response(formData)` carries. It is HTML's and not
   Fetch's: Fetch §5.2 "BodyInit unions"'s extract REACHES it for the FormData arm and lists it among the terms
   it defines by reference, and "Fetch §5.1's" stood here, which is "Headers class".
   `*out_n` is the length; the BOUNDARY it chose is written to `boundary`,
   which must hold at least FORM_DATA_BOUNDARY_MAX bytes, because the Content-Type has to name it. */
#define FORM_DATA_BOUNDARY_MAX 64
char *form_data_serialize_multipart(JSContext *ctx, JSValueConst fd, char *boundary, size_t *out_n);

#endif
