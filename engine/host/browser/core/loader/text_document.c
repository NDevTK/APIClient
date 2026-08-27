/* HTML §7.5.4 "Loading text documents", and nothing else.
 *
 * THE ALGORITHM, in its own words: "To load a text document, given a navigation params navigationParams and a
 * string type: Let document be the result of creating and initializing a Document object given "html", type,
 * and navigationParams. Set document's parser cannot change the mode flag to true. Set document's mode to
 * "no-quirks". Create an HTML parser and associate it with the document. Act as if the tokenizer had emitted a
 * start tag token with the tag name "pre" followed by a single U+000A LINE FEED (LF) character, and switch the
 * HTML parser's tokenizer to the PLAINTEXT state. … Return document."
 *
 * SO A TEXT DOCUMENT IS AN HTML DOCUMENT WITH MARKUP AROUND IT, and that is the half a reader working from
 * memory gets wrong. The response's characters are not the document's text nodes laid out under `body`: they
 * are the content of a `pre` element the loader SYNTHESISES, which is why `documentElement.textContent` of a
 * text document answers the response and `body.firstElementChild.tagName` answers "PRE". A loader that put the
 * characters straight under `body` would answer the first question right and the second one wrong, with
 * nothing to say so.
 *
 * AND WHY THE PARSER IS DRIVEN RATHER THAN THE TREE BUILT. The obvious shortcut — parse the six bytes of a
 * skeleton and then hang one Text node inside the `pre` — reproduces the DOM for the inputs one thinks to try
 * and diverges on the ones the tokenizer exists for: HTML §13.2.5.5 "PLAINTEXT state" emits a U+FFFD
 * REPLACEMENT CHARACTER for a U+0000 NULL, and HTML §13.2.3.5 "Preprocessing the input stream" normalizes a
 * U+000D CARRIAGE RETURN and a CR/LF pair to a single U+000A. A `text/plain` response is exactly the kind of
 * response that carries those bytes. The real tokenizer already does all of it, in the state the standard
 * names, so the standard's state is what this switches it into.
 *
 * THE SYNTHETIC PREFIX IS SPLIT ACROSS THE STATE SWITCH, AND THAT IS THE ONE DESIGN DECISION HERE.
 * §7.5.4 emits the `pre` start tag token and the LF character token and THEN switches the tokenizer, and the
 * literal transcription of that — feed "<pre>\n" and then switch — LOSES THE LINE FEED in a chunked tokenizer.
 * A character run that has not been terminated is held in the tokenizer's temporary buffer between chunks, and
 * `lxb_html_tokenizer_state_plaintext_before` opens its run with `lxb_html_tokenizer_state_token_set_begin`,
 * whose first act is `tkz->pos = tkz->start` — it DISCARDS whatever the previous state left pending. The LF
 * would vanish, the tree would still be standing in its skip-a-leading-newline mode, and the FIRST LINE FEED
 * OF THE RESPONSE would be eaten in its place: a document silently one character short, for exactly the
 * responses (a `text/plain` file beginning with a blank line) where nobody would look.
 *
 * So the `pre` start tag is fed on its own — after `>` the tokenizer is back in HTML §13.2.5.1 "Data state"
 * with an empty pending run, which is the one point where a state switch costs nothing — and the LINE FEED is
 * fed as the first characters of the PLAINTEXT run. Nothing observable moves: a character token is a character
 * token whatever state produced it, the token sequence reaching §13.2.6 tree construction is still the `pre`
 * start tag then a LF then the response, and §13.2.6.4.7 'The "in body" insertion mode''s `pre` rule — "if the
 * next token is a U+000A LINE FEED (LF) character token, then ignore that token" — consumes the synthetic LF
 * and leaves the response's own leading newline alone. It is that rule the synthetic LF exists to feed; drop
 * the LF and the rule eats the response instead.
 *
 * "PARSER CANNOT CHANGE THE MODE" IS MODELLED BY ITS ONE OBSERVABLE EFFECT, AND ASSERTED. The flag is read in
 * exactly one place — HTML §13.2.6.4.1 'The "initial" insertion mode', whose "anything else" arm sets the
 * Document to quirks mode when the flag is false, which is the arm the `pre` start tag takes because a
 * synthesised prefix carries no DOCTYPE. Steps 2 and 3 together say: that arm must not move the mode off
 * "no-quirks". Nothing else can move it afterwards, because the only other writer is a DOCTYPE token and the
 * PLAINTEXT state emits none — so setting the mode once the prefix is through, and CHECKING at the close that
 * it is still what it was set to, is the flag's whole behaviour with the check that catches the day it is not.
 */
#include <lexbor/html/tokenizer/state.h>

#include "core/html/html_parse.h"
#include "core/loader/document_load_type.h"
#include "core/loader/text_document.h"
#include "core/mime/mime_type.h"
#include "check.h"

/* §7.5.4 step 4's TWO SYNTHETIC TOKENS, in the two pieces the paragraph above splits them into. Static storage
   because lexbor's chunked tokenizer holds pointers into the buffer it was handed until the run terminates,
   and a run opened by the second of these is terminated by the response that follows it. */
static const lxb_char_t TEXT_DOC_PRE[] = "<pre>";
static const lxb_char_t TEXT_DOC_LF[]  = "\n";

