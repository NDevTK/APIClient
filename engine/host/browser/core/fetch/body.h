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
/* Copy `len` bytes in, or NULL for the spec's null body. Returns -1 on OOM with an exception live. */
int  body_state_set(JSContext *ctx, BodyState *b, const char *bytes, size_t len);

/* DECLARE that an interface includes Body: the class its instances wear, and how to find the BodyState on one.
   `of` returns NULL when the value is not an instance, which is the receiver check every member performs.
   Returns a handle. There is ONE set of reader machines for the whole platform; the handle is what tells them
   which interface the receiver belongs to. */
int  body_declare(JSContext *ctx, JSClassID class_id, BodyState *(*of)(JSValueConst v), const char *iface);
/* INSTALL text/json/arrayBuffer/bytes and `bodyUsed` on the interface's prototype. */
void body_install(JSContext *ctx, JSValueConst proto, int handle);

#endif
