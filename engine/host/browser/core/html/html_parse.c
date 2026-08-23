/* See html_parse.h. */
#include <lexbor/core/mraw.h>
#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>
/* NOT REACHED THROUGH `html.h` — the four §4.9 attribute-change steps this component composes over are named
   directly, and nothing else pulls their declarations in. */
#include <lexbor/html/attribute_steps.h>

#include "check.h"
#include "core/dom/node_heap.h"
#include "core/html/html_parse.h"
/* §13.2.6's DOM writes are the per-flow delta's — this component owns the one place a parser is made, so it is
   where that implementation is installed. */
#include "solver/dom_cow.h"

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
   The callback cannot be handed a context: `lxb_dom_element_attr_change_f` takes the attribute's element, its
   name and its bytes, and nothing else. */
static lxb_html_token_t *g_token_in_construction;

/* THE ADOPTION, WATCHED. `lxb_dom_element_attr_append` calls this AFTER `lxb_dom_attr_set_value_wo_copy` has
   already stored the token's own allocation in `attr->value->data`, and hands that exact pointer over as
   `value` — so a pointer match is the DOM taking ownership of those bytes, recorded by CLEARING the token's
   field, which is the same statement `lxb_dom_attr_interface_destroy` makes from the other side when it frees
   exactly that pointer into `doc->text`. Everything left on the token at token-done is then, by construction,
   unowned.
   IT MATCHES ON THE POINTER AND NOTHING ELSE. An Attr whose value is a COPY — `append_attributes_from_element`
   clones name and value for the adoption agency's fake tokens, and every `setAttribute` allocates its own —
   names an address no live token attribute holds, so it falls through without a case of its own.
   IT IS THE APPEND AND NOT THE CHANGE. `lxb_dom_attr_set_value` is the only caller of `attr_mutation->change`
   and it MEMCPYs its input into a fresh `doc->text` allocation before firing, so a token's bytes can never be
   what a change reports; `lxb_dom_attr_set_value_wo_copy`, which is what tree construction uses and what does
   hold the token's pointer, fires nothing at all. Append is therefore the one edge those bytes cross, which is
   why it is the one member the table below wraps rather than passes through. */
static lxb_status_t html_parse_attr_appended(lxb_dom_element_t *element,
                                             lxb_dom_attr_id_t local_name,
                                             const lxb_char_t *old_value, size_t old_len,
                                             const lxb_char_t *value, size_t value_len,
                                             lxb_ns_id_t ns)
{
    lxb_html_token_attr_t *ta;

    DCHECK(element != NULL, "the DOM reported an attribute append onto no element");
    DCHECK(old_value == NULL && old_len == 0,
           "an attribute APPEND reported a previous value — an append is §4.9's \"append an attribute\", which "
           "puts an attribute that was on no element onto one, so there is nothing it could replace; a "
           "non-NULL old value means these bytes came from \"change an attribute\" and the table below wired "
           "the wrong member");
    if (g_token_in_construction != NULL && value != NULL) {
        for (ta = g_token_in_construction->attr_first; ta != NULL; ta = ta->next) {
            if (ta->value != value)
                continue;
            DCHECK(ta->value_size == value_len,
                   "an Attr took a token attribute's exact allocation and recorded a different length for it — "
                   "`lxb_dom_attr_set_value_wo_copy` is handed `token_attr->value` and `token_attr->value_size` "
                   "together, so the two disagreeing means something rewrote one of them in between and the "
                   "bytes past the shorter of the pair belong to nobody");
            /* THE DOM IS THE OWNER FROM HERE. Nothing reads a token attribute's value after tree construction
               has appended it: `lxb_html_tree_append_attributes` advances to the next attribute without
               re-reading this one, and the only two other readers of the field in lexbor run BEFORE any element
               exists — §13.2.6.4.9's `<input type=hidden>` test, which reads the token to decide whether to
               insert at all, and `lxb_html_token_doctype_parse`, which a DOCTYPE token never reaches an element
               from. */
            ta->value = NULL;
            ta->value_size = 0;
            break;
        }
    }
    /* AND THEN LEXBOR'S OWN STEPS RUN — the composition, on EVERY path out of the bookkeeping above. The match
       ends in a `break` rather than a return for exactly that reason: a claimed token attribute is the common
       case, and returning there is how the delegation silently stops happening for it. */
    return lxb_html_attribute_steps_append(element, local_name, old_value, old_len,
                                           value, value_len, ns);
}

