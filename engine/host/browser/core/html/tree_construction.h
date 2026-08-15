/* HTML §13.2.4's PARSE STATE, MID-PARSE, COPIED — the tree-construction half of forking a flow that is parked
 * inside §13.4's fragment parsing algorithm. See tree_construction.c for what each field of the copy is.
 *
 * THE OTHER HALF IS §13.2.5's TOKENIZER, and the two are named together on purpose: element.c's
 * frag_unforkable is the one sentence that owns the ORDER, and it names this half first because this half needs
 * nothing lexbor does not already expose. A caller reaches this component with a tokenizer already copied — the
 * `tkz` argument — because lxb_html_tree_init BINDS the two (it takes the tokenizer's reference and points its
 * token-done callback at the tree), which is exactly why frag_unforkable says the two halves cannot be cloned
 * separately. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_TREE_CONSTRUCTION_H
#define ENGINE_HOST_BROWSER_CORE_HTML_TREE_CONSTRUCTION_H
#include <lexbor/html/html.h>

/* ONE POINTER INTO THE PARSE'S INPUT, MOVED ONTO THE COPY'S — supplied by §13.2.5's half, which owns the
 * incoming buffer and has to move its own cursors by exactly the same rule.
 *
 * IT IS A PARAMETER AND NOT A COMMENT because the tree holds such pointers and they are not the tree's to
 * interpret: every lxb_html_tree_error_t records the token EXTENT that raised it (`begin`/`end`, straight off
 * lxb_html_token_t), and those name bytes in the tokenizer's incoming buffer. A copy that took them verbatim
 * would hand the sibling flow two pointers into the ORIGINAL arm's input, which is a dangling read the moment
 * that arm's parse ends and which nothing would ever report. Mandatory (asserted), so this half cannot be
 * called by a caller that has not built the other one. */
typedef const lxb_char_t *(*HtmlInputRebase)(const lxb_char_t *p, void *ctx);

/* WHAT A COPY IS: a tree builder, the temporary document it builds into, and the two node-valued pieces of
 * §13.4 that belong to the PARSER rather than to the tree builder. All four are handed back together because
 * they are one object graph — every node pointer in `tree` already names a node of `document`'s tree, and the
 * caller assembles them into its lxb_html_parser_t (`parser->tree`, `parser->root`, `parser->form`). */
typedef struct {
    lxb_html_tree_t     *tree;       /* §13.2.4's parse state; every node pointer names the copy */
    lxb_html_document_t *document;   /* §13.4's temporary document — the copy's OWN, destroyed with its parse */
    lxb_dom_node_t      *root;       /* §13.4 step 8's root element, the copy's */
    lxb_dom_node_t      *form;       /* §13.4 step 5's synthetic form, or NULL when the source parse had none */
} HtmlTreeConstructionCopy;

/* Copy `src` — an lxb_html_tree_t standing mid-fragment-parse — and the partial tree it is standing in.
 *
 * `src_root` and `src_form` are the parser's own two node pointers into that tree (lxb_html_parser_t's `root`
 * and `form`); they are ARGUMENTS rather than fields read back off the tree because they belong to the
 * OPERATION's caller, and one of them (`form`) is unreachable from the tree the moment a `</form>` clears
 * `tree->form`. `src_form` is NULL for every context element that is not a `<form>`.
 *
 * `tkz` is the COPY's tokenizer — §13.2.5's half — and not the source's: lxb_html_tree_init points a
 * tokenizer's token-done callback at the tree it is initialising, so passing the source's would redirect the
 * ORIGINAL arm's tokens into the sibling's tree builder.
 *
 * Fatal on allocation failure (CHECK): a fork that half-copies a parse is two flows sharing one open-elements
 * stack, which is the corruption this exists to prevent. */
void html_tree_construction_copy(const lxb_html_tree_t *src, lxb_dom_node_t *src_root, lxb_dom_node_t *src_form,
                                 lxb_html_tokenizer_t *tkz, HtmlInputRebase rebase, void *rebase_ctx,
                                 HtmlTreeConstructionCopy *out);

#endif
