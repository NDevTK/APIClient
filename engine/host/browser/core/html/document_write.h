/* HTML §8.4 DYNAMIC MARKUP INSERTION — `document.write()`, `document.writeln()` and `document.close()`.
 *
 * WHY IT IS ITS OWN COMPONENT rather than three more members of core/dom/document.c. Every other member of
 * `partial interface Document` answers a question ABOUT the tree; these three drive the PARSER that builds it.
 * §8.4.3's own eleven steps are a Trusted Types conversion, three throw conditions over document state, and a
 * branch on §13.2.3.5's INSERTION POINT — a fact that belongs to core/html/html_parse.c and to nothing else in
 * the platform. One problem per file, and this is the input stream's.
 *
 * WHY IT MATTERS OUT OF PROPORTION TO ITS SIZE. `document.write` is one of the canonical DOM-XSS sinks: a tag
 * manager, an ad slot and an analytics snippet reach it constantly, and `document.write(location.hash)` is the
 * shape a scanner testbed uses to define the class. It was ABSENT here, which is not a false negative the
 * solver can recover from — §@S's search is opened by a DETECTION, so a sink nothing announces is a search that
 * never starts and a finding that can never be emitted, no matter how much of the page is explored. The route
 * to that detector (solver/solve.h's solve_html_sink) is the SAME one core/dom/element.c's innerHTML,
 * outerHTML and insertAdjacentHTML take; there is deliberately no second seam.
 *
 * WHAT THIS COMPONENT DOES NOT HAVE, AND THE ONE MECHANISM BOTH GAPS ARE WAITING ON.
 *   - `open()` is ABSENT (§NO STUBS: a web API not yet built is honestly absent, and the page's own throw on a
 *     missing member is the forcing function). §8.4.1's document open steps need three primitives this engine
 *     does not have — "erase all event listeners and handlers" over a document's shadow-including inclusive
 *     descendants, "replace all with null" as an exported operation, and the ENTRY global object's Document for
 *     the same-origin check in step 4 — and inventing any of them here would put them in the wrong file.
 *   - Steps 10 and 11 — the insert into the input stream and the parse — reach html_parse_document_write, which
 *     CRASHES today, naming what to build. Two things stand between here and a `document.write` that actually
 *     inserts, and neither is in this file:
 *       (1) THE ACTIVE DOCUMENT'S PARSE IS ALREADY CLOSED before any of its scripts run. This engine parses a
 *           document to completion and then seeds its scripts as flows, so §13.2.7 "The end" has already set
 *           the insertion point to undefined for every document by the time a page can call anything. In a
 *           browser an inline script runs with the insertion point DEFINED (§13.2.6.4.8 'The "text" insertion
 *           mode' sets it around `prepare the script element` and restores it after), which is why a real
 *           `document.write` appends to the document instead of replacing it.
 *       (2) THE LIVE PARSER'S NODES BELONG TO NO FLOW — which is an OWNERSHIP gap and no longer a CAPTURE one.
 *           This entry used to read "lexbor's tree construction BYPASSES the per-flow DOM delta … a live
 *           parser inserts through Lexbor's own mutators, which nothing captures", and called that the deeper
 *           of the two because "the first without the second would make one flow's `document.write` visible to
 *           every sibling". That is no longer true and has not been for some time: html_parse_new_parser calls
 *           dom_cow_install_tree_construction and ASSERTS dom_cow_owns_tree_construction, a document parse
 *           declares DOM_PARSE_ROOT_SHARED, and §13.2.6's inserts and removals each push the delta entry that
 *           reverts them. The writes ARE captured and a sibling would see nothing.
 *           What is still owed is who FREES them. Every other parse here is out-of-tree and its result is
 *           PLACED through the chokepoint, where dom_cow_take_private makes the flow the owner of what it
 *           placed; a parser feeding the document's own tree has no placement step, so a discarded flow
 *           detaches its written nodes and nothing frees them. core/html/html_parse.c states this at the site
 *           that owes it, together with the other hazard of (1) — lexbor emits the EOF token in `chunk_end`
 *           and §13.2.6 builds `html`/`head`/`body` from it, so a document left open has a NULL
 *           `documentElement` until the close, which is what a browser does too and which each reader that
 *           assumes otherwise has to be changed for.
 *     THE TWO SITES ARE CROSS-REFERENCED ON PURPOSE. They describe one mechanism from two ends, html_parse.c
 *     was kept current and this was not, and a reader who arrived here first would have gone off to build a
 *     capture primitive that already exists — which is the stale-`DFAIL` failure mode in prose, and cost
 *     nothing only because nobody had taken it yet.
 *
 * WHAT IT DOES HAVE IS THE WHOLE SINK. §8.4.3 steps 1-9 run, and the value that reaches step 9 is announced to
 * the @S detector on the way — so `document.write(location.hash)` opens a search, and a candidate re-run
 * reaches the same announcement with the breakout substituted, which is where solve.c's own fire oracle takes
 * over. Neither half of that path needs the document's tree, which is why the sink lands before the parser
 * does. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DOCUMENT_WRITE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DOCUMENT_WRITE_H

#include "quickjs.h"

/* The AGENT's half: the three members' declarations — `write` and `writeln` are `(TrustedHTML or DOMString)...
   text`, so the declaration is what performs Web IDL's variadic DOMString conversion (and runs the page's own
   `toString` on a request, suspending, rather than from inside the algorithm). Declared once per agent,
   beside document_domain_init. */
void document_write_init(JSContext *ctx);
/* The REALM's half: `write`, `writeln` and `close` on Document.prototype. */
void document_write_install(JSContext *ctx, JSValueConst proto);
/* Reached from document_agent_free — declared by document_init, so released by its declarer. */
void document_write_free(void);

#endif
