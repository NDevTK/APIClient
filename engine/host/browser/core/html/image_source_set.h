/* THE SOURCE SET — HTML §4.8.4.3 "Processing model": "All img and link elements are associated with a source
 * set. A source set is an ordered set of zero or more image sources and a source size. An image source is a
 * URL, and optionally either a pixel density descriptor, or a width descriptor."
 *
 * SIX ALGORITHMS BUILD ONE AND PICK FROM IT, and they are one problem: §4.8.4.3.7 "Selecting an image source",
 * §4.8.4.3.8 "Creating a source set from attributes", §4.8.4.3.9 "Updating the source set", §4.8.4.3.10
 * "Parsing a srcset attribute", §4.8.4.3.11 "Parsing a sizes attribute" and §4.8.4.3.12 "Normalizing the source
 * densities". They are HERE and not in core/html/html_image.c because that file's problem is the img element's
 * IMAGE REQUESTS — the state machine, the queued events, the fetch — and this one's is a GRAMMAR plus a choice
 * over what it produces. §4.8.4.3.5 "Updating the image data" reaches this component through exactly one
 * question ("what source does this element select") and nothing here knows what an image request is.
 *
 * WHY IT EXISTS AT ALL: an `img` that USES SRCSET OR PICTURE (§4.8.4.3: "if it has a srcset attribute specified
 * or if it has a parent that is a picture element") selected NO source before this component, so html_image.c
 * aborted a dev build at the step that asks. That is most image-bearing markup on the modern web, and a build
 * that aborts there analyses none of those pages at all.
 *
 * WHAT `link` GETS AND WHY IT IS NOT HERE YET. §4.8.4.3.9 is written over "a given img or link element", with
 * `imagesrcset`/`imagesizes`/`href` standing in for `srcset`/`sizes`/`src`; §4.8.4.3.9's own note is that for a
 * link element "elements contains only el, so this step will be reached immediately and the rest of the
 * algorithm will not run". This component's entry takes an `img`, and asserts it: the link half belongs to
 * `<link rel=preload as=image>`'s preload machinery, which is a different caller with a different consumer of
 * the answer, and the day it is written it passes the same three strings into the same create-a-source-set.
 *
 * ---------------------------------------------------------------------------------------------------------
 * THE SOURCE SIZE IS A LENGTH, AND EVERY UNIT IN IT IS ANSWERED BY A COMPONENT THAT ALREADY OWNS IT.
 * §4.8.4.3 states the rule in one sentence: "When a source size has a unit relative to the viewport, it must be
 * interpreted relative to the img element's node document's viewport. OTHER UNITS MUST BE INTERPRETED THE SAME
 * AS IN MEDIA QUERIES." So an `em` in a `sizes` attribute is css-values-4 §6.1.1's outside-an-element case — the
 * INITIAL `font-size`, not the element's computed one — and core/css/media_query.h is the component that
 * already resolves exactly that table. This file asks it (`media_query_length_px`) rather than carrying a
 * second copy: one fact answered from two places is the defect CLAUDE.md names, and the copy that is not
 * maintained is the one that goes on being wrong.
 * IT IS A `double` AND NOT A `CssPx`, which is core/css/media_query.h's layering and not a weaker form of it.
 * The source size is consumed by §4.8.4.3.12's division and then by a C comparison in §4.8.4.3.7, and a
 * concolic in front of a C `if` silently picks one arm — the one thing core/frame/viewport.h forbids. The
 * modelled viewport is the EXAMPLE, exactly as Media Queries §4 evaluates `(max-width: 768px)` against it, and
 * the alternate-viewport world is reached the way this engine reaches every other one: a `<source media>` and a
 * sizes entry's `<media-condition>` are evaluated through `media_query_matches_now`, which takes the arm THIS
 * FLOW has committed to and forks nothing in C.
 * AND THE COVERAGE A SINGLE CHOICE WOULD COST IS NOT LOST, because the choice is not what reaches the @H
 * surface. Every image source in the set is an address the bundle SHIPPED and a browser at some device pixel
 * ratio requests, so core/html/html_image.c records all of them and fetches the one selected — which is the
 * §What-the-tool-produces sentence exactly ("what the bundle CAN do but didn't"), and is why over-reporting
 * here is the right direction.
 *
 * ---------------------------------------------------------------------------------------------------------
 * A CONCOLIC ATTRIBUTE MAKES THE SET UNDECIDED, AND UNDECIDED IS A POSITIVE ANSWER.
 * `img.srcset = "/img/" + user + " 1x"` is an address no host can be asked for. Lexbor holds SOME string for
 * that attribute (it would ToString the value away), and parsing it would hand back a list of concrete URLs
 * that are examples wearing the shape of measurements — the defect CLAUDE.md counts seven of. So every read
 * this walk makes asks solver/dom_cow.h for the attribute's taint FIRST, and a taint sets `undecided`: the walk
 * stops, `selected` is -1, and the URL-bearing taint travels out as `undecided_url` so html_image.c can record
 * the SHAPE on the @H surface and owe no fetch. That is the identical answer html_image.c already gave a
 * concolic `src` and core/html/html_script.c gives an unknown `<script src>`; this component makes it one path
 * instead of two.
 * IT IS ASKED WHERE THE VALUE IS USED, NOT WHERE IT IS READ, which is the difference between precision and
 * over-declaring: a tainted `src` on an element whose `srcset` already supplies a 1x candidate is never
 * appended by §4.8.4.3.8's step 4, so it decides nothing and the set stays decided; a tainted `sizes` matters
 * only if some image source carries a WIDTH descriptor, because the source size has no other consumer.
 *
 * ---------------------------------------------------------------------------------------------------------
 * WHAT CRASHES RATHER THAN ANSWERING SOMETHING PLAUSIBLE, each naming a component that has to exist first:
 *   - `sizes="auto"` on an element that ALLOWS AUTO-SIZES and is BEING RENDERED. §4.8.4.3.11 step 3.3 sets the
 *     size to "the concrete object size width of img, in CSS pixels", and CSS Images 3 §4.5 "Sizing Objects:
 *     the object-fit property" makes that the element's USED WIDTH under the initial `fill` ("the object's
 *     concrete object size is the element's used width and height"). So there is nothing extra to build here:
 *     this file asks core/layout/used_value.h for that used width, and the crash lands in the component that
 *     owns the unbuilt case — CSS 2.1 §10.3.4's send of a block-level REPLACED element's width to §10.3.2 —
 *     and disappears by itself when replaced-element sizing lands. An element that does NOT allow auto-sizes,
 *     or that is not being rendered, needs none of it: §4.8.4.3.11's own next sentence is "If size is still
 *     auto, then it will be ignored", so the entry is skipped and the next one decides.
 *
 * WHAT IS ABSENT AND IS NOT A HOLE, twice, because both look like skipped steps and neither is:
 *   - §4.8.4.3.9's "set el's dimension attribute source to child" is not STORED. It is a pure function of this
 *     walk — the child that matched, when that child has a `width` or `height` attribute, and `el` otherwise —
 *     and its only consumers are §4.8.3's `width` and `height`, which core/html/html_image.c declares ABSENT
 *     because their first branch is the element's rendered size. A stored copy of a derived fact is a second
 *     thing to keep in step; the day those members exist they take it from this walk.
 *   - §4.8.4.3.13 "Reacting to environment changes" is a "may at any time" algorithm — a UA is not required to
 *     ever run it — and it is the SECOND caller of §4.8.4.3.7, not part of it. It belongs beside the resize
 *     steps that would trigger it, with the pending request half of §4.8.4.3.5 that it is written in terms of.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_IMAGE_SOURCE_SET_H
#define ENGINE_HOST_BROWSER_CORE_HTML_IMAGE_SOURCE_SET_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* §4.8.4.3's IMAGE SOURCE: "a URL, and optionally either a pixel density descriptor, or a width descriptor".
   The two descriptors are carried as the OPTIONS they are — `has_density`/`has_width` — because §4.8.4.3.12
   "Normalizing the source densities" branches on which one is present and a defaulted number could not tell
   `1x` from "no descriptor at all", which are the same answer only AFTER normalization. Once the set has been
   normalized every source has a pixel density descriptor and `has_density` is true for all of them; that is the
   whole of what normalizing means, and it is asserted rather than assumed. */
