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
 * `doc->attr_mutation->append` with the token's pointer already in `attr->value->data` and handed straight
 * through as the callback's `value`. That callback is lexbor's own per-document extension point
 * (`lxb_dom_document_attr_mutation_cb_t`, default all-NULL), so this component
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

/* ---- HTML §13.2.3.5 "Preprocessing the input stream" — THE INPUT STREAM AND ITS INSERTION POINT -------------
 *
 * §13.2.3.5: "The insertion point is the position (just before a character or just before the end of the input
 * stream) where content inserted using document.write() is actually inserted. … Initially, the insertion point
 * is undefined." So the insertion point is not a coordinate this engine invents — it is a STATE OF THE PARSER,
 * and lexbor already holds it: `lxb_html_parse_chunk_process` refuses a chunk unless the parser is in
 * LXB_HTML_PARSER_STATE_PROCESS, which is exactly the window between `chunk_prepare` and `chunk_end`. A parser
 * standing in that state with its source consumed has its insertion point just before the END of the input
 * stream, which §13.2.3.5's own definition names as one of the two positions an insertion point can hold.
 *
 * SO THE THREE ENTRIES BELOW ARE THE THREE MOMENTS THE STANDARD NAMES, and nothing else:
 *   - OPEN  is the parse with the stream still open. §13.2.6.4.8 'The "text" insertion mode' is why that state
 *     has to be reachable at all: at a `script` end tag it says "Let the insertion point be just before the
 *     next input character", runs the script, and then "Let the insertion point have the value of the old
 *     insertion point" — so the insertion point is DEFINED for the whole of a parser-inserted script's
 *     execution, and §8.4.3's step 9 test turns on it.
 *   - WRITE is §8.4.3 "document.write()" step 11's "have the HTML parser process string, one code point at a
 *     time, processing resulting tokens as they are emitted". It is the DOCUMENT's parser and not a fragment
 *     parse, which is the whole difference between this sink and §8.5.4's innerHTML: the tokens go through
 *     §13.2.6 tree construction in the mode the parse is standing in, so §13.2.6.4.4 'The "in head" insertion
 *     mode' PREPARES a written `script` element (its fragment-case "already started" clause does not apply)
 *     and the written script runs.
 *   - CLOSE is §13.2.7 "The end" step "Set the insertion point to undefined", which is also where lexbor emits
 *     the EOF token — so a document whose source produced no `html` element yet (an empty response) grows one
 *     HERE and not before.
 *
 * `html_parse_document` IS OPEN-THEN-CLOSE and not a second implementation: a caller that wants a COMPLETE
 * document — §8.5.1's DOMParser, §4.5.1's createHTMLDocument, XHR's responseXML, an @S fire oracle's scratch
 * parse — is asking for a Document the standard gives no live parser and no insertion point at all, which is
 * the state `document.write` reaches through §8.4.1's document open steps rather than through step 11.
 *
 * THE STEP NUMBERS ABOVE ARE THE ORDERED LIST'S OWN AND WERE OFF BY TWO WHEN THIS PARAGRAPH WAS FIRST WRITTEN
 * (step 7 for the insertion-point test, step 9 for the parser processing the string). §8.4.3's "document write
 * steps" list eleven items — 1 `string`, 2 `isTrusted`, 3 the append loop, 4 the Trusted Types compliant
 * string, 5 the line feed, 6 the XML throw, 7 the throw-on-dynamic-markup-insertion throw, 8 the aborted-parser
 * return, 9 the undefined-insertion-point arm, 10 the insert into the input stream, 11 the parser processing —
 * and a citation nobody can look up is one nobody can check, which is the whole value of citing at all. */

/* THE ONE PLACE A DOCUMENT IS PARSED, WITH ITS INPUT STREAM LEFT OPEN — the parser created here first so the
   tokenizer is owned before its first token, then lexbor's own chunk begin/process over `html`. On return the
   whole of `html` has been tokenized and the insertion point is just before the end of the input stream. */
lxb_status_t html_parse_document_open(lxb_html_document_t *document, const lxb_char_t *html, size_t size);

/* §13.2.3.5's QUESTION, asked of a real parser: is `doc`'s insertion point DEFINED. False for a document with
   no parser at all and for one whose parse has reached §13.2.7 "The end" — both of which are the standard's
   "undefined", not a hole. */
