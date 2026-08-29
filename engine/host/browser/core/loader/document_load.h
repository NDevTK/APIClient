/* HTML §7.4.5 "Populating a session history entry"'s LOAD A DOCUMENT — the routing of a response onto the
   §7.5 subsection that loads it. See document_load.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/html/html.h>

#include "core/mime/mime_type.h"
#include "solver/dom_cow.h"   /* DomParseRootKind — whose tree a parse builds, declared by whoever opens it */
#include "core/html/html_parse.h"   /* HtmlScriptingMode — HTML §13.2.4.5's flag, stated by whoever opens the parse */

/* §7.4.5's load-a-document, from the computed type through to a parsed Document: the ONE route from a
   RESPONSE to a parser in this engine.
 *
 * `type` is §7.4.5's "the computed type of navigationParams's response" — MIME Sniffing §7 "Determining the
 * computed MIME type of a resource"'s answer, which core/loader/document_load_type.h's
 * `document_load_computed_type` produces out of the response's header list and its bytes. `document` is a
 * Document the caller created and has not parsed into; `text` is the characters that response decoded to and
 * `size` may be zero; `root_kind` is core/html/html_parse.h's.
 *
 * A RESPONSE IS WHAT THIS TAKES, WHICH IS WHY §7.4's INITIAL `about:blank` DOES NOT COME THROUGH IT. That
 * Document has no response, so there is no type to compute and nothing to dispatch on: it is an HTML document
 * by §7.4 and its caller parses its skeleton directly. Every OTHER document built out of bytes this engine
 * fetched belongs here, and the reason is CLAUDE.md §Browser half's: a question some entries ask and others do
 * not is one missing capability wearing two names. Three entries build a Document out of a response in this
 * engine — a child navigable's, the WPT runner's top-level document, and the production ABI's — and each of
 * them used to hold its own copy of the dispatch and its own crash. Three copies of a rule are three rules.
 *
 * `scripting` is core/html/html_parse.h's HTML §13.2.4.5 flag, carried through unread: every response this
 * dispatches on belongs to a Document whose browsing context the CALLER knows and this component does not.
 *
 * A CALLER THAT IGNORES THE RETURN VALUE HAS REINSTATED THE SILENT WRONG TREE THIS COMPONENT EXISTS TO ABOLISH
 * — the release half of the unbuilt-arm crash is the status, and it only stops a response reaching the wrong
 * parser if somebody reads it. All three entries CHECK it. */

/* ---- THE LOAD AS A PULL, WHICH IS THE SHAPE §7.5 ITSELF DESCRIBES ------------------------------------------
 *
 * WHAT THIS IS FOR. A document parse is O(document) and not O(1), which is exactly the criterion the engine's
 * step machinery already uses to decide what may run inside one opcode: CLAUDE.md §C-stack states it as
 * "running no user code is not what makes a C span safe to leave un-parkable; being O(1) is". Every §7.5
 * subsection was reached through a single call that opened a parser, consumed the whole response and closed
 * it, so a page-sized document could not be preempted by a higher-value flow, could not give the thread back
 * for a cooperative quantum, and could not be parked to the IDB cold tier half-parsed. That is the
 * drive-to-completion CLAUDE.md §scheduler forbids at ANY depth.
 *
 * AND THE STANDARD AGREES, WHICH IS WHY THIS IS NOT AN ENGINE-SHAPED WRAPPER AROUND A SPEC-SHAPED CALL. §7.5.2
 * "Loading HTML documents" and §7.5.4 "Loading text documents" both say a TASK PER ARRIVAL of bytes fills the
 * parser's input byte stream, followed by a task for the implied EOF character; §7.5.3 "Loading XML documents"
 * reaches a parser this engine already built as a one-construct-per-call walk (core/xml/xml_tree.h). The
 * completing call was the approximation; the pull is the algorithm.
 *
 * THE DRIVER IS THE CALLER'S AND THERE IS NO SECOND SCHEDULER HERE. This component holds no loop, no queue and
 * no notion of when to yield — it holds a POSITION, and answers the one question a driver's loop asks. A driver
 * that is a flow steps it and returns to the one WFQ; a driver that is not steps it to the end. Which of those
 * a caller is, is a fact about the caller.
 *
 * EVERY STEP IS ONE ITEM AND EVERY ITEM IS O(1): one byte into the input byte stream for §7.5.2 and §7.5.4,
 * one construct of XML §2.1's [1] `document` for §7.5.3 — and, for §7.5.3, one node of each of the two walks
 * that follow a parse (the discard of a failed parse's partial tree, and HTML §14.2's script refusal). A byte
 * is the finest unit lexbor offers and it needs no chosen quantum, which is what a "N bytes then yield" would
 * have to invent and defend.
 *
 * `text` IS BORROWED FOR THE LIFE OF THE LOAD. lexbor's chunked tokenizer holds pointers into whatever buffer
 * it was handed until the run terminates, so the caller owes the bytes until `document_load_finish` returns;
 * the XML arm's build takes its own copy at `begin` and owes nothing after it. A driver that parks ACROSS
 * SESSIONS owes more than that — a snapshot crosses a session and a borrowed pointer does not — and that is
 * the arm's own subproblem when a cold-tier resume of a half-parsed document is built. */
typedef struct DocumentLoad DocumentLoad;

/* OPEN THE §7.5 SUBSECTION `type` ROUTES TO. Returns NULL for an arm this build has no loader for, having
   already crashed by name in a dev build — the same two halves `document_load` returns a status for. */
DocumentLoad *document_load_begin(lxb_html_document_t *document, DomParseRootKind root_kind,
                                  HtmlScriptingMode scripting,
                                  const MimeType *type, const lxb_char_t *text, size_t size);

/* IS THERE NOTHING LEFT TO DO? What a driver's loop tests; stepping past it is a DCHECK. */
bool document_load_ended(const DocumentLoad *load);

/* ONE ITEM. The status is LATCHED into the load rather than returned, for the reason core/xml/xml_parse.h
   states about its own step: a driver that must ask after every step is a driver that can forget to, and
   `document_load_ended` already answers the only question the loop has. */
void document_load_step(DocumentLoad *load);

/* CLOSE THE ARM and destroy the load. Returns the status the parse ended with — non-OK is the allocation floor
   and nothing else, since §13.2 rejects no input and §7.5.3 defines no failure for an ill-formed document. */
lxb_status_t document_load_finish(DocumentLoad *load);

/* THE COMPLETE LOAD — begin, step to the end, finish — FOR A CALLER WITH NO FLOW TO YIELD TO, and the DCHECK
   inside it is what holds that to being true. HTML §7.4.5's load-a-document reaches this engine from three
   places and two of them run before the frontier has a member: the production ABI's `qjs_init`, which must
   return the document identity synchronously, and the WPT runner's top-level document. The third —
   core/frame/navigable.c's child navigable — runs INSIDE a step machine, so it has a driver to park in and
   this entry crashes for it by name.
   RETURNS non-OK for an arm this build has no loader for, having already crashed by name in a dev build. Both
   halves are load-bearing and they are for different builds: the `DFAIL` names the §7.5 subsection to BUILD,
   which is the dev forcing function, and it compiles out in release — where the status is what stops a
   response §7.4.5 does not load as HTML from being handed to the HTML parser anyway. The caller's own always-
   fatal CHECK on the status is what makes the release half real. */
lxb_status_t document_load(lxb_html_document_t *document, DomParseRootKind root_kind,
                           HtmlScriptingMode scripting,
                           const MimeType *type, const lxb_char_t *text, size_t size);

#endif
