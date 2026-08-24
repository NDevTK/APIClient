/* CSS 2.1 §10 — the used value of a box-model length. See used_value.h for the contract, for why the BOX TYPE
   is the first question, and for what the root element's `width: auto` is blocked on. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/frame/screen.h"
#include "core/frame/viewport.h"
#include "core/layout/block_flow.h"
#include "core/layout/replaced_element.h"
#include "core/layout/used_value.h"

/* CSS 2.1 §10.3's OWN LIST OF BOX TYPES, which is the list of algorithms `width` has. The names are the
   spec's, and there is one member per section because the sections DISAGREE at exactly the arms this component
   computes. §10.3.5 and §10.3.7 used to be one member here on the ground that a non-auto size and a non-auto
   margin read the same in both; that is true and it was still the wrong shape, because a computed `auto`
   MARGIN does not — §10.3.5 and §10.3.9 both say outright that its used value is 0, while §10.3.7 solves it
   from a constraint equation between `left` and `right`. One member for the two turned a rule this component
   can state into a crash it had no way to tell apart from one it cannot. */
typedef enum {
    UV_BOX_INLINE = 0,     /* §10.3.1 / §10.6.1 — an inline box; `width` and `height` do not apply to it */
    UV_BOX_BLOCK_FLOW,     /* §10.3.3 / §10.6.3 — block-level, in normal flow: the constraint equation's box */
    UV_BOX_FLOAT,          /* §10.3.5 — floating: shrink-to-fit when auto, `auto` margins are 0 */
    UV_BOX_ABS,            /* §10.3.7 — absolutely positioned: the equation between `left`, `width`, `right` */
    UV_BOX_INLINE_BLOCK,   /* §10.3.9 — shrink-to-fit when auto, `auto` margins are 0 */
    UV_BOX_TABLE,          /* CSS 2.1 §17.5 — the table's own width and height algorithms, not §10's */
    UV_BOX_ITEM            /* a flex or grid item: css-flexbox §9.7 / css-grid sizes it, and §10 does not */
} UvBox;

static char *uv_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

static bool uv_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = uv_computed(el, name);
    bool same = strcmp(v, kw) == 0;

    free(v);
    return same;
}

/* THE SAME QUESTION OF A LENGTH-VALUED PROPERTY, which is a different entry and not a different spelling: a
   length's computed value is a `CssPx` carrying the environment fact a `50vw` or a snapped border width
   derives from (core/css/css_computed_value.h), so the keyword arm is one of three answers rather than the
   whole of it. §10's rules branch on `auto` and on `none` constantly, and this is how they ask. */
static bool uv_length_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    CssLength len = css_computed_length(el, name);

    return len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, kw) == 0;
}

/* css-display §2.4's table display types, plus the two `<display-inside>`/`<display-legacy>` spellings that
   generate a table box. CSS 2.1 §17.5 owns every one of their sizes. */
static bool uv_display_is_table(const char *d)
{
    static const char *const TABLE[] = {
        "table", "inline-table", "table-row-group", "table-header-group", "table-footer-group",
        "table-row", "table-cell", "table-column-group", "table-column", "table-caption",
    };
    unsigned i;

    for (i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++)
        if (strcmp(TABLE[i], d) == 0) return true;
    return false;
}

static bool uv_display_is_flex_or_grid(const char *d)
{
    return d != NULL && (strcmp(d, "flex") == 0 || strcmp(d, "inline-flex") == 0 ||
                         strcmp(d, "grid") == 0 || strcmp(d, "inline-grid") == 0);
}

/* THE BOX TYPE, in the order the questions have to be asked. Each test is a fact about the element that makes
   the LATER tests inapplicable, which is why the order is the spec's and not a preference:
     - a table box is a table box whatever else is true of it;
     - a child of a flex or grid container is an ITEM, and being one is what makes `float` compute to `none`
       for it (css-flexbox §3), so the item test precedes the float test;
     - an ABSOLUTELY POSITIONED child of a flex container is NOT a flex item (css-flexbox §4.1), so the
       out-of-flow test precedes the item test;
     - and `display` is read last because css_computed_value.c has already BLOCKIFIED it for a float, for an
       absolutely positioned box and for a flex item, so by here it can no longer say `inline` for any of them.
*/
static UvBox uv_box_kind(lxb_dom_element_t *el)
{
    char *display = uv_computed(el, "display");
    char *parent_display;
    UvBox kind;

    if (uv_display_is_table(display)) { free(display); return UV_BOX_TABLE; }
    /* css-position §2: `absolute` and `fixed` take the box OUT OF FLOW. `relative` and `sticky` do not — a
       relatively positioned box is laid out in normal flow and then OFFSET, so §10.3.3 is still its section. */
    if (uv_computed_is(el, "position", "absolute") || uv_computed_is(el, "position", "fixed")) {
        free(display);
        return UV_BOX_ABS;
    }
    parent_display = css_box_parent_display(lxb_dom_interface_node(el));
    if (uv_display_is_flex_or_grid(parent_display)) {
        free(parent_display);
        free(display);
        return UV_BOX_ITEM;
    }
    free(parent_display);
    if (!uv_computed_is(el, "float", "none")) { free(display); return UV_BOX_FLOAT; }
    if (strcmp(display, "inline") == 0)            kind = UV_BOX_INLINE;
    else if (strcmp(display, "inline-block") == 0) kind = UV_BOX_INLINE_BLOCK;
    else                                           kind = UV_BOX_BLOCK_FLOW;
    free(display);
    return kind;
}

/* ---- the conjuncts a non-auto size still has to clear ----------------------------------------------------- */

/* CSS 2.1 §10.4 and §10.7: "The used value of width is then clamped by min-width and max-width" — a SECOND
   pass over the whole of §10.3, run with the clamped value substituted. Both are at their initial values on
   almost every element (`min-width: auto`, whose automatic minimum size css-sizing §4.1 defines only for a
   flex or grid item, and which is 0 for every box this arm reaches; `max-width: none`), so the clamp has
   nothing to apply — and that is ASSERTED here rather than assumed, because a declared `max-width` silently
   ignored is a used width that is simply wrong. */
static void uv_require_unclamped(lxb_dom_element_t *el, bool vertical)
{
    CssLength lmn = css_computed_length(el, vertical ? "min-height" : "min-width");
    CssLength lmx = css_computed_length(el, vertical ? "max-height" : "max-width");
    bool no_min = (lmn.kind == CSS_LENGTH_ABSOLUTE && lmn.px.px == 0.0) ||
                  (lmn.kind == CSS_LENGTH_KEYWORD && strcmp(lmn.keyword, "auto") == 0);
    bool no_max = lmx.kind == CSS_LENGTH_KEYWORD && strcmp(lmx.keyword, "none") == 0;

    if (no_min && no_max) return;
    DFAIL("CSS 2.1 §10.4 / §10.7 CLAMP the used size by `min-width`/`max-width` (`min-height`/`max-height`), "
          "and this element declares one that is not its initial value, so the tentative used value §10.3 "
          "produced is not the answer. The clamp is not a `min`/`max` over two numbers: the spec runs the "
          "WHOLE of §10.3 again with the clamped size substituted, which re-solves whichever margin was `auto`. "
          "BUILD that second pass, and with it the two limits' own used values — a PERCENTAGE `max-width` "
          "resolves against the containing block's width and `min-width: auto` is css-sizing §4.1's AUTOMATIC "
          "MINIMUM SIZE, which is a content-based minimum and therefore a real layout");
}

/* THE FOUR TERMS OF css-sizing §5's CONVERSION BETWEEN THE TWO BOXES, on one axis, and the ONE place that
   computes them. §5 converts a border box into a content box by "subtracting the border and padding widths of
   the respective sides", and every arm below needs that sum or half of it — the `border-box` size ADDS it, the
   padding edge subtracts it and adds the paddings back. Two copies of a four-term sum are two places for the
   terms to come to disagree, which is the only way the padding edge and the size it is derived from can stop
   describing the same box. */
typedef struct {
    CssPx padding;   /* padding-left + padding-right, or padding-top + padding-bottom */
    CssPx border;    /* border-left-width + border-right-width, or the top/bottom pair */
} UvSurround;

