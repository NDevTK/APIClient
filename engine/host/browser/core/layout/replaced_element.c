/* HTML §15.4 "Replaced elements" over css-images-3 §4.1 "Object-Sizing Terminology". See replaced_element.h. */
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/dom/element.h"       /* §2.6.1's `unsigned long` model — §4.12.5's bitmap reads one */
#include "core/html/html_form.h"
#include "core/html/html_image.h"
#include "core/layout/replaced_element.h"

/* Not a replaced element: every field zeroed, per replaced_element.h's contract. */
static ReplacedElement rep_not(void)
{
    ReplacedElement r;

    memset(&r, 0, sizeof r);
    r.width = css_px(0.0);
    r.height = css_px(0.0);
    return r;
}

/* A replaced element with no natural dimensions at all: css-images-3 §4.1's iframe example, and §15.4.2's
   second rule for an `img` whose image is not available. CSS 2.1 §10.3.2's last arm sizes the box. */
static ReplacedElement rep_bare(void)
{
    ReplacedElement r = rep_not();

    r.replaced = true;
    return r;
}

/* A replaced element with both natural sizes. css-images-3 §4.1: "if an object has a degenerate natural aspect
   ratio (at least one part being zero or infinity), it is treated as having no natural aspect ratio", so a 0
   by 0 object (§15.4.2's fourth rule) has two sizes and no ratio. The ratio is derived here so no §10.3.2 arm
   multiplies by a degenerate one. */
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

/* ---- HTML §4.8.3 "The img element": what an img element represents ---------------------------------------
   §15.4.2's rule list is written in these terms. "What an img element represents depends on the src attribute
   and the alt attribute"; the cases below follow §4.8.3's order. */
typedef enum {
    IMG_REPRESENTS_IMAGE = 0,   /* "the element represents the element's image data" */
    IMG_REPRESENTS_TEXT,        /* "the element represents the text given by the alt attribute" */
    IMG_REPRESENTS_NOTHING      /* "the element represents nothing" */
} ImgRepresents;

/* Attribute presence and emptiness are two questions §4.8.3 asks separately ("set to the empty string" versus
   "not set"). Lexbor's `get_attribute` answers NULL for both, so presence goes through `has_attribute`, as
   core/html/html_image.c's `complete` does for `src`. */
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
    /* "If the image is available and the user agent is configured to display that image, then the element
       represents the element's image data." This agent is configured to display images (it runs html_image.h's
       state machine), so availability decides: §4.8.4.3's partially or completely available. */
    bool available = state == HTML_IMAGE_PARTIALLY_AVAILABLE || state == HTML_IMAGE_COMPLETELY_AVAILABLE;

    if (has_src) {
        if (available) return IMG_REPRESENTS_IMAGE;
        /* "If the src attribute is set and the alt attribute is set to the empty string … Otherwise, the
           element represents nothing." */
        if (has_alt && !alt_text) return IMG_REPRESENTS_NOTHING;
        /* "If the src attribute is set and the alt attribute is set to a value that isn't empty … Otherwise,
           the element represents the text given by the alt attribute." */
        if (alt_text) return IMG_REPRESENTS_TEXT;
        /* "If the src attribute is set and the alt attribute is not …": that case gives the element no
           "represents" except for an empty `src` ("represents nothing"). §15.4.2's second rule catches such an
           element by "the element has no alt attribute" before asking, so this answer is not load-bearing. */
        return IMG_REPRESENTS_NOTHING;
    }
    /* "If the src attribute is not set and either the alt attribute is set to the empty string or the alt
       attribute is not set at all: The element represents nothing. Otherwise: The element represents the text
       given by the alt attribute." */
    return alt_text ? IMG_REPRESENTS_TEXT : IMG_REPRESENTS_NOTHING;
}

/* ---- HTML §15.4.2 "Images", the `img` half ---------------------------------------------------------------
   "User agents are expected to render img elements and input elements whose type attributes are in the Image
   Button state, according to the first applicable rules from the following list", so the arms are ordered.
   Rules 1 and 2 apply to both elements, rules 3 and 4 open "If the element is an img element", and rule 5 opens
   "If the element is an input element"; so the four arms below are the whole list for an `img`. The `input`
   half is `replaced_element_of`'s `input` arm. */
