/* See html_parse.h. */
#include <lexbor/core/mraw.h>
#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "core/dom/node_heap.h"
#include "core/html/html_parse.h"

/* LEXBOR'S TOKEN-DONE CALLBACK IS STATIC, SO IT IS CAPTURED RATHER THAN NAMED — `lxb_html_tree_token_callback`
   has no declaration in any header, and re-writing its six lines here would be a second copy of a function
   that is already right and would drift the day lexbor's does. It is ONE function for the whole program:
   `lxb_html_tree_init` is the only thing that installs it, and it installs the same pointer for every tree. So
   one static holds it and the install ASSERTS that every parser hands over the same one — the day that stops
   being true, the inner callback has to be stored per tokenizer and this crash is what says so.
   ITS CONTEXT IS NOT STORED, deliberately: the tree pointer is passed straight through, so this component adds
   nothing to the callback's state and there is no per-tokenizer holder to allocate, hand to lexbor, and free
   when the parser dies. */
static lxb_html_tokenizer_token_f g_lxb_token_done;

/* A DOCTYPE token, released. Every attribute value it carries is an allocation out of the arena
   `lxb_html_tokenizer_attrs_mraw` names, and tree construction has just finished with all of them — see
   html_parse.h for why that is the whole answer for this token kind and for no other. */
static void html_parse_release_doctype_token(lxb_html_tokenizer_t *tkz, lxb_html_token_t *token,
                                             const lxb_html_tree_t *tree)
{
    lexbor_mraw_t *arena = lxb_html_tokenizer_attrs_mraw(tkz);
    const lxb_dom_document_type_t *dt;
    lxb_html_token_attr_t *attr;

    DCHECK(tree->document != NULL,
           "a token was completed with no document attached to the tree builder — `lxb_html_tree_begin` "
           "attaches one before the first byte of every parse, the document parse and HTML §13.4's fragment "
           "parse alike, so the arena this token's bytes came out of cannot be identified");
    DCHECK(arena == tree->document->dom_document.text,
           "the tokenizer is allocating attribute values out of an arena that is not the text arena of the "
           "document being built — `lxb_html_parse_chunk_prepare` and `lxb_html_parse_fragment_chunk_begin` "
           "are the two entries that set it and both name `doc->text`, so a third entry reached this parse and "
           "the free below would hand the bytes to the wrong allocator");
    dt = tree->document->dom_document.doctype;

    for (attr = token->attr_first; attr != NULL; attr = attr->next) {
        /* NO VALUE, NO ALLOCATION. `lxb_html_tokenizer_state_set_value_m` is the only thing that fills this
           field, and it runs only where §13.2.5's doctype states read a quoted string — so `<!doctype html>`
           owns nothing at all and its one attribute, the NAME, is a `lxb_dom_attr_data_t` id interned in the
           document's `attrs` hash rather than bytes. Absence is the POSITIVE statement that this attribute
           holds no allocation, which is the same statement core/dom/document_type.c reads out of a
           `lexbor_str_t` whose `data` is NULL. */
        if (attr->value == NULL)
            continue;
        DCHECK(node_heap_arena_of(attr->value) == NODE_ARENA_TEXT,
               "a doctype token's attribute value is not an allocation of the agent's TEXT arena — the "
               "tokenizer takes it from `tkz->attrs_mraw`, which core/dom/node_heap.h has made that arena for "
               "every document in this instance, so a value from anywhere else means the parse was set up by "
               "something this engine does not own");
        /* AND LEXBOR REALLY COPIED IT, which is the claim the free rests on and therefore the claim that is
           asserted rather than trusted. `lxb_html_token_doctype_parse` runs `lexbor_str_init` +
           `lexbor_str_append` into `owner_document->mraw`, so the doctype node's ids are a DIFFERENT address
           holding the same bytes; the day that becomes an adoption this free is a use-after-free on a live
           node and this is where it stops. A token processed in a mode that ignores it built no doctype at
           all, and `dt` is then the document's earlier one or none — either way it does not name these bytes. */
        DCHECK(dt == NULL || (dt->public_id.data != attr->value && dt->system_id.data != attr->value),
               "the doctype node built from this token holds the TOKEN's own attribute value as one of its "
               "ids — lexbor copied it into the node arena when this component was written, so it has started "
               "adopting instead, and releasing the token's value here would free the live node's bytes");
        (void) lexbor_mraw_free(arena, attr->value);
        attr->value = NULL;
        attr->value_size = 0;
    }
}

static lxb_html_token_t *html_parse_token_done(lxb_html_tokenizer_t *tkz, lxb_html_token_t *token, void *ctx)
{
    lxb_html_token_t *out;

    DCHECK(g_lxb_token_done != NULL,
           "a token reached this component before any parser handed over lexbor's own token-done callback — "
           "the capture happens at html_parse_new_parser and there is no other way to be installed");
    DCHECK(token != NULL && ctx != NULL, "a token-done callback was reached with no token or no tree builder");
    /* TREE CONSTRUCTION FIRST, ALWAYS. The release below is only correct AFTER lexbor has consumed the token:
       it is the copy `lxb_html_token_doctype_parse` makes that leaves the original unreferenced, so releasing
       first would hand the doctype's own ids a freed source to copy from. A failing inner callback (it returns
       NULL and sets `tkz->status`) has abandoned the token rather than freed it, so the release is right on
       that path too — the token's attributes are still exactly as the tokenizer left them. */
    out = g_lxb_token_done(tkz, token, ctx);
    if (token->tag_id == LXB_TAG__EM_DOCTYPE)
        html_parse_release_doctype_token(tkz, token, (const lxb_html_tree_t *)ctx);
    return out;
}