lxb_status_t text_document_load(lxb_html_document_t *document, DomParseRootKind root_kind,
                                const MimeType *type, const lxb_char_t *text, size_t size)
{
    lxb_dom_document_t *doc;
    lxb_html_parser_t *parser;
    lxb_dom_node_t *first;
    lxb_html_body_element_t *body;
    lxb_status_t st;

    DCHECK(document != NULL,
           "HTML §7.5.4 was reached with no Document — its step 1 creates one and hands it to the parser, so "
           "a loader with nothing to load into never ran that step");
    DCHECK(text != NULL,
           "HTML §7.5.4 was handed a NULL pointer for the response's characters — an empty text document is a "
           "zero SIZE over a real pointer, which is what the tokenizer takes and what a response with no body "
           "already is");
    /* THE CLOSURE, AND IT IS NOT VACUOUS BECAUSE THIS IS A PUBLIC ENTRY. core/loader/document_load.c routes
       here having asked the same question, and this asks it again about the same record — so the day a second
       route reaches §7.5.4 with a response §7.4.5 loads as something else, it crashes HERE, at the parse,
       rather than producing a `pre` full of HTML source. */
    DCHECK(document_load_type_of(type) == DOC_LOAD_TEXT,
           "HTML §7.5.4 \"Loading text documents\" was handed a response HTML §7.4.5's load-a-document does "
           "not send to it — the arm is a fact about the COMPUTED TYPE and this loader re-asks it, so this is "
           "a route into the text loader that never asked what it fetched");

    /* §7.5.4 step 4's `pre` START TAG, alone. See the file comment for why the LINE FEED is not here. */
    st = html_parse_document_open(document, root_kind, TEXT_DOC_PRE, sizeof TEXT_DOC_PRE - 1);
    if (st != LXB_STATUS_OK)
        return st;

    doc = lxb_dom_interface_document(document);
    /* §7.5.4 steps 2 and 3, in the order the standard writes them and at the one moment they are both true:
       the prefix has been through §13.2.6.4.1 'The "initial" insertion mode' (which is where the mode a text
       document must not have is set), and nothing that could move it again is left in the stream. */
    doc->compat_mode = LXB_DOM_DOCUMENT_CMODE_NO_QUIRKS;

    parser = (lxb_html_parser_t *)doc->parser;
    DCHECK(parser != NULL,
           "an HTML parse that lexbor reported OK left the document with no parser — html_parse_document_open "
           "creates one before its first token and the document owns it until teardown, so a null field here "
           "is a parse that opened somewhere this component cannot see");
    /* §7.5.4 step 4's "switch the HTML parser's tokenizer to the PLAINTEXT state" — HTML §13.2.5.5 "PLAINTEXT
       state", entered through the same helper §13.2.6.4.7 'The "in body" insertion mode' uses for a
       `plaintext` start tag, which is what makes this the standard's state rather than a state like it. */
    lxb_html_tokenizer_state_set(parser->tkz, lxb_html_tokenizer_state_plaintext_before);

    /* §7.5.4 step 4's LINE FEED, then the response. Two chunks of ONE character run: the tokenizer holds the
       run across the boundary, so §13.2.6 sees a single character token beginning with the synthetic LF, which
       is exactly what §13.2.6.4.7's `pre` rule is written to consume. */
    st = html_parse_document_write(document, TEXT_DOC_LF, sizeof TEXT_DOC_LF - 1);
    if (st != LXB_STATUS_OK)
        return st;
    st = html_parse_document_write(document, text, size);
    if (st != LXB_STATUS_OK)
        return st;
    /* §13.2.7 "The end" — the implied EOF character §7.5.4's "when no more bytes are available" asks for. */
    st = html_parse_document_close(document);
    if (st != LXB_STATUS_OK)
        return st;

    /* §7.5.4 steps 2 and 3 AGAIN, as the check that they held. The parser cannot change the mode flag has one
       reader and one writer, and both have now run; a mode that moved means a DOCTYPE token reached §13.2.6,
       which the PLAINTEXT state cannot emit. */
    DCHECK(doc->compat_mode == LXB_DOM_DOCUMENT_CMODE_NO_QUIRKS,
           "a text document's mode moved off no-quirks during its own parse — HTML §7.5.4 step 2 sets the "
           "parser cannot change the mode flag, whose one reader is §13.2.6.4.1 'The initial insertion mode', "
           "so something emitted a DOCTYPE token into a stream the PLAINTEXT state cannot produce one from");
    /* AND THE TREE IS THE ONE §7.5.4 DESCRIBES. This is what every consumer of a text document reads — the
       response is the `pre`'s content and not `body`'s — so it is asserted here rather than discovered from a
       `textContent` that is right by accident. */
    body = lxb_html_document_body_element(document);
    DCHECK(body != NULL,
           "a text document's parse produced no body element — §13.2.6 builds html, head and body out of the "
           "`pre` start tag and the EOF token whatever else the stream held, so a document without one is a "
           "parse that never reached tree construction");
    first = lxb_dom_interface_node(body)->first_child;
    DCHECK(first != NULL && first->type == LXB_DOM_NODE_TYPE_ELEMENT &&
           first->local_name == LXB_TAG_PRE && first->ns == LXB_NS_HTML && first->next == NULL,
           "a text document's body does not hold exactly one `pre` element — HTML §7.5.4 step 4 synthesises "
           "that element and the PLAINTEXT state emits nothing but characters after it, so a second child or "
           "a different first one means the response's own bytes were tokenized as markup");
    return st;
}
