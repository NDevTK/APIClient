/* HTML §4.2.4 "The link element" — ITS PROCESSING MODEL, which is the half `<link>` did not have.
 *
 * WHAT WAS HERE BEFORE: nothing. `<link>` had an interface — core/html/html_element.c's `R_LINK` reflects
 * `href`/`rel`/`type`/`media`/`as`/`crossorigin`/`integrity`/`referrerpolicy`/`disabled`, and `relList` and
 * `sizes` are installed beside them — and no algorithm anywhere ran for one. So an element that IS an external
 * resource link fetched nothing, and, decisively, FIRED NOTHING: with no processing model there is no
 * §4.2.4.3 "Fetching and processing a resource from a link element", so there is no response, so there is no
 * `load` event, so a `link.onload` handler is a listener on an event this engine could not produce.
 *
 * WHY THAT IS AN @H DEFECT AND NOT A RENDERING ONE. The dominant chunk-loading idiom of every modern bundler
 * is a `<link rel=preload as=script>` whose `load` handler INJECTS THE `<script src>`:
 *
 *     const l = document.createElement("link");
 *     l.rel = "preload"; l.as = "script"; l.href = chunkUrl;
 *     l.onload = () => { const s = document.createElement("script"); s.src = chunkUrl; head.appendChild(s); };
 *     head.appendChild(l);
 *
 * With no `load`, the `<script>` is never created, so `core/html/html_script.c` never prepares it, so the chunk
 * is never fetched, never executed, and its endpoints never learned. CLAUDE.md §What-the-tool-produces names
 * the lazy chunk as a headline target by name ("dead-but-shipped, lazy-chunk"), and §Attacker sources makes a
 * same-origin JavaScript body a thing that is "ALWAYS fetched + EXECUTED (a lazy chunk reveals real endpoints —
 * the headline moat surface)". One absent event was the whole of why that surface was unreachable: measured on
 * a live document whose only script is an inline bootstrap of exactly this shape, the engine emitted nine @H
 * records and every one of them was an `<img src>` from the static markup — not one address came from the
 * bundle's own execution.
 *
 * WHICH LINK TYPES THIS COMPONENT PROCESSES, AND WHY THAT IS ROUTING RATHER THAN AN ALLOWLIST. §4.2.4.3 states
 * the algorithms per TYPE — "Individual link types may provide their own fetch and process the linked resource
 * algorithm" — and §4.6.8 gives each type its OWN "appropriate times to fetch and process the linked resource".
 * A type whose times nothing registers is therefore not a case falling through to some other behavior; it has
 * no behavior anywhere, exactly as it had none before this file existed. What this file adds is §4.6.8.20 Link
 * type "preload", whose steps are a plain fetch and whose two events are the ones the idiom above hangs on.
 * `link_external_type_of` names the other states rather than folding them into one, because "no keyword here
 * creates an external resource link" and "an external resource link this engine has no steps for" are
 * different facts and a single value for both is the defect CLAUDE.md counts seven of, one layer up.
 *
 * THE TWO IT NAMES AND DOES NOT RUN, each for its own reason and neither of them a fallback:
 *   §4.6.8.12 Link type "modulepreload" is a GENUINELY DIFFERENT ALGORITHM — "Fetch a modulepreload module
 *   script graph given url, destination, settings object, options", which is core/loader/module_loader.c's
 *   graph and not a fetch — so it is not preload's slow path and must not be tracked as debt against it. It
 *   arrives with a caller in that component.
 *   §4.6.8.23 Link type "stylesheet" needs a CSS style sheet to APPLY the response to, and
 *   core/css/css_rule.h already records that there is none ("there is no `@import` fetch and no
 *   `<link rel=stylesheet>` sheet either"). Building the fetch without the consumer would be a response nothing
 *   reads, which is the mirror of the read-with-no-writer defect and is what §NO STUBS forbids.
 *
 * THE PRELOAD CACHE IS NOT BUILT, AND THAT IS THE SAME RULE APPLIED AGAIN. §4.6.8.20 commits a preload entry
 * into the Document's "map of preloaded resources", whose only reader is "consume a preloaded resource" — an
 * algorithm Fetch runs when a LATER request matches a preload key. This engine has no such consumer, so the map
 * would be a producer with no reader; core/html/html_image.c states the same rule at the same kind of site for
 * Fetch's `initiator` ("A field a producer writes and nothing reads is the mirror of the defect CLAUDE.md
 * counts seven of, so it arrives with its first consumer"). The observable of not having it is that the
 * injected `<script src>` is fetched a second time rather than served from the preload — which is what a real
 * browser does on a preload miss, and is a modelled behavior rather than an absent one.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_LINK_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_LINK_H

#include "quickjs.h"
#include <lexbor/dom/interfaces/element.h>

/* Declared once per AGENT (the queued element task's step machine and this file's state key), then reached by
   the hooks below. Mirrors core/html/html_image.c's split for the same reason: a Symbol minted twice would
   leave every element carrying its state under the first key answering as though it had none under the
   second. */
