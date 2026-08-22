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
 * FALSE *for `already started`*, which is §4.12.1's own initial value for that flag, so the reader never
 * allocates a wrapper to learn a default — an element the parser never marked is one nothing has written, and
 * node_wrap_peek answers for it without minting anything. The other flag's initial value is the other one, and
 * its reader answers absent accordingly; see below.
 *
 * WHAT IS NOT HERE. `parser document` (and with it `parser-inserted`) is the OTHER half of §13.2.6.4.4's stamp,
 * and it has no reader in this engine: its readers are the script HTML element post-connection steps' step 1
 * ("if insertedNode is parser-inserted, then return") and §13.2.6.4.8's `</script>` handling, and both belong to
 * a DOCUMENT parser that prepares its own scripts — which this engine does not have, because its document parse
 * completes before any script runs and its scripts are collected afterwards by core/loader/document_scripts.c.
 * A flag written by nobody and read by nobody is a stub.
 *
 * `force async` IS HERE NOW, AND IT IS THE SECOND FLAG BECAUSE IT GAINED THE TWO READERS THE OTHER STILL LACKS.
 * §4.12.1.1: "A script element has a force async boolean, INITIALLY TRUE. It is set to false by the HTML parser
 * and the XML parser on script elements they insert, and when the element gets an async content attribute
 * added." Its readers are §4.12.1's destination branch — "if el has an async attribute or el's force async is
 * true", the test that puts an element in the `set of scripts that will execute as soon as possible` rather than
 * in the `list of scripts that will execute in order as soon as possible` — and the `async` IDL getter, whose
 * step 1 is "if this's force async is true, then return true". Without it `s = createElement('script'); s.async
 * = false; s.src = u` was an UNORDERED script: the setter is the whole of how a page asks for in-order lazy
 * loading, and it wrote nothing this engine read, so the two chunks a bundler emits in a fixed order ran in
 * whichever order the network answered. The flag's initial value is TRUE, so absence cannot mean false the way
 * `already started`'s does — an element nothing has written is one whose force async is true. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_SCRIPT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_SCRIPT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* The slot key. Once per runtime, beside the other per-element slot declarations, and released with them —
   the key is a Symbol and its atom is an interned reference, so an agent torn down without this leaves both
   for the runtime's own leak walk to count. */
void html_script_init(JSContext *ctx);
void html_script_free(JSRuntime *rt);

/* WHAT A PARSER STAMPS ON THE `script` ELEMENTS IT INSERTS, applied to the tree the parse produced. Two stamps
   and they are not the same population, which is why one walk takes the mode rather than two walking the tree:
     ALWAYS — §4.12.1.1's `force async` is "set to false by the HTML parser and the XML parser on script
   elements they insert". Every parse, document and fragment alike, and it is what makes a parsed `<script>`
   with no `async` attribute answer `async` FALSE — without it the boolean's initial true would answer true for
   every element in the page's markup.
     `inert` — §13.2.6.4.4's `script` start tag under §13.2.4.5's INERT scripting mode, which §13.4 makes the
   default of every FRAGMENT parse: "set the script element's already started to true (fragment case)". A
   DOCUMENT parse's scripts run, so it passes false.
   IT RUNS AT THE PARSE BOUNDARY rather than at each start tag because the tree builder is lexbor's and not
   this engine's — the same boundary, and for the same reason, as dom_attr_normalize_parsed's namespace
   correction, which is the statement immediately before it. That substitution is unobservable and is so
   because of something this engine asserts elsewhere: a parse runs NO page code, so nothing can look at a
   `script` element between the start tag that created it and the end of the parse that produced it. */
void html_script_parsed(JSContext *ctx, lxb_dom_node_t *root, bool inert);

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

/* HTML §4.12.1's `async` IDL attribute — the getter that reads `force async` and the setter that CLEARS it.
   It is not a [Reflect]ed boolean and was declared as one, which got both directions wrong at once: the getter
   answered the attribute's presence where the spec answers `force async || attribute present`, so a freshly
   created element read `false` while its force async was true; and the setter merely removed an absent
   attribute, so `s.async = false` — the one line whose entire purpose is to move the element into the ordered
   ASAP list — changed nothing at all. Handed the prototype by core/html/html_element.c for the same reason
   §4.2.6's `disabled` is: that file owns the table of which interface a tag wears, this one owns the state the
   member answers from. */
void html_script_install(JSContext *ctx, JSValueConst proto);

/* §4.12.1's "when an async attribute is added to a script element el, the user agent must set el's force async
   to false", as one of §4.9's ATTRIBUTE CHANGE STEPS — registered on core/dom/element.c's element_attr_changed
   beside media_element_attr_changed, and there rather than in the `async` setter for the same reason: `s.async =
   true`, `s.setAttribute('async','')` and `s.attributes.async.value = ''` are one write of one attribute, and a
   setter-side call answers for the first spelling only. `val` is the operation's input, because "ADDED" is a
   rule about the change: removing the attribute does not set the flag back. */
void html_script_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                              const char *val);

#endif
