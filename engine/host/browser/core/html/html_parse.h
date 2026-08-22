/* THE ONE PLACE A DOCUMENT IS PARSED — and it exists because A TOKEN'S ATTRIBUTE VALUES HAVE NO OWNER.
 *
 * WHERE THE BYTES COME FROM. `lxb_html_parse_chunk_prepare` and `lxb_html_parse_fragment_chunk_begin` both run
 * `lxb_html_tokenizer_attrs_mraw_set(parser->tkz, doc->text)`, and `lxb_html_tokenizer_state_set_value_m` then
 * allocates every attribute VALUE the tokenizer reads with `lexbor_mraw_alloc(tkz->attrs_mraw, attr->value_size
 * + 1)`. So a token's attribute values are allocated out of the TEXT arena — which core/dom/node_heap.h has
 * made the AGENT'S, shared by every document in this instance and destroyed with the last of them.
 *
 * NOTHING FREES THEM. `lxb_html_token_attr_t` has no destructor for its `value`: `lxb_html_token_attr_clean`
 * memsets the struct and `lxb_html_token_attr_destroy` hands it back to the tokenizer's `lexbor_dobject_t`
 * pool, both dropping the pointer. The field is sound only because TREE CONSTRUCTION ADOPTS it —
 * `lxb_html_tree_append_attributes` calls `lxb_dom_attr_set_value_wo_copy(attr, token_attr->value,
 * token_attr->value_size)`, which stores the token's own allocation as the DOM attribute's `value->data` and
 * makes the Attr node its owner (`lxb_dom_attr_interface_destroy` frees exactly that pointer into `doc->text`).
 * Upstream never has to think about the rest: it destroys both arenas together at document teardown and asks
 * neither for a count.
 *
 * BUT ADOPTION IS NOT WHAT HAPPENS TO EVERY ATTRIBUTE, and the three ways it does not are ordinary markup, not
 * corner cases. `lxb_html_tree_append_attributes` SKIPS a token attribute whose name the element already
 * carries, so `<div a=1 a=2>` strands the second value (the duplicate survives into the token because
 * `lxb_html_parser_init` sets LXB_HTML_TOKENIZER_OPT_ATTR_KEEP_DUPLICATE, so lexbor's own
 * `lxb_html_tokenizer_attr_last_duplicate` never runs — and it would have stranded the same bytes anyway, since
 * it destroys the token attribute and drops the pointer). §13.2.6.4.7's re-attribution of `<html>` and `<body>`
 * attributes merges a token onto an element that already exists, so every name that element already had is
 * stranded. And a token an insertion mode IGNORES — including §4.6's DOCTYPE on every path, whose one consumer
 * `lxb_html_token_doctype_parse` COPIES into `owner_document->mraw` with `lexbor_str_init` + `lexbor_str_append`
 * rather than adopting — strands all of them.
 *
 * WHICH MAKES IT A LEAK HERE AND NOT UPSTREAM, for node_heap.h's reason: the arena outlives the document, so
 * the bytes are not dropped with it. Measured on real Chrome by driving `qjs_teardown` through the renderer
 * frame at d8901b3b: `<!doctype html>` left the text arena at 0, `<!DOCTYPE html SYSTEM "about:legacy-compat">`
 * at +1 and a PUBLIC + SYSTEM doctype at +2 — one allocation per doctype id that carries a value.
 *
 * SO THE TOKEN GETS AN OWNER, AND THE ONLY MOMENT IT CAN IS THE TOKEN-DONE CALLBACK. After
 * `lxb_html_tokenizer_state_token_done_m` returns from that callback the token is cleaned and its attributes go
 * back to the pool, so the pointer is unreachable from anything this repo can see; before it, tree construction
 * has not run and there is nothing to release yet. The callback is the last instant the value is reachable and
 * the first instant tree construction has finished with it — `lxb_html_tree_token_callback` runs
 * `lxb_html_tree_insertion_mode`, which loops the construction dispatcher until the token is CONSUMED, so
 * reprocessing across insertion modes has already happened by the time the wrapper regains control.
 *
 * AND "WAS THIS ONE ADOPTED?" IS ANSWERED BY WATCHING THE ADOPTION, NOT BY GUESSING FROM THE TOKEN. An earlier
 * version of this file released a DOCTYPE token's values only, and said the rest needed "the element the tree
 * built, which the callback is not handed". The element is the wrong thing to want. Every adoption is
 * `lxb_dom_attr_set_value_wo_copy` at html/tree.c's ONE call site, and it is followed — after the MathML/SVG
 * `before_append_attr` rename, which never touches `value` — by `lxb_dom_element_attr_append`, which calls
 * `doc->node_cb->insert` on the Attr with the token's pointer already in `attr->value->data`. That callback is
 * lexbor's own per-document extension point (`lxb_dom_document_node_cb_t`, default all-NULL), so this component
 * installs one and CLAIMS the matching token attribute there: the DOM has taken these bytes, so the token's
 * field is cleared. What is left over at token-done is therefore EXACTLY what nothing owns, for every insertion
 * mode, every ignored token, every void element popped in the same step, every foster-parented element, every
 * foreign-content rename and every fragment parse — with no element identified and no skip rule replicated.
 *
 * THE CLAIM IS BY POINTER IDENTITY, AND THAT IS WHY IT IS NOT A NAME TEST. The tempting cheap answer — walk
 * the token and call an attribute stranded when an earlier one carried the same name — is WRONG, because the
 * skip is `lxb_dom_element_attr_by_local_name_data`, which matches the ELEMENT's `local_name` OR its
 * `qualified_name`, and `before_append_attr` rewrites both between the search and the append.
 * `<svg xlink:href="a" href="b">` is the case: the two token attributes have DIFFERENT names, so a name test
 * says both were taken, while `lxb_html_tree_adjust_foreign_attributes` has already turned the first Attr's
 * local name into `href` (qualified name `xlink:href`, namespace XLink), so lexbor's search for `href` finds it
 * and `"b"` is stranded with nothing naming it. Replicating that means replicating a two-key lookup and three
 * rename tables; the adopted POINTER is the answer lexbor already computed.
 *
 * THE SCAN IS OVER THE TOKEN'S OWN ATTRIBUTE LIST and is quadratic in one element's attribute count, which is
 * stated rather than optimised away: `lxb_html_tree_append_attributes` already runs
 * `lxb_dom_element_attr_by_local_name_data` — a walk of the element's whole list — once per token attribute, so
 * the shape of this cost is lexbor's and this adds a constant to it. A cursor that only moved forward would be
 * exact only while `append_attributes` runs at most once per token, which is a claim about insertion modes
 * rather than about ownership, and ownership is what this file is allowed to know.
 *
 * AND THE PARSER IS MADE HERE FOR THE SAME REASON A DOCUMENT IS MADE IN core/dom/node_interface.h: a property
 * every parse must have is not a thing you install where you remember to. `lxb_html_document_parse` CREATES the
 * parser it uses (`lxb_html_document_parser_prepare`, which is static), so a caller that reaches lexbor's entry
 * directly gets a tokenizer this component never saw — which is why `dom_document_destroy` asserts
 * `html_parse_owns_tokens_of` rather than trusting a list of call sites. The node callback's half of that is
 * asserted from the other end: the token-done wrapper checks the document it is building into carries it, on
 * every token, because a document that does not would have its adoptions go unseen and its LIVE attribute
 * values freed. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_PARSE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_PARSE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

/* THE ONE PLACE AN HTML PARSER IS MADE — `lxb_html_parser_create` + `lxb_html_parser_init` with this
   component's token ownership installed on the tokenizer, and nothing else. NULL comes back exactly as lexbor
   returned it, because each caller's OOM sentence is its own measurement (the same rule `dom_document_create`
   states). Its OTHER caller is HTML §13.4's fragment parse in core/dom/element.c, which builds a parser per
   parse so a suspended parse cannot share a tokenizer with a sibling flow's. */