lxb_html_parser_t *html_parse_new_parser(void)
{
    lxb_html_parser_t *parser = lxb_html_parser_create();
    lxb_html_tokenizer_token_f inner;
    void *inner_ctx;

    if (parser == NULL)
        return NULL;
    if (lxb_html_parser_init(parser) != LXB_STATUS_OK) {
        /* lexbor's own `lxb_html_document_parser_prepare` unwinds a failed init exactly this way, and
           `lxb_html_parser_destroy` is written for a half-built parser (each piece destroys NULL as a no-op). */
        (void) lxb_html_parser_destroy(parser);
        return NULL;
    }

    /* AFTER `lxb_html_parser_init`, WHICH IS WHERE THE CALLBACK TO WRAP COMES FROM. `lxb_html_tokenizer_init`
       leaves an identity callback on the tokenizer and `lxb_html_tree_init` — called by parser init, with the
       parser's tree — replaces it with the tree builder's. Installing before that would capture the identity
       one and then be overwritten, which is the one ordering mistake this sequence can make. */
    inner = parser->tkz->callback_token_done;
    inner_ctx = lxb_html_tokenizer_callback_token_done_ctx(parser->tkz);
    DCHECK(inner != html_parse_token_done,
           "a parser fresh out of `lxb_html_parser_init` already has this component's callback on it — the "
           "install runs exactly once, at the parser's creation, and a second would wrap this component "
           "around itself and release each doctype token twice");
    DCHECK(inner_ctx == parser->tree,
           "the callback this component is about to wrap is not the tree builder's — `lxb_html_tree_init` "
           "points a tokenizer's token-done callback at the tree it initialises, so a context that is not "
           "that tree means the wrap would hand every token of this parse to a stranger");
    DCHECK(g_lxb_token_done == NULL || g_lxb_token_done == inner,
           "two different lexbor token-done callbacks reached this component — `lxb_html_tree_token_callback` "
           "is one static function that `lxb_html_tree_init` installs on every tree, so a second one means the "
           "inner callback has to be held per tokenizer instead of once for the program");
    g_lxb_token_done = inner;
    lxb_html_tokenizer_callback_token_done_set(parser->tkz, html_parse_token_done, inner_ctx);
    return parser;
}

lxb_status_t html_parse_document(lxb_html_document_t *document, const lxb_char_t *html, size_t size)
{
    lxb_dom_document_t *doc;

    DCHECK(document != NULL, "no document was parsed");
    DCHECK(html != NULL,
           "a document was parsed from a NULL pointer — an empty parse is a zero SIZE over a real pointer, "
           "which is what lexbor's chunk process takes and what every caller here already builds");
    /* ONCE PER DOCUMENT, AND THIS IS THE ONE LINE THAT CAN SAY SO. `lxb_html_document_parse` opens by cleaning
       a document whose ready state is past LOADING, and `lxb_dom_document_clean` runs `lexbor_mraw_clean` over
       `mraw` and `text` — which core/dom/node_heap.h has made the AGENT'S. A second parse of one document
       would therefore hand back every chunk of every OTHER document in this instance while their trees still
       point into them, with no crash site of its own. Upstream is safe because the arenas are the document's.
       A re-parse builds a NEW document; there is nothing to fix inside this one. */
    DCHECK(document->ready_state == LXB_HTML_DOCUMENT_READY_STATE_UNDEF ||
           document->ready_state == LXB_HTML_DOCUMENT_READY_STATE_LOADING,
           "a document that has already been parsed is being parsed again — lexbor cleans it first and that "
           "clean empties the AGENT's two DOM arenas, so every node of every other document in this instance "
           "would be returned to the allocator with its tree still naming it; parse into a new document");
    doc = lxb_dom_interface_document(document);
    if (doc->parser == NULL) {
        /* THE CREATION LEXBOR WOULD HAVE MADE, MADE HERE. `lxb_html_document_parser_prepare` is static and
           does exactly this when the field is NULL — `lxb_html_parser_create` + `lxb_html_parser_init`, whose
           `keep_duplicate` it then re-sets to the value init already gave it — and having made it here, its
           `doc->parser != NULL` arm reuses this one. The document keeps ownership either way:
           `lxb_html_document_interface_destroy` unrefs the field. */
        doc->parser = html_parse_new_parser();
        CHECK(doc->parser != NULL,
              "html-parse-oom: a document's HTML parser could not be created, so the document cannot be "
              "parsed at all — every flow in this instance reads the DOM this parse produces");
    }
    return lxb_html_document_parse(document, html, size);
}

bool html_parse_owns_tokens_of(const lxb_dom_document_t *doc)
{
    const lxb_html_parser_t *parser;

    DCHECK(doc != NULL, "the token-ownership question was asked of no document");
    if (doc->parser == NULL)
        return true;
    parser = (const lxb_html_parser_t *)doc->parser;
    DCHECK(parser->tkz != NULL,
           "a document holds an HTML parser with no tokenizer — `lxb_html_parser_init` creates one before "
           "anything else and a parser that failed it is destroyed rather than stored");
    return parser->tkz->callback_token_done == html_parse_token_done;
}