static ReplacedElement rep_img(JSContext *ctx, lxb_dom_element_t *el)
{
    bool complete = false;
    HtmlImageState state = html_image_current_request_state(ctx, el, &complete);
    ImgRepresents represents = rep_img_represents(el, state);

    /* Rule 1: "If the element represents an image — the user agent is expected to treat the element as a
       replaced element and render the image according to the rules for doing so defined in CSS."
       The natural dimensions are read from the image header, not decoded: §4.8.4.3.5's supported-format arm
       needs only to "determine image request's image's width and height" (core/image/image_header.h). They are
       asked of core/html/html_image.h, which applies §4.8.4.3's pixel-density correction; not re-derived here. */
    if (represents == IMG_REPRESENTS_IMAGE) {
        double w = 0.0, h = 0.0;
        bool have = html_image_natural_dimensions(ctx, el, &w, &h);

        /* Rule 1 is reached when rep_img_represents saw an available state, and the dimensions exist when
           html_image.c saw one: two reads of one record, asserted to agree. */
        DCHECK(have,
               "HTML §15.4.2's first rule reached an `img` that REPRESENTS AN IMAGE and whose current request "
               "reports NO density-corrected natural width and height — §4.8.4.3 defines the two available "
               "states as having obtained \"at least the image dimensions\", so representing an image and "
               "having them are the same fact read through two components, and this is those two disagreeing");
        return rep_sized(w, h);
    }

    /* Rule 2: "If the element does not represent an image and either: the user agent has reason to believe the
       image will become available and be rendered in due course, or the element has no alt attribute, or the
       Document is in quirks mode and the element already has natural dimensions … the user agent is expected
       to treat the element as a replaced element whose content is the text that the element represents, if
       any." With no natural dimensions, CSS 2.1 §10.3.2's last arm sizes it 300 x 150.
       The first disjunct is answered from the request at this moment, as a real browser does while a fetch is
       outstanding; it turns false when the reply lands broken. It is §4.8.3's `complete` getter (computed by
       core/html/html_image.c), not "the current URL is non-empty": §4.8.4.3.5 sets that URL after a microtask,
       so a layout in the same task as `img.src = u` (the `sizes="auto"` case) would misread a loading image.
       The third disjunct needs a decision: HTML's parenthetical counts dimension attributes and CSS rules as
       natural dimensions, but css-images-3 §4.1 says natural dimensions "are not a function of the context in
       which the object is used". Under §4.1 the disjunct is vacuous here (rule 1's assert makes representing an
       image and having natural dimensions one fact); under HTML it is live for quirks-mode images. */
    if (!complete) return rep_bare();
    if (!rep_attr_present(el, "alt")) return rep_bare();

    /* Rule 2's third disjunct belongs here and is not asked, so the quirks-mode case is asserted unreachable.
       Building it needs HTML §4.8.3 "The img element"'s dimension attribute source shared (it is `static` in
       core/css/css_presentational_hints.c, the only walk of §4.8.4.3.9 step 5.9) and the reading above settled.
       Reading a computed `width` from here does not cycle: core/css/css_property_applies.c calls
       `replaced_element_of` from `css_resolved_value`, not from `css_computed_value`. */
    DCHECK(lxb_dom_interface_node(el)->owner_document->compat_mode != LXB_DOM_DOCUMENT_CMODE_QUIRKS,
           "HTML §15.4.2 \"Images\"' second rule reached a QUIRKS-MODE `img` and this engine cannot ask that "
           "rule's THIRD DISJUNCT — \"the Document is in quirks mode, and the element already has natural "
           "dimensions (e.g., from the dimension attributes or CSS rules)\" — so the rule this element is "
           "about to be given below was not derived. The disjunct OUTRANKS rules 3 and 4, and the answers "
           "differ wherever it holds: rule 2 makes the element REPLACED, which is what lets HTML §15.4.3's "
           "`width` hint reach it at all — CSS 2.1 §10.2 \"Content width: the 'width' property\" applies to "
           "\"all elements but non-replaced inline elements\" — where rule 3 makes it a NON-REPLACED phrasing "
           "element whose `width` does not apply and rule 4 gives it natural dimensions of 0. BUILD it here");

    /* Rule 3: "If the element is an img element that represents some text and the user agent does not expect
       this to change — the user agent is expected to treat the element as a non-replaced phrasing element
       whose content is the text." A broken image with an `alt` is the common case. `width` then does not
       apply (CSS 2.1 §10.2), so CSSOM §9 resolves it to the computed `auto` (core/css/css_property_applies.c). */
    if (represents == IMG_REPRESENTS_TEXT) return rep_not();

    /* Rule 4: "If the element is an img element that represents nothing and the user agent does not expect
       this to change — the user agent is expected to treat the element as a replaced element whose natural
       dimensions are 0. (In the absence of further styles, this will cause the element to essentially not be
       rendered.)" */
    DCHECK(represents == IMG_REPRESENTS_NOTHING,
           "HTML §15.4.2's fourth rule was reached for an `img` that represents something other than nothing "
           "— the three preceding rules cover an image and text, and §4.8.3 gives an img exactly those three "
           "answers, so this is the two lists having come apart");
    return rep_sized(0.0, 0.0);
}

