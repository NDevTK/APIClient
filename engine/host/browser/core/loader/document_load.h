/* HTML §7.4.5 "Populating a session history entry"'s LOAD A DOCUMENT — the routing of a response onto the
   §7.5 subsection that loads it. See document_load.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_H

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
 * RETURNS non-OK for an arm this build has no loader for, having already crashed by name in a dev build. Both
 * halves are load-bearing and they are for different builds: the `DFAIL` names the §7.5 subsection to BUILD,
 * which is the dev forcing function, and it compiles out in release — where the status is what stops a
 * response §7.4.5 does not load as HTML from being handed to the HTML parser anyway. The caller's own always-
 * fatal CHECK on the status is what makes the release half real, so a caller that ignores the return value has
 * reinstated the silent wrong tree this component exists to abolish. */
/* `scripting` is core/html/html_parse.h's HTML §13.2.4.5 flag, carried through unread: every response this
 * dispatches on belongs to a Document whose browsing context the CALLER knows and this component does not. */
lxb_status_t document_load(lxb_html_document_t *document, DomParseRootKind root_kind,
                           HtmlScriptingMode scripting,
                           const MimeType *type, const lxb_char_t *text, size_t size);

#endif