bool html_parse_insertion_point_defined(const lxb_dom_document_t *doc);

/* §8.4.3 "document.write()" steps 10 AND 11 — insert `text` into the input stream just before the insertion
   point, and have the HTML parser process it. They are ONE entry because in lexbor they are one call: the
   input stream is not a buffer this engine holds, it is the tokenizer's own cursor, so "insert just before the
   insertion point" of a parser standing at the end of its stream IS handing the bytes to `chunk_process`.
   CRASHES on a document whose insertion point is undefined: step 9 is what answers that case (§8.4.1's
   document open steps), so reaching here without a live stream means the caller skipped it.
 *
 * AND IT WRITES INTO THE SHARED TREE WITH NO COW CAPTURE, WHICH IS WHY IT HAS NO CALLER YET. §13.2.6 tree
 * construction inserts through lexbor's own mutators, and solver/dom_cow.h is a CONVENTION over the browser
 * components rather than a hook inside Lexbor — every other parse in this engine is out-of-tree (a fragment,
 * a scratch document) and its RESULT is then placed through the chokepoint, which is what keeps the delta
 * honest. A live parser feeding the document's own tree has no such placement step, so the nodes it builds
 * would be shared-baseline writes no flow's delta captured: one flow's `document.write` visible to every
 * sibling and unapplied by none. That is the mechanism `document.write` is waiting on, and it is named at the
 * assert below rather than here alone. */
lxb_status_t html_parse_document_write(lxb_html_document_t *document, const lxb_char_t *text, size_t size);

/* §13.2.7 "The end" — the EOF token, and the insertion point becomes undefined. Crashes on a document whose
   stream is not open, because closing twice would hand lexbor a parser in the wrong stage and the second call
   would silently do nothing. */
lxb_status_t html_parse_document_close(lxb_html_document_t *document);

/* THE COMPLETE PARSE — open then close, in one call. What every caller that wants a finished Document uses. */
lxb_status_t html_parse_document(lxb_html_document_t *document, const lxb_char_t *html, size_t size);

/* Whether every token `doc`'s parses produced went through this component. TRUE for a document with no parser
   at all, which is a POSITIVE statement rather than a hole: lexbor's entry creates the parser it uses, so a
   document that has one this component did not install on was parsed by a call this engine does not own. */
bool html_parse_owns_tokens_of(const lxb_dom_document_t *doc);

/* THE OTHER HALF OF THE OWNERSHIP — install the attribute callback that watches adoption. Called from
   `dom_document_create` on the document `dom_document_of_nodes` resolves, because that is the one the DOM
   READS it off: `lxb_dom_element_attr_append` takes `lxb_dom_interface_node(element)->owner_document`, and
   `lxb_dom_document_init` stamps every node of an OWNED document with the owner rather than with the document
   being built. That stamp is why §13.4's temporary fragment document does not have to carry this itself: its
   elements and its Attrs name the REAL document, so the table the append reads is the real one's, whatever the
   fragment document's own field says.
   AND IT NO LONGER SAYS "EMPTY". An owned document used to be handed lexbor's all-NULL default, because the
   default was assigned before the owner branch; it is now assigned INSIDE that branch from `owner->mutation`
   and `owner->attr_mutation`, so a fragment document INHERITS this component's table. That is a widening of
   what `html_parse_owns_token_values_of` answers true for, and it widens toward the truth — the documents it
   newly admits are exactly the ones whose appends were already reaching this callback through the owner stamp,
   so the predicate now agrees with the dispatch instead of understating it.
   `lxb_dom_document_set_default_node_cb` is NOT the way back to the default: it resets `mutation` and leaves
   `attr_mutation` — the half this component installs on — untouched.
   It refuses to overwrite a callback table that is neither lexbor's default nor this one, because a second
   component wanting `append` needs the two composed rather than one of them silently dropped. */
void html_parse_own_token_values(lxb_dom_document_t *doc);

/* Whether `doc` carries that callback — asked by the token-done wrapper about the document being built into,
   on every token. A document without it sees no adoptions, so every value tree construction took would be
   released underneath a live Attr; this is the assertion that stands between that and a silent
   use-after-free. */
bool html_parse_owns_token_values_of(const lxb_dom_document_t *doc);

#endif