typedef struct {
    char  *url;         /* OWNED — the image candidate string's URL, exactly as written, unresolved */
    double density;     /* the pixel density descriptor value, in `x` */
    bool   has_density;
    double width;       /* the width descriptor value, in `w` */
    bool   has_width;
} ImageSource;

typedef struct {
    ImageSource *items;         /* OWNED */
    int          n;
    /* §4.8.4.3.11's answer, in CSS pixels — the `<source-size-value>` the sizes attribute resolved to, or the
       `100vw` §4.8.4.3.11's last step returns. Read only by §4.8.4.3.12's division. */
    double       source_size;
    /* §4.8.4.3.7's choice as an INDEX into `items`, and -1 for "return null as the URL and undefined as the
       pixel density" — which is step 2's answer for an empty source set and is also what an UNDECIDED set
       answers, because a choice made out of a value nobody can read is not a choice. */
    int          selected;
    /* An attribute this walk had to read to decide the set carries a CONCOLIC value (solver/dom_cow.h) — see
       the header note. A POSITIVE statement that the selection is not determinable, never a hole. */
    bool         undecided;
    /* The concolic value of the URL-BEARING attribute (`srcset` or `src`) that made the set undecided, so the
       @H surface records the SHAPE the page composed. JS_UNDEFINED when the undecidable attribute was not one
       that names an address (`sizes`, `media`, `type`) — which is a different fact and not a missing one.
       OWNED: image_source_set_release. */
    JSValue      undecided_url;
} ImageSourceSet;

/* §4.8.4.3.7 "Selecting an image source" for `el`, WHOLE: step 1's update the source set (§4.8.4.3.9, with the
   `<picture>` walk), the two parsers it runs (§4.8.4.3.10, §4.8.4.3.11), §4.8.4.3.12's normalization and
   step 3's select-an-image-source-from-a-source-set.
   `el` MUST be an `img` element — asserted, see the header for what the `link` half would add.
   `ctx` is the element's NODE DOCUMENT's realm, never the realm that performed the write, because every
   viewport-relative length and every media query in here is answered per document (core/css/media_query.h).
   `out` is filled entirely; the caller releases it. */
void image_source_set_select(JSContext *ctx, lxb_dom_element_t *el, ImageSourceSet *out);
void image_source_set_release(JSContext *ctx, ImageSourceSet *s);

#endif
