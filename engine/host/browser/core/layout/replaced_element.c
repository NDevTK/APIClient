/* HTML §15.4 "Replaced elements" over css-images-3 §4.1 "Object-Sizing Terminology". See replaced_element.h
   for why this is a component of its own and for what the predicate it replaced got wrong. */
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/html/html_image.h"
#include "core/layout/replaced_element.h"

/* NOT A REPLACED ELEMENT — every field zeroed, which replaced_element.h makes the contract: a caller reading a
   dimension off this is asking §10.3.2 about a box §10.3.3 owns. */
static ReplacedElement rep_not(void)
{
    ReplacedElement r;

    memset(&r, 0, sizeof r);
    r.width = css_px(0.0);
    r.height = css_px(0.0);
    return r;
}

/* A REPLACED ELEMENT WITH NO NATURAL DIMENSIONS AT ALL — css-images-3 §4.1's own example ("embedded documents,
   such as the iframe element in HTML"), and the answer §15.4.2's second rule gives an `img` whose image is not
   available. CSS 2.1 §10.3.2's last arm is what turns it into a box. */
static ReplacedElement rep_bare(void)
{
    ReplacedElement r = rep_not();

    r.replaced = true;
    return r;
}

/* A REPLACED ELEMENT WITH BOTH NATURAL SIZES AND THEREFORE, NORMALLY, A RATIO — css-images-3 §4.1: "many
   objects, such as most images, cannot have only two natural dimensions, as any two automatically define the
   third". The exception is §4.1's very next sentence and it is the one this build actually reaches: "if an
   object has a DEGENERATE natural aspect ratio (at least one part being zero or infinity), it is treated as
   having NO natural aspect ratio" — so §15.4.2's fourth rule, whose natural dimensions are 0 by 0, produces an
   object with two natural sizes and no ratio, and the arms of §10.3.2 that multiply by a ratio must not see
   one. Deriving the ratio HERE rather than at each caller is what makes that impossible to forget. */
static ReplacedElement rep_sized(double w, double h)
{
    ReplacedElement r = rep_bare();

    DCHECK(isfinite(w) && isfinite(h) && w >= 0.0 && h >= 0.0,
           "a replaced element was given a natural size that is not a finite non-negative number of CSS "
           "pixels — a natural dimension is a measurement of an object's own preferred size (css-images-3 "
           "§4.1), so an infinity or a negative here is a derivation that lost an operand rather than an "
           "object a page could contain");
    r.has_width = true;
    r.has_height = true;
    r.width = css_px(w);
    r.height = css_px(h);
    r.has_ratio = w > 0.0 && h > 0.0;
    r.ratio = r.has_ratio ? w / h : 0.0;
    DCHECK(!r.has_ratio || (isfinite(r.ratio) && r.ratio > 0.0),
           "a natural aspect ratio survived css-images-3 §4.1's DEGENERATE test and is still not a finite "
           "positive number — the test is over exactly the two operands the division uses, so this is the two "
           "having come apart");
    return r;
}

/* ---- HTML §4.8.3 "The img element": WHAT AN img ELEMENT REPRESENTS -----------------------------------------
   §15.4.2's first-applicable-rule list is written entirely in terms of this, so it is derived once and named
   the way the standard names it. "What an img element represents depends on the src attribute and the alt
   attribute", and the five cases below are §4.8.3's own five, in its order. */
typedef enum {
    IMG_REPRESENTS_IMAGE = 0,   /* "the element represents the element's image data" */
    IMG_REPRESENTS_TEXT,        /* "the element represents the text given by the alt attribute" */
    IMG_REPRESENTS_NOTHING      /* "the element represents nothing" */
} ImgRepresents;

/* Attribute PRESENCE and attribute EMPTINESS are two questions and §4.8.3 asks both — its cases split on "the
   alt attribute is set to the empty string" versus "the alt attribute is NOT set", and §15.4.2's second rule
   asks presence alone. Lexbor's `get_attribute` answers NULL for both, which is why presence goes through
   `has_attribute` — the same split core/html/html_image.c's `complete` draws for `src`. */
static bool rep_attr_present(lxb_dom_element_t *el, const char *name)
{
    return lxb_dom_element_has_attribute(el, (const lxb_char_t *)name, strlen(name));
}