static UvSurround uv_surround(lxb_dom_element_t *el, bool vertical)
{
    static const char *const PADDINGS[2][2] = {
        { "padding-left", "padding-right" }, { "padding-top", "padding-bottom" },
    };
    static const char *const BORDERS[2][2] = {
        { "border-left-width", "border-right-width" }, { "border-top-width", "border-bottom-width" },
    };
    int axis = vertical ? 1 : 0, i;
    UvSurround s = { css_px(0.0), css_px(0.0) };

    for (i = 0; i < 2; i++) {
        CssLength len = css_computed_length(el, BORDERS[axis][i]);

        DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
               "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
               "§3.3's `Computed value:` line is `absolute length, snapped as a border width` and every arm of "
               "that derivation produces one — 0 for a `none`/`hidden` style, 1/3/5px for the three keywords, "
               "the absolutized length otherwise — so a percentage or a keyword here is a rule that did not "
               "run");
        s.border  = css_px_add(s.border, len.px);
        s.padding = css_px_add(s.padding, used_value_px(el, PADDINGS[axis][i]));
    }
    DCHECK(s.padding.px >= 0.0 && s.border.px >= 0.0,
           "css-sizing §5's conversion between the content box and the border box was handed a NEGATIVE "
           "surround. CSS 2.1 §8.4 states outright that 'negative values for padding properties are not "
           "allowed' and css-backgrounds-3 §3.3's <line-width> is a non-negative <length>, so lexbor drops "
           "either declaration — a negative here is a used value this component derived rather than one an "
           "author wrote, and every box it is a term of would be smaller than the box it contains");
    return s;
}

/* §5's SUM, as one function rather than as the expression `s.padding + s.border` written twice. That is not
   tidiness: one direction of the conversion ADDS this sum and the other SUBTRACTS it, and IEEE addition is not
   associative, so two spellings of the same sum can differ by an ulp and leave a content box a few times 10^-17
   below zero — a real disagreement and a rounding artifact then look identical, and the assert that is supposed
   to catch the first would be tripped by the second. Subtracting the very number that was added cancels
   exactly, which is what makes that assert mean what it says. */
static CssPx uv_surround_total(UvSurround s)
{
    return css_px_add(s.padding, s.border);
}

/* css-sizing §5's `box-sizing`, asked as the ONE question both directions of its conversion turn on. The
   grammar is `content-box | border-box` and lexbor validates the declaration against it, so a third value is a
   cascade layer that answered without one — asserted HERE, at the classification, rather than by whichever arm
   a not-content-box value happened to fall into. */
static bool uv_is_border_box(lxb_dom_element_t *el)
{
    char *v = uv_computed(el, "box-sizing");
    bool border = strcmp(v, "border-box") == 0;

    DCHECK(border || strcmp(v, "content-box") == 0,
           "a `box-sizing` computed to something that is neither of the two keywords css-sizing §5's "
           "`content-box | border-box` grammar admits, and its `Computed value:` line is `specified keyword` — "
           "so nothing between the declaration and here had a rule that could produce a third one");
    free(v);
    return border;
}

/* css-sizing §5's `box-sizing: border-box`, and the one sentence in it that decides what this function
   returns: "Used values, AS EXPOSED FOR INSTANCE THROUGH getComputedStyle(), also refer to the border box."
 * SO THE ANSWER IS THE BORDER BOX'S SIZE, NOT THE CONTENT BOX'S. What stood here said the opposite — that
 * "CSS 2.1 §10.2 and CSSOM §9 both mean the CONTENT width", so the used value was the declared one minus the
 * paddings and the border widths — and it crashed rather than computing that. Both halves were wrong. The
 * subtraction §5 describes ("The content width and height are calculated by subtracting the border and padding
 * widths of the respective sides from the specified <length-percentage>") produces the CONTENT box, which is
 * the number CSS 2.1 §10.3.3's equation and CSSOM VIEW's padding edge want; the number CSSOM §9 exposes is the
 * border box, which is why `getComputedStyle(el).width` is `100px` and not `80px` for a `box-sizing:
 * border-box; width: 100px; padding: 10px` box in every user agent. A crash there took out most of the modern
 * web, which sets `box-sizing: border-box` on `*`.
 * IT IS STILL NOT THE DECLARED LENGTH VERBATIM, and the exception is the one §5 spells out with its own
 * example: the content box floors at zero, so when the paddings and borders alone exceed the declared
 * border-box size the border box GROWS to hold them — "the border-box size ends up at 120px, even though
 * width: 100px is specified for the border box". The used border-box size is therefore the LARGER of the two,
 * and computing it needs exactly the four terms §10.3.3 was waiting on, two of which are the border widths. */
static CssPx uv_border_box_size(lxb_dom_element_t *el, CssPx declared, bool vertical)
{
    UvSurround s = uv_surround(el, vertical);

    DCHECK(uv_is_border_box(el),
           "css-sizing §5's border-box arm was reached for a box whose `box-sizing` is `content-box` — the "
           "declared length is then the CONTENT box's and this function would report it as the border box's");
    return css_px_max(declared, uv_surround_total(s));
}

/* THE CONTENT BOX'S EXTENT on one axis, which is css-sizing §5's conversion run in the one direction two
   callers need it in: CSSOM VIEW §6's padding edge, and §10.1's containing block (the content EDGE of the
   nearest block container ancestor). Under `content-box` the used size already IS the content box; under
   `border-box` it is the border box, and §5 converts by "subtracting the border and padding widths of the
   respective sides".
   `s` IS THE CALLER'S OWN SURROUND, passed in rather than recomputed, so the sum subtracted here is byte-
   identical to the one the caller adds back — see the note on uv_surround_total for why that is the difference
   between an assert that means what it says and one a rounding artifact can trip. */
static CssPx uv_content_size(lxb_dom_element_t *el, bool vertical, UvSurround s)
{
    CssPx used = used_value_px(el, vertical ? "height" : "width");

    if (!uv_is_border_box(el)) return used;
    return css_px_sub(used, uv_surround_total(s));
}

/* ---- CSS 2.1 §10.1's CONTAINING BLOCK ---------------------------------------------------------------------
   Every percentage in §8.3, §8.4 and §10.2 and every `auto` §10.3.3 solves for is stated against this one
   rectangle's WIDTH, so it is the recursion the whole of §10.3 stands on and its base case is the reason the
   recursion terminates. */


/* "The containing block in which the ROOT ELEMENT lives is a rectangle called the initial containing block" —
   the element whose parent is the Document itself. */
