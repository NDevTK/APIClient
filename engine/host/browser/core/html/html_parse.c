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

/* THE TOKEN LEXBOR'S TREE BUILDER IS CONSUMING RIGHT NOW, and NULL whenever it is not consuming one — which is
   most of the program's life, and is what makes an attribute a page's own `setAttribute` appends a no-op here
   rather than a case to exclude.
   IT IS A STACK FACT AND IT IS SAVED AND RESTORED LIKE ONE. CLAUDE.md's rule against a module static is about
   one PLACE answering a per-agent question for many agents; this is the opposite shape — a C activation naming
   itself for the duration of one call, with the previous value restored on the way out, so a nested parse
   (§13.4's fragment parse reached from inside a token, which lexbor does not do today because it executes no
   script during tree construction) is exact rather than a case that silently claims the wrong token's bytes.
   The callback cannot be handed a context: `lxb_dom_node_cb_insert_f` takes the node and nothing else. */
static lxb_html_token_t *g_token_in_construction;

/* THE ADOPTION, WATCHED. `lxb_dom_element_attr_append` calls this with the Attr AFTER
   `lxb_dom_attr_set_value_wo_copy` has already stored the token's own allocation in `attr->value->data`, so a
   pointer match is the DOM taking ownership of those bytes — recorded by CLEARING the token's field, which is
   the same statement `lxb_dom_attr_interface_destroy` makes from the other side when it frees exactly that
   pointer into `doc->text`. Everything left on the token at token-done is then, by construction, unowned.
   IT MATCHES ON THE POINTER AND NOTHING ELSE. An Attr whose value is a COPY — `append_attributes_from_element`
   clones name and value for the adoption agency's fake tokens, and every `setAttribute` allocates its own —
   names an address no live token attribute holds, so it falls through without a case of its own. */
static lxb_status_t html_parse_node_inserted(lxb_dom_node_t *node)
{
    lxb_dom_attr_t *attr;
    lxb_html_token_attr_t *ta;

    DCHECK(node != NULL, "the DOM reported the insertion of no node");
    if (node->type != LXB_DOM_NODE_TYPE_ATTRIBUTE || g_token_in_construction == NULL)
        return LXB_STATUS_OK;
    attr = lxb_dom_interface_attr(node);
    if (attr->value == NULL || attr->value->data == NULL)
        return LXB_STATUS_OK;
    for (ta = g_token_in_construction->attr_first; ta != NULL; ta = ta->next) {
        if (ta->value != attr->value->data)
            continue;
        DCHECK(ta->value_size == attr->value->length,
               "an Attr took a token attribute's exact allocation and recorded a different length for it — "
               "`lxb_dom_attr_set_value_wo_copy` is handed `token_attr->value` and `token_attr->value_size` "
               "together, so the two disagreeing means something rewrote one of them in between and the bytes "
               "past the shorter of the pair belong to nobody");
        /* THE DOM IS THE OWNER FROM HERE. Nothing reads a token attribute's value after tree construction has
           appended it: `lxb_html_tree_append_attributes` advances to the next attribute without re-reading
           this one, and the only two other readers of the field in lexbor run BEFORE any element exists —
           §13.2.6.4.9's `<input type=hidden>` test, which reads the token to decide whether to insert at all,
           and `lxb_html_token_doctype_parse`, which a DOCTYPE token never reaches an element from. */
        ta->value = NULL;
        ta->value_size = 0;
        return LXB_STATUS_OK;
    }
    return LXB_STATUS_OK;
}

/* INSERT ONLY. `remove`, `destroy` and `set_value` stay lexbor's NULL because this component's question is
   asked exactly once per allocation — at the instant ownership moves — and a table that answered more would be
   a second place the same fact is decided. */
static const lxb_dom_document_node_cb_t g_node_cb = {
    .insert = html_parse_node_inserted, .remove = NULL, .destroy = NULL, .set_value = NULL
};

void html_parse_own_token_values(lxb_dom_document_t *doc)
{
    DCHECK(doc != NULL, "no document was given ownership of its parses' token attribute values");
    DCHECK(doc->node_cb != NULL,
           "a document reached this install with no node-callback table at all — `lxb_dom_document_init` "
           "points every document at one before anything else, so a NULL means these bytes are not a document");
    DCHECK(doc->node_cb == &g_node_cb ||
           (doc->node_cb->insert == NULL && doc->node_cb->remove == NULL &&
            doc->node_cb->destroy == NULL && doc->node_cb->set_value == NULL),
           "a document already carries a node-callback table that is neither lexbor's empty default nor this "
           "one — installing over it would silently drop whatever that component watches; COMPOSE the two "
           "here instead, naming both");
    doc->node_cb = &g_node_cb;
}

bool html_parse_owns_token_values_of(const lxb_dom_document_t *doc)
{
    DCHECK(doc != NULL, "the token-value ownership question was asked of no document");
    return doc->node_cb == &g_node_cb;
}