lxb_html_parser_t *html_parse_new_parser(void);

/* THE ONE PLACE A DOCUMENT IS PARSED. `lxb_html_document_parse` over `document`, with the parser created here
   first so the tokenizer is owned before its first token — after that lexbor's entry finds `doc->parser`
   already set and reuses it, which is the same object it would have made itself. */
lxb_status_t html_parse_document(lxb_html_document_t *document, const lxb_char_t *html, size_t size);

/* Whether every token `doc`'s parses produced went through this component. TRUE for a document with no parser
   at all, which is a POSITIVE statement rather than a hole: lexbor's entry creates the parser it uses, so a
   document that has one this component did not install on was parsed by a call this engine does not own. */
bool html_parse_owns_tokens_of(const lxb_dom_document_t *doc);

/* THE OTHER HALF OF THE OWNERSHIP — install the node callback that watches adoption. Called from
   `dom_document_create` on the document `dom_document_of_nodes` resolves, because that is the one the DOM
   READS it off: `lxb_dom_element_attr_append` takes `lxb_dom_interface_node(element)->owner_document`, and
   `lxb_dom_document_init` stamps every node with `lxb_dom_document_owner(document)` while resetting the
   INHERITED document's own `node_cb` to lexbor's all-NULL default. That reset is why §13.4's temporary
   fragment document cannot carry this itself and does not have to: its elements and its Attrs are stamped with
   the REAL document, so the callback the append finds is the real one's.
   It refuses to overwrite a callback table that is neither lexbor's default nor this one, because a second
   component wanting `insert` needs the two composed rather than one of them silently dropped. */
void html_parse_own_token_values(lxb_dom_document_t *doc);

/* Whether `doc` carries that callback — asked by the token-done wrapper about the document being built into,
   on every token. A document without it sees no adoptions, so every value tree construction took would be
   released underneath a live Attr; this is the assertion that stands between that and a silent
   use-after-free. */
bool html_parse_owns_token_values_of(const lxb_dom_document_t *doc);

#endif