static bool uv_is_root(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* §10.1's FIRST CASE, and the base case of the recursion above: "For continuous media, it has the dimensions
   of the VIEWPORT and is anchored at the canvas origin." core/frame/viewport.h owns both halves — the number
   and the fact that the number is a PICKED environment choice rather than a derived one — so what this
   function does is find the realm the viewport is answered per and ask it. The realm is the ELEMENT'S own
   document's, never the running one: a layout is a fact about the document the element is in, and an iframe's
   ICB is 300 CSS pixels wide while its parent's is 1280. */
static CssPx uv_icb_width(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSContext *dctx;

    DCHECK(n->owner_document != NULL,
           "the initial containing block was asked for an element whose node has no owner document — every "
           "node this engine mints belongs to the document that created it");
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    if (dctx == NULL || !viewport_exists(dctx))
        DFAIL("CSS 2.1 §10.1's INITIAL CONTAINING BLOCK 'has the dimensions of the VIEWPORT', and this "
              "element's document is not being presented by any navigable — a DOMParser document, an XHR "
              "`responseXML`, a `<template>`'s contents owner, or the document of a navigable that has been "
              "destroyed. There is no viewport, so there is no ICB, so §10's used values have no rectangle to "
              "resolve against and no LAYOUT ran to produce one. CSSOM §9 does not state this escape: its two "
              "conjuncts are the property's `Applies to:` line and the element's OWN computed `display`, and "
              "both are true here, yet every user agent answers the COMPUTED value (`auto`) because the "
              "element generates no box. BUILD §9's missing conjunct over the predicate that already decides "
              "it — core/dom/element_view.h's `element_view_has_box`, which is defined over exactly this "
              "question — so the resolved value takes §9's computed-value escape before §10 is ever asked");
    return viewport_icb_width(dctx);
}

/* §10.1's four cases, in the spec's own order, answering the ELEMENT whose CONTENT EDGE is the rectangle —
   NULL for the first case, the initial containing block, which is no element's box. Each case this component
   cannot answer crashes naming ITS case and not the neighbouring one, because the three that are missing are
   missing for three different reasons.
   THE BOX IS THE ANSWER AND THE WIDTH IS DERIVED FROM IT, which is the split used_value.h states: §10 needs
   only the width, and §9.4.1's placement needs the box's origin and its child list, so a second walk for the
   second question would be these four cases implemented twice. */
lxb_dom_element_t *used_value_containing_block(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n, *a;

    DCHECK(el != NULL, "§10.1's containing block was asked for with no element");
    n = lxb_dom_interface_node(el);
    if (uv_is_root(n)) return NULL;
    if (uv_computed_is(el, "position", "fixed"))
        DFAIL("CSS 2.1 §10.1's THIRD case: a `position: fixed` box's containing block 'is established by the "
              "VIEWPORT in the case of continuous media'. The rectangle is the one uv_icb_width above already "
              "answers, so the WIDTH is not what is missing — what is missing is everything else about a fixed "
              "box: §10.3.7's constraint equation between `left`, `width` and `right` is what turns that "
              "rectangle into a used width, and its `auto` cases need the STATIC POSITION, which is where the "
              "box would have been in normal flow and therefore a real flow layout. BUILD §10.3.7 over §9.4's "
              "flow layout; this case and the fourth one below are then the same code with a different "
              "rectangle");
    if (uv_computed_is(el, "position", "absolute"))
        DFAIL("CSS 2.1 §10.1's FOURTH case: an absolutely positioned box's containing block is established by "
              "'the nearest ancestor with a position of absolute, relative or fixed', and is formed by that "
              "ancestor's PADDING EDGE — not its content edge, which is the second case's rectangle and the "
              "only one this component computes. Two pieces are missing and they are different: the padding "
              "edge as a RECTANGLE (core/layout/used_value.h computes a padding edge's EXTENT on one axis, and "
              "an extent locates nothing), and §10.1's own exception for an INLINE ancestor, whose containing "
              "block is 'the bounding box around the padding boxes of the first and the last inline boxes' and "
              "is undefined in CSS 2.1 when that ancestor is split across lines. BUILD the box POSITIONS: "
              "§9.4's normal flow places each in-flow box within its containing block, and that is what turns "
              "every extent this component computes into a rectangle");
    /* §10.1's SECOND case: "For other elements, if the element's position is 'relative' or 'static', the
       containing block is formed by the CONTENT EDGE of the nearest BLOCK CONTAINER ANCESTOR BOX." The walk
       asks for a BOX, so an ancestor whose computed `display` is `contents` is stepped over rather than
       answered — it generates none, and §10.1 is asking which box the rectangle is the content edge of. */
    for (a = n->parent; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        lxb_dom_element_t *anc = lxb_dom_interface_element(a);
        char *d = uv_computed(anc, "display");
        bool contents = strcmp(d, "contents") == 0;
        bool container = block_flow_display_is_block_container(d);

        if (container) {
            free(d);
            return anc;
        }
        if (!contents)
            DFAIL("CSS 2.1 §10.1's second case asks for the nearest BLOCK CONTAINER ancestor box, and the "
                  "nearest ancestor that generates a box is not one — its computed `display` makes it a table "
                  "box, a table row or row group, a flex or grid container, an inline box, or a box whose "
                  "`display: none` means it generates nothing at all and neither does this element. Each of "
                  "those establishes a containing block by a DIFFERENT spec: CSS 2.1 §17.5's table layout owns "
                  "the first three, css-flexbox §4.1 and css-grid §9 make a flex or grid container establish "
                  "one for its items (which is why a box whose parent is one is a flex ITEM here and is sized "
                  "by that container's algorithm rather than by §10), and an INLINE ancestor is §10.1's own "
                  "exception — the bounding box around the padding boxes of its first and last inline boxes. "
                  "A `display: none` ancestor is not a missing algorithm at all: this element generates no box "
                  "either, and the answer is CSSOM §9's computed-value escape over element_view.h's has-a-box "
                  "predicate, which uv_icb_width above names in full");
        free(d);
        /* CSS Display §2.8: "a display of contents computes to block on the root element", so the walk can
           never step OVER the root — it is a block container however it was declared. */
        DCHECK(!uv_is_root(a),
               "the containing-block walk stepped over the ROOT ELEMENT as a `display: contents` box. CSS "
               "Display §2.8 makes `contents` compute to `block` on the root, so a root that reached this line "
               "is a computed-value rule that did not run");
    }
    DFAIL("the containing-block walk ran out of ancestors without finding a block container box. Every walk "
          "starts below the ROOT ELEMENT — §10.1's first case answers the root itself — and the root is a "
          "block container whatever it declares (CSS Display §2.8 blockifies it), so an element in a tree with "
          "a root cannot reach here. What can is an element whose ancestors do NOT reach a root element: a "
          "node in a DocumentFragment or a detached subtree, which has no box in any user agent and whose "
          "resolved value is CSSOM §9's computed value for the reason uv_icb_width above states in full");
    return NULL;
}

/* THE CONTAINING BLOCK'S WIDTH — §10.1's first case answered by the viewport, and its second by the CONTENT
   EDGE of the box above. */
CssPx used_value_containing_block_width(lxb_dom_element_t *el)
{
    lxb_dom_element_t *cb = used_value_containing_block(el);

    if (cb == NULL) return uv_icb_width(el);
    return uv_content_size(cb, false, uv_surround(cb, false));
}

CssPx used_value_content_px(lxb_dom_element_t *el, bool vertical)
{
    DCHECK(el != NULL, "a content extent was asked for with no element");
    return uv_content_size(el, vertical, uv_surround(el, vertical));
}

/* ---- CSS 2.1 §10.3.2's LAST ARM AND §10.6.2's, WHICH ARE ONE RECTANGLE STATED TWICE ------------------------
   §10.3.2 "Inline, replaced elements": "Otherwise, if 'width' has a computed value of 'auto', but none of the
   conditions above are met, then the used value of 'width' becomes 300px. If 300px is TOO WIDE TO FIT THE
   DEVICE, UAs should use the width of the largest rectangle that has a 2:1 ratio and fits the device instead."
   §10.6.2 "Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced
   elements in normal flow and floating replaced elements": "Otherwise, if 'height' has a computed value of
   'auto', but none of the conditions above are met, then the used value of 'height' must be set to the height
   of the largest rectangle that has a 2:1 ratio, has a height not greater than 150px, and has a WIDTH NOT
   GREATER THAN THE DEVICE WIDTH."
   The two sentences describe ONE 2:1 rectangle — 300 by 150 — capped by the device, which is why they are one
   function and not a constant written at each of the two arms.

   THE DEVICE IS THE OUTPUT DISPLAY, NOT THE VIEWPORT, and CSS 2.1 says so in its own vocabulary: §9.1.1 "The
   viewport" defines the viewport as "a window or other viewing area ON THE SCREEN", so the screen is the thing
   the window is on and the two are different rectangles. This engine already answers "the device" from
   core/frame/screen.h at the one other place that asks for it — the `device-width` media feature, whose
   evaluator reads that component and whose own header records that it held a second literal 1920 until it
   existed — and one fact answered from two places is the defect CLAUDE.md §per-realm names.
   Reading it from the VIEWPORT would also be a definition of itself here: core/frame/viewport.c derives a
   child navigable's viewport FROM this number, so a default replaced size that was a function of the viewport
   would be a function of the default replaced size.

   THE ANSWER CARRIES NO ENVIRONMENT FACT, AND THAT IS THE ASSERT RATHER THAN AN OMISSION. css_length.h makes
   `CSS_ENV_NONE` a POSITIVE statement — the value's domain is a single point — and it is one here only because
   the device clause does NOT bind: the modelled display is far larger than the rectangle, so the answer is 300
   by 150 at every point of the domain the model admits and there is no arm a page could take. The moment a
   narrower display is modelled that stops being true, and the DCHECK below fires naming what has to be built:
   the device's own dimensions would then have to become a `CssEnvFact` (core/css/css_length.h) with a row in
   core/frame/viewport.c's seam, and that row's source key must be screen.c's own `screen.width` rather than a
   per-document one, because a Screen member IS its own source and a page reading `screen.width` and a page
   measuring an image would otherwise fork two identities for one fact. */
#define UV_DEFAULT_REPLACED_WIDTH  300.0
#define UV_DEFAULT_REPLACED_HEIGHT 150.0

CssPx used_value_default_replaced_size(bool vertical)
{
    /* The largest 2:1 rectangle that fits a `w` by `h` device is `min(w, 2h)` wide, so the clause binds
       exactly when the display is narrower than 300 or shorter than 150. */
    DCHECK(screen_width() >= UV_DEFAULT_REPLACED_WIDTH && screen_height() >= UV_DEFAULT_REPLACED_HEIGHT,
           "CSS 2.1 §10.3.2's \"if 300px is TOO WIDE TO FIT THE DEVICE\" and §10.6.2's \"a width not greater "
           "than the device width\" have become REACHABLE: core/frame/screen.h now models a display smaller "
           "than the 300 x 150 rectangle, so the default replaced size is the largest 2:1 rectangle that fits "
           "it and is a FUNCTION OF THE DEVICE. Two things follow and neither is arithmetic. The size is no "
           "longer a single-point domain, so it must carry the device's dimensions as environment facts — a "
           "`CssEnvFact` row in core/css/css_length.h and a row in core/frame/viewport.c's seam whose member "
           "name is screen.c's own `screen.width`/`screen.height` and NOT a per-document key, because a Screen "
           "member is its own source (core/frame/screen.c) and two spellings of one fact fork one predicate "
           "twice. And core/frame/viewport.c derives a CHILD NAVIGABLE's viewport from this number, so the "
           "viewport would become a function of the device too. BUILD the fact, then this becomes "
           "`css_px_min` over it");
    return css_px(vertical ? UV_DEFAULT_REPLACED_HEIGHT : UV_DEFAULT_REPLACED_WIDTH);
}

/* ---- the used value ------------------------------------------------------------------------------------- */

/* CSS 2.1 §10.3.3's rules 2, 4 and 6 — the used value of a horizontal `auto` margin on a block-level box in
   normal flow whose `width` is NOT `auto`, which is one computation and not three:
     rule 4, "if there is exactly one value specified as 'auto', its used value follows from the equality" —
       the margin takes all the slack;
     rule 6, "if both 'margin-left' and 'margin-right' are 'auto', their used values are equal" — they split
       it, which is what `margin: 0 auto` means;
     rule 2, "if 'width' is not 'auto' and border + padding + width (plus any of 'margin-left' or
       'margin-right' that are not 'auto') is larger than the width of the containing block, then any 'auto'
       values for 'margin-left' or 'margin-right' are, for the following rules, treated as zero" — which is
       exactly the slack being negative.
   THE SIGN TEST RUNS ON THE EXAMPLE, and that is css_length.h's stated layering rather than a shortcut past
   it: the containing block's width may be the viewport's, so `slack < 0` is a question the environment could
   answer either way, and it is decided here on the modelled viewport exactly as Media Queries §4 decides
   `(max-width: 768px)` on the same number. The fact rides the result either way, so the page's own branch on
   the margin still forks both worlds. */
static CssPx uv_block_auto_margin(lxb_dom_element_t *el, const char *opposite)
{
    UvSurround s = uv_surround(el, false);
    CssPx cb = used_value_containing_block_width(el);
    CssPx inner = css_px_add(uv_content_size(el, false, s), uv_surround_total(s));
    bool both = uv_length_is(el, opposite, "auto");
    CssPx other = both ? css_px(0.0) : used_value_px(el, opposite);
    CssPx slack = css_px_sub(css_px_sub(cb, inner), other);

    if (slack.px < 0.0) return css_px(0.0);              /* rule 2 */
    return both ? css_px_scale(slack, 0.5) : slack;      /* rule 6, then rule 4 */
}

/* `opposite` is the OTHER horizontal margin's property name — `margin-right` when this is `margin-left` and
   the reverse — or NULL when this is a vertical margin, which is also how this function knows which pair it
   is in. §10.3.3's over-constraint is a statement about the three horizontal values TOGETHER, so the one that
   is not this margin and is not `width` has to be read. */
static CssPx uv_margin(lxb_dom_element_t *el, const char *opposite, CssLength len, UvBox box)
{
    bool vertical = opposite == NULL;

    if (len.kind == CSS_LENGTH_ABSOLUTE) {
        /* §10.3.3's OVER-CONSTRAINED case is the one configuration in CSS 2.1 in which a non-auto margin's
           used value differs from its computed value: a block-level non-replaced box in normal flow whose
           `width` and both horizontal margins are all non-auto cannot satisfy the equation, and the spec
           resolves it by IGNORING one of the two — `margin-right` when the containing block's `direction` is
           `ltr`, `margin-left` when it is `rtl` — and recomputing it so the equality holds. It crashes on the
           half that is still missing: WHICH margin is ignored is a fact about the containing block's computed
           `direction`, and `direction` is inherited. The recomputed VALUE is no longer missing at all.
           ALL THREE HAVE TO BE NON-AUTO for the box to be over-constrained, which is why the OTHER margin is
           read: "if there is exactly one value specified as 'auto', its used value follows from the equality",
           so `margin-left: 10px; margin-right: auto; width: 100px` is not over-constrained at all and
           `margin-left`'s used value is the 10px it computed to. */
        if (!vertical && box == UV_BOX_BLOCK_FLOW && !uv_length_is(el, "width", "auto") &&
            !uv_length_is(el, opposite, "auto"))
            DFAIL("CSS 2.1 §10.3.3: this is a block-level box in normal flow whose `width` and horizontal "
                  "margins are all non-auto, so its used values are OVER-CONSTRAINED and one horizontal "
                  "margin's used value is NOT its computed value — the spec ignores the specified "
                  "`margin-right` (`margin-left` when the containing block's `direction` is `rtl`) and "
                  "recomputes it to make the constraint equation true. ONE thing is missing and it is which "
                  "margin: it is the containing block's computed `direction`, and `direction` is not among the "
                  "properties css_computed_value.c models, so there is nothing to read it through. THE "
                  "CASCADE'S INHERITANCE IS NO LONGER THE BLOCKER — CSS Cascade §7 is built "
                  "(core/css/css_defaulting.h) and it knows `direction` inherits — and neither is the VALUE: "
                  "the recomputed margin is the containing block's width minus the other six terms, and "
                  "§10.1's chain answers that width. What is left is one row: css-writing-modes gives "
                  "`direction` the `Computed value: specified keyword` line, so it is a row of "
                  "css_computed_models' as-specified arm and a row of css_shorthand_complete_for, and this "
                  "crash then becomes a branch");
        return len.px;
    }
    /* CSS 2.1 §8.3: a percentage margin "is calculated with respect to the WIDTH of the generated box's
       containing block. NOTE THAT THIS IS TRUE FOR 'margin-top' AND 'margin-bottom' AS WELL." So the axis is
       not asked: a vertical margin resolves against the same horizontal measure, which is the counter-intuitive
       half of the rule and the reason this arm takes no `vertical`. */
    if (len.kind == CSS_LENGTH_PERCENTAGE)
        return css_px_scale(used_value_containing_block_width(el), len.pct / 100.0);
    DCHECK(strcmp(len.keyword, "auto") == 0,
           "a margin's computed value is neither a length, nor a percentage, nor `auto` — CSS 2.1 §8.3's "
           "<margin-width> grammar admits exactly those three, and lexbor validates the declaration against "
           "it, so a fourth form here is a serializer that produced something the grammar does not");
    if (vertical) {
        /* §10.6.3: "If 'margin-top', or 'margin-bottom' are 'auto', their used value is 0." */
        if (box == UV_BOX_BLOCK_FLOW) return css_px(0.0);
        DFAIL("a VERTICAL margin computes to `auto` on a box CSS 2.1 §10.6.3 does not cover. §10.6.3 is the "
              "block-level-in-normal-flow section and it is the only one that gives `margin-top: auto` a used "
              "value of 0 outright; §10.6.4 solves the vertical `auto` margins of an ABSOLUTELY POSITIONED box "
              "from its own constraint equation (they centre the box between `top` and `bottom` when both are "
              "given), §10.6.1 says nothing at all about an INLINE box's vertical margins, and css-flexbox §9.6 "
              "gives a flex item's `auto` cross-axis margins the free space. BUILD the section this box's type "
              "names — uv_box_kind above says which it is");
        return css_px(0.0);
    }
    /* THREE SECTIONS SAY THE SAME SENTENCE and one solves an equation instead, which is why the box type is
       what this arm branches on. §10.3.1 (inline), §10.3.5 (floating) and §10.3.9 (inline-block) each state
       outright that "a computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of
       '0'"; §10.3.3 says it too, but only in its rule 5, "if 'width' is set to 'auto', any other 'auto' values
       become '0'" — with a non-auto `width` its `auto` margins take the slack instead. */
    if (box == UV_BOX_INLINE || box == UV_BOX_FLOAT || box == UV_BOX_INLINE_BLOCK) return css_px(0.0);
    if (box == UV_BOX_BLOCK_FLOW)
        return uv_length_is(el, "width", "auto") ? css_px(0.0) : uv_block_auto_margin(el, opposite);
    if (box == UV_BOX_TABLE)
        DFAIL("a horizontal `auto` margin on a TABLE box. CSS 2.1 §17.5.2 derives the table's own width first "
              "— an intrinsic size over its columns — and only then is there a slack for §10.3.3's margin "
              "rules to divide, which is what makes `table { margin: 0 auto }` centre it. BUILD §17.2's "
              "anonymous table-object generation and §17.5.2's algorithms; the margin rule is then the same "
              "code the block-level arm above already runs");
    if (box == UV_BOX_ITEM)
        DFAIL("a horizontal `auto` margin on a FLEX or GRID ITEM, which css-flexbox §9.5 answers before "
              "alignment does: 'if the remaining free space is positive and at least one main-axis auto margin "
              "is on the line, distribute it equally among those margins'. It is the container's FREE SPACE "
              "and not §10.3.3's slack — the two differ because the container has already flexed every item. "
              "BUILD the flex layout algorithm over the container's own used content size");
    DFAIL("a HORIZONTAL margin computes to `auto` on an ABSOLUTELY POSITIONED box, whose used value CSS 2.1 "
          "§10.3.7 solves from its own constraint equation — 'left + margin-left + border + padding + width + "
          "border + padding + margin-right + right = width of containing block' — under the section's own "
          "ordered rules: an `auto` `left` or `right` replaces an `auto` margin with 0 first, both `auto` "
          "margins then get EQUAL values 'unless this would make them negative', and an over-constrained set "
          "ignores one offset depending on the containing block's `direction`. Three things are missing and "
          "none of them is the containing block's WIDTH, which §10.1's chain answers now: the used `left` and "
          "`right` (CSSOM §9's inset arm crashes on the same equation), the STATIC POSITION its `auto` cases "
          "fall back to, which is where the box would have been in normal flow, and `direction`, which is "
          "inherited. BUILD §9.4's flow layout, then §10.3.7 over it");
    return css_px(0.0);
}

/* CSS 2.1 §8.4: a percentage padding "is calculated with respect to the WIDTH of the generated box's
   containing block, EVEN FOR 'padding-top' and 'padding-bottom'" — so, as for a margin, the axis is not asked.
   §8.4's other sentence is the one this component cannot yet be asked to break: "if the containing block's
   width depends on this element, then the resulting layout is undefined in CSS 2.1". No containing block this
   component computes depends on its contents — every one of them is §10.1's second case over a block container
   whose own width came from a declaration or from §10.3.3's equation, and the shrink-to-fit widths that WOULD
   depend on the element crash in uv_size below. */
static CssPx uv_padding(lxb_dom_element_t *el, CssLength len)
{
    if (len.kind == CSS_LENGTH_ABSOLUTE) return len.px;
    DCHECK(len.kind == CSS_LENGTH_PERCENTAGE,
           "a padding's computed value is neither a length nor a percentage. CSS 2.1 §8.4's <padding-width> "
           "grammar has no `auto` and no keyword at all — a padding is a length or a percentage and nothing "
           "else — so this is a value lexbor's own validation should have dropped");
    return css_px_scale(used_value_containing_block_width(el), len.pct / 100.0);
}

/* CSS 2.1 §10.3.3's RULE 5 — "if 'width' is set to 'auto', any other 'auto' values become '0' and 'width'
   follows from the resulting equality", which is the constraint equation
       margin-left + border-left-width + padding-left + width + padding-right + border-right-width +
       margin-right = width of containing block
   solved for the one unknown. Every other term is read back through this component's own arms, so the `auto`
   margins rule 5 zeroes are zeroed by uv_margin and not a second time here.
   §10.4's CLAMP IS PART OF THE ANSWER AND NOT A SEPARATE PASS, for the one limit that is always in effect:
   `min-width`'s initial value is 0 (css-sizing's `auto` is the same 0 for every box this arm reaches), and
   §10.4 step 3 re-runs the rules with it substituted whenever "the resulting width is smaller than
   'min-width'". A negative content box is exactly that case, so the floor is §10.4 running rather than a guard
   against it — uv_require_unclamped is what asserts that no OTHER limit was declared.
   AND THE ANSWER IS THE BORDER BOX under `box-sizing: border-box`, because css-sizing §5 says the used value
   "as exposed for instance through getComputedStyle()" refers to that box. The equation solves for the CONTENT
   width either way — its seven terms are CSS 2.1's, which knows only the content box — so §5's conversion is
   applied to the result rather than to the equation. */
static CssPx uv_block_auto_width(lxb_dom_element_t *el)
{
    UvSurround s;
    CssPx cb;
    CssPx margins, content;

    uv_require_unclamped(el, false);
    s = uv_surround(el, false);
    cb = used_value_containing_block_width(el);
    margins = css_px_add(used_value_px(el, "margin-left"), used_value_px(el, "margin-right"));
    content = css_px_max(css_px_sub(css_px_sub(cb, uv_surround_total(s)), margins), css_px(0.0));
    if (!uv_is_border_box(el)) return content;
    return css_px_add(content, uv_surround_total(s));
}

/* CSS 2.1 §10.3.2 "Inline, replaced elements", ITS FIVE ARMS IN THE SPEC'S OWN ORDER — reached only with a
   computed `width` of `auto`, which is the condition every one of them carries and the caller has established.
   THE ORDER IS LOAD-BEARING AND THE ARMS ARE NOT INDEPENDENT TESTS. Arm 2's second disjunct ("'width' has a
   computed value of 'auto', 'height' has some other computed value, and the element does have an intrinsic
   ratio") can be true at the same time as arm 4's condition, and it comes first: a replaced element with an
   intrinsic width of 50, an intrinsic ratio of 2 and a declared `height: 100px` is 200 wide and not 50.
   THE ANSWER IS THE CONTENT WIDTH. Every term §10.3.2 names is CSS 2.1's, and CSS 2.1 knows only the content
   box; css-sizing §5's `box-sizing` conversion is applied to the RESULT by the caller, exactly as it is to
   §10.3.3's equation. Which is also why arm 2's "(used height)" is read through `uv_content_size` and not
   through `used_value_px` — under `border-box` the latter is the BORDER box (§5's exposed used value), and
   multiplying that by an aspect ratio would be a ratio of two different boxes. */
static CssPx uv_replaced_width(lxb_dom_element_t *el, const ReplacedElement *rep)
{
    bool h_auto = uv_length_is(el, "height", "auto");

    /* "If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic
       width, then that intrinsic width is the used value of 'width'." */
    if (h_auto && rep->has_width) return rep->width;
    /* "If 'height' and 'width' both have computed values of 'auto' and the element has no intrinsic width, but
       does have an intrinsic height and intrinsic ratio; or if 'width' has a computed value of 'auto',
       'height' has some other computed value, and the element does have an intrinsic ratio; then the used
       value of 'width' is: (used height) * (intrinsic ratio)."
       THIS DOES NOT RECURSE, and the reason is the spec's conditions rather than a guard here: §10.6.2's own
       ratio arm — the one that would ask back for the used width — runs only when its FIRST arm did not, and
       that first arm takes every case in which both sizes are `auto` and there is an intrinsic height. The
       both-auto disjunct here REQUIRES an intrinsic height, so it is precisely the case §10.6.2 answers
       without asking; and the other disjunct has a non-auto `height`, which §10.6.2 never reaches at all. */
    if ((h_auto && !rep->has_width && rep->has_height && rep->has_ratio) || (!h_auto && rep->has_ratio)) {
        DCHECK(!h_auto || rep->has_height,
               "CSS 2.1 §10.3.2's intrinsic-ratio arm was reached with both sizes `auto` and NO intrinsic "
               "height, which is the one shape that would make it and §10.6.2's ratio arm ask each other for "
               "the answer forever — the arm's own condition names the intrinsic height, so reaching here "
               "without one is that condition and this test having come apart");
        return css_px_scale(uv_content_size(el, true, uv_surround(el, true)), rep->ratio);
    }
    /* "If 'height' and 'width' both have computed values of 'auto' and the element has an intrinsic ratio but
       no intrinsic height or width, then the used value of 'width' is UNDEFINED IN CSS 2.1." */
    if (h_auto && rep->has_ratio)
        DFAIL("CSS 2.1 §10.3.2 \"Inline, replaced elements\" reached the one arm it declines to define: both "
              "`width` and `height` compute to `auto` and the element has an INTRINSIC RATIO but neither an "
              "intrinsic width nor an intrinsic height — a scalable SVG image is the object css-images-3 §4.1 "
              "\"Object-Sizing Terminology\" names for this. The spec's words are \"the used value of 'width' "
              "is undefined in CSS 2.1\", followed by a SUGGESTION and not a rule: \"however, it is suggested "
              "that, if the containing block's width does not itself depend on the replaced element's width, "
              "then the used value of 'width' is calculated from the constraint equation used for block-level, "
              "non-replaced elements in normal flow\". That equation is `uv_block_auto_width` above and it "
              "would run today. It is NOT written here because nothing in this build can produce an object "
              "with a ratio and no sizes — there is no image decoder and no SVG document sizing — so the arm "
              "would be a choice among several UAs' answers with no way to exercise it, which is the plausible "
              "datum CLAUDE.md refuses. BUILD the object that has this shape first (an SVG image with a "
              "`viewBox` and no `width`/`height`), then write the suggestion beside a test that fires");
    /* "Otherwise, if 'width' has a computed value of 'auto', and the element has an intrinsic width, then that
       intrinsic width is the used value of 'width'." */
    if (rep->has_width) return rep->width;
    /* "Otherwise … the used value of 'width' becomes 300px." */
    return used_value_default_replaced_size(false);
}

/* CSS 2.1 §10.6.2's FOUR ARMS, in its order and reached only with a computed `height` of `auto`. The section's
   title names four box types — "Inline replaced elements, block-level replaced elements in normal flow,
   'inline-block' replaced elements in normal flow and floating replaced elements" — and §10.6.5 "Absolutely
   positioned, replaced elements" adds the fifth by reference ("the used value of 'height' is determined as for
   inline replaced elements"), so every replaced box this engine can classify is covered by this one function.
   ITS SECOND ARM READS THE USED WIDTH AS A CONTENT EXTENT, for the reason `uv_replaced_width` states. */
static CssPx uv_replaced_height(lxb_dom_element_t *el, const ReplacedElement *rep)
{
    bool w_auto = uv_length_is(el, "width", "auto");

    /* "If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic
       height, then that intrinsic height is the used value of 'height'." */
    if (w_auto && rep->has_height) return rep->height;
    /* "Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic ratio then the
       used value of 'height' is: (used width) / (intrinsic ratio)." */
    if (rep->has_ratio) {
        DCHECK(rep->ratio > 0.0,
               "CSS 2.1 §10.6.2 divides the used width by an INTRINSIC RATIO that is not positive. "
               "css-images-3 §4.1 states that an object with a degenerate ratio — \"at least one part being "
               "zero or infinity\" — is treated as having NO natural aspect ratio, so a ratio that reached a "
               "division is one core/layout/replaced_element.c's own test let through");
        return css_px_scale(uv_content_size(el, false, uv_surround(el, false)), 1.0 / rep->ratio);
    }
    /* "Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic height, then
       that intrinsic height is the used value of 'height'." */
    if (rep->has_height) return rep->height;
    /* "Otherwise … the height of the largest rectangle that has a 2:1 ratio, has a height not greater than
       150px, and has a width not greater than the device width." */
    return used_value_default_replaced_size(true);
}

/* THE REPLACED-ELEMENT SIZE, WHICH IS ONE ALGORITHM FOR FIVE BOX TYPES — the shape §10.3 does not have anywhere
   else. §10.3.4 "Block-level, replaced elements in normal flow" ("the used value of 'width' is determined as
   for inline replaced elements. Then the rules for non-replaced block-level elements are applied to determine
   the MARGINS"), §10.3.6 "Floating, replaced elements", §10.3.8 "Absolutely positioned, replaced elements" and
   §10.3.10 "'Inline-block', replaced elements in normal flow" ("exactly as inline replaced elements") every one
   of them delegates the SIZE to §10.3.2 and keeps only its own MARGIN rules — which is why the box type is not
   a branch here and is still the branch in `uv_margin`. Two box types genuinely disagree and crash below.
   §10.4/§10.7's CLAMP IS ASSERTED AS IT IS ON EVERY OTHER PATH, and §10.4 has a whole SECOND algorithm for
   this case — a table of constraint violations "for replaced elements with an INTRINSIC RATIO and both 'width'
   and 'height' specified as 'auto'" — rather than its usual three steps. Nothing in this build has an intrinsic
   ratio, so that table's antecedent is false and the assert below is the ordinary one; the day a decoder makes
   it true, the table is what `uv_require_unclamped` has to grow. */
static CssPx uv_replaced_size(lxb_dom_element_t *el, const ReplacedElement *rep, UvBox box, bool vertical)
{
    CssPx content;

    if (box == UV_BOX_TABLE)
        DFAIL("a REPLACED element whose computed `display` makes it a TABLE BOX. CSS 2.1 §17.5 owns a table's "
              "width and height and §10.3.2's intrinsic-dimension arms do not apply to it — §17.5.2's two "
              "algorithms derive the width from the COLUMNS, and a replaced element has none. BUILD §17.2's "
              "anonymous table-object generation and §17.5's algorithms; what a table box does with a replaced "
              "element's own natural dimensions is then their question and not this section's");
    if (box == UV_BOX_ITEM)
        DFAIL("a REPLACED element that is a FLEX or GRID ITEM, whose used main and cross sizes come from its "
              "container's algorithm. css-flexbox §9.7 makes a declared size the FLEX BASE SIZE and then "
              "flexes it, and for an `auto` one §9.2 makes the item's natural size its content contribution — "
              "so §10.3.2's arms are an INPUT to that algorithm rather than the answer, and returning one here "
              "would report an unflexed size as the used value. BUILD the flex layout over the container's own "
              "used content size");
    uv_require_unclamped(el, vertical);
    content = vertical ? uv_replaced_height(el, rep) : uv_replaced_width(el, rep);
    DCHECK(content.px >= 0.0,
           "CSS 2.1 §10.3.2 or §10.6.2 produced a NEGATIVE used size for a replaced element. Every arm of both "
           "is either a natural dimension (core/layout/replaced_element.c asserts those non-negative at their "
           "origin), a product of a non-negative extent with a positive ratio, or the 300 x 150 default — so a "
           "negative here is a derivation that lost an operand");
    /* css-sizing §5's conversion, applied to the RESULT — the arms above solve for the CONTENT box, which is
       the only box CSS 2.1 knows, and §5 makes the used value "as exposed for instance through
       getComputedStyle()" the border box's. Identical to `uv_block_auto_width`'s last two lines and stated
       here rather than shared with them because the two solve different equations for the same box. */
    if (!uv_is_border_box(el)) return content;
    return css_px_add(content, uv_surround_total(uv_surround(el, vertical)));
}

static CssPx uv_size(lxb_dom_element_t *el, CssLength len, UvBox box, bool vertical)
{
    /* CSS 2.1 §10.2: a percentage `width` "is calculated with respect to the width of the generated box's
       containing block", which §10.1 answers — and past that resolution it is a declared length like any
       other, which is why the two arms join here rather than each carrying its own copy of §5's `box-sizing`
       conversion and §10.4's clamp. §10.2's other sentence, "if the containing block's width depends on this
       element's width, then the resulting layout is undefined in CSS 2.1", is unreachable for the reason
       uv_padding states. A percentage HEIGHT is not here: §10.5 makes it a computed-value question. */
    if (len.kind == CSS_LENGTH_ABSOLUTE || (len.kind == CSS_LENGTH_PERCENTAGE && !vertical)) {
        CssPx declared = len.kind == CSS_LENGTH_ABSOLUTE
                             ? len.px
                             : css_px_scale(used_value_containing_block_width(el), len.pct / 100.0);

        /* §10.3.3, §10.3.5, §10.3.7 and §10.3.9 all agree on this one case and each says it in its own words:
           the equation solves for `width` only when `width` is `auto`, so a declared length IS the used value.
           §10.6.2 and §10.6.3 say the same for `height`. What does NOT agree is a table box and a flex or grid
           item, and both crash below. */
        if (box == UV_BOX_TABLE)
            DFAIL("CSS 2.1 §17.5 owns a TABLE box's width and height, and §10 does not apply to it. A declared "
                  "`width` on a table is a MINIMUM in both of §17.5.2's algorithms — the fixed layout "
                  "algorithm distributes it over the columns and the automatic one may widen the table past it "
                  "to fit the content — and §17.5.3's height behaves the same way over the rows. BUILD §17.5's "
                  "two table layout algorithms, which need the table's internal box structure (§17.2's "
                  "anonymous table-object generation) before either can run");
        if (box == UV_BOX_ITEM)
            DFAIL("this box is a FLEX or GRID ITEM, so its used main and cross sizes come from its container's "
                  "algorithm and not from CSS 2.1 §10 at all — css-flexbox §9.7 resolves the flexible lengths "
                  "(a declared `width` is only the FLEX BASE SIZE that `flex-grow` and `flex-shrink` then "
                  "adjust against the container's free space), and css-grid §11 sizes a grid item to its "
                  "track. BUILD the flex layout algorithm, which needs the container's own used content size "
                  "first — the same §10.3.3 subproblem, one level up");
        uv_require_unclamped(el, vertical);
        /* css-sizing §5 decides which BOX EDGE the declared length is on, and the used value it exposes. */
        if (uv_is_border_box(el)) return uv_border_box_size(el, declared, vertical);
        return declared;
    }
    if (len.kind == CSS_LENGTH_PERCENTAGE) {
        DCHECK(vertical, "a horizontal PERCENTAGE size reached the vertical arm — §10.2's resolution above "
                         "takes every one of them and this branch is what is left");
        DFAIL("CSS 2.1 §10.5: a PERCENTAGE `height` resolves against the containing block's HEIGHT, and "
              "when that height 'is not specified explicitly (i.e., it depends on content height)' the "
              "percentage COMPUTES TO `auto` instead — so this is a COMPUTED-value rule that reads the "
              "parent's own computed `height`, and it has to be decided before any used value is asked "
              "for. BUILD it in css_computed_value.c beside `height`'s other computed-value rule, then "
              "§10.6.3's content-based height for the `auto` result it produces");
    }
    DCHECK(len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, "auto") == 0,
           "a `width` or `height` computed to a keyword that is not `auto` — CSS 2.1 §10.2 and §10.5 admit "
           "`<length> | <percentage> | auto`, and css-sizing's `min-content`/`max-content`/`fit-content` "
           "keywords are a level-3 addition this engine has not recorded a computed-value rule for");
    /* THE REPLACED QUESTION IS ASKED BEFORE THE BOX TYPE because §10.3 answers it before the box type too:
       five of its ten sections delegate a replaced element's size to §10.3.2, and only the box types §10 does
       not own at all (a table box, a flex or grid item) disagree. It is asked ONLY on this arm — a DECLARED
       length is the used value for a replaced element and a non-replaced one alike, so a `<video
       style="width:100px">` must not pay for a classification whose answer cannot change its size. */
    {
        ReplacedElement rep = replaced_element_of(el);

        if (rep.replaced) return uv_replaced_size(el, &rep, box, vertical);
    }
    if (vertical) {
        /* CSS 2.1 §10.6.3's CONTENT-BASED HEIGHT — "the distance from its top content edge to … the bottom
           edge of the bottom (possibly collapsed) margin of its last in-flow child" — is core/layout/
           block_flow.h's walk, and so is §10.6.6's redirection of a float's and an inline-block's own `auto`
           height to §10.6.7. Which of the two sections runs is the SAME walk under §8.3.1's escape flags, so
           the three box types below share this arm; every other one has its height from a different spec and
           says so.
           §5's CONVERSION IS APPLIED TO THE RESULT, exactly as it is to §10.3.3's equation two functions down:
           the walk answers the CONTENT box, which is what CSS 2.1 knows, and css-sizing §5 makes the used
           value "as exposed for instance through getComputedStyle()" the border box's. */
        if (box == UV_BOX_ABS)
            DFAIL("CSS 2.1 §10.6.4 solves an ABSOLUTELY POSITIONED box's `height: auto` from its own constraint "
                  "equation — 'top + margin-top + border-top-width + padding-top + height + padding-bottom + "
                  "border-bottom-width + margin-bottom + bottom = height of containing block' — and its rules 1 "
                  "and 3, where `top` or `bottom` is `auto` alongside `height`, send it on to §10.6.7's "
                  "content-based height instead. TWO things are missing and the walk is not one of them: the "
                  "used `top` and `bottom` (CSSOM §9's inset arm crashes on the same equation) and the STATIC "
                  "POSITION its `auto` cases fall back to, which is where the box would have been in normal "
                  "flow. BUILD §9.4's flow layout for out-of-flow boxes, then §10.6.4 over it");
        if (box == UV_BOX_TABLE)
            DFAIL("a TABLE box with `height: auto` is CSS 2.1 §17.5.3's, not §10.6.3's: the table's height is "
                  "distributed over its ROWS, each row's height comes from the content of its cells, and a "
                  "declared height on the table is a MINIMUM the algorithm may exceed. It needs the table's "
                  "internal box structure first — §17.2's anonymous table-object generation. BUILD §17.2 and "
                  "then §17.5.3");
        if (box == UV_BOX_ITEM)
            DFAIL("a FLEX or GRID ITEM with `height: auto`. Its cross size is its CONTAINER's algorithm — "
                  "css-flexbox §9.4 collects the items into flex lines and §9.7 resolves the flexible lengths, "
                  "css-grid §11 sizes the item to its TRACK — and CSS 2.1 §10.6.3's stack of block-level "
                  "children is not it. BUILD the flex layout over the container's own used content size");
        DCHECK(box == UV_BOX_BLOCK_FLOW || box == UV_BOX_FLOAT || box == UV_BOX_INLINE_BLOCK,
               "an `auto` height reached §10.6.3's walk on a box type whose own section is elsewhere — every "
               "one of them has left through its own crash above, so uv_box_kind's list and this one have come "
               "apart");
        uv_require_unclamped(el, true);
        {
            CssPx content = block_flow_auto_height(el);

            if (!uv_is_border_box(el)) return content;
            return css_px_add(content, uv_surround_total(uv_surround(el, true)));
        }
    }
    /* §10.3.5, §10.3.7 and §10.3.9 send an `auto` width to SHRINK-TO-FIT, which is a different algorithm from
       §10.3.3's equation and is blocked on a different thing — so it gets its own message rather than the
       block-level one below. It was always reachable through CSSOM §9's resolved value; CSSOM VIEW §6's
       `clientWidth` on a floated or inline-block box is the ordinary way a page arrives here. */
    if (box == UV_BOX_FLOAT || box == UV_BOX_ABS || box == UV_BOX_INLINE_BLOCK)
        DFAIL("CSS 2.1 §10.3.5 (floating) and §10.3.9 (inline-block) send an `auto` width to the SHRINK-TO-FIT "
              "formula — 'min(max(preferred minimum width, available width), preferred width)' — and §10.3.7 "
              "sends an absolutely positioned box's there too in its rules 1 and 3, which are the ones where "
              "`left` or `right` is `auto` alongside `width` (its rule 5, with both offsets given, solves the "
              "equation for `width` instead and is the block-level arm's subproblem rather than this one). TWO "
              "of the formula's three terms are INTRINSIC SIZES of the box's own content "
              "— css-sizing §5.1's max-content and min-content, 'the narrowest inline size that would fit' and "
              "the size 'assuming no line breaks' — so this is not the containing-block subproblem the "
              "block-level arm below is waiting on, and building that one answers nothing here. BUILD the "
              "intrinsic size contributions: an inline formatting context that measures the box's text with a "
              "real font and finds its break opportunities, over every in-flow descendant's own contribution. "
              "The third term, the available width, is §10.1's containing block, so this arm needs BOTH and the "
              "other needs one of them");
    /* The two box types §10 does not own at all reach the `auto` arm as well as the declared one, and their
       `auto` case is a DIFFERENT algorithm from their declared case, so each says which. */
    if (box == UV_BOX_TABLE)
        DFAIL("a TABLE box with `width: auto` is sized by CSS 2.1 §17.5.2's AUTOMATIC table layout algorithm: "
              "the table's width comes from its COLUMNS, each of which is derived from its cells' minimum and "
              "maximum content widths — an intrinsic size over the box structure §17.2's anonymous table-object "
              "generation builds, and not §10.3.3's equation, which does not apply to a table at all. BUILD "
              "§17.2 and then §17.5.2's two algorithms");
    if (box == UV_BOX_ITEM)
        DFAIL("a FLEX or GRID ITEM with `width: auto`. css-flexbox §9.7 makes the FLEX BASE SIZE the item's "
              "max-content contribution and then flexes it against the container's free space; css-grid §11 "
              "sizes the item to its TRACK, which is itself sized from the items in it. Both are intrinsic "
              "sizes and neither is §10.3.3's equation. BUILD the flex layout over the container's own used "
              "content size, which §10.1 and §10.3.3 answer now");
    DCHECK(box == UV_BOX_BLOCK_FLOW,
           "an `auto` width reached §10.3.3's constraint equation on a box that is not block-level in normal "
           "flow — every other box type in uv_box_kind's list has left through its own section above, so the "
           "two lists have come apart");
    return uv_block_auto_width(el);
}