static bool rep_attr_nonempty(lxb_dom_element_t *el, const char *name)
{
    size_t n = 0;

    return lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &n) != NULL && n > 0;
}

static ImgRepresents rep_img_represents(lxb_dom_element_t *el, HtmlImageState state)
{
    bool has_src = rep_attr_present(el, "src");
    bool has_alt = rep_attr_present(el, "alt");
    bool alt_text = rep_attr_nonempty(el, "alt");
    /* "If the image is AVAILABLE and the user agent is configured to display that image, then the element
       represents the element's image data." This user agent IS configured to display images — it makes the
       request and runs the whole state machine (html_image.h) — so the conjunct that decides this is
       availability, which §4.8.4.3 defines as partially or completely available. */
    bool available = state == HTML_IMAGE_PARTIALLY_AVAILABLE || state == HTML_IMAGE_COMPLETELY_AVAILABLE;

    if (has_src) {
        if (available) return IMG_REPRESENTS_IMAGE;
        /* "If the src attribute is set and the alt attribute is set to the empty string … Otherwise, the
           element represents nothing." */
        if (has_alt && !alt_text) return IMG_REPRESENTS_NOTHING;
        /* "If the src attribute is set and the alt attribute is set to a value that isn't empty … Otherwise,
           the element represents the text given by the alt attribute." */
        if (alt_text) return IMG_REPRESENTS_TEXT;
        /* "If the src attribute is set and the alt attribute is not … If the image has a src attribute whose
           value is the empty string, then the element represents nothing." The remaining arm of that case
           gives the element no "represents" at all — it says the user agent "should display some sort of
           indicator that there is an image that is not being rendered" — and §15.4.2 does not need one: its
           second rule catches this element by "the element has no alt attribute" before any question about
           what it represents is asked. NOTHING is the honest answer to a question §4.8.3 declines to answer,
           and it is not load-bearing here. */
        return IMG_REPRESENTS_NOTHING;
    }
    /* "If the src attribute is not set and either the alt attribute is set to the empty string or the alt
       attribute is not set at all: The element represents nothing. Otherwise: The element represents the text
       given by the alt attribute." */
    return alt_text ? IMG_REPRESENTS_TEXT : IMG_REPRESENTS_NOTHING;
}

/* ---- HTML §15.4.2 "Images" ---------------------------------------------------------------------------------
   "User agents are expected to render img elements … according to the FIRST APPLICABLE RULES from the
   following list" — so the order below is the standard's and the arms are not independent tests. */
