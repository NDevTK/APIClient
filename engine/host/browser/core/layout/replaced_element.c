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
#include "core/html/html_form.h"
#include "core/html/html_image.h"
#include "core/html/media_element.h"
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

/* ---- HTML §15.4.2 "Images", THE `img` HALF -----------------------------------------------------------------
   "User agents are expected to render img elements and input elements whose type attributes are in the Image
   Button state, according to the first applicable rules from the following list" — so the order below is the
   standard's and the arms are not independent tests.
   THE LIST HAS FIVE RULES AND GOVERNS TWO ELEMENTS, which is why this function is the `img` half and named for
   it rather than for the section. Rules 1 and 2 are written over "the element" and reach both; rules 3 and 4
   open "If the element is an img element" and reach only this one; rule 5 opens "If the element is an input
   element" and cannot reach it at all. So the four arms below ARE §15.4.2 for an `img`, and the missing fifth
   is not a skipped step — it is a rule whose antecedent this element fails by its tag. `replaced_element_of`'s
   `input` arm is where the other three-arm walk over the same list belongs, and it crashes there for the image
   request both of its first two rules turn on. */
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

/* ---- HTML §4.8.6 "The embed element": WHEN AN `embed` REPRESENTS NOTHING -----------------------------------
   §15.4.1 "Embedded content" makes an `embed` a replaced element with no state to consult — "The embed,
   iframe, and video elements are expected to be treated as replaced elements" — so the only question left is
   css-images-3 §4.1's natural dimensions, and those are the CONTENT's. §4.8.6 states outright when there is no
   content: "While any of the following conditions are occurring, any plugin instantiated for the element must
   be removed, and the embed element represents nothing:", followed by three conditions. It is a "while ANY"
   list, so one of them is the whole answer and the three are not a conjunction to complete.
   THE FIRST TWO ARE COMPUTED HERE AND THE THIRD IS NOT, and the split is not arbitrary: "The element has
   neither a src attribute nor a type attribute" and "The element has a media element ancestor" are questions
   about the TREE, which this engine holds in full, while "The element has an ancestor object element that is
   not showing its fallback content" is a question about §4.8.7 "The object element"'s representation
   algorithm, which is the very thing `replaced_element_of`'s `object` arm crashes for. A false from this
   function therefore says only that the first two conditions do not hold; it never says the element represents
   something — which is why its ONE caller crashes rather than answering, and why this returns a bool rather
   than a ReplacedElement it would have to invent.
   PRESENCE, NOT EMPTINESS. §4.8.6's first condition is written over the attributes being SET, and the section
   asks the other question one paragraph later where it means it (a potentially active embed needs its "src
   attribute is either absent or its value is not the empty string"), so `<embed src="">` HAS a src attribute
   and this condition does not catch it. That is not a gap being papered over — it is the case §4.8.6 leaves
   to the setup steps, and the caller's crash names them.
   THE MEDIA-ELEMENT SET IS ASKED, NOT RESTATED. §4.8.11 "Media elements"' first sentence is answered by
   core/html/media_element.h, over the interned tag id and the namespace, so an SVG `<video>` ancestor is not
   one — a tag-name walk written here would have said it was. */