void html_link_declare(JSContext *ctx);
void html_link_free(JSRuntime *rt);

/* §4.6.8.20's "The appropriate times to fetch and process the linked resource for such a link are: … When the
   external resource link's link element becomes browsing-context connected." An HTML ELEMENT INSERTION STEPS
   entry, so it needs the INSERTED node's document realm and not the mutating one — the same seam and the same
   position core/dom/element.c gives `<script>`, `<iframe>`, `<source>` and `<img>`. Asks for itself whether
   `el` is a `link`, so the drain states no brand it would have to keep in step with this file. */
void html_link_inserted(JSContext *ctx, lxb_dom_element_t *el);

/* The other three of §4.6.8.20's appropriate times, as one of §4.9's attribute change steps — the chokepoint
   every spelling of a write reaches (`l.href = u`, `setAttribute`, `attributes.href.value = u`), which is why
   it is here and not inside the reflection's setter:
     "When the external resource link is CREATED on a link element that is already browsing-context connected"
        — the `rel` attribute gaining the keyword;
     "When the href attribute … is changed";
     "When the as attribute … is changed";
   and the two conditional ones, "when the type attribute … but was PREVIOUSLY NOT OBTAINED due to the type
   attribute specifying an unsupported type for the request destination, is set, removed, or changed" and the
   same sentence for `media`. Those two carry a condition about the PAST, and this component answers it with one
   boolean rather than with the three reasons the prose distinguishes — see the derivation at `link_obtained`. */
void html_link_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);

/* The PARSER's `<link>` elements, INVENTORIED. A parser-inserted element reaches no mutation chokepoint
   (core/dom's drain runs for script-driven insertion), so the tree the parse produced is walked once, exactly
   as core/html/html_image.c's `html_image_parsed` and core/html/media_element.c's walk are — but this walk
   RECORDS the document instead of triggering, and the pair of entries is one algorithm rather than two.
   WHY THE SPLIT EXISTS, and it is a property of §4.2.4.3 rather than of this component. Every other parsed-tree
   walk either performs a DOM-only step or queues the SPEC'S OWN microtask before it fetches (§4.8.4.3.5
   "Updating the image data" is the worked example, and §scheduler makes every enqueued job a flow). §4.2.4.3's
   default fetch and process the linked resource has no such step: it ends in "Fetch request with
   processResponseConsumeBody set to …", and this engine's fetch parks on the flow's own pending register. The
   document a session opens over is installed at the pre-boot BASELINE, where there is no flow — so triggering
   here aborted the engine on a `<link rel=preload>` in the head, which is what every modern bundler emits.
   Queueing a task HERE would invent a step §4.2.4.3 does not have and reorder the request against the rest of
   the parse, so what moves is the MOMENT the trigger is performed and never the order of its steps. */
void html_link_parsed(JSContext *ctx, lxb_dom_node_t *root);

/* …AND THE FLOW THAT SERVES THEM. Runs §4.6.8.20's browsing-context-connected time for ONE inventoried element
   and returns 1, or 0 when every document this timeline can see has been drained. Registered with the
   scheduler as a work item on the ONE frontier (solver/engine.h's engine_set_link_connected_hook), asked
   AHEAD of the document's own programs because in a browser the parser connects a `<link>` during tree
   construction and every script of the document is written after it. It is declared here as well because a
   host that drives its own pump owes the same step; a host that runs none of it has documents whose own
   preloads are never requested, which is a lazy-chunk graph that never loads. */
int html_link_connected_step(JSContext *ctx);

#endif
