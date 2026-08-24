/* THE `img` ELEMENT — HTML §4.8.3 "The img element" and §4.8.4.3 "Processing model".
 *
 * WHAT WAS MISSING AND WHAT EACH HALF COST. `img` was a row in core/html/html_element.c's element-interface
 * table with ten REFLECTIONS and nothing else, which is a description of the element's ATTRIBUTES and not of
 * the element: setting `src` mirrored a string into the tree and issued no request, so §4.8.4.3.5 "Updating
 * the image data" — the algorithm that turns a `src` into a fetch and then into `load` or `error` — had no
 * implementation at all. Two things followed and they are the two halves of this project.
 *   AN IMAGE REQUEST IS A REQUEST. A page that builds an image address out of its config or out of an API
 * reply is describing its own surface in exactly the way a `fetch()` does, and none of it reached the @H
 * endpoint surface, because the request was never made.
 *   `onerror` IS THE CANONICAL AUTO-FIRING SINK. CLAUDE.md §@S names it in as many words — "`innerHTML` does
 * not execute `<script>` yet DOES fire `onerror`/`onload`" — and an element that never fetches never fires
 * either, so every candidate whose firing vector is an image's error handler had nothing to fire at.
 *
 * AND `Image` DID NOT EXIST. §4.8.3's IDL carries `[LegacyFactoryFunction=Image(optional unsigned long width,
 * optional unsigned long height)]`, which is where `new Image()` comes from; the name was on no global, so a
 * bundle writing `new Image()` — which is how a page loads an image it never inserts — did not run. It is a
 * LEGACY FACTORY FUNCTION (Web IDL §3.7.2 "Legacy factory functions"), not a constructor and not an alias for
 * `document.createElement("img")`: HTML gives it its own five steps, its `prototype` property is non-writable
 * and non-configurable and carries no `constructor` back-pointer, and a call without `new` is a TypeError.
 *
 * WHAT THIS BUILD LACKS IS A DECODER, AND THAT IS NOT A LICENCE TO SHRUG. §4.8.4.3.5 offers a UA "cannot
 * support images" exit at its second step which issues no request and fires no event; taking it would delete
 * both halves above for the sake of a shorter file. The request, the image request STATE MACHINE, the queued
 * events and the `complete`/`currentSrc`/`naturalWidth` observables are all defined without a decoder — so
 * this engine fetches, and the reply lands in the algorithm's own "the image data is not in a supported file
 * format" arm: current request state broken, and an `error` event queued on the DOM manipulation task source.
 * That is the honest answer for a user agent that supports no image format, and it is the same one a real
 * browser gives for an image it cannot decode.
 *
 * WHAT IS HONESTLY ABSENT, EACH CRASHING WHERE IT IS REACHED RATHER THAN ANSWERING SOMETHING PLAUSIBLE:
 *   - §4.8.4.3.7 "Selecting an image source" and §4.8.4.3.10 "Parsing a srcset attribute". An element that
 *     USES SRCSET OR PICTURE (§4.8.4.3 "Processing model": it has a `srcset` attribute, or its parent is a
 *     `picture`) selects its source through them; this build does the `src` half of §4.8.4.3.5 and DFAILs
 *     naming those two, which is the next subproblem in spec order rather than a fallback.
 *   - The DECODER itself: `naturalWidth`/`naturalHeight` answer 0 for an image that is not available, which
 *     is §4.8.3's own step 1 and is every image in this build; the step that reads a decoded image's
 *     density-corrected natural dimensions crashes naming what has to exist first.
 *   - §4.8.3's `decode()`, `width`, `height`, `fetchPriority` and `controls` are ABSENT rather than
 *     shape-only, declared as such per realm (idl_members_excluded) so the gap auditor and the next reader
 *     read one answer. `width`/`height` are §4.8.3's "determine the dimensions" algorithm, whose first branch
 *     is the element's RENDERED size, and this file must not answer a layout question.
 *
 * WHAT IS NOT ABSENT AND LOOKS LIKE IT, twice.
 *   §4.8.4.3.3 "The list of available images" is EMPTY in this build and that is a derived fact, not a missing
 * cache: the only step that adds to it is in the supported-file-format arm of §4.8.4.3.5, which no reply of
 * this user agent can reach, so a lookup in it can never hit. Nothing is stubbed for it and nothing needs to
 * be.
 *   §4.8.4.3.5's LAZY BRANCH resumes immediately, and that is this agent's VIEWPORT rather than a step
 * skipped: §2.5.7's lazy load intersection observer runs the resumption steps for an element that is already
 * intersecting, and an agent that presents the whole document at once — no scrolling, so no part of a document
 * outside the viewport — has every connected element intersecting at the first observation. The deferral's
 * only observable is WHEN the request is made, which is a question about a scroll position this agent does not
 * have. The day a real layout gives it one, the branch is written at the step with the rectangle that decides
 * it.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_IMAGE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_IMAGE_H

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* §4.8.3's AGENT-WIDE DECLARATIONS — the class the per-element state hangs off, the argument declarations of
   the legacy factory function, and the step machine that runs a queued element task. Called once per agent
   from core/html/html_element.c's declare, which is the file that owns the element-interface table this
   element is a row of. */