static ReplacedElement rep_img(JSContext *ctx, lxb_dom_element_t *el)
{
    bool complete = false;
    HtmlImageState state = html_image_current_request_state(ctx, el, &complete);
    ImgRepresents represents = rep_img_represents(el, state);

    /* RULE 1: "If the element represents an image — the user agent is expected to treat the element as a
       replaced element and render the image according to the rules for doing so defined in CSS." Its natural
       dimensions are the decoded image's, and there is no decoder. */
    if (represents == IMG_REPRESENTS_IMAGE)
        DFAIL("HTML §15.4.2 \"Images\"'s FIRST rule — an `img` that REPRESENTS AN IMAGE — was reached, so an "
              "image request of this agent became AVAILABLE, which §4.8.4.3 defines as having obtained the "
              "image data AND AT LEAST THE IMAGE DIMENSIONS. Those dimensions are css-images-3 §4.1's natural "
              "width and height and they come from a DECODER this agent does not have; core/html/html_image.h "
              "states outright that every reply lands in the not-a-supported-file-format arm and leaves the "
              "request BROKEN, so nothing in this build can reach this line. BUILD the decoder and the "
              "density-corrected natural dimensions together — the same pair §4.8.3's `naturalWidth` crashes "
              "for — and this arm becomes a read of them");

    /* RULE 2: "If the element does not represent an image and either: the user agent has reason to believe the
       image will become available and be rendered in due course, OR the element has no alt attribute, OR the
       Document is in quirks mode and the element already has natural dimensions … the user agent is expected
       to treat the element as a REPLACED ELEMENT whose content is the text that the element represents, if
       any." The rule gives that replaced element NO natural dimensions, so CSS 2.1 §10.3.2's last arm sizes
       it — 300 x 150.
       THE FIRST ARM IS A QUESTION ABOUT THIS MOMENT AND IT IS ANSWERED FROM THE REQUEST, not from what this
       agent knows about its own decoder. While the fetch is outstanding the user agent has not yet learned
       that it cannot decode the reply, and that is the same position a real browser is in — html_image.h says
       so in as many words about the whole state machine ("the same one a real browser gives for an image it
       cannot decode"). The moment the reply lands the state is BROKEN and this arm stops being true, which is
       exactly when a real browser stops reserving the box.
       "HAS NOT SETTLED" IS §4.8.3's `complete`, NOT A PROXY FOR IT. An `<img>` with no `src` sits in the SAME
       initial `unavailable` state as a fetch in flight, so the state alone cannot tell them apart — and the
       obvious proxy, whether the current request has a non-empty CURRENT URL, is WRONG at the moment that
       matters most: §4.8.4.3.5 writes that URL after a microtask, so a layout in the same task as
       `img.src = u` would see nothing outstanding and classify a LOADING image as a broken one. That is the
       `sizes="auto"` case exactly — the image is measured while its own request is being set up — so the
       predicate is §4.8.3's `complete` getter's own four conditions, computed by the one function in
       core/html/html_image.c that the member itself uses.
       THE THIRD ARM IS FALSE IN THIS BUILD AND IS NOT A SKIPPED STEP: "already has natural dimensions (e.g.
       from the dimension attributes or CSS rules)" needs HTML §15.4.3 "Attributes for embedded content and
       images" to map a `width`/`height` content attribute to a presentational hint, and
       core/css/css_presentational_hints.c carries no such row — so no `img` in this agent has natural
       dimensions from that source, in quirks mode or out of it. The day that row exists this arm is written
       beside it. */
    if (!complete) return rep_bare();
    if (!rep_attr_present(el, "alt")) return rep_bare();

    /* RULE 3: "If the element is an img element that represents some text and the user agent does not expect
       this to change — the user agent is expected to treat the element as a NON-REPLACED PHRASING ELEMENT
       whose content is the text." This is the one arm in which an `img` is not a replaced element, and it is
       reachable and common: a broken image with an `alt`. CSS 2.1 §10.2's "Applies to: all elements but
       non-replaced inline elements" then makes `width` not apply to it, so CSSOM §9 resolves it to the
       computed `auto` — core/css/css_property_applies.c is what asks. */
    if (represents == IMG_REPRESENTS_TEXT) return rep_not();

    /* RULE 4: "If the element is an img element that represents nothing and the user agent does not expect
       this to change — the user agent is expected to treat the element as a replaced element whose NATURAL
       DIMENSIONS ARE 0. (In the absence of further styles, this will cause the element to essentially not be
       rendered.)" */
    DCHECK(represents == IMG_REPRESENTS_NOTHING,
           "HTML §15.4.2's fourth rule was reached for an `img` that represents something other than nothing "
           "— the three preceding rules cover an image and text, and §4.8.3 gives an img exactly those three "
           "answers, so this is the two lists having come apart");
    return rep_sized(0.0, 0.0);
}