/* THE COMPOSITION, NAMING BOTH. Every HTML document carries lexbor's OWN §4.9 attribute change steps —
   `lxb_html_document_mutation_init` installs all four the moment `lxb_html_document_interface_create` finishes
   `lxb_dom_document_init`, so there is no such thing here as an HTML document whose table is empty. Replacing
   it wholesale would silently disable them, which is what the install below asserts against.
   SO THREE MEMBERS ARE LEXBOR'S, UNCHANGED, and only `append` is wrapped — this component has exactly one
   question and it is asked at exactly one edge. A member this component wrapped without needing to would be a
   second place the same fact is decided, and three pass-through wrappers that only delegate would be three
   chances to forget to.
   THERE IS EXACTLY ONE OTHER INSTALLER IN LEXBOR AND THIS ENGINE DOES NOT REACH IT: `lxb_style_init` calls
   `lxb_html_document_style_mutation_init`, which swaps in the STYLE module's tables (they chain to these same
   HTML steps). Nothing here calls `lxb_style_*` or `lxb_html_document_css_init`, so the table found below is
   the HTML one. The day the style module IS initialised, the install below FIRES rather than silently
   delegating to the wrong steps — which is the correct outcome, because the composition target changes and
   this file has to name the new one.
   THE NODE-MUTATION TABLE IS LEFT ALONE ENTIRELY. Attributes are not nodes to lexbor's mutation callbacks any
   more: `lxb_dom_element_attr_append` reaches `attr_mutation`, while `mutation->inserted` is now a TREE edge
   that fires over shadow-including descendants for every element §13.2.6 inserts. Wiring this component there
   would put a per-subtree walk on the hot parse path to be told about nodes it has no question about — and
   would drop `lxb_html_element_steps_*`, the same defect one table over. */
static const lxb_dom_document_attr_mutation_cb_t g_attr_cb = {
    .change  = lxb_html_attribute_steps_change,
    .append  = html_parse_attr_appended,
    .remove  = lxb_html_attribute_steps_remove,
    .replace = lxb_html_attribute_steps_replace
};

void html_parse_own_token_values(lxb_dom_document_t *doc)
{
    DCHECK(doc != NULL, "no document was given ownership of its parses' token attribute values");
    DCHECK(doc->attr_mutation != NULL,
           "a document reached this install with no attribute-mutation table at all — `lxb_dom_document_init` "
           "points every document at one before anything else, so a NULL means these bytes are not a document");
    DCHECK(doc->attr_mutation == &g_attr_cb ||
           (doc->attr_mutation->change  == lxb_html_attribute_steps_change &&
            doc->attr_mutation->append  == lxb_html_attribute_steps_append &&
            doc->attr_mutation->remove  == lxb_html_attribute_steps_remove &&
            doc->attr_mutation->replace == lxb_html_attribute_steps_replace),
           "a document already carries an attribute-mutation table that is neither lexbor's own HTML steps nor "
           "this one — the table above composes over `lxb_html_attribute_steps_*` BY NAME, so installing on a "
           "third component's table would drop whichever of the two it is not; COMPOSE them here instead, "
           "naming all three. An ALL-NULL table means this is not an HTML document: "
           "`lxb_html_document_mutation_init` runs for every one, so the steps to delegate to are missing and "
           "the wrapper above would be delegating into lexbor's DOM-level default that does nothing");
    doc->attr_mutation = &g_attr_cb;
}

