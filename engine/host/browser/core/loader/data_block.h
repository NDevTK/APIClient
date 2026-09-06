/* HTML §4.12.1 "The script element" — A DATA BLOCK'S CONTENT, AND WHAT A VALUE READ OUT OF IT IS.
 *
 * §4.12.1 splits a `<script>` element's content four ways on the `type` attribute, and the fourth is not a
 * program at all: "Setting the attribute to any other value means that the script is a data block, which is
 * not processed by the user agent, but instead by author script or other tools." So the bytes sit in the
 * document, the user agent does nothing with them, and the BUNDLE parses them — which is the whole of why
 * this file exists, because that parse is the one route by which server-rendered per-visitor state reaches a
 * bundle without any of it ever having been written by the document's own code.
 *
 * THE CHANNEL IS THE DOCUMENT'S TEXT, NOT THE DOCUMENT'S CODE. An inline `<script>` and a data block are the
 * same delivery: both arrive inside the response the server rendered against THIS visitor's credentials,
 * while a subresource bundle at a content-addressed URL is byte-identical for everybody. The engine already
 * knows the first half — JS_EVAL_FLAG_INLINE_SCRIPT marks a PROGRAM whose source text came in the document,
 * and the object allocator stamps the records that program builds. A data block has no program, so nothing
 * marks it: `JSON.parse(document.getElementById("x").textContent)` is bundle code however server-rendered its
 * input, and every record it builds looks exactly like one the bundle composed for itself.
 *
 * WHICH MAKES ITS CONTENT A LOADED CONFIG, AND THAT IS A CATEGORY THIS ENGINE ALREADY HAS. CLAUDE.md's
 * symbolic/trust boundary: "a config/data fetch is ALWAYS loaded so its fields become concrete examples,
 * while its use in a BRANCH still forks (a loaded `features.admin:false` must NOT concretize the gate, or the
 * admin endpoint is lost — config is opaque-for-control-flow yet carries its loaded value as the example)."
 * A data block is that payload delivered in the document instead of over the network, and the difference is
 * the transport only. So its text is minted as the solver's triple — provenance naming the block, and the
 * REAL BYTES as the example — and the engine's own `JSON.parse` carries it the rest of the way:
 * ECMAScript §25.5.2 "JSON.parse ( text [ , reviver ] )" over an unknown text forks its two completions,
 * runs the REAL codec on the example, and derives the result from the source it came from. Nothing here
 * parses anything, and nothing derives a transform expression from one. (THE NUMBER STOOD AS `25.5.1`,
 * which the standard now titles JSON.isRawJSON ( obj ) — a retired number that still RESOLVES, so no
 * channel can accuse it. Read off the standard's text rather than recalled.)
 *
 * WHY THE PRESENT MEMBERS DO NOT GO CONCRETE AND THE ABSENT ONES DO NOT GO `undefined`. This visitor's block
 * says `"isBot":false` and `"user":null`; the next visitor's says something else, and the fields it holds are
 * not even the same fields. Answering the concrete `null` decides `if (data.props.pageProps.user)` for the
 * whole run and buries every endpoint behind it — which is precisely the surface this tool exists to reach.
 * Answering `undefined` for a field the server did not write this time is the same loss wearing an absence.
 * The triple answers both: a member the block HOLDS carries its real bytes as an example (so an @H parameter
 * reports what the server actually sent), a member it does not hold carries none (so nothing is invented),
 * and either way the value is opaque for control flow, so the gate over it FORKS.
 *
 * THE ENTRIES ARE EVERY DOOR THE ELEMENT'S CHILD TEXT CONTENT LEAVES BY. §4.12.1 puts the data IN the element
 * — "When used to include data blocks, the data must be embedded inline, the format of the data must be given
 * using the type attribute, and the contents of the script element must conform to the requirements defined
 * for the format used" — so what the author's own code reads is DOM §4.11 Interface Text's "child text
 * content" of that element, and it reaches it three ways: DOM §4.4 Interface Node's `textContent`; DOM §4.10
 * Interface CharacterData's `data` and §4.4's `nodeValue` on the child Text node; and HTML §8.5.4 The
 * innerHTML property, whose §13.3 Serializing HTML fragments appends a Text node's data LITERALLY when its
 * parent is a `script` element, which makes that serialization the same bytes. One question asked at all
 * three, never an `if` at one: an entry that skips it does not report a missing capability, it reports a
 * record the SERVER filled as a record the bundle composed, with nothing to say so.
 */
#ifndef ENGINE_HOST_BROWSER_LOADER_DATA_BLOCK_H
#define ENGINE_HOST_BROWSER_LOADER_DATA_BLOCK_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* Is `el` a §4.12.1 DATA BLOCK — a `script` element whose type is none of the four algorithms the section
   names? Answered from the element's own `type` attribute through document_scripts.h's script_block_type,
   which is §4.12.1's type-string steps, so there is one parser of that attribute and not two. */
int data_block_is(lxb_dom_element_t *el);

/* `text` is `el`'s child text content as one of the doors above computed it, and it is CONSUMED. Returns it
   unchanged unless `el` is a data block AND this host is exploring (concolic_is_exploring — a conformance run
   gets the browser's own answer, which is the string); otherwise the solver triple carrying those bytes as
   its example. The caller passes the element the text belongs to, because that is the only thing that knows
   which block these bytes are: a Text node's is its parent, a serialization's is the node whose children were
   serialized. */
JSValue data_block_wrap_text(JSContext *ctx, lxb_dom_element_t *el, JSValue text);

#endif
