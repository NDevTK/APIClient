/* THE BODY MIXIN — WHATWG Fetch §5.2, which both Request and Response include. See body.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_BODY_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_BODY_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* §5.2's BODY, as the state an including interface holds. `has` is not `len != 0`: the spec distinguishes a
   NULL body from an empty one — `new Response()` has the first and `new Response("")` the second — and `.body`
   reports null for exactly one of them. `used` is the single-use latch, which a page's retry path tests. */
typedef struct { char *bytes; size_t len; int used; int has; } BodyState;

void body_state_free(JSContext *ctx, BodyState *b);

/* §5.1's "extract a body" — the ONE implementation of the BodyInit union, for both interfaces that take one.
   `*out_mime` is the arm's own Content-Type (malloc'd) or NULL for an arm that has none; the caller's remaining
   job is §6.4's "set it only if the header list has none". Returns -1 with a throw live. */
int  body_extract(JSContext *ctx, BodyState *b, JSValueConst init, char **out_mime);
/* Copy `len` bytes in, or NULL for the spec's null body. Returns -1 on OOM with an exception live. */
int  body_state_set(JSContext *ctx, BodyState *b, const char *bytes, size_t len);

/* DECLARE that an interface includes Body: the class its instances wear, and how to find the BodyState on one.
   `of` returns NULL when the value is not an instance, which is the receiver check every member performs.
   Returns a handle. There is ONE set of reader machines for the whole platform; the handle is what tells them
   which interface the receiver belongs to. */
/* `mime` is the object's Content-Type as its own header list reports it, or NULL — §5.2's `formData()` reads
   it to decide which parser the body goes through, and only the including interface knows where its headers
   live. Caller frees. */
int  body_declare(JSContext *ctx, JSClassID class_id, BodyState *(*of)(JSValueConst v),
                  char *(*mime)(JSContext *ctx, JSValueConst v), const char *iface);
/* INSTALL text/json/arrayBuffer/bytes and `bodyUsed` on the interface's prototype. */
void body_install(JSContext *ctx, JSValueConst proto, int handle);

#endif