CssPx used_value_px(lxb_dom_element_t *el, const char *name)
{
    static const char *const MARGINS[] = { "margin-top", "margin-right", "margin-bottom", "margin-left" };
    static const char *const PADDINGS[] = { "padding-top", "padding-right", "padding-bottom", "padding-left" };
    CssLength len;
    UvBox box;
    CssPx out;
    bool vertical = false;
    unsigned i;
    int group = -1;   /* 0 = margin, 1 = padding, 2 = size */
    int side = -1;    /* the index into the two four-side tables above: top, right, bottom, left */

    DCHECK(el != NULL && name != NULL, "a used value was asked for with no element or no property name");
    for (i = 0; i < sizeof(MARGINS) / sizeof(MARGINS[0]); i++) {
        if (strcmp(MARGINS[i], name) == 0)  { group = 0; side = (int)i; }
        if (strcmp(PADDINGS[i], name) == 0) { group = 1; side = (int)i; }
    }
    if (side >= 0) vertical = (side % 2) == 0;   /* top and bottom are the even entries */
    if (group < 0 && (strcmp(name, "width") == 0 || strcmp(name, "height") == 0)) {
        group = 2;
        vertical = strcmp(name, "height") == 0;
    }
    if (group < 0) {
        DFAIL("CSSOM §9 routed a property to CSS 2.1 §10's used value that this component does not compute. It "
              "carries the ten PHYSICAL box-model lengths — the four margins, the four paddings, `width` and "
              "`height`. §9's own used-if-rendered list also names the LOGICAL spellings (`inline-size`, "
              "`margin-block-start`, `padding-inline-end`), which need css-writing-modes §6's mapping to a "
              "physical property before §10 can be asked anything, and `transform-origin`, whose used value is "
              "a position in the TRANSFORM REFERENCE BOX (css-transforms §5) and not a length at all");
        return css_px(0.0);
    }
    len = css_computed_length(el, name);
    box = uv_box_kind(el);
    /* CSS 2.1 §10.3.1 "Inline, NON-REPLACED elements" and §10.6.1 are what make `width` and `height` not apply
       to an inline box, and the word that used to be missing from this assert is the one in their titles:
       §10.3.2 "Inline, REPLACED elements" gives an inline `img` a real used width, and §10.2's own
       "Applies to:" line says so too — "all elements but NON-REPLACED inline elements". So the assert is over
       the pair, and the replaced question is asked only when the first two conjuncts have already failed,
       which is exactly the shape whose answer it can change. core/css/css_property_applies.c decides the same
       pair; if these two disagree, one of them read a different image request state than the other. */
    DCHECK(box != UV_BOX_INLINE || group != 2 || replaced_element_of(el).replaced,
           "CSS 2.1 §10.3.1 and §10.6.1 say `width` and `height` do not apply to an inline NON-REPLACED box, "
           "so CSSOM §9's first conjunct is false and the resolved value is the computed value — this call "
           "should never have been made. css_property_applies.c decides it, and the two have come apart");
    if (group == 0)      out = uv_margin(el, vertical ? NULL : MARGINS[(side + 2) % 4], len, box);
    else if (group == 1) out = uv_padding(el, len);
    else                 out = uv_size(el, len, box, vertical);
    return out;
}