ReplacedElement replaced_element_of(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n;
    JSContext *dctx;

    DCHECK(el != NULL, "HTML §15.4's replaced-element question was asked about no element");
    n = lxb_dom_interface_node(el);
    /* HTML §15.4 "Replaced elements", first sentence: "The following elements can be replaced elements: audio,
       canvas, embed, iframe, img, input, object, and video." EVERY OTHER ELEMENT IS NOT ONE, which is a real
       answer and the half `css_element_may_be_replaced` got wrong — css-display Appendix B's list also carries
       `br`, `wbr`, `frame`, `frameset`, `meter`, `progress`, `select` and `textarea`, and none of those is on
       this one. A `<select>` is a WIDGET (§15.5 "Widgets"), which is a different rendering mechanism with a
       different sizing rule, and answering it as replaced sent its `width` through an algorithm that does not
       apply to it. */
    if (!lxb_html_tree_node_is(n, LXB_TAG_IMG) && !lxb_html_tree_node_is(n, LXB_TAG_IFRAME) &&
        !lxb_html_tree_node_is(n, LXB_TAG_EMBED) && !lxb_html_tree_node_is(n, LXB_TAG_VIDEO) &&
        !lxb_html_tree_node_is(n, LXB_TAG_AUDIO) && !lxb_html_tree_node_is(n, LXB_TAG_CANVAS) &&
        !lxb_html_tree_node_is(n, LXB_TAG_OBJECT) && !lxb_html_tree_node_is(n, LXB_TAG_INPUT))
        return rep_not();

    /* §15.4.1 "Embedded content", first sentence: "The embed, iframe, and video elements are expected to be
       treated as REPLACED ELEMENTS." Unconditionally — there is no state to consult and no rule list to walk.
       What differs between the three is only their NATURAL DIMENSIONS, and css-images-3 §4.1 answers one of
       them outright: an embedded document, "such as the iframe element in HTML", is "an example of an object
       with NO NATURAL DIMENSIONS AT ALL". So an `iframe` is complete here with no child layout — its box is
       CSS 2.1 §10.3.2's 300 x 150, which is the number core/frame/viewport.c derives a child navigable's
       viewport from, and the two are one statement now rather than two constants. */
    if (lxb_html_tree_node_is(n, LXB_TAG_IFRAME)) return rep_bare();
    if (lxb_html_tree_node_is(n, LXB_TAG_EMBED))
        DFAIL("HTML §15.4.1 \"Embedded content\" makes an `embed` a REPLACED ELEMENT unconditionally, and its "
              "css-images-3 §4.1 natural dimensions are its CONTENT's — a plugin's, or the image or document "
              "the resource turns out to be, which §4.8.6 \"The embed element\" resolves by TYPE SNIFFING the "
              "response. This agent runs no plugins and decodes no images, so the honest answer is not 'no "
              "natural dimensions' (that is the answer for an embedded DOCUMENT and would silently make every "
              "`embed` 300 x 150): it is that the resource has never been classified. BUILD §4.8.6's "
              "determine-the-type-of-the-content steps and the per-type natural dimensions they select");
    if (lxb_html_tree_node_is(n, LXB_TAG_VIDEO))
        DFAIL("HTML §15.4.1 \"Embedded content\" makes a `video` a REPLACED ELEMENT unconditionally, and its "
              "css-images-3 §4.1 natural dimensions are the DECODED video's — §4.8.11 \"Media elements\" makes "
              "them observable as `videoWidth`/`videoHeight`, and until the resource's metadata is parsed the "
              "element has none. core/html/media_element.h holds the readyState machine this hangs off and "
              "this agent has no video decoder to advance it past HAVE_NOTHING. BUILD the metadata parse and "
              "the natural size it yields; §15.4.1's own `video { object-fit: contain }` UA rule is the second "
              "half and needs core/css/css_computed_value.c to carry `object-fit` first");
    if (lxb_html_tree_node_is(n, LXB_TAG_CANVAS))
        DFAIL("HTML §15.4.1 \"Embedded content\": \"A canvas element that REPRESENTS EMBEDDED CONTENT is "
              "expected to be treated as a replaced element; the contents of such elements are the element's "
              "bitmap, if any, or else a transparent black bitmap with the same natural dimensions as the "
              "element. OTHER canvas elements are expected to be treated as ORDINARY elements in the rendering "
              "model.\" Both halves are missing and they are different: which canvas represents embedded "
              "content is §4.12.5 \"The canvas element\"'s fallback-content rule, and the natural dimensions "
              "are the element's `width` and `height` CONTENT ATTRIBUTES, whose §4.12.5 missing-value defaults "
              "are 300 and 150 — the same pair, arrived at by a completely different route, which is precisely "
              "why it must not be borrowed from §10.3.2's default here. BUILD §4.12.5's two attributes and "
              "their defaults, then this arm is a read of them");
    if (lxb_html_tree_node_is(n, LXB_TAG_OBJECT))
        DFAIL("HTML §15.4.1 \"Embedded content\": \"An `object` element that REPRESENTS AN IMAGE, PLUGIN, OR "
              "ITS CONTENT NAVIGABLE is expected to be treated as a replaced element. Other object elements "
              "are expected to be treated as ORDINARY elements in the rendering model.\" Which of the four an "
              "`object` is is §4.8.7 \"The object element\"'s own multi-step representation algorithm over the "
              "resource's type, and this agent runs none of it. BUILD §4.8.7's steps; the natural dimensions "
              "then follow the branch they select, and only the content-navigable branch is answerable today "
              "(css-images-3 §4.1: an embedded document has none)");
    if (lxb_html_tree_node_is(n, LXB_TAG_AUDIO))
        DFAIL("HTML §15.4.1 \"Embedded content\": \"The `audio` element, WHEN IT IS EXPOSING A USER INTERFACE, "
              "is expected to be treated as a replaced element ABOUT ONE LINE HIGH, as wide as is necessary to "
              "expose the user agent's user interface features. When an audio element is NOT exposing a user "
              "interface, the user agent is expected to force its `display` property to compute to `none`, "
              "IRRESPECTIVE OF CSS RULES.\" So neither answer is a natural dimension this component can state: "
              "one is a UA control strip's own size and the other is a forced computed `display` that belongs "
              "in core/css/css_computed_value.c beside every other UA rule, where it would make this element "
              "generate no box at all. BUILD the forced `display: none` first — it is the case every `audio` "
              "without a `controls` attribute takes — and then §15.5 \"Widgets\"' native appearance for the "
              "other");
    if (lxb_html_tree_node_is(n, LXB_TAG_INPUT))
        DFAIL("HTML §15.4 lists `input` among the elements that CAN be replaced, and which rule applies "
              "depends on its TYPE. §15.4.2 \"Images\"' list is written for \"img elements and input elements "
              "WHOSE TYPE ATTRIBUTES ARE IN THE IMAGE BUTTON STATE\", whose last rule makes a non-image one a "
              "replaced BUTTON \"about one line in height and whatever width is necessary to render the text "
              "on one line\"; every other type is §15.5 \"Widgets\", a different mechanism with a different "
              "size. The type-state machine is what is missing — §4.10.5.1's states are not modelled — and it "
              "decides which of the two this is. BUILD the `type` attribute's enumerated states, then §15.4.2 "
              "over the Image Button one. The text-sized arms additionally need \"whatever width is necessary "
              "to render the text on one line\", which is css-sizing-3 §2.1's MAX-CONTENT INLINE SIZE of that "
              "text — the same quantity §10.3.5's shrink-to-fit takes as its preferred width, and it is BUILT "
              "(core/layout/intrinsic_size.h), so that half is a read rather than a subproblem");

    DCHECK(lxb_html_tree_node_is(n, LXB_TAG_IMG),
           "the element dispatch above admitted an element §15.4's list does not name — the guard and this "
           "chain are one list and they have come apart");
    /* §15.4.2's rules are a fact about the IMAGE REQUEST, which lives on the element's own wrapper and is
       therefore per flow. The realm is the ELEMENT'S OWN DOCUMENT'S, never the running one, for the reason
       core/layout/used_value.c gives about the initial containing block: a classification is a fact about the
       document the element is in. */
    DCHECK(n->owner_document != NULL,
           "HTML §15.4.2's rules were asked about an `img` whose node has no owner document — every node this "
           "engine mints belongs to the document that created it");
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    if (dctx == NULL)
        DFAIL("HTML §15.4.2 \"Images\"' rules were asked about an `img` in a document NO NAVIGABLE PRESENTS — "
              "a DOMParser document, an XHR `responseXML`, a `<template>`'s contents owner, or the document of "
              "a destroyed navigable. The rules are stated over the element's IMAGE REQUEST, which §4.8.4.3 "
              "keeps on the element's own wrapper so that it is per flow, and there is no realm to read that "
              "wrapper out of. The answer is NOT a classification: §15.4.2's own opening is \"user agents are "
              "expected to RENDER img elements … according to the first applicable rules\", and this element "
              "is not being rendered by anything. BUILD CSSOM §9's missing conjunct over "
              "core/dom/element_view.h's `element_view_has_box` — the same escape core/layout/used_value.c's "
              "`uv_icb` names in full — so the resolved value takes §9's computed-value arm before any "
              "rendering rule is asked");
    return rep_img(dctx, el);
}