/* ---- HTML §4.12.5 "The canvas element": one dimension of the element's bitmap ------------------------------
   A canvas whose context mode is none has a bitmap "with a natural width equal to the numeric value of the
   element's width attribute and a natural height equal to the numeric value of the element's height
   attribute", parsed by the rules for non-negative integers, "If an attribute is missing, or if parsing its
   value returns an error, then the default value must be used instead. The width attribute defaults to 300,
   and the height attribute defaults to 150."
   The range is the IDL's (`[CEReactions] attribute unsigned long width`), so the value is asked of
   core/dom/element.h's HTML §2.6.1 "Reflecting content attributes in IDL attributes" implementation; that keeps
   `canvas.width` and the box's natural width one number. Lexbor answering NULL for absent and empty is fine
   here: §4.12.5 sends both to the default. */
static double rep_canvas_dimension(lxb_dom_element_t *el, const char *name, long long dflt)
{
    size_t n = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &n);

    return (double)element_reflect_ulong_value((const char *)v, n, dflt, true, 0, 0, false);
}

/* HTML §8.1.3.4 "Enabling and disabling scripting"'s question asked of a node: "The object implements Node,
   and object's node document's browsing context is null." `document_active_realm_of` answers NULL for exactly
   those documents (a `DOMParser` parse, `createHTMLDocument`, an XHR `responseXML`, a `<template>`'s contents
   owner, a destroyed navigable's document). The canvas and img arms both need it. It is the element's own
   document's realm, never the running one: a classification is a fact about the element's document. */
static JSContext *rep_scripting_realm(lxb_dom_node_t *n)
{
    DCHECK(n != NULL && n->owner_document != NULL,
           "HTML §8.1.3.4's scripting question was asked about a node with no owner document — every node this "
           "engine mints belongs to the document that created it");
    return document_active_realm_of(lxb_dom_interface_node(n->owner_document));
}