/* CSS 2.1 §8's BOX MODEL — "the padding edge surrounds the box padding", and the padding box is the content box
   plus the padding on each side — over css-sizing §5, which is the only thing that varies: WHICH BOX the used
   size is the size of. That is why the paddings are added ONCE below, to the content box `uv_content_size`
   derives, rather than in two arms that would each have to remember the other's convention.
   THE ASSERT IS THE WHOLE MECHANISM AND IT IS TWO-SIDED. §5 floors the content box at zero — "as the content
   width and height cannot be negative, this computation is floored at zero" — and `uv_border_box_size`
   IMPLEMENTS that floor, by returning the LARGER of the declared length and the four-term surround rather than
   the declared length alone. So under `border-box` the used size is a number that same sum already dominates,
   and subtracting the sum back out cannot go below zero: the floored case cancels to exactly zero because it is
   the identical `uv_surround_total` result going back, and the other case is a subtraction the `>` that chose
   it already decided the sign of. A negative content box here is therefore not a strange page and not a
   rounding artifact — it is the two derivations having stopped describing the same box: a used size that did
   not come through §5's floor, or a surround computed from different terms than the one that produced it.
   Under `content-box` the same assert says something simpler and just as necessary — CSS 2.1 §10.2's `width`
   is a non-negative <length>, so a negative used size is a derivation that lost an operand.
   AND THE TWO EDGES ARE ONE DERIVATION, which is why §8.1's border edge is a `with_border` here and not a
   second function that adds the border widths to what this one returned. §8.1 states the box model as one
   nesting — content, then padding, then border — so the border edge is the padding edge plus the same two
   border widths `uv_surround` already computed for it, and reading them a second time from a second surround
   is the one way the two edges could come to describe different boxes. */
