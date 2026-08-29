/* HTML §7.5.4 "Loading text documents" — the loader every arm HTML §7.4.5 "Populating a session history
   entry"'s load-a-document sends to it reaches. See text_document.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_TEXT_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_TEXT_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/html/html.h>

#include "core/mime/mime_type.h"
#include "solver/dom_cow.h"   /* DomParseRootKind — whose tree a parse builds, declared by whoever opens it */
#include "core/html/html_parse.h"   /* HtmlScriptingMode — HTML §13.2.4.5's flag, stated by whoever opens the parse */

/* §7.5.4's "To load a text document, given a navigation params navigationParams and a string type", over a
   Document the caller has already created and the characters the response decoded to.
 *
 * `type` IS §7.4.5's COMPUTED TYPE, and it is taken rather than assumed because §7.5.4's own signature takes
 * it — and because it is what this entry ASSERTS itself against. The closure CLAUDE.md §Browser half asks for
 * is at the consumer, not at the dispatch: a parser must be unreachable for a type it does not serve, asserted
 * where the parse is, so a route added later FIRES instead of silently widening an old wrong answer. This
 * entry re-asks `document_load_type_of` about the type it was handed, so a caller that reaches §7.5.4 with an
 * HTML response crashes here even if it never went through core/loader/document_load.h at all.
 *
 * `text` IS CHARACTERS, NOT THE RESPONSE'S BYTES, and that is the same contract core/html/html_parse.h's entry
 * has. §7.5.4's last paragraph says the rules for converting a text document's bytes into characters are the
 * COMPUTED TYPE's own and not HTML §13.2.3.2 "Determining the character encoding" — a difference that belongs
 * to whoever decodes a response, which is one step above both loaders and identical for both of them today.
 * `size` may be zero: an empty response is still a text document, and §7.5.4 gives it an empty `pre`.
 *
 * `root_kind` is html_parse.h's — DOM_PARSE_ROOT_PRIVATE for a Document the same uninterrupted operation
 * created, DOM_PARSE_ROOT_SHARED for the active one. It is carried through unread.
 *
 * IT IS A PULL, for core/loader/html_document.h's reason and out of the same sentence of §7.5.4 step 4: "each
 * task that the networking task source places on the task queue while fetching runs must then fill the
 * parser's input byte stream with the fetched bytes and cause the HTML parser to perform the appropriate
 * processing of the input stream". A task per arrival is the algorithm, and handing the tokenizer the whole
 * response in one call collapsed all of them into an un-interruptible span the length of the document.
 * `begin` performs §7.5.4 steps 1-4 — the parser, the mode, the synthetic `pre` and LINE FEED, and the switch
 * to HTML §13.2.5.5 "PLAINTEXT state" — `step` fills ONE byte of the response, and `finish` emits §13.2.7
 * "The end"'s implied EOF and asserts the tree §7.5.4 describes. `text` is BORROWED for the life of the load
 * (core/loader/html_document.h states why nothing is copied). */
typedef struct TextDocumentLoad TextDocumentLoad;

TextDocumentLoad *text_document_load_begin(lxb_html_document_t *document, DomParseRootKind root_kind,
                                           HtmlScriptingMode scripting,
                                           const MimeType *type, const lxb_char_t *text, size_t size);
bool             text_document_load_ended(const TextDocumentLoad *load);
void             text_document_load_step(TextDocumentLoad *load);
lxb_status_t     text_document_load_finish(TextDocumentLoad *load);
/* ABANDON THE LOAD MID-ITEM — the flow driving it is gone. It closes whatever the arm has open (a stream that
   is still taking bytes; the tree build's own reader) and destroys the handle. The partially built tree is
   LEFT WHERE IT STANDS: whoever owns the Document decides what becomes of it, exactly as they do for a failed
   parse, and a loader that emptied it would be deciding a consequence that is not its own. */
void             text_document_load_abort(TextDocumentLoad *load);

#endif
