/* A REFERENCE TO AN OBJECT IN ANOTHER AGENT — see remote_object.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_OBJECT_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_OBJECT_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

void remote_object_init(JSContext *ctx);
void remote_object_free(JSContext *ctx);

/* The object an id names, BORROWED. JS_UNDEFINED for an id this agent never exported, which is a peer naming
   something that was never lent — a protocol error, not a missing property. Lending is not a separate door: an
   object is exported by ENCODING it, which is the only way one ever crosses. */
JSValueConst remote_object_by_id(uint32_t id);

/* THE ONE GRAMMAR FOR A VALUE THAT CROSSES AN INSTANCE BOUNDARY, both directions, both processes.
 *
 * WHAT CROSSES IS TEXT AND IT CARRIES ITS TYPE — `otherW.length === 0` distinguishes a number from the string
 * "0", so the leading byte says which. A live JSValue crosses neither a process, nor an instance, nor a
 * session, nor a park, which is why there is an encoding at all.
 *
 *   u  undefined            N  null              b0 / b1  a boolean
 *   n  a double, %.17g      s  base64 UTF-8      o / f / c  an OBJECT, by NAME: `<document>:<id>`
 *
 * A STRING RIDES AS BASE64 because these records are TAB-SEPARATED and a property name or a written value may
 * contain a tab or a newline. The codec is the ENGINE's (JS_Base64Encode), the one the spec already made it
 * implement for `btoa`, rather than a second one grown here.
 *
 * `o`, `f` and `c` are one kind of thing said three ways, and the distinction is not decoration: a Proxy is
 * callable only if its TARGET is, and constructible only if its target is, so an object whose callability did
 * not cross would arrive as a reference that answers `typeof` wrong and is not a function. `c` additionally
 * carries [[Construct]].
 *
 * IDENTITY HOLDS IN BOTH DIRECTIONS AND A NAME THAT COMES HOME RESOLVES TO THE ORIGINAL. The name carries the
 * DOCUMENT, so an encoder that is handed a reference re-emits the name it already has rather than exporting the
 * proxy (which would make a round trip a proxy of a proxy, and `x === original` false), and a decoder handed a
 * name in its OWN agent's namespace answers with the exported object itself rather than minting a reference to
 * this heap. `remote_object_encode` returns a malloc'd record the caller frees. */
char   *remote_object_encode(JSContext *ctx, JSValueConst v);
JSValue remote_object_decode(JSContext *ctx, const char *text);

/* IS THIS a reference to an object in another agent, and WHICH object — the (document, id) the name it was
   minted from carried. Asked by the encoder, which must re-emit that name rather than lend the proxy. */
bool     remote_object_is(JSContext *ctx, JSValueConst v);
uint32_t remote_object_doc(JSContext *ctx, JSValueConst v);
uint32_t remote_object_id(JSContext *ctx, JSValueConst v);

#endif
