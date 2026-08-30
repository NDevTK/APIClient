/* A REFERENCE TO AN OBJECT IN ANOTHER AGENT — see remote_object.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_OBJECT_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_OBJECT_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

void remote_object_init(JSContext *ctx);
/* THE AGENT'S HALF UNDONE — a ROW on core/platform.h's third column, which is why it takes the RUNTIME: the
   two target classes, this session's export table, the imported-reference table, the well-known symbols
   captured from this agent's own %Symbol% and the four trap machines are all registrations in a JSRuntime,
   and not one of them is anything a realm owns. */
void remote_object_free(JSRuntime *rt);

/* The object a name names, BORROWED. JS_UNDEFINED for an id this agent never exported, which is a peer naming
   something that was never lent — a protocol error, not a missing property. Lending is not a separate door: an
   object is exported by ENCODING it, which is the only way one ever crosses.
   AN ID IS AN INDEX INTO ONE SESSION'S TABLE, SO THE GENERATION IS HALF THE NAME. The export table dies with
   the instance and a resumed session mints from 1 again, while the DOCUMENT name is stable across a park by
   requirement — so a name lent before a park is IN RANGE in the new table and would resolve, silently, to an
   unrelated object: `w.document === w.document` across the park answered by two different objects, with the
   "never lent" check passing. The generation is world.h's, because it is the same fact about the same session
   and a second counter would be a second answer to it. A name from an ended session is REFUSED here rather
   than resolved — that is a capability to build, and it aborts saying which. */
JSValueConst remote_object_by_id(uint32_t session, uint32_t id);

/* THE ONE GRAMMAR FOR A VALUE THAT CROSSES AN INSTANCE BOUNDARY, both directions, both processes.
 *
 * WHAT CROSSES IS TEXT AND IT CARRIES ITS TYPE — `otherW.length === 0` distinguishes a number from the string
 * "0", so the leading byte says which. A live JSValue crosses neither a process, nor an instance, nor a
 * session, nor a park, which is why there is an encoding at all.
 *
 *   u  undefined            N  null              b0 / b1  a boolean
 *   n  a double, %.17g      s  base64 UTF-8      o / f / c  an OBJECT, by NAME: `<document>:<generation>:<id>`
 *   w  a WELL-KNOWN symbol, by its [[Description]]   g  a REGISTERED symbol, by base64 of its Symbol.for key
 *
 * A STRING RIDES AS BASE64 because these records are TAB-SEPARATED and a property name or a written value may
 * contain a tab or a newline. The codec is the ENGINE's (JS_Base64Encode), the one the spec already made it
 * implement for `btoa`, rather than a second one grown here. A REGISTERED symbol's key is a string the page
 * chose and rides the same way for the same reason; a well-known symbol's [[Description]] is the ENGINE's own
 * and is asserted tab-free where it is captured.
 *
 * A SYMBOL IS NOT ONE THING, and the claim that it cannot cross is false for the two kinds pages use. 6.1.5's
 * WELL-KNOWN symbols are a distinct value in every agent that denotes the same well-known slot — `@@iterator`
 * here IS `@@iterator` there — so one crosses as its [[Description]] and is resolved to the receiving agent's
 * own. A REGISTERED symbol (20.4.2.2) is defined BY its key: `Symbol.for("k")` in any agent is that agent's
 * one symbol for "k", so the key is the whole of its identity. Only a UNIQUE symbol has identity and nothing
 * else, and that one is an export like an object — remote_object_encode says so at its own site.
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

/* AND WHAT AN ANSWER IS: A COMPLETION, NOT A VALUE (ECMA-262 6.2.4). The peer resolves every operation by
 * RUNNING A PROGRAM, and a program either returns or throws; an answer grammar with a slot for the value and
 * none for its type hands the asking flow `undefined` where the spec propagates a throw, so a page's
 * `try { remote.x = 1 } catch (e) {}` never runs its handler and the flow proceeds on a write that did not
 * happen.
 *
 * A completion is the value record with its TYPE in front — `.` for a normal completion, `!` for a throw —
 * because a completion is not a value and must not be spelled as one: neither character is a value tag, so a
 * decoder reading one where the other belongs crashes instead of resolving a throw to `undefined`. The THROWN
 * VALUE crosses by the ordinary rules, which is the point of layering it this way rather than inventing a
 * second encoding: an Error is an OBJECT, so it crosses as a name, and the catch clause holds a reference to
 * the peer's Error whose `.message` is another cross-agent read.
 *
 * `completion` is engine.h's ENGINE_COMPLETION_*, the same enumeration the flow's register stores, because a
 * completion type spelled twice is two things to keep in step. `remote_completion_encode` returns a malloc'd
 * record the caller frees. */
char   *remote_completion_encode(JSContext *ctx, int completion, JSValueConst v);
JSValue remote_completion_decode(JSContext *ctx, const char *text, int *pcompletion);

#endif