static bool rep_embed_represents_nothing(lxb_dom_element_t *el)
{
    const lxb_dom_node_t *a;

    DCHECK(lxb_html_tree_node_is(lxb_dom_interface_node(el), LXB_TAG_EMBED),
           "HTML §4.8.6 \"The embed element\"'s represents-nothing conditions were asked about an element that "
           "is not an `embed` — the section is written about that element alone, and the tag dispatch in "
           "`replaced_element_of` is this function's only caller, so this is the two having come apart");
    if (!rep_attr_present(el, "src") && !rep_attr_present(el, "type")) return true;
    /* The condition is about an ANCESTOR, which is strictly above the element, so the walk starts at its
       parent — an `embed` is not a media element and could not be its own. */
    for (a = lxb_dom_interface_node(el)->parent; a != NULL; a = a->parent)
        if (media_element_is(a)) return true;
    return false;
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
    /* AN `embed` THAT REPRESENTS NOTHING HAS NO NATURAL DIMENSIONS, WHICH IS A COMPUTED ANSWER AND NOT A SHRUG.
       css-images-3 §4.1 "Object-Sizing Terminology" is what makes the derivation short: "These natural
       dimensions represent the preferred sizing intrinsic to the object itself; that is, they are not a
       function of the context in which the object is used", and an element §4.8.6 says represents NOTHING has
       no object for a preferred size to be intrinsic to — so none of the three exists. Nothing in HTML supplies
       one either, and the one rule that looks as though it might is written for other elements: HTML §15.4.2
       "Images"' fourth rule gives natural dimensions of 0 to an element that represents nothing, and its list
       governs "img elements and input elements whose type attributes are in the Image Button state", which
       this is not. §15.4.1 "Embedded content" gives an `embed` no dimensions at all.
       CSS 2.1 §10.3.2 "Inline, replaced elements"' LAST ARM then sizes the box — "Otherwise, if 'width' has a
       computed value of 'auto', but none of the conditions above are met, then the used value of 'width'
       becomes 300px" — and §10.6.2's last arm gives 150.
       IT IS THE `iframe`'s PAIR OF NUMBERS AND NOT THE `iframe`'s DERIVATION, which is why this is a second
       call to `rep_bare` rather than a shared arm: an embedded document has no natural dimensions because
       css-images-3 §4.1 names it as an object that has none, and this element has none because there is no
       object. The box is not empty of CONTENT, which is the other half of why 0 by 0 would be wrong here —
       §4.8.6's display-no-plugin steps say "Display an indication that no plugin could be found for element,
       as the contents of element", so the rectangle is one something is drawn in. */
    if (lxb_html_tree_node_is(n, LXB_TAG_EMBED)) {
        if (rep_embed_represents_nothing(el)) return rep_bare();
        DFAIL("HTML §15.4.1 \"Embedded content\" makes an `embed` a REPLACED ELEMENT unconditionally, and this "
              "one carries a `src` or a `type` and has no media element ancestor, so §4.8.6 \"The embed "
              "element\"'s represents-nothing list does not answer it and its css-images-3 §4.1 natural "
              "dimensions would be its CONTENT's. THE RESOURCE HAS NEVER BEEN FETCHED: §4.8.6's EMBED ELEMENT "
              "SETUP STEPS are what fetch it — a request whose destination is `embed`, whose credentials mode "
              "is `include` and whose mode is `navigate` — and nothing in this engine queues them on the embed "
              "task source. BUILD those steps, with the `load` event they fire on a network error and the "
              "delay they put on the document's load event; the fetch is also an ENDPOINT this tool exists to "
              "find, so what the absence costs is more than a box size. THE INSTRUCTION THAT STOOD HERE WAS "
              "SPEC-WRONG AND IS DELETED: it said to build §4.8.6's determine-the-type-of-the-content steps "
              "and the per-type natural dimensions they select, and that algorithm selects none. Each of its "
              "three non-null arms is conditioned on a PLUGIN supporting the type, its last step is to return "
              "null, and §4.8.6's switch sends null to display-no-plugin, whose last step makes the element "
              "represent nothing — the answer the arm above already gives. A user agent may have no plugins at "
              "all: HTML §2.1.6 \"Plugins\" says \"Indeed, this specification doesn't require user agents to "
              "support plugins at all\". THE ONE CONDITION GENUINELY UNANSWERABLE HERE is §4.8.6's third, "
              "\"The element has an ancestor object element that is not showing its fallback content\", which "
              "needs §4.8.7 \"The object element\"'s representation algorithm — the same one the `object` arm "
              "below crashes for, so building that arm answers this one too");
    }
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
    /* AN `input` IS GOVERNED BY EXACTLY ONE OF §15's TWO RENDERING MECHANISMS AND ITS `type` DECIDES WHICH.
       §15.4 lists `input` among the elements that CAN be replaced; §15.4.2 "Images"' list is what makes one
       actually replaced, and its subject is not every `input` — the section opens "User agents are expected to
       render img elements and input elements whose type attributes are in the Image Button state, according to
       the first applicable rules from the following list". That is ONE of §4.10.5.1 "States of the type
       attribute"'s states, and `html_form_input_state` (core/html/html_form.h) resolves that section's whole
       keyword table — ASCII case-insensitively, with both of its missing- and invalid-value defaults — where
       `INPUT_STATE_IMAGE` IS the Image Button state.
       EVERY OTHER STATE IS §15.5 "Widgets" AND IS THEREFORE NOT A REPLACED ELEMENT, which is a COMPUTED answer
       and not a shrug: §15.5.1 "Native appearance" names the whole class — "The following elements can have a
       native appearance for the purpose of the CSS 'appearance' property", over `button`, `input`, `meter`,
       `progress`, `select` and `textarea` — and this component already answers `rep_not` for the other five,
       through §15.4's list not naming them. An `input` is the ONE member of §15.5.1's list that §15.4's list
       also names, so it is the one that has to be split here rather than at the guard. §15.5's own per-state
       sections are what a widget's box IS: §15.5.10 "The input element as a checkbox and radio button widgets"
       gives the Checkbox state "a non-devolvable widget expected to render as an 'inline-block' box containing
       a single checkbox control, with no label", and §15.5.12 "The input element as a button" sends the Submit
       Button, Reset Button and Button states to §15.5.3 "Button layout". NONE of those is a replaced element,
       and answering one as replaced is the defect the `select` note above records — it sends the widget's
       `width` through CSS 2.1 §10.3.2, an algorithm that does not apply to it.
       NAMED RESIDUAL — §15.5.3 "Button layout"'s TWO as-if clauses are not covered by this `false`, and they
       are as-if rather than a classification, which is why this arm is right and narrower rather than wrong.
       §15.5.3 says "If the element is absolutely-positioned, then for the purpose of the CSS visual formatting
       model, act as if the element is a replaced element." and "For the purpose of the 'normal' keyword of the
       'align-self' property, act as if the element is a replaced element." — each scoped to one purpose, so
       neither makes `replaced_element_of` answer true, whose subject is css-images-3 §4.1's natural
       dimensions. THE NEXT DIFF BUILDS a per-purpose replaced question in core/layout/used_value.c: §15.5.3's
       first clause is an operand of CSS 2.1 §10.3.7 versus §10.3.8 (absolutely positioned, non-replaced versus
       replaced), so it belongs where that choice is made and not here. ITS ABSENCE SHOWS as an absolutely
       positioned `<input type=submit>` with `auto` on `left`, `width` and `right` taking §10.3.7's shrink-to-
       fit instead of §10.3.8's step 1, which is a different used `width` that `getBoundingClientRect` reports.
       §15.5.3's third clause, "If the computed value of 'inline-size' is 'auto', then the used value is the
       fit-content inline size", is the same file's question and not this one's either. */
    if (lxb_html_tree_node_is(n, LXB_TAG_INPUT)) {
        HtmlInputState state = html_form_input_state(n);

        /* `INPUT_STATE_NONE` is that function's answer to "is this an `input` at all", which the tag dispatch
           above has already answered yes to — so the two asking it differently is the two having come apart,
           and it is asserted rather than folded into the state test below, where it would silently take the
           §15.5 arm and report a widget for an element that is not one. */
        DCHECK(state != INPUT_STATE_NONE,
               "§4.10.5.1 \"States of the type attribute\"'s state was read as NONE for an element HTML §15.4's "
               "own list matched as an `input` — NONE is not one of that section's states but the answer to "
               "whether the element is an `input`, so this is the tag dispatch here and the tag test in "
               "core/html/html_form.c disagreeing about one element");
        if (state != INPUT_STATE_IMAGE) return rep_not();
        DFAIL("HTML §15.4.2 \"Images\"' rules were reached for an `input` in §4.10.5.1.19 \"Image Button state "
              "(type=image)\"'s state, and every one of them turns on what the element REPRESENTS — which for "
              "this element is §4.10.5.1.19's own two-case sentence and NOT §4.8.3's five-case one that "
              "`rep_img_represents` above implements: \"If the src attribute is set, and the image is "
              "available and the user agent is configured to display that image, then the element represents a "
              "control for selecting a coordinate from the image specified by the src attribute\", and "
              "\"Otherwise, the element represents a submit button whose label is given by the value of the "
              "alt attribute.\" BOTH CASES TURN ON THE IMAGE REQUEST AND THIS ELEMENT HAS NONE. §4.10.5.1.19 "
              "is what would create it — \"Let request be a new request whose URL is url, client is the "
              "element's node document's relevant settings object, destination is \\\"image\\\", initiator "
              "type is \\\"input\\\", credentials mode is \\\"include\\\", and whose use-URL-credentials flag "
              "is set.\" — and nothing in this engine queues those steps for an `input`: "
              "core/html/html_image.c runs §4.8.4.3's processing model for `img` ALONE and asserts it, so "
              "`html_image_current_request_state` would abort on this element rather than answer for it. BUILD "
              "§4.10.5.1.19's fetch, its `load`/`error` element tasks on the user interaction task source, and "
              "the delay it puts on the document's load event; the request is also an ENDPOINT this tool "
              "exists to find, so what the absence costs is more than a box size. THEN HTML §15.4.2 \"Images\"' "
              "list runs here with THREE arms reachable for an `input` and not five. Its FIRST rule (the "
              "element represents an image) needs the same DECODER `rep_img`'s first rule does. Its SECOND "
              "gives a replaced element with no natural dimensions — HTML §15.4.2: \"For input elements, the "
              "element is expected to appear button-like to indicate that the element is a button.\" — which "
              "CSS 2.1 §10.3.2's last arm then sizes 300 by 150. Its THIRD and FOURTH open HTML §15.4.2's "
              "\"If the element is an img element\" and cannot apply here at all. ITS FIFTH IS THE INPUT'S OWN "
              "and is the one this file previously mislabelled as a four-rule list — HTML §15.4.2: \"The user "
              "agent is expected to treat the element as a replaced element consisting of a button whose "
              "content is the element's alternative text.\" THE CLAIM THAT ITS WIDTH IS A READ WAS WRONG AND "
              "IS DELETED. HTML §15.4.2 states that rule's natural dimensions as \"about one line in height "
              "and whatever width is necessary to render the text on one line\", and this line used to send the "
              "reader to core/layout/intrinsic_size.h for the second as though it were already answered. IT IS "
              "NOT, for two reasons that are each on their own decisive: `intrinsic_inline_sizes` CRASHES BY "
              "NAME on its first step for a REPLACED element, which is what §15.4.2's fifth rule has just made "
              "this one, and its walk measures a box's CHILDREN while an `input` has none — the label is the "
              "`alt` attribute's value, a string that is nowhere in the tree. What is genuinely needed is an "
              "advance measure over a STRING, and core/layout/text_run.h's collection takes DOM TEXT NODES "
              "(`text_run_measure_add_text` asserts its node's type and its parent), so BUILD that entry "
              "there — one run over supplied characters carrying a style element — which §15.5.12's button "
              "label needs too and is the reason it belongs in that component rather than here");
    }

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
