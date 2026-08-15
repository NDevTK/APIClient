/* THE `script` ELEMENT'S PARSE STATE AND HTML §4.12.1's "prepare the script element".
 *
 * WHY IT IS A COMPONENT AND NOT A STATIC IN element.c. §4.12.1 step 1 is "if el's already started is true,
 * then return", and `already started` is not a property of the element's markup — it is written by the PARSER
 * that built the element, read by the preparation that would run it, and copied by §4.12.1's cloning steps.
 * Three call sites in three files over one boolean is a component; a static beside one of them is the same
 * boolean answered from wherever the reader happened to be.
 *
 * WHAT THE FLAG DECIDES, AND WHAT IT COST NOT TO HAVE IT. HTML §13.2.4.5 gives an HTML parser a SCRIPTING MODE
 * — one of Normal, Disabled, Inert, Fragment — and §13.2.6.4.4's `script` start tag says of the third: "if the
 * parser's scripting mode is Inert, then set the script element's already started to true (fragment case)".
 * §13.2.4.5 states what that buys in one line: "Inert: Scripts are enabled, however they are marked as already
 * started, essentially preventing them from executing. This is the default mode of the HTML fragment parsing
 * algorithm." So Inert is not a mode this engine chooses — §13.4 makes it the default of every fragment parse,
 * and all five of this engine's markup members (innerHTML, outerHTML, insertAdjacentHTML, setHTML,
 * setHTMLUnsafe) are §13.4 with no scriptingMode argument.
 * With no flag there was nothing for §4.12.1 step 1 to return on, and the fragment machine's placement puts
 * every parsed node through dom_cow_append_child, which runs §4.2.3's insertion steps, which prepare an
 * inserted `<script>`. So `el.innerHTML = "<script>…</script>"` EXECUTED that script and `<script src=…>`
 * RECORDED an endpoint no browser would ever fetch — a fidelity bug in both halves at once: the browser half
 * ran code a browser does not run, and the solver half reported a request the page cannot make. element.c said
 * in prose that "markup parsed into innerHTML does not execute its scripts" while its own code did.
 *
 * WHERE THE FLAG LIVES. On the element's WRAPPER, as an own slot under a Symbol this file minted and never
 * published — the same store DOM §4.9's custom element state uses (core/html/custom_elements.c), and for the
 * same two reasons: nothing outside can reach a key the page cannot mint, and the write is an ordinary
 * property write, so the heap COW captures it and one flow's marked script is not another flow's. ABSENT MEANS
 * FALSE, which is §4.12.1's own initial value, so the reader never allocates a wrapper to learn a default —
 * an element the parser never marked is one nothing has written, and node_wrap_peek answers for it without
 * minting anything.
 *
 * WHAT IS NOT HERE. `parser document` (and with it `parser-inserted`, and §4.12.1's `force async`) is the
 * OTHER half of §13.2.6.4.4's stamp, and it has no reader in this engine yet: its readers are the script HTML
 * element post-connection steps' step 1 ("if insertedNode is parser-inserted, then return") and §13.2.6.4.8's
 * `</script>` handling, and both belong to a DOCUMENT parser that prepares its own scripts — which this engine
 * does not have, because its document parse completes before any script runs and its scripts are collected
 * afterwards by core/loader/document_scripts.c. A flag written by nobody and read by nobody is a stub; this
 * file holds exactly the one that all three of its call sites use. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_SCRIPT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_SCRIPT_H

#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* The slot key. Once per runtime, beside the other per-element slot declarations, and released with them —
   the key is a Symbol and its atom is an interned reference, so an agent torn down without this leaves both
   for the runtime's own leak walk to count. */
void html_script_init(JSContext *ctx);
void html_script_free(JSContext *ctx);

/* HTML §13.2.6.4.4's `script` start tag under §13.2.4.5's INERT scripting mode, applied to the tree a fragment
   parse produced: every `script` element in `root`'s subtree gets `already started` true.
   IT RUNS AT THE PARSE BOUNDARY rather than at each start tag because the tree builder is lexbor's and not
   this engine's — the same boundary, and for the same reason, as dom_attr_normalize_parsed's namespace
   correction, which is the statement immediately before it. That substitution is unobservable and is so
   because of something this engine asserts elsewhere: a fragment parse runs NO page code, so nothing can look
   at a `script` element between the start tag that created it and the end of the parse that produced it. */
void html_script_parsed_inert(JSContext *ctx, lxb_dom_node_t *root);

/* HTML §4.12.1 "prepare the script element", reached from DOM §4.2.3's insertion steps. `el` is any inserted
   element; one that is not a `script` returns having done nothing, because the caller is a walk over every
   node of an inserted subtree and the tag test is that walk's filter rather than a step of the algorithm. */
void html_script_prepare(JSContext *ctx, lxb_dom_element_t *el);

/* HTML §4.12.1's CLONING STEPS — "the cloning steps for script elements given node, copy, and subtree are to
   set copy's already started to node's already started" — run from DOM §4.4 clone a node's step 3, which is
   where every one of a clone's nodes passes.
   THEY ARE NOT BOOKKEEPING. Without them `parent.innerHTML = "<script>…</script>"` produces an inert script
   whose CLONE is a live one, so `doc.body.appendChild(parent.firstChild.cloneNode(true))` runs exactly the
   code the Inert mode exists to stop. A flag is only worth writing if it survives the operations the standard
   says it survives. `src` and `copy` may be any node kind; a pair that is not two `script` elements is a
   no-op, for the same reason the preparation above tolerates one. */
void html_script_cloned(JSContext *ctx, lxb_dom_node_t *src, lxb_dom_node_t *copy);

#endif