void html_image_declare(JSContext *ctx);
/* §4.8.3's members that are not reflections, onto THIS REALM's HTMLImageElement.prototype — handed the
   prototype by core/html/html_element.c for the reason §4.12.1's `async` and §4.2.6's `disabled` are: that
   file owns which interface a tag wears, this one owns the algorithms behind the members. */
void html_image_install(JSContext *ctx, JSValueConst proto);
/* Web IDL §3.7.2's legacy factory function object for `Image`, on THIS REALM's global. Separate from the
   install above because a legacy factory function is a global NAME and not a prototype member, and because
   §4.8.3's steps name "the CURRENT global object's associated Document" — so the function object is minted
   per realm and answers with that realm's document. `proto` is THIS realm's HTMLImageElement.prototype, which
   Web IDL §3.7.2 makes the factory's non-configurable `prototype` property; it is a parameter because
   core/html/html_element.c's table is what owns that object, exactly as the install above is handed one. */
void html_image_install_global(JSContext *ctx, JSValueConst global, JSValueConst proto);
/* The agent's atoms and ids, given back at teardown — core/platform.h's release column. */
void html_image_free(JSRuntime *rt);

/* §4.8.4.3.5 "Updating the image data" for one element. Every caller below reaches it through §4.8.4.3.2
   "Reacting to DOM mutations", which is the list of mutations that count; the algorithm itself asks the
   element what its state and its attributes are, so a caller states only THAT a relevant mutation happened.
   `ctx` is the element's node document's realm — every step of the algorithm resolves a URL against that
   document, reads its policy container and queues its tasks — never the realm that performed the write. */
void html_image_update(JSContext *ctx, lxb_dom_element_t *el);

/* §4.8.4.3.2's "The element's src, srcset, width, or sizes attributes are set, changed, or removed" and its
   `crossorigin`/`referrerpolicy` state changes, as one of §4.9's attribute change steps — the chokepoint every
   spelling of those writes reaches (`img.src = u`, `setAttribute`, `attributes.src.value = u`), which is why
   it is here and not inside the reflection's setter. Asks whether `el` is an `img` itself, so core/dom's drain
   states no brand it would have to keep in step with this file. */
void html_image_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);

/* §4.8.4.3.2's "The img or source HTML element insertion steps ... count the mutation as a relevant mutation",
   from core/dom/element.c's §4.2.3 insertion-steps drain, in the inserted node's own document realm — beside
   `<script>` preparation and the child-navigable creation, which are the same family of HTML element insertion
   steps and need the same realm and the same position. */
void html_image_inserted(JSContext *ctx, lxb_dom_element_t *el);

/* THE SAME MUTATION FOR THE ELEMENTS A PARSE CREATED, which reach neither chokepoint above: lexbor builds a
   parsed tree with no per-token hook, so a parsed `<img src=…>` has its attribute set by nothing this engine
   can observe and is inserted by nothing that runs §4.2.3's steps. Called at the seams this engine already
   treats as a parse boundary — the document's, and every fragment's — exactly as core/html/media_element.h's
   media_element_parsed is, and for the identical reason. Costs a tag test per node and allocates for nothing
   else. */
void html_image_parsed(JSContext *ctx, lxb_dom_node_t *root);

#endif