/* A CONSUMED TOKEN, RELEASED. Every attribute value still on it is an allocation out of the arena
   `lxb_html_tokenizer_attrs_mraw` names that no Attr took — see html_parse.h for why that is the whole answer
   and why it needs neither the element nor a replica of §13.2.6.4's skip rule. */
static void html_parse_release_token(lxb_html_tokenizer_t *tkz, lxb_html_token_t *token,
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
        /* NO VALUE, NO ALLOCATION — or one the DOM has taken, which html_parse_node_inserted records the same
           way, because they are the same statement: this field is non-NULL exactly while these bytes are the
           token's. `lxb_html_tokenizer_state_set_value_m` is the only thing that fills it, and it runs only
           where §13.2.5 reads a quoted or unquoted attribute string — so a bare `<br>` and `<!doctype html>`
           own nothing at all, and an attribute NAME is a `lxb_dom_attr_data_t` id interned in the document's
           `attrs` hash rather than bytes. Absence is the POSITIVE statement that this attribute holds no
           allocation, which is the same statement core/dom/document_type.c reads out of a `lexbor_str_t` whose
           `data` is NULL. */
        if (attr->value == NULL)
            continue;
        DCHECK(node_heap_arena_of(attr->value) == NODE_ARENA_TEXT,
               "a token's attribute value is not an allocation of the agent's TEXT arena — the tokenizer takes "
               "it from `tkz->attrs_mraw`, which core/dom/node_heap.h has made that arena for every document "
               "in this instance, so a value from anywhere else means the parse was set up by something this "
               "engine does not own");
        /* AND §4.6'S DOCTYPE REALLY COPIED IT. Its consumer is the one that takes a token attribute's bytes
           WITHOUT going through `lxb_dom_element_attr_append`, so the claim above cannot see it:
           `lxb_html_token_doctype_parse` runs `lexbor_str_init` + `lexbor_str_append` into
           `owner_document->mraw`, and the doctype node's ids are therefore a DIFFERENT address holding the
           same bytes. The day that becomes an adoption this free is a use-after-free on a live node and this
           is where it stops. A token processed in a mode that ignores it built no doctype at all, and `dt` is
           then the document's earlier one or none — either way it does not name these bytes. */
        DCHECK(token->tag_id != LXB_TAG__EM_DOCTYPE || dt == NULL ||
               (dt->public_id.data != attr->value && dt->system_id.data != attr->value),
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
    lxb_html_tree_t *tree = (lxb_html_tree_t *)ctx;
    lxb_html_token_t *prev, *out;

    DCHECK(g_lxb_token_done != NULL,
           "a token reached this component before any parser handed over lexbor's own token-done callback — "
           "the capture happens at html_parse_new_parser and there is no other way to be installed");
    DCHECK(token != NULL && ctx != NULL, "a token-done callback was reached with no token or no tree builder");
    DCHECK(tree->document != NULL,
           "a token reached tree construction with no document attached to the tree builder — see the same "
           "statement in html_parse_release_token, which cannot then name the arena either");
    /* THE CLAIM HAS TO BE INSTALLED ON THE DOCUMENT THE APPEND WILL READ IT OFF, which is the one
       `lxb_dom_document_owner` resolves — §13.4's temporary fragment document is INHERITED and lexbor's init
       resets its own table to the empty default, while stamping every element and every Attr it makes with the
       real document. Asserted here rather than installed here, and on every token rather than at the parse's
       start, because this is the line that would otherwise release a LIVE Attr's bytes. */
    DCHECK(html_parse_owns_token_values_of(lxb_dom_document_owner(&tree->document->dom_document)),
           "a document is being parsed into whose node-callback table is not this component's — every "
           "adoption `lxb_html_tree_append_attributes` performs would go unseen, and the release below would "
           "hand the DOM's own attribute values back to the allocator underneath live Attr nodes; the install "
           "belongs at dom_document_create, which is the one place a Document is made");
    /* TREE CONSTRUCTION FIRST, ALWAYS. The release below is only correct AFTER lexbor has consumed the token:
       `lxb_html_tree_token_callback` runs `lxb_html_tree_insertion_mode`, whose loop reprocesses the token
       until a mode consumes it, and it is that pass which either adopts each value (claimed as it happens) or
       leaves it with nothing naming it. A failing inner callback (it returns NULL and sets `tkz->status`) has
       abandoned the token rather than freed it, so the release is right on that path too — the token's
       attributes are still exactly as the tokenizer and the claim left them. */
    prev = g_token_in_construction;
    g_token_in_construction = token;
    out = g_lxb_token_done(tkz, token, ctx);
    DCHECK(g_token_in_construction == token,
           "the token this call published is not the one standing when tree construction returned — the "
           "publish and the restore are one bracket, so a mismatch means something unwound out of the middle "
           "of it and the next claim would clear a field belonging to another parse");
    g_token_in_construction = prev;
    html_parse_release_token(tkz, token, tree);
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
           "around itself and release each token's values twice");
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