bool html_parse_owns_token_values_of(const lxb_dom_document_t *doc)
{
    DCHECK(doc != NULL, "the token-value ownership question was asked of no document");
    return doc->attr_mutation == &g_attr_cb;
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
        /* NO VALUE, NO ALLOCATION — or one the DOM has taken, which html_parse_attr_appended records the same
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

    /* HTML §13.2.6 "Tree construction"'s DOM WRITER, INSTALLED WHERE A PARSER IS MADE — the same reason the
       token-done wrapper is installed here and not at each parse: `lxb_html_document_parse` creates the parser
       it uses, so a call that reached lexbor's entry directly would get a tree builder writing the document
       through the vendor's own table, where no delta can see it. This is the one line every parse in this
       engine passes, so there is no parse left to forget. Idempotent by construction — one process-wide table
       whose address does not change — and asserted from the other side on every parse open. */
    dom_cow_install_tree_construction();
    DCHECK(dom_cow_owns_tree_construction(),
           "installing this engine's §13.2.6 DOM writer did not take — the table is one process-wide pointer "
           "and this is the line that sets it, so a mismatch here means a second component is competing for "
           "tree construction's writes and the two have to be composed rather than one silently dropped");

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

bool html_parse_insertion_point_defined(const lxb_dom_document_t *doc)
{
    const lxb_html_parser_t *parser;

    DCHECK(doc != NULL, "§13.2.3.5's insertion point was asked of no document");
    /* A DOCUMENT WITH NO PARSER HAS NO INPUT STREAM, which is §13.2.3.5's "initially, the insertion point is
       undefined" and not a hole: `lxb_html_document_parser_prepare` is what creates one, so a document nothing
       has parsed into has never had a stream to insert into. */
    if (doc->parser == NULL)
        return false;
    parser = (const lxb_html_parser_t *)doc->parser;
    /* PROCESS IS THE WINDOW, and it is lexbor's own statement of it rather than a second flag beside it:
       `lxb_html_parse_chunk_process` answers LXB_STATUS_ERROR_WRONG_STAGE in every other state, and
       `lxb_html_parse_chunk_end` moves the parser to END — which is §13.2.7 "The end"'s "Set the insertion
       point to undefined". A parser in ERROR is undefined too, because nothing may be handed to it. */
    return parser->state == LXB_HTML_PARSER_STATE_PROCESS;
}

lxb_status_t html_parse_document_open(lxb_html_document_t *document, const lxb_char_t *html, size_t size)
{
    lxb_html_document_opt_t opt;
    lxb_dom_document_t *doc;
    lxb_status_t st;

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
    /* §13.2.6's WRITES ARE THIS ENGINE'S NOW, AND THE HALF THAT IS STILL UNCAPTURED IS WHAT THIS ASSERTS.
       HTML §13.2.6 "Tree construction" wrote the document through lexbor's own `_wo_events` primitives at
       eight sites and, for §13.2.6.1's character merge, through a `lexbor_str_append` into a Text node's own
       storage that was not a mutator call at all — so none of it had an interception point and FIRING A
       CALLBACK IS NOT A CAPTURE anyway. The vendored parser now makes every one of those writes through one
       interposable table and solver/dom_cow.h installs itself as the implementation
       (dom_cow_install_tree_construction, asserted below on every parse this component opens).
       WHAT THAT BOUGHT AND WHAT IT DID NOT. The character merge IS captured — §13.2.6.1's own example is the
       parser appending to a Text node a SCRIPT created, so its target is a node the parse did not build and
       its undo is a value restore rather than a node removal. The STRUCTURAL writes are not: an entry per node
       a parse builds is O(everything the flow built) rather than O(shared state it touched), which is the
       invariant solver/dom_cow.h's private-tree entries exist to keep. They may be written without one only
       while the tree is one NO OTHER FLOW CAN REACH — every caller here creates the document immediately
       before parsing into it and wraps it immediately after, so at this line the target has no children and
       nothing in the agent names it. Open a parse on a document that already HAS a tree and those same
       insertions become shared-baseline writes belonging to no flow: one flow's markup visible to every
       sibling and unapplied on every context switch.
       SO THIS IS THE LINE §8.4.3 `document.write` ARRIVES AT FROM THE OTHER END. Its step 9 needs a live parser
       on the ACTIVE document — a document whose tree is the page's — and this assert is what says that cannot
       be had by opening one here. It is checkable rather than argued: the emptiness is the reachability, and
       solver/dom_cow.c's own members assert the per-write echo of it. */
    DCHECK(lxb_dom_interface_node(document)->first_child == NULL,
           "an HTML parse was opened on a document that already has a tree — §13.2.6's STRUCTURAL writes push "
           "no delta entry, which is harmless only while the document is one no other flow can observe. Give "
           "the parse a capture context that declares its root and route those writes at the capturing "
           "chokepoint (dom_cow_append_child / dom_cow_insert_before / dom_cow_remove_child) before parsing "
           "into a tree the page already holds");
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
    /* AND THAT §13.2.6's WRITES COME TO THIS ENGINE AT ALL — asked HERE, after the parser is resolved, because
       a document that already carried one got it from somewhere and the install rides parser creation. A tree
       builder still writing through lexbor's own DOM table produces a document whose text no delta ever saw,
       and there is no later point at which that becomes visible, so it is asked before the first token rather
       than discovered from a wrong answer. */
    DCHECK(dom_cow_owns_tree_construction(),
           "an HTML parse was opened while §13.2.6 tree construction was still writing through lexbor's own "
           "DOM table — every character token this parse merges into an existing Text node is then a write the "
           "running flow's delta cannot revert; dom_cow_install_tree_construction runs at html_parse_new_parser, "
           "so something replaced the table after this component installed it");
    /* THE `opt` BRACKET IS LEXBOR'S OWN AND IS KEPT BYTE-FOR-BYTE. `lxb_html_document_parse` saves
       `document->opt` across the prepare and the chunk and writes it back on BOTH its exits, while
       `lxb_html_document_parse_chunk_begin` — the entry this pair is built out of — does not. Dropping it
       would make an open-then-close parse answer differently from the one-shot one it replaces, which is
       exactly the silent divergence a re-expression must not introduce. */
    opt = document->opt;
    st = lxb_html_document_parse_chunk_begin(document);
    if (st == LXB_STATUS_OK)
        st = lxb_html_document_parse_chunk(document, html, size);
    document->opt = opt;
    DCHECK(st != LXB_STATUS_OK || html_parse_insertion_point_defined(doc),
           "a document parse that lexbor reported OK left no live input stream behind it — §13.2.3.5's "
           "insertion point is the parser standing in PROCESS, which is what `lxb_html_parse_chunk_prepare` "
           "puts it in and only `chunk_end` takes it out of, so an OK status with the stream already shut "
           "means something closed this parse between the two calls above");
    return st;
}

lxb_status_t html_parse_document_write(lxb_html_document_t *document, const lxb_char_t *text, size_t size)
{
    lxb_status_t st;

    DCHECK(document != NULL, "§8.4.3 steps 10-11 were reached with no document to write into");
    DCHECK(text != NULL,
           "§8.4.3 steps 10-11 were handed a NULL pointer — the standard's `string` is always a string, and an "
           "empty write is a zero SIZE over a real pointer exactly as an empty parse is");
    /* §8.4.3 STEP 9 IS WHAT ANSWERS AN UNDEFINED INSERTION POINT, so reaching step 10 without a live stream is a
       caller that skipped it — or a HOST that never opened one. Both are named here because the reader of this
       crash cannot tell them apart from where they stand:
         - the caller half is core/html/document_write.c, which runs step 9 before this;
         - the HOST half is that a document whose parse ran to §13.2.7 "The end" before its scripts ever ran has
           no insertion point for any of them, and every `document.write` on that document lands here. The fix
           is that the ACTIVE document's parse is opened with html_parse_document_open and closed at §13.2.7's
           own moment — the lifecycle stage that moves the readiness to "interactive" — instead of being
           completed by html_parse_document before the first flow is seeded. That change has TWO hazards that
           must be answered with it, and the second is the bigger one:
             (a) lexbor emits the EOF token in `chunk_end`, and §13.2.6 builds `html`/`head`/`body` from that
                 token when the source produced none, so a ZERO-LENGTH response left open has no document
                 element until the close and every walk of the tree in between sees an empty document. That is
                 what a browser does too — a still-parsing empty document has a null `documentElement` — so
                 what has to change is each reader that assumes otherwise, not this.
             (b) THE TREE CONSTRUCTION THIS CALL DRIVES BYPASSES THE PER-FLOW DOM DELTA. solver/dom_cow.h owns
                 the one place a tree write is captured and it is a convention over the BROWSER components;
                 lexbor's §13.2.6 inserts through its own mutators, which nothing captures. Every other parse
                 in this engine is out-of-tree and its result is PLACED through the chokepoint, which is what
                 keeps the delta honest; a live parser feeding the document's own tree has no placement step,
                 so its nodes are shared-baseline writes belonging to no flow — one flow's `document.write`
                 visible to every sibling and unapplied on every context switch. That is the mechanism this
                 entry is waiting on. */
    DCHECK(html_parse_insertion_point_defined(lxb_dom_interface_document(document)),
           "§8.4.3 \"document.write()\" step 10 was reached on a document whose §13.2.3.5 insertion point is "
           "undefined — see the paragraph above this assert for the two ways that happens and for what the "
           "host has to do about the second one");
    st = lxb_html_document_parse_chunk(document, text, size);
    /* A `CHECK` AND NOT A `DCHECK`, for html_fire's reason one layer up: §13.2 tokenization and tree
       construction are error-RECOVERING and define no input they reject, so a non-OK status here is the
       allocation floor — and a markup sink that silently dropped its markup in release would be a document
       this engine reports on having never built. */
    CHECK(st == LXB_STATUS_OK,
          "html-parse-oom: the markup a document wrote into its own input stream could not be tokenized — "
          "§13.2 rejects no input, so this is memory, and continuing would analyse a document the page's own "
          "`document.write` was supposed to have built");
    return st;
}

lxb_status_t html_parse_document_close(lxb_html_document_t *document)
{
    DCHECK(document != NULL, "§13.2.7 \"The end\" was reached with no document");
    DCHECK(html_parse_insertion_point_defined(lxb_dom_interface_document(document)),
           "§13.2.7 \"The end\" was reached for a document whose input stream is not open — the close is the "
           "ONE moment the insertion point becomes undefined, so a second one would hand lexbor a parser in "
           "the END stage, which answers LXB_STATUS_ERROR_WRONG_STAGE and emits no EOF token at all");
    return lxb_html_document_parse_chunk_end(document);
}

lxb_status_t html_parse_document(lxb_html_document_t *document, const lxb_char_t *html, size_t size)
{
    lxb_status_t st = html_parse_document_open(document, html, size);

    /* THE FAILURE PATH DOES NOT CLOSE, which is `lxb_html_document_parse`'s own `goto failed` — a parse that
       did not begin has no EOF token to emit and no stage `chunk_end` would accept. */
    if (st != LXB_STATUS_OK)
        return st;
    return html_parse_document_close(document);
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
