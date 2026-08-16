/* THE ONE PLACE A DOCUMENT IS PARSED — and it exists because A TOKEN'S ATTRIBUTE VALUES HAVE NO OWNER.
 *
 * WHERE THE BYTES COME FROM. `lxb_html_parse_chunk_prepare` and `lxb_html_parse_fragment_chunk_begin` both run
 * `lxb_html_tokenizer_attrs_mraw_set(parser->tkz, doc->text)`, and `lxb_html_tokenizer_state_set_value_m` then
 * allocates every attribute VALUE the tokenizer reads with `lexbor_mraw_alloc(tkz->attrs_mraw, size + 1)`. So a
 * token's attribute values are allocated out of the TEXT arena — which core/dom/node_heap.h has made the
 * AGENT'S, shared by every document in this instance and destroyed with the last of them.
 *
 * NOTHING FREES THEM. `lxb_html_token_attr_t` has no destructor for its `value`: `lxb_html_token_attr_clean`
 * memsets the struct and `lxb_html_token_attr_destroy` hands it back to the tokenizer's `lexbor_dobject_t`
 * pool, both dropping the pointer. The field is sound only because TREE CONSTRUCTION ADOPTS it —
 * `lxb_html_tree_append_attributes` calls `lxb_dom_attr_set_value_wo_copy(attr, token_attr->value,
 * token_attr->value_size)`, which stores the token's own allocation as the DOM attribute's `value->data` and
 * makes the Attr node its owner. Every attribute value that reaches an element is therefore accounted for by
 * the node that took it, and upstream never has to think about the rest: it destroys both arenas together at
 * document teardown and asks neither for a count.
 *
 * §4.6'S DOCTYPE IS THE ONE CONSUMER THAT COPIES INSTEAD. `lxb_html_token_doctype_parse` takes
 * `doc_type->node.owner_document->mraw` and runs `lexbor_str_init` + `lexbor_str_append` for the public and
 * system ids — a SECOND allocation, in the NODE arena, holding the same bytes — and returns without touching
 * `attr->value`. Its only caller is `lxb_html_tree_create_document_type_from_token`, reached from exactly one
 * insertion mode (§13.2.6.4.1 "initial"); every other mode ignores a DOCTYPE token outright. So for a DOCTYPE
 * token the adoption question has one answer and it is NONE, on every path, including the ones that ignore the
 * token and the one that fails.
 *
 * WHICH MAKES IT A LEAK HERE AND NOT UPSTREAM, for node_heap.h's reason: the arena outlives the document, so
 * the bytes are not dropped with it. Measured on real Chrome by driving `qjs_teardown` through the renderer
 * frame at d8901b3b: `<!doctype html>` left the text arena at 0, `<!DOCTYPE html SYSTEM "about:legacy-compat">`
 * at +1 and a PUBLIC + SYSTEM doctype at +2 — one allocation per doctype id that carries a value, exactly the
 * ids `lxb_html_token_doctype_parse` copied.
 *
 * SO THE TOKEN GETS AN OWNER, AND THE ONLY MOMENT IT CAN IS THE TOKEN-DONE CALLBACK. After
 * `lxb_html_tokenizer_state_token_done_m` returns from that callback the token is cleaned and its attributes go
 * back to the pool, so the pointer is unreachable from anything this repo can see — and it is unreachable
 * before it, too, because the doctype node holds a COPY at a different address and nothing else names the
 * original. The callback is the last instant the value is reachable and the first instant tree construction has
 * finished with it, which is why the ownership lives here rather than at a post-parse sweep.
 *
 * WHAT THIS COMPONENT DOES NOT DO, stated because the file would otherwise read as if it covered them. It
 * releases the values of a DOCTYPE token only. It cannot release an ELEMENT token's, because "was this one
 * adopted?" has no token-local answer: `lxb_html_tree_append_attributes` skips a token attribute whose name the
 * element already carries, so a DUPLICATE attribute (`<div a=1 a=2>`, kept in the token because
 * `lxb_html_parser_init` sets LXB_HTML_TOKENIZER_OPT_ATTR_KEEP_DUPLICATE) and §13.2.6.4.7's re-attribution of
 * `<html>`/`<body>` attributes onto an element that already exists both leave a value behind, and telling them
 * from the adopted ones needs the element the tree built, which the callback is not handed. That leak is REAL
 * and it is what core/dom/node_heap.c's teardown assertion names as the text arena's remaining arm; closing it
 * needs lexbor to report which attributes it took, which is a capability this tree does not have.
 *
 * AND THE PARSER IS MADE HERE FOR THE SAME REASON A DOCUMENT IS MADE IN core/dom/node_interface.h: a property
 * every parse must have is not a thing you install where you remember to. `lxb_html_document_parse` CREATES the
 * parser it uses (`lxb_html_document_parser_prepare`, which is static), so a caller that reaches lexbor's entry
 * directly gets a tokenizer this component never saw — which is why `dom_document_destroy` asserts
 * `html_parse_owns_tokens_of` rather than trusting a list of call sites. */
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

#endif