static CssPx uv_edge_px(lxb_dom_element_t *el, bool vertical, bool with_border)
{
    UvSurround s;
    CssPx content;

    s = uv_surround(el, vertical);
    content = uv_content_size(el, vertical, s);
    DCHECK(content.px >= 0.0,
           "the CONTENT box derived from a used size is NEGATIVE. Under `box-sizing: border-box` css-sizing §5 "
           "floors it at zero and this component's border-box arm implements that floor by taking the larger of "
           "the declared length and the four-term surround, so subtracting that same sum back out cannot reach "
           "a negative — a negative one means the used size and this surround were computed from "
           "different terms and no longer describe the same box. Under `content-box` the used size IS the "
           "content box, and CSS 2.1 §10.2's `width` is a non-negative <length>, so a negative one is a "
           "derivation that lost an operand");
    return css_px_add(content, with_border ? uv_surround_total(s) : s.padding);
}

CssPx used_value_padding_edge_px(lxb_dom_element_t *el, bool vertical)
{
    DCHECK(el != NULL, "a padding edge's extent was asked for with no element");
    return uv_edge_px(el, vertical, false);
}

CssPx used_value_border_edge_px(lxb_dom_element_t *el, bool vertical)
{
    DCHECK(el != NULL, "a border edge's extent was asked for with no element");
    return uv_edge_px(el, vertical, true);
}

CssPx used_value_border_edge_from_content_px(lxb_dom_element_t *el, CssPx content, bool vertical)
{
    DCHECK(el != NULL, "a border edge was asked for from a content extent with no element");
    DCHECK(content.px >= 0.0,
           "CSS 2.1 §8.1's border edge was derived from a NEGATIVE content extent. The caller is §10.6.3's "
           "walk, whose answer is a running sum of used heights and collapsed margins — and a collapsed margin "
           "is a maximum of positives minus a maximum of negatives, so a negative total is a run whose "
           "operands were not the box's own margins");
    uv_require_unclamped(el, vertical);
    return css_px_add(content, uv_surround_total(uv_surround(el, vertical)));
}
