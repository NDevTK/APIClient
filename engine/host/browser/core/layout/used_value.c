/* CSS 2.1 §10 — the used value of a box-model length. See used_value.h for the contract, for why the BOX TYPE
   is the first question, and for what the root element's `width: auto` is blocked on. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_property_applies.h"
#include "core/layout/used_value.h"

/* CSS 2.1 §10.3's OWN LIST OF BOX TYPES, which is the list of algorithms `width` has. The names are the
   spec's; the ones this file collapses are collapsed for a stated reason and never for convenience:
   §10.3.5 (floating) and §10.3.7 (absolutely positioned) are ONE member here because the two arms this
   component computes — a non-auto size, and a non-auto margin — are the same in both, and every arm where they
   differ crashes naming both sections rather than either one. */
typedef enum {
    UV_BOX_INLINE = 0,     /* §10.3.1 / §10.6.1 — an inline box; `width` and `height` do not apply to it */
    UV_BOX_BLOCK_FLOW,     /* §10.3.3 / §10.6.3 — block-level, in normal flow: the constraint equation's box */
    UV_BOX_OUT_OF_FLOW,    /* §10.3.5 / §10.3.7 — floating, or absolutely positioned */
    UV_BOX_INLINE_BLOCK,   /* §10.3.9 — shrink-to-fit when auto, the computed value otherwise */
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
        return UV_BOX_OUT_OF_FLOW;
    }
    parent_display = css_box_parent_display(lxb_dom_interface_node(el));
    if (uv_display_is_flex_or_grid(parent_display)) {
        free(parent_display);
        free(display);
        return UV_BOX_ITEM;
    }
    free(parent_display);
    if (!uv_computed_is(el, "float", "none")) { free(display); return UV_BOX_OUT_OF_FLOW; }
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
    char *mn = uv_computed(el, vertical ? "min-height" : "min-width");
    char *mx = uv_computed(el, vertical ? "max-height" : "max-width");
    CssLength lmn = css_length_parse(mn), lmx = css_length_parse(mx);
    bool no_min = (lmn.kind == CSS_LENGTH_ABSOLUTE && lmn.px == 0.0) ||
                  (lmn.kind == CSS_LENGTH_KEYWORD && strcmp(mn, "auto") == 0);
    bool no_max = lmx.kind == CSS_LENGTH_KEYWORD && strcmp(mx, "none") == 0;

    free(mn);
    free(mx);
    if (no_min && no_max) return;
    DFAIL("CSS 2.1 §10.4 / §10.7 CLAMP the used size by `min-width`/`max-width` (`min-height`/`max-height`), "
          "and this element declares one that is not its initial value, so the tentative used value §10.3 "
          "produced is not the answer. The clamp is not a `min`/`max` over two numbers: the spec runs the "
          "WHOLE of §10.3 again with the clamped size substituted, which re-solves whichever margin was `auto`. "
          "BUILD that second pass, and with it the two limits' own used values — a PERCENTAGE `max-width` "
          "resolves against the containing block's width and `min-width: auto` is css-sizing §4.1's AUTOMATIC "
          "MINIMUM SIZE, which is a content-based minimum and therefore a real layout");
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
static double uv_border_box_size(lxb_dom_element_t *el, double declared, bool vertical)
{
    static const char *const PADDINGS[2][2] = {
        { "padding-left", "padding-right" }, { "padding-top", "padding-bottom" },
    };
    static const char *const BORDERS[2][2] = {
        { "border-left-width", "border-right-width" }, { "border-top-width", "border-bottom-width" },
    };
    int axis = vertical ? 1 : 0, i;
    double surround = 0.0;

    DCHECK(uv_computed_is(el, "box-sizing", "border-box"),
           "css-sizing §5's border-box arm was reached for a `box-sizing` that is neither of the two keywords "
           "its `content-box | border-box` grammar admits — lexbor validates the declaration against it, so a "
           "third value here is a cascade layer that answered without one");
    for (i = 0; i < 2; i++) {
        char *bw = uv_computed(el, BORDERS[axis][i]);
        CssLength len = css_length_parse(bw);

        DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
               "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
               "§3.3's `Computed value:` line is `absolute length` and every arm of that derivation produces "
               "one — 0 for a `none`/`hidden` style, 1/3/5px for the three keywords, the absolutized length "
               "otherwise — so a percentage or a keyword here is a rule that did not run");
        free(bw);
        surround += len.px + used_value_px(el, PADDINGS[axis][i]);
    }
    return declared > surround ? declared : surround;
}

