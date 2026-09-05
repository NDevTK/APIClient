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
 * THE TWO WRITES A PAGE CAN MAKE ARE DIFFERENT ALGORITHMS AND THIS ENGINE SERVES ONE OF THEM.
 *   - A write from a POST-LOAD task — a `load` handler, a timer, an event — finds §13.2.3.5's insertion point
 *     undefined and takes step 9's other arm: §8.4.1's DOCUMENT OPEN STEPS, which replace the document in
 *     place. Those are built (core/html/document_open.c), so is §8.4.2's close, and so is the `open()` member
 *     that shares them — AND WHAT THEY LEAVE BEHIND IS NOT: §8.4.1 opens a stream and only §8.4.2's close ends
 *     one, so a page that writes and never closes (which is nearly every page that writes) carries an open
 *     `lxb_html_parser_t` for the rest of the document's life, while the RECORD that says whose it is rides
 *     the writing flow's COW delta. The next flow to reach §8.4.3 step 9 or §8.4.2 step 3 on that document
 *     therefore meets the two-sided assert at document_open.c's `document_open_stream_is_ours`, which is
 *     where the isolation gap is stated and where the capability that closes it is named. That is not a race
 *     to be reproduced under load: CLAUDE.md §Every-runtime-job makes each queued job its own flow, so the
 *     second arrival is what the scheduler does next.
 *   - A write from a PARSE-TIME script must APPEND at the insertion point instead, and it CANNOT here: this
 *     engine parses a document to completion and then seeds its scripts as flows, so by the time any of them
 *     runs, §13.2.7 "The end" has set the insertion point to undefined. In a browser that script runs with the
 *     insertion point DEFINED (§13.2.6.4.8 'The "text" insertion mode' sets it around `prepare the script
 *     element` and restores it after), which is why a real parse-time `document.write` appends. The two are
 *     not interchangeable and §8.4.1 step 5 is where the standard separates them, so document_open.c CRASHES
 *     there rather than serving the first out of the second's algorithm. What that crash asks for is a
 *     document's own parse kept OPEN across script execution, and its ISOLATION half is now built: §13.2.6's
 *     writes go through solver/dom_cow.c's table (a document parse declares DOM_PARSE_ROOT_SHARED, so inserts
 *     and removals push the entries that revert them) AND its `create` member records every node the parse
 *     makes into the running flow's delta, so a live parser's nodes die with the flow that built them exactly
 *     as an `appendChild`'s do. WHAT IS LEFT IS *WHEN* THE PARSE IS CLOSED, AND NOT WHICH ENTRY OPENS IT.
 *     This paragraph used to send its reader to core/loader/document_load.c to replace an `html_parse_document`
 *     that is not called there; the §7.5.2 loader that IS there already opens with `html_parse_document_open`
 *     and fills the stream a rest unit at a time, so the parse is incremental and genuinely open while it
 *     runs. What ends it before any script of that document exists is the FINISH, and the finish is
 *     load-bearing where it stands: the realm is built around the FINISHED tree because CSP §3.3's `<meta>`
 *     policies are collected off it. So moving §13.2.7 "The end" past the realm is not a relocation of one
 *     call — it is giving this engine an ORDER a browser gets by construction, since §4.2.5.3 "Pragma
 *     directives" runs a pragma's algorithm "when a meta element is inserted into the document" rather than
 *     over a finished tree. Its hazard is
 *     html_parse.c's own: lexbor emits the EOF token in `chunk_end` and §13.2.6 builds `html`/`head`/`body`
 *     from it, so a document whose source produced none has a NULL `documentElement` until the close — which
 *     is what a browser does too, and which each reader that assumes otherwise has to be changed for.
 *
 * WHAT IS WRITTEN RUNS ITS SCRIPTS, AND THE STANDARD DOES NOT REQUIRE THAT — which is why the omission had to
 * be found by reading rather than by any instrument. §8.4.3's own definition says "User agents are explicitly
 * allowed to avoid executing script elements inserted via this method", so a written `<script>` that never ran
 * broke no conformance test, earned no crash and moved no column. CLAUDE.md's §Boot settles it for this
 * project by name — "Code-loading async ALWAYS executes (`await import(x)`, a chunk `fetch().then(eval)`, a
 * `document.write`'d `<script>`)" — because a written script is CONDITIONALLY-LOADED JS in its purest form:
 * code that exists only if a branch reached the write, which is the surface this whole product exists to reach.
 * The route is §13.2.6.4.8 'The "text" insertion mode' at the written `</script>`, performed in
 * core/html/html_parse.c's token-done wrapper and prepared by core/html/html_script.c; the program becomes a
 * row of the writing flow's own script sequence, so it runs under that flow's delta and its pins and a sibling
 * that never took the branch never sees it. What is still deviating is ORDER WITHIN ONE WRITE: §13.2.6.4.8
 * blocks the tokenizer on a parser-blocking script, and this engine tokenizes the whole chunk first, so the
 * markup written after a `<script src>` builds its DOM before that script runs. The scripts themselves keep
 * their order; closing the rest is the same parser-suspension capability the paragraph above is waiting on.
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
/* Reached from document_agent_free — declared by document_init, so released by its declarer. It takes the
   RUNTIME because §8.4.1's script-created-parser key (core/html/document_open.h), minted by the init above, is
   a Symbol that outlives every realm. */
void document_write_free(JSRuntime *rt);

#endif
