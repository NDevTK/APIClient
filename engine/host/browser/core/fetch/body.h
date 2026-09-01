/* THE BODY MIXIN — WHATWG Fetch §5.3 "Body mixin", which both Request and Response include. See body.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_BODY_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_BODY_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* §5.3's BODY, as the state an including interface holds. `has` is not `len != 0`: the spec distinguishes a
   NULL body from an empty one — `new Response()` has the first and `new Response("")` the second — and `.body`
   reports null for exactly one of them. `used` is the single-use latch, which a page's retry path tests. */
/* `stream` is §5.3's `body`, built ON DEMAND and then held: the attribute answers the SAME stream every time,
   and a second one would give a page two independent readers over one body. JS_UNDEFINED until asked for. */
/* `source_null` IS §5.2 BodyInit unions' `source`, ASKED AS THE ONE QUESTION ANY ALGORITHM ASKS OF IT. The
   extraction opens "Let source be null" and then sets it in every arm but ONE — a Blob, a byte sequence, a
   BufferSource, a FormData, a URLSearchParams and a scalar value string each name their bytes, and the
   ReadableStream arm names nothing, because the bytes do not exist yet. Fetch §5.4 new Request(input, init)
   step 39 is the reader, and what it branches on is exactly "inputOrInitBody's source is null" — never what
   the source IS — so the field is the predicate rather than the value.
   IT IS NOT `bytes == NULL` AND IT IS NOT `stream != undefined`, which is why it is stored rather than
   derived. `new Response()` and a DETACHED BufferSource both carry no bytes and have a source that is not
   null; and `stream` is ALSO minted on demand by the `.body` getter, so an ordinary byte-sourced body that a
   page has read once is indistinguishable from a stream-sourced one by that slot. Deriving it either way
   answers step 39 for a body the page never streamed.
   §2.2.4 Bodies' clone-a-body says "other members are copied from body", so it rides a clone and a proxy. */
typedef struct { char *bytes; size_t len; int used; int has; int source_null; JSValue stream; } BodyState;

/* RELEASE EVERYTHING A BodyState OWNS — the bytes AND the stream. It takes a RUNTIME because the place that
   must call it is an including interface's FINALIZER, which has no context; the two that free the bytes by
   hand instead each forgot the stream, and once a body could BE the page's stream that leak was the whole
   runtime graph held by one Response. One declaration of what this owns, one release. */
void body_state_free(JSRuntime *rt, BodyState *b);

/* §5.2's "extract a body" — the ONE implementation of the BodyInit union, for both interfaces that take one.
   `*out_mime` is the arm's own Content-Type (malloc'd) or NULL for an arm that has none; the caller's remaining
   job is §5.5's "set it only if the header list has none". Returns -1 with a throw live.
   `keepalive` IS §5.2's OWN OPTIONAL ARGUMENT ("with an optional boolean keepalive (default false)"), and it
   is a parameter rather than a caller-side test because it decides a step INSIDE one arm: the ReadableStream
   arm's first step is "If keepalive is true, then throw a TypeError". A caller that tested the brand itself
   would be a second copy of the union's arm list — the thing this function exists to have exactly one of.
   Beacon §3 step 6.1 is the one caller that sets it; everything else extracts with the flag unset, which is
   what `keepalive`'s IDL default already says for a Request that never declared one. */
int  body_extract(JSContext *ctx, BodyState *b, JSValueConst init, bool keepalive, char **out_mime);
/* Copy `len` bytes in, or NULL for the spec's null body. Returns -1 on OOM with an exception live. */
int  body_state_set(JSContext *ctx, BodyState *b, const char *bytes, size_t len);

/* §2.2.4's "CLONE A BODY", which is a TEE and nothing else: « out1, out2 » are the result of teeing the source
   body's stream, the source keeps out1 and the clone gets out2. Copying the bytes instead is not a cheaper
   spelling of the same thing — after a clone the ORIGINAL's `body` is a different object from the one it was,
   the old one is locked, and a page teeing a response's body depends on exactly that. It also means a body
   whose source IS a page's stream can be cloned at all, which a byte copy could not do: there are no bytes.
 *
 * A TEE IS A CALL of code the page can reach, so this is a REQUEST: it returns JS_STEP_CALL (which the calling
 * machine returns), 0 once both sides are set, or -1 with a throw live. `cb`/`cb_cap` are the caller's
 * step_call_run buffer, which must hold at least 2 slots. `dst` must be a zeroed BodyState. */
int  body_clone_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, BodyState *dst, BodyState *src,
                    JSValue in, JSValue **out_cb, int *out_argc);

/* Streams §9.5 Piping's "CREATE A PROXY", which is the OTHER thing and must never be read as the clone above:
   the proxy pulls from the source "while stream itself becomes immediately LOCKED AND DISTURBED", so exactly
   ONE of the two is readable afterwards. Fetch §5.4 new Request(input, init) step 41 is its caller — a Request
   built out of another Request takes that one's body and leaves it unusable, which is why `new Request(r)`
   followed by `r.text()` throws where `r.clone()` would not. `src_obj` is the object holding `src`, because the
   disturbance latch is COW-captured state and the capture is keyed on it. `dst` must be a zeroed BodyState.
   Returns 0, or -1 with an exception live. */
int  body_create_proxy(JSContext *ctx, JSValueConst src_obj, BodyState *src, BodyState *dst);

/* DECLARE that an interface includes Body: the class its instances wear, and how to find the BodyState on one.
   `of` returns NULL when the value is not an instance, which is the receiver check every member performs.
   Returns a handle. There is ONE set of reader machines for the whole platform; the handle is what tells them
   which interface the receiver belongs to. */
/* `mime` is the object's Content-Type as its own header list reports it, or NULL — §5.3's `formData()` reads
   it to decide which parser the body goes through, and only the including interface knows where its headers
   live. Caller frees. */
/* `source` is core/byte_reader.h's question — where these bytes came from — and it is the including
   interface's for the same reason `mime` is: a Response was filled by a server at an address it holds, and a
   Request's body is bytes the PAGE composed. It answers a MALLOC'D name the caller frees, or NULL, which is
   the POSITIVE statement that these bytes are not a server's and never a hole a default fills. An interface
   whose bodies are never a server's declares NULL here and says so at the call. */
int  body_declare(JSContext *ctx, JSClassID class_id, BodyState *(*of)(JSValueConst v),
                  char *(*mime)(JSContext *ctx, JSValueConst v),
                  char *(*source)(JSContext *ctx, JSValueConst v), const char *iface);
/* INSTALL text/json/arrayBuffer/bytes and `bodyUsed` on the interface's prototype. */
void body_install(JSContext *ctx, JSValueConst proto, int handle);

/* Trace and release the stream a BodyState may hold — the including interface owns the state, so its gc_mark
   and its finalizer are where this belongs. */
void body_state_mark(JSRuntime *rt, BodyState *b, JS_MarkFunc *mark_func);

#endif