/* ---- the used value ------------------------------------------------------------------------------------- */

/* `opposite` is the OTHER horizontal margin's property name — `margin-right` when this is `margin-left` and
   the reverse — or NULL when this is a vertical margin, which is also how this function knows which pair it
   is in. §10.3.3's over-constraint is a statement about the three horizontal values TOGETHER, so the one that
   is not this margin and is not `width` has to be read. */
static double uv_margin(lxb_dom_element_t *el, const char *computed, const char *opposite, CssLength len,
                        UvBox box)
{
    bool vertical = opposite == NULL;

    if (len.kind == CSS_LENGTH_ABSOLUTE) {
        /* §10.3.3's OVER-CONSTRAINED case is the one configuration in CSS 2.1 in which a non-auto margin's
           used value differs from its computed value: a block-level non-replaced box in normal flow whose
           `width` and both horizontal margins are all non-auto cannot satisfy the equation, and the spec
           resolves it by IGNORING one of the two — `margin-right` when the containing block's `direction` is
           `ltr`, `margin-left` when it is `rtl` — and recomputing it so the equality holds. Both spellings
           crash: which one is ignored is a fact about the CONTAINING BLOCK's computed `direction`, and the
           recomputed value needs the containing block's WIDTH, so the two halves are blocked on different
           things and naming only one would send the next reader to the wrong file.
           ALL THREE HAVE TO BE NON-AUTO for the box to be over-constrained, which is why the OTHER margin is
           read: "if there is exactly one value specified as 'auto', its used value follows from the equality",
           so `margin-left: 10px; margin-right: auto; width: 100px` is not over-constrained at all and
           `margin-left`'s used value is the 10px it computed to. */
        if (!vertical && box == UV_BOX_BLOCK_FLOW && !uv_computed_is(el, "width", "auto") &&
            !uv_computed_is(el, opposite, "auto"))
            DFAIL("CSS 2.1 §10.3.3: this is a block-level box in normal flow whose `width` and horizontal "
                  "margins are all non-auto, so its used values are OVER-CONSTRAINED and one horizontal "
                  "margin's used value is NOT its computed value — the spec ignores the specified "
                  "`margin-right` (`margin-left` when the containing block's `direction` is `rtl`) and "
                  "recomputes it to make the constraint equation true. TWO things are missing and they are "
                  "different: `direction` is an INHERITED property and this engine's cascade has no "
                  "inheritance step, so the containing block's computed `direction` cannot be read; and the "
                  "recomputed value is the containing block's WIDTH minus the other six terms, which is "
                  "§10.3.3's equation over the CONTAINING BLOCK CHAIN §10.1 defines (used_value.h states that "
                  "subproblem — every term of the equation is readable now, and the chain and its base case "
                  "in the ICB are what is left)");
        return len.px;
    }
    if (len.kind == CSS_LENGTH_PERCENTAGE)
        DFAIL("CSS 2.1 §8.3: a PERCENTAGE margin — including `margin-top` and `margin-bottom`, which the spec "
              "is explicit about — 'is calculated with respect to the WIDTH of the generated box's containing "
              "block'. §10.1 makes that containing block the content edge of the nearest block container "
              "ancestor, and its width is §10.3.3's constraint equation. BUILD §10.3.3 and the containing-block "
              "chain §10.1 defines — all seven of the equation's terms are readable now, and used_value.h "
              "states what the chain's base case in the ICB costs");
    DCHECK(strcmp(computed, "auto") == 0,
           "a margin's computed value is neither a length, nor a percentage, nor `auto` — CSS 2.1 §8.3's "
           "<margin-width> grammar admits exactly those three, and lexbor validates the declaration against "
           "it, so a fourth form here is a serializer that produced something the grammar does not");
    if (vertical) {
        /* §10.6.3: "If 'margin-top', or 'margin-bottom' are 'auto', their used value is 0." */
        if (box == UV_BOX_BLOCK_FLOW) return 0.0;
        DFAIL("a VERTICAL margin computes to `auto` on a box CSS 2.1 §10.6.3 does not cover. §10.6.3 is the "
              "block-level-in-normal-flow section and it is the only one that gives `margin-top: auto` a used "
              "value of 0 outright; §10.6.4 solves the vertical `auto` margins of an ABSOLUTELY POSITIONED box "
              "from its own constraint equation (they centre the box between `top` and `bottom` when both are "
              "given), §10.6.1 says nothing at all about an INLINE box's vertical margins, and css-flexbox §9.6 "
              "gives a flex item's `auto` cross-axis margins the free space. BUILD the section this box's type "
              "names — uv_box_kind above says which it is");
        return 0.0;
    }
    /* §10.3.1: an inline box — "A computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used
       value of '0'." §10.3.3: a block-level box in normal flow whose `width` is also `auto` — "If 'width' is
       set to 'auto', any other 'auto' values become '0'". */
    if (box == UV_BOX_INLINE) return 0.0;
    if (box == UV_BOX_BLOCK_FLOW && uv_computed_is(el, "width", "auto")) return 0.0;
    DFAIL("a HORIZONTAL margin computes to `auto` on a box whose used value follows from a CONSTRAINT "
          "EQUATION rather than from a rule. §10.3.3 with a non-auto `width` is the centring case — one `auto` "
          "margin takes up all the slack, and two `auto` margins split it, which is what `margin: 0 auto` "
          "means — and §10.3.7 does the same for an absolutely positioned box between `left` and `right`. "
          "Every one of them needs the containing block's WIDTH, so they are all waiting on the same "
          "subproblem: §10.3.3's equation over §10.1's containing-block chain (used_value.h states it in full)");
    return 0.0;
}

