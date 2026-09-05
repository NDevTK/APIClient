/* HTML §8.4.1 "Opening the input stream" and §8.4.2 "Closing the input stream" — THE INPUT STREAM'S LIFETIME.
 *
 * WHY IT IS A SEPARATE COMPONENT FROM §8.4.3. `document.write()` decides WHAT is written; these two decide
 * WHETHER THERE IS A STREAM TO WRITE INTO, and the second question is the one with the state in it. §8.4.1
 * REPLACES a Document in place — it erases every listener the page registered, empties the tree, resets the
 * readiness and mints a parser — while §8.4.3 is a value, a Trusted Types conversion and a hand-off. One
 * problem per file, and this file's problem is the stream, which is why §8.4.2's steps 3-6 are here beside the
 * steps that created what they close rather than beside the throws they share with `write`.
 *
 * WHAT MAKES IT REACHABLE AT ALL IS THAT THIS ENGINE PARSES AHEAD OF ITS SCRIPTS. A browser runs an inline
 * script with §13.2.3.5's insertion point DEFINED (§13.2.6.4.8 'The "text" insertion mode' sets it around
 * `prepare the script element` and restores it afterwards), so a parse-time `document.write` APPENDS. This
 * engine parses a document to completion and then seeds its scripts as flows, so the same call finds the
 * insertion point undefined and takes §8.4.3 step 9's other arm — the DESTRUCTIVE one. The two calls are not
 * interchangeable and the difference is the whole of what separates
 * html/browsers/history/the-location-interface/reload_document_write.html (a write from a parse-time script,
 * which must append) from reload_document_write_onload.html (a write from a `load` handler, which must
 * replace), so this component refuses the first rather than serving it out of the second's algorithm — see
 * step 5.
 *
 * THE SCRIPT-CREATED PARSER IS RECORDED PER FLOW, AND THAT IS A TWO-SIDED ASSERTION RATHER THAN BOOKKEEPING.
 * §8.4.2 step 3 is "if there is no script-created parser associated with this, then return", so `close()` has
 * to tell a parser §8.4.1 minted from one a document load left open. The fact is written as an ordinary
 * property on the Document's own wrapper under a Symbol this file mints and never publishes, which makes it
 * per-flow for free: the heap COW delta captures the write, so a sibling that never opened the stream reads
 * the baseline. LEXBOR'S PARSER STATE IS NOT IN THAT DELTA — it is one `lxb_html_parser_t` on the document —
 * so the record and `html_parse_insertion_point_defined` can DISAGREE, and they disagree in exactly one
 * situation: another flow opened the stream and this one is looking at it. That is a real isolation gap in the
 * parser rather than in this record, and the two answers are compared at every entry so it crashes at the
 * flow that would have written into a sibling's parser instead of silently doing it.
 * AND THAT SITUATION IS THE ORDINARY ONE, which the sentence above does not say and which is what decides how
 * much the gap is worth. §8.4.1 OPENS a stream and only §8.4.2's close ends one, so a document written into
 * once carries an open parser for the rest of its life; CLAUDE.md §Every-runtime-job makes each timer
 * callback, each reaction and each queued job its own flow. The second arrival is therefore not an
 * interleaving to be reproduced — it is what the scheduler does next, and a single `document.write` from a
 * single `setTimeout` callback is enough to reach it. Adding `document.close()` to that same callback removes
 * it, which is the discriminator: the assert is about a stream left OPEN across a flow boundary, never a write.
 *
 * WHAT §8.4.1 STILL OWES, NAMED HERE BECAUSE THE STEP THAT OWES IT IS HERE. Step 8's "stop loading" of a
 * navigable with an ongoing navigation is a write to state core/frame/navigable.c owns, and there is nothing
 * there to ask: that file exposes neither a navigable's ongoing navigation nor a stop. It is named rather than
 * approximated, because every approximation available is about the FLOW and the question is about the
 * NAVIGABLE — it would show as a `document.open()` racing a navigation of the same navigable, whose document
 * then arrives on top of the one these steps just built. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DOCUMENT_OPEN_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DOCUMENT_OPEN_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT, from document_write_init — the Symbol the script-created-parser record lives under.
   There is no per-realm install: §8.4.1 states the initial value ("Document objects have an active parser was
   aborted boolean … It is initially false" is its neighbour, and a Document has no parser until something
   creates one), and an ABSENT property is that initial value rather than a default filling a hole. */
void document_open_init(JSContext *ctx);
/* Given back from document_agent_free, by the declarer. */
void document_open_free(JSRuntime *rt);

/* HTML §8.4.1's DOCUMENT OPEN STEPS, given a document. `doc_obj` is that Document's wrapper — the receiver the
   member was called on, which is where the script-created-parser record lives and which every caller already
   holds. Returns false with an exception pending (step 4's SecurityError); true otherwise, INCLUDING the steps
   that "return document" without opening anything, because those are not failures. */
bool document_open_steps(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom);

/* HTML §8.4.2 "Closing the input stream" steps 3-6 — the script-created-parser test, the explicit "EOF"
   character and running the tokenizer to it. Steps 1 and 2 are `close()`'s own two throws and stay with the
   member (core/html/document_write.c), which shares them with §8.4.3. */
void document_close_input_stream(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom);

/* Does THIS FLOW hold an open script-created parser on `dom` — §8.4.2 step 3's question, and §8.4.3 step 9's.
   ASSERTS the record against §13.2.3.5's real insertion point on every call; see the header comment for the
   one state in which they differ and why it must crash rather than resolve to either answer. */
bool document_open_stream_is_ours(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom);

#endif