ReplacedElement replaced_element_of(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n;
    JSContext *dctx;

    DCHECK(el != NULL, "HTML §15.4's replaced-element question was asked about no element");
    n = lxb_dom_interface_node(el);
    /* HTML §15.4 "Replaced elements", first sentence: "The following elements can be replaced elements: audio,
       canvas, embed, iframe, img, input, object, and video." Every other element is not one; a `<select>` is a
       §15.5 "Widgets" box with its own sizing rule. */
    if (!lxb_html_tree_node_is(n, LXB_TAG_IMG) && !lxb_html_tree_node_is(n, LXB_TAG_IFRAME) &&
        !lxb_html_tree_node_is(n, LXB_TAG_EMBED) && !lxb_html_tree_node_is(n, LXB_TAG_VIDEO) &&
        !lxb_html_tree_node_is(n, LXB_TAG_AUDIO) && !lxb_html_tree_node_is(n, LXB_TAG_CANVAS) &&
        !lxb_html_tree_node_is(n, LXB_TAG_OBJECT) && !lxb_html_tree_node_is(n, LXB_TAG_INPUT))
        return rep_not();

    /* §15.4.1 "Embedded content", first sentence: "The embed, iframe, and video elements are expected to be
       treated as replaced elements." An embedded document, "such as the iframe element in HTML", is "an example
       of an object with no natural dimensions at all" (css-images-3 §4.1), so its box is CSS 2.1 §10.3.2's 300 x
       150, the number core/frame/viewport.c derives a child navigable's viewport from. */
    if (lxb_html_tree_node_is(n, LXB_TAG_IFRAME)) return rep_bare();
    /* An `embed` is a replaced element with no natural dimensions, on every route through HTML §4.8.6 "The
       embed element" in an agent without plugins. Not potentially active: "the embed element represents
       nothing" (its third condition, an ancestor `object` "not showing its fallback content", is always false
       because the `object` arm below always shows fallback). Potentially active: every non-null arm of
       "determining the type of the content" needs a plugin, HTML §2.1.6 "Plugins" lets an agent have none, and
       "Display no plugin for element" ends "element now represents nothing".
       Representing nothing, it has no object to have natural dimensions, and §15.4.2's zero-size rule governs
       only `img` and Image Button `input`; so CSS 2.1 §10.3.2's last arm gives 300px and §10.6.2's 150. The box
       holds "an indication that no plugin could be found", so 0 by 0 would also be wrong.
       Named residual: §4.8.6's embed element setup steps are not built. Their outcome here is the answer above
       either way, but their request (destination "embed", mode "navigate") is an endpoint. Next: build them on
       the embed task source with their `load` event and load-event delay. Absence shows as an `<embed src=…>`'s
       address never reaching the @H endpoint surface. */
    if (lxb_html_tree_node_is(n, LXB_TAG_EMBED)) return rep_bare();

    /* A `video` is a replaced element with no natural dimensions. HTML §4.8.8 "The video element": the natural
       width is the poster frame's, "otherwise, it is the natural width of the video resource, if that is
       available; otherwise the natural width is missing", and likewise the height. No poster frame exists
       (`poster` is a reflection and nothing fetches it), and `videoWidth`/`videoHeight` ("0 if the dimensions
       are not known") are unknown at every ready state: the mock device in core/html/media_element.h states a
       duration and ranges but no frame size. §4.8.8's own default object size is 300 by 150.
       Named residual: a video whose natural dimensions are known is not covered. Next: `videoWidth` and
       `videoHeight` over a frame size the mock device states, making this arm a `rep_sized`. Absence shows as
       a `<video>` with a set width laying out 150 CSS pixels tall with no ratio. */
    if (lxb_html_tree_node_is(n, LXB_TAG_VIDEO)) return rep_bare();

    /* A `canvas`. §15.4.1: "A canvas element that represents embedded content is expected to be treated as a
       replaced element … Other canvas elements are expected to be treated as ordinary elements in the
       rendering model." Per §4.12.5 "The canvas element", it represents embedded content "if scripting is
       enabled for the canvas element, and if support for canvas elements has been enabled"; support is enabled
       in this agent, so scripting (§8.1.3.4, per node) is the conjunct that varies.
       "The natural dimensions of the canvas element when it represents embedded content are equal to the
       dimensions of the element's bitmap". In context mode none that bitmap's size is the two attributes; in
       context mode 2d (core/html/html_canvas_element.c's `getContext`), §4.12.5's set bitmap dimensions writes
       the attributes back to match, so the attributes remain the natural dimensions. A zero is a real natural
       dimension, and `rep_sized` drops the degenerate ratio.
       Named residual: context mode placeholder is not covered; only `transferControlToOffscreen`, which this
       agent lacks with `OffscreenCanvas`, enters it. Next: build those, and read the OffscreenCanvas's bitmap
       here. Absence would show as such a canvas reporting its content attributes as natural dimensions. */
    if (lxb_html_tree_node_is(n, LXB_TAG_CANVAS)) {
        if (rep_scripting_realm(n) == NULL) return rep_not();
        return rep_sized(rep_canvas_dimension(el, "width", 300), rep_canvas_dimension(el, "height", 150));
    }

    /* An `object` represents its fallback content here, one of HTML §4.8.7 "The object element"'s own
       outcomes. §15.4.1: "An object element that represents an image, plugin, or its content navigable is
       expected to be treated as a replaced element. Other object elements are expected to be treated as
       ordinary elements in the rendering model." §4.8.7 jumps to fallback when the element is not rendered or
       its document has no browsing context, and when "the resource is not yet available"; nothing here queues
       that fetch, so it never restarts. An `object` without `data` reaches the same label: "Fallback: The
       object element represents the element's children."
       Named residual: §4.8.7's fetch is not built; an agent that has not fetched takes this jump, so the arm
       is right. Its request (destination "object", mode "navigate") is an endpoint. Next: build §4.8.7's steps
       on the DOM manipulation task source with `error`/`load` tasks and the load-event delay. Absence shows as
       an `<object data=…>`'s address never reaching the @H endpoint surface. */
    if (lxb_html_tree_node_is(n, LXB_TAG_OBJECT)) return rep_not();

    /* No `audio` is a replaced element here. HTML §15.4.1 "Embedded content": "The audio element, when it is
       exposing a user interface, is expected to be treated as a replaced element about one line high … When an
       audio element is not exposing a user interface, the user agent is expected to force its display property
       to compute to none, irrespective of CSS rules." This agent exposes no media user interface (HTML
       §4.8.11.13 "User interface" leaves that to the agent), with or without `controls`, so `controls` is not
       read.
       Named residual: §15.4.1's forced `display: none` is not covered; a classification is not a computed
       value. Next: core/css/css_computed_value.c forces it, overriding the cascade. Absence shows as
       `getComputedStyle(audio).display` answering `inline` and the `<audio>` taking part in a line box. */
    if (lxb_html_tree_node_is(n, LXB_TAG_AUDIO)) return rep_not();
    /* An `input` is governed by one of §15's two rendering mechanisms, chosen by its `type`. §15.4.2's list
       covers only "input elements whose type attributes are in the Image Button state" (`INPUT_STATE_IMAGE`
       from core/html/html_form.h's `html_form_input_state`). Every other state is a §15.5 "Widgets" box (e.g.
       §15.5.10's checkbox "inline-block" box, §15.5.12's buttons via §15.5.3 "Button layout"), not replaced.
       Named residual: §15.5.3's as-if clauses ("If the element is absolutely-positioned, then for the purpose
       of the CSS visual formatting model, act as if the element is a replaced element.", and the same for
       `align-self: normal`) are per purpose, not a classification. Next: a per-purpose replaced question in
       core/layout/used_value.c where CSS 2.1 §10.3.7 versus §10.3.8 is chosen. Absence shows as an absolutely
       positioned `<input type=submit>` with `auto` left, width and right taking §10.3.7's shrink-to-fit width. */
    if (lxb_html_tree_node_is(n, LXB_TAG_INPUT)) {
        HtmlInputState state = html_form_input_state(n);

        /* `INPUT_STATE_NONE` answers "not an `input`", which the tag dispatch already excluded; asserted rather
           than folded into the state test, where it would silently take the widget arm. */
        DCHECK(state != INPUT_STATE_NONE,
               "§4.10.5.1 \"States of the type attribute\"'s state was read as NONE for an element HTML §15.4's "
               "own list matched as an `input` — NONE is not one of that section's states but the answer to "
               "whether the element is an `input`, so this is the tag dispatch here and the tag test in "
               "core/html/html_form.c disagreeing about one element");
        if (state != INPUT_STATE_IMAGE) return rep_not();
        /* Always fatal: past a compiled-out assert, release would read an `img` image request off an `input`'s
           wrapper and feed the result into §10.3.2. That is a data-integrity failure (check.h's `CHECK`). */
        CHECK_FAIL("HTML §15.4.2 \"Images\"' rules were reached for an `input` in §4.10.5.1.19 \"Image Button state "
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

    /* Every other tag on §15.4's list has returned or failed fatally by now in both builds, so this is a
       `DCHECK` about the dispatch's own logic. */
    DCHECK(lxb_html_tree_node_is(n, LXB_TAG_IMG),
           "the element dispatch above admitted an element §15.4's list does not name — the guard and this "
           "chain are one list and they have come apart");
    /* §15.4.2's rules read the image request, which lives on the element's wrapper and is per flow; the realm
       is the element's own document's, from the function the canvas arm uses. */
    dctx = rep_scripting_realm(n);
    if (dctx == NULL)
        /* Always fatal: release would otherwise pass a NULL realm to core/html/html_image.c. Unlike the
           canvas arm, §15.4.2 states no outcome for an `img` nothing renders, so no classification is made. */
        CHECK_FAIL("HTML §15.4.2 \"Images\"' rules were asked about an `img` in a document NO NAVIGABLE PRESENTS — "
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