static double uv_padding(CssLength len)
{
    if (len.kind == CSS_LENGTH_ABSOLUTE) return len.px;
    DCHECK(len.kind == CSS_LENGTH_PERCENTAGE,
           "a padding's computed value is neither a length nor a percentage. CSS 2.1 §8.4's <padding-width> "
           "grammar has no `auto` and no keyword at all — a padding is a length or a percentage and nothing "
           "else — so this is a value lexbor's own validation should have dropped");
    DFAIL("CSS 2.1 §8.4: a PERCENTAGE padding 'is calculated with respect to the WIDTH of the generated box's "
          "containing block, EVEN FOR padding-top and padding-bottom'. That width is §10.3.3's constraint "
          "equation over the nearest block container ancestor, which this engine cannot evaluate yet — see "
          "used_value.h for the containing-block chain it is blocked on");
    return 0.0;
}

static double uv_size(lxb_dom_element_t *el, const char *computed, CssLength len, UvBox box, bool vertical)
{
    if (len.kind == CSS_LENGTH_ABSOLUTE) {
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
        if (!uv_computed_is(el, "box-sizing", "content-box"))
            return uv_border_box_size(el, len.px, vertical);
        return len.px;
    }
    if (len.kind == CSS_LENGTH_PERCENTAGE) {
        if (vertical)
            DFAIL("CSS 2.1 §10.5: a PERCENTAGE `height` resolves against the containing block's HEIGHT, and "
                  "when that height 'is not specified explicitly (i.e., it depends on content height)' the "
                  "percentage COMPUTES TO `auto` instead — so this is a COMPUTED-value rule that reads the "
                  "parent's own computed `height`, and it has to be decided before any used value is asked "
                  "for. BUILD it in css_computed_value.c beside `height`'s other computed-value rule, then "
                  "§10.6.3's content-based height for the `auto` result it produces");
        DFAIL("CSS 2.1 §10.2: a PERCENTAGE `width` 'is calculated with respect to the width of the generated "
              "box's containing block'. §10.1 makes that the content edge of the nearest block container "
              "ancestor, whose own used width is §10.3.3's constraint equation (used_value.h states it in "
              "full)");
    }
    DCHECK(strcmp(computed, "auto") == 0,
           "a `width` or `height` computed to a keyword that is not `auto` — CSS 2.1 §10.2 and §10.5 admit "
           "`<length> | <percentage> | auto`, and css-sizing's `min-content`/`max-content`/`fit-content` "
           "keywords are a level-3 addition this engine has not recorded a computed-value rule for");
    if (css_element_may_be_replaced(el))
        DFAIL("CSS 2.1 §10.3.4 sends a block-level REPLACED element's used width to §10.3.2, and §10.6.2 does "
              "the same for its height: with a computed value of `auto` the used value is the element's "
              "INTRINSIC width, or the used height times the INTRINSIC RATIO — the decoded image's dimensions, "
              "the embedded document's — and this engine has no image decoder and no child-navigable layout to "
              "ask for either. §10.3.2 states the fallback itself when there is no intrinsic size and no "
              "ratio: 300px wide, and 150px tall by §10.6.2, which is the very pair core/frame/viewport.c "
              "already derives a child navigable's viewport from. BUILD the intrinsic-dimension source (the "
              "image load state for `img`, the child document's own layout for `iframe`), and with it the "
              "replaced-element predicate css_property_applies.c's DFAIL asks for");
    if (vertical)
        DFAIL("CSS 2.1 §10.6.3: a `height` of `auto` is CONTENT-BASED — 'the distance from its top content "
              "edge to the bottom edge of the last line box' when the box establishes an inline formatting "
              "context, or to the bottom margin edge of its last in-flow child when it has block-level "
              "children, and zero when it has neither. So the used height needs the box's CHILDREN to have "
              "been laid out and, for the line-box case, an inline formatting context with real line breaking "
              "and a font. BUILD the in-flow child walk first — the zero-child and block-level-children arms "
              "of §10.6.3 need no font at all, only every child's used height and vertical margins, and "
              "§8.3.1's margin COLLAPSING between them");
    DFAIL("CSS 2.1 §10.3.3: a `width` of `auto` on a block-level box 'follows from the resulting equality' — "
          "the used width is the CONTAINING BLOCK's width minus the six other terms of the constraint "
          "equation. ALL SEVEN TERMS ARE READABLE NOW: the margins and paddings are this component's own §8.3 "
          "and §8.4 arms, and `border-left-width`/`border-right-width` are css_computed_value.c's, which used "
          "to be the blocker and is not one. What is missing is the CONTAINING BLOCK itself, and it is two "
          "things rather than one. (1) §10.1's CHAIN: every block-level box's containing block is the content "
          "edge of its nearest block container ancestor, so the equation is recursive and the recursion has to "
          "walk to a base case — which exists, because §10.1 makes the ROOT ELEMENT's containing block the "
          "INITIAL CONTAINING BLOCK that core/frame/viewport.c models. (2) THE ICB'S WIDTH IS A CONCOLIC, not "
          "a number: viewport.h makes the viewport size a PICKED environment fact minted through "
          "`viewport_env_value`, so a used width derived from it carries that domain — "
          "`parseInt(getComputedStyle(el).width) < 768` is the same responsive gate as `innerWidth < 768` and "
          "answering it with a bare 1280 deletes the mobile arm. So the resolved-value path stops being a "
          "`char *` on the same day this lands, which is the same plumbing css_length.c's viewport-unit and "
          "snap-a-border-width crashes both ask for. BUILD the chain and that path together");
    return 0.0;
}

double used_value_px(lxb_dom_element_t *el, const char *name)
{
    static const char *const MARGINS[] = { "margin-top", "margin-right", "margin-bottom", "margin-left" };
    static const char *const PADDINGS[] = { "padding-top", "padding-right", "padding-bottom", "padding-left" };
    char *computed;
    CssLength len;
    UvBox box;
    double out;
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
        return 0.0;
    }
    computed = uv_computed(el, name);
    len = css_length_parse(computed);
    box = uv_box_kind(el);
    DCHECK(box != UV_BOX_INLINE || group != 2,
           "CSS 2.1 §10.3.1 and §10.6.1 say `width` and `height` DO NOT APPLY to an inline box, so CSSOM §9's "
           "first conjunct is false and the resolved value is the computed value — this call should never have "
           "been made. css_property_applies.c decides it, and the two have come apart");
    if (group == 0)      out = uv_margin(el, computed, vertical ? NULL : MARGINS[(side + 2) % 4], len, box);
    else if (group == 1) out = uv_padding(len);
    else                 out = uv_size(el, computed, len, box, vertical);
    free(computed);
    return out;
}
