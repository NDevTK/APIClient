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
/* `unknown` IS §5.2 BodyInit unions' SCALAR VALUE STRING ARM'S `object` WHERE THAT OBJECT IS UNKNOWN EXTERNAL
   INPUT, and it is a JSValue because the arm's own step cannot be performed over one: "Set source to the UTF-8
   encoding of object" names a byte sequence, and the UTF-8 encoding of a value nobody knows is not one.
   `bytes` is a `char *` and a `char *` cannot BE an unknown, so before this slot existed the commonest POST a
   bundle writes — `fetch(u, {method:"POST", body: cfg.payload})` over server-injected state — aborted at the
   extraction and the whole request went unreported, which is precisely the surface CLAUDE.md §Attacker-sources
   says forced execution derives and a sniffer cannot.
   IT HOLDS THE VALUE AND NEVER ITS DISPLAY SHAPE. The shape is what a consumer that needs bytes is handed
   (body_state_content below); storing it here instead would make this body indistinguishable from a page that
   literally wrote those characters, which is the de-tainting core/fetch/fetch.c's fetch_park describes at the
   ToString boundary it makes the same exception for.
   IT IS NOT §2.2.4's `source`, AND THE RESIDUAL THAT ASKED FOR THIS CALLED IT THAT — in its prose, in its
   abort message, and citing §2.2.5 Requests, which is the section defining a REQUEST's `body` and says nothing
   about a body's members. §2.2.4 Bodies is where the member lives and it states a CLOSED type — "A source
   (null, a byte sequence, a Blob object, or a FormData object), initially null" — which a string is not in.
   What §5.2 sets for this arm is that string's UTF-8 ENCODING, so the source is a byte sequence this engine
   cannot spell and this is the value whose encoding it would be. `source_null` therefore stays exactly as it
   was and still answers 0 here, because §5.2's string arm does set a source.
   A ZEROED STATE READS AS ABSENT WITH NO GUARD, unlike `stream`: an including interface allocates its record
   with js_mallocz and a zeroed JSValue is the INTEGER 0, which `concolic_is` answers 0 for — so the question
   asked of this slot is "is it a concolic", which is also the only thing it is ever set to. */
typedef struct {
    char *bytes; size_t len; int used; int has; int source_null; JSValue stream; JSValue unknown;
} BodyState;

/* WHAT A BODY IS, FOR A CONSUMER THAT NEEDS BYTES — ONE question with FOUR answers, because they are four
   different facts and a consumer that averages any two of them reports something that did not happen.
   `*bytes`/`*len` are written on every arm and are never a hole a default fills:
     BODY_NONE   — there is no body (`new Response()`); `*bytes` is NULL. It is not an EMPTY body, which is
                   BODY_BYTES with a zero length, and `.body` reports null for exactly one of the two.
     BODY_BYTES  — `*bytes`/`*len` ARE the body.
     BODY_SHAPE  — the body is UNKNOWN EXTERNAL INPUT, and `*bytes`/`*len` are its DISPLAY SHAPE
                   ("{cfg.payload}") — what the @H surface records, the same answer solver/endpoint.c already
                   gives a concolic URL. It is NEVER a value the page computed, so a consumer that hands it to
                   the PAGE rather than to the SURFACE has de-tainted the body.
     BODY_STREAM — §5.2's ReadableStream arm: the bytes do not exist yet; `*bytes` is NULL.
   THE THREE WAYS THERE ARE NO BYTES ARE TOLD APART HERE and not by `bytes == NULL` at each consumer, which is
   what let a stream-backed body and an unknown one arrive at one abort under one message about the other. */
typedef enum { BODY_NONE, BODY_BYTES, BODY_SHAPE, BODY_STREAM } BodyContent;
BodyContent body_state_content(const BodyState *b, const char **bytes, size_t *len);

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
/* Copy `len` bytes in, or NULL for the spec's null body. Returns -1 on OOM with an exception live.
   It also RELEASES any `unknown` the state held: a body filled from bytes has no unknown content, and the two
   are the disjoint arms body_state_content tells apart. */
int  body_state_set(JSContext *ctx, BodyState *b, const char *bytes, size_t len);
/* Fill from §5.2's string arm where the arm's object is UNKNOWN EXTERNAL INPUT — see `unknown` above. `v` is
   BORROWED. Returns -1 on OOM with an exception live. */
int  body_state_set_unknown(JSContext *ctx, BodyState *b, JSValueConst v);
/* §2.2.4 Bodies' "other members are copied from body", for the two operations that copy a body's CONTENT
   rather than teeing its stream — §5.4's `clone()` and its step 41 proxy. ONE entry, because the alternative
   is every such site re-spelling §5.2's arms as `src->has ? src->bytes : NULL`, which is the union's rule
   written a second time and which answers an UNKNOWN body as a NULL one. `dst` keeps its own `used` latch.
   Returns -1 with an exception live. */
int  body_state_copy(JSContext *ctx, BodyState *dst, const BodyState *src);

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
