/* CSS Cascade §Computed Value + CSSOM §9's resolved value. See css_computed_value.h for the split. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_length.h"
#include "core/css/css_property_applies.h"
#include "core/css/css_shorthand.h"
#include "core/css/css_style_declaration.h"
#include "core/css/font_metrics.h"
#include "core/css/font_size_functions.h"
#include "core/dom/document.h"
#include "core/dom/shadow_root.h"
#include "core/frame/viewport.h"
#include "core/layout/used_value.h"

static char *css_cv_strdup(const char *s)
{
    char *out = strdup(s);
    CHECK(out != NULL, "cssom: OOM deriving a computed value — a dropped value would read as undeclared");
    return out;
}

static bool css_cv_is(const char *v, const char *kw)
{
    return v != NULL && strcmp(v, kw) == 0;
}

/* The same question, ASCII CASE-INSENSITIVELY and over the surrounding whitespace a serialization may leave —
   CSS Syntax makes a keyword ASCII case-insensitive, so `currentColor` and `currentcolor` are one value, and
   the spelling with the capital C is the one every stylesheet on the web actually writes. */
static bool css_cv_kw_is(const char *v, const char *kw)
{
    size_t n, k = strlen(kw), j;

    if (v == NULL) return false;
    while (*v && isspace((unsigned char)*v)) v++;
    for (n = strlen(v); n > 0 && isspace((unsigned char)v[n - 1]); n--) { }
    if (n != k) return false;
    for (j = 0; j < k; j++)
        if ((char)tolower((unsigned char)v[j]) != kw[j]) return false;
    return true;
}

/* "The ROOT ELEMENT" — the element whose parent is the Document itself. CSS Display §2.8 gives it its own
   computed-value rules, and CSSOM VIEW §6 asks the same question of the same node. */
static bool css_is_root_element(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* ---- CSS Cascade §7's DEFAULTING, over the two shapes a computed value comes in ---------------------------- */

/* §7.2's PARENT ELEMENT, "on the flattened element tree": the parent node when it is an element, and the HOST
   when it is a SHADOW ROOT — a shadow tree's children inherit across the boundary, which is why §7.2 states the
   flat tree rather than the node tree. NULL is the case §7.2 answers itself: "for the root element, which has
   no parent element, the inherited value is the initial value", and an element whose parent is a Document or a
   plain DocumentFragment, or which has no parent at all, has no parent element in exactly that sense. */
static lxb_dom_element_t *css_cv_parent_element(lxb_dom_element_t *el)
{
    lxb_dom_node_t *p = lxb_dom_interface_node(el)->parent;

    if (p == NULL) return NULL;
    if (shadow_root_is(p)) return shadow_root_host(p);
    if (p->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;
    return lxb_dom_interface_element(p);
}

/* CSS Cascade §7's SPECIFIED VALUE of a property whose value this file carries as TEXT — the cascaded value, or
   §7.1's initial value, or §7.2's inherited value, decided by core/css/css_defaulting.h. OWNED, and NULL only
   for a property that no layer declares AND that has no initial value to fall to (a custom property nobody
   set), which is §6.6.1's empty string and not a hole.
   §7.2's INHERITED VALUE IS THE PARENT'S COMPUTED VALUE, and this asks for it through the entry that derives
   one where there is one: a property this file models goes to `css_computed_value`, and a property it does not
   takes its computed value to BE its specified value (css_computed_value.h says why that is the majority of
   CSS's own `Computed value:` lines and what ends the assumption), which is this same entry one node up. */
static char *css_cv_specified(lxb_dom_element_t *el, const char *name)
{
    char *cascaded = cssom_cascaded_value(el, name);
    lxb_dom_element_t *parent;

    switch (css_defaulting_of(name, cascaded)) {
    case CSS_DEFAULTING_DECLARED:
        return cascaded;
    case CSS_DEFAULTING_INHERITED:
        free(cascaded);
        DCHECK(!css_computed_models_length(name),
               "a LENGTH-valued property was inherited through the entry that answers TEXT. §7.2's inherited "
               "value is the parent's COMPUTED value, which for one of these is an ABSOLUTE LENGTH carrying "
               "the environment fact it derives from — serializing it here to hand it down the tree is exactly "
               "the drop css_computed_value.h describes, and `css_computed_length` is the entry that carries "
               "it whole");
        parent = css_cv_parent_element(el);
        if (parent == NULL) break;
        return css_computed_models(name) ? css_computed_value(parent, name) : css_cv_specified(parent, name);
    case CSS_DEFAULTING_INITIAL:
        free(cascaded);
        break;
    }
    return cssom_initial_value(name);
}

/* ---- css-overflow §3.1's computed value ------------------------------------------------------------------ */

/* §3.1: "The scroll, auto, and hidden values are known as the scrollable values of overflow." */
static bool overflow_scrollable(const char *v)
{
    return css_cv_is(v, "scroll") || css_cv_is(v, "auto") || css_cv_is(v, "hidden");
}

/* §3.1's legacy alias — `overlay` IS `auto`, so it aliases before every question is asked of it. */
static const char *overflow_alias(const char *v)
{
    return css_cv_is(v, "overlay") ? "auto" : v;
}

static bool overflow_value(const char *v)
{
    return overflow_scrollable(v) || css_cv_is(v, "visible") || css_cv_is(v, "clip");
}

/* §3.1's one computed-value rule, which is why "usually specified value, but see text" is not "as specified":
   "if the other axis specifies a scrollable value, a specified value of visible computes to auto, enabling
   scrolling in its axis". It reads the other axis's SPECIFIED value, so there is no recursion between the two
   and no order in which they must be asked. */
static char *computed_overflow(lxb_dom_element_t *el, const char *name, char *spec)
{
    bool xaxis = strcmp(name, "overflow-x") == 0;
    char *other = css_cv_specified(el, xaxis ? "overflow-y" : "overflow-x");
    const char *self_v, *other_v;
    char *out;

    DCHECK(other != NULL,
           "§7's defaulting produced no SPECIFIED value for the other overflow axis — lexbor's property "
           "registry carries `overflow-x` and `overflow-y` with an initial value of `visible`, so §7.1's "
           "arm always answers and a NULL here is a defaulting step that stopped early");
    self_v = overflow_alias(spec);
    other_v = overflow_alias(other);
    DCHECK(overflow_value(self_v) && overflow_value(other_v),
           "an overflow axis cascaded to a value outside css-overflow §3.1's grammar — lexbor validates the "
           "longhand and css_shorthand.c validates the `overflow` shorthand, so a third writer has reached the "
           "cascade without a grammar");
    out = (css_cv_is(self_v, "visible") && overflow_scrollable(other_v)) ? css_cv_strdup("auto")
                                                                        : css_cv_strdup(self_v);
    free(other);
    free(spec);
    return out;
}

/* ---- CSS Display §2.7 and §2.8's computed value ----------------------------------------------------------- */

/* §2.7's BLOCKIFICATION: "sets the box's computed outer display type to block". The values whose outer type is
   already block are unchanged; a layout-internal box additionally has "its inner display type convert to flow
   so that it becomes a block container", which is why every table-internal row below maps to plain `block`. */
static char *blockified(char *spec)
{
    static const struct { const char *from, *to; } MAP[] = {
        { "inline", "block" }, { "inline-block", "block" },   /* §2.7's legacy inline flow-root rule */
        { "inline-table", "table" }, { "inline-flex", "flex" }, { "inline-grid", "grid" },
        { "run-in", "block" },
        { "table-row-group", "block" }, { "table-header-group", "block" }, { "table-footer-group", "block" },
        { "table-row", "block" }, { "table-cell", "block" }, { "table-column-group", "block" },
        { "table-column", "block" }, { "table-caption", "block" },
        { "ruby-base", "block" }, { "ruby-text", "block" },
        { "ruby-base-container", "block" }, { "ruby-text-container", "block" },
    };
    static const char *const BLOCK_OUTER[] = { "block", "flow", "flow-root", "flex", "grid", "table",
                                               "list-item" };
    unsigned i;

    for (i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (strcmp(spec, MAP[i].from) == 0) { free(spec); return css_cv_strdup(MAP[i].to); }
    for (i = 0; i < sizeof(BLOCK_OUTER) / sizeof(BLOCK_OUTER[0]); i++)
        if (strcmp(spec, BLOCK_OUTER[i]) == 0) return spec;
    DFAIL("CSS Display §2.7 blockifies a box whose computed `display` this file cannot map. The two forms it "
          "does not carry are the TWO-VALUE `<display-outside> <display-inside>` syntax (`inline flow-root`, "
          "`block flow list-item`), which lexbor parses into the three-slot value this file only ever sees "
          "serialized, and `ruby`, whose blockified form is `block ruby` and NOT `block` (its inner type "
          "survives — only a LAYOUT-INTERNAL box converts its inner type to flow). BUILD the outer/inner pair "
          "as the value this file carries, so blockification sets the outer half and leaves the inner one "
          "alone, instead of mapping whole keywords");
    return spec;
}

/* The element's BOX PARENT's `display` — the nearest ancestor element that GENERATES a box, because
   `display: contents` generates none and a flex item's container is therefore the first ancestor past it. The
   walk reads SPECIFIED values on purpose: blockification never makes a box a flex or grid container and never
   stops one being one, so the question this walk asks has the same answer either way, and asking for the
   computed value would recurse up the whole ancestor chain to answer it. OWNED, or NULL at the root. */
char *css_box_parent_display(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *p;

    for (p = n->parent; p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT; p = p->parent) {
        char *d = css_cv_specified(lxb_dom_interface_element((lxb_dom_node_t *)p), "display");

        DCHECK(d != NULL, "§7's defaulting produced no `display` for an ancestor element — the UA layer answers "
                          "`inline` for every element it does not name, so this cannot be unset");
        if (strcmp(d, "contents") != 0) return d;
        free(d);
        /* §2.8: "a display of contents computes to block on the root element", so the root is a box parent
           however it is declared. */
        if (css_is_root_element(p)) return css_cv_strdup("block");
    }
    return NULL;
}

static bool display_is_flex_or_grid_container(const char *d)
{
    return css_cv_is(d, "flex") || css_cv_is(d, "inline-flex") ||
           css_cv_is(d, "grid") || css_cv_is(d, "inline-grid");
}

static char *computed_display(lxb_dom_element_t *el, char *spec)
{
    const lxb_dom_node_t *n = lxb_dom_interface_node(el);
    bool root = css_is_root_element(n), blockify;

    /* §2.8: "Additionally, a display of contents computes to block on the root element." */
    if (root && strcmp(spec, "contents") == 0) { free(spec); return css_cv_strdup("block"); }
    /* §2.7: blockification "has no effect on display types that generate no box at all, such as none or
       contents". */
    if (strcmp(spec, "none") == 0 || strcmp(spec, "contents") == 0) return spec;
    /* §2.8's root rule, and the three computed-value fixups §2.7 lists that the TREE decides: "Absolute
       positioning or floating an element blockifies the box's display type" and "A parent with a grid or flex
       display value blockifies the box's display type". */
    blockify = root;
    if (!blockify) {
        char *f = css_computed_value(el, "float");
        blockify = f != NULL && strcmp(f, "none") != 0;
        free(f);
    }
    if (!blockify) {
        char *p = css_computed_value(el, "position");
        blockify = css_cv_is(p, "absolute") || css_cv_is(p, "fixed");
        free(p);
    }
    if (!blockify) {
        char *pd = css_box_parent_display(n);
        blockify = display_is_flex_or_grid_container(pd);
        free(pd);
    }
    return blockify ? blockified(spec) : spec;
}

/* ---- the BOX-MODEL LENGTHS' computed value ---------------------------------------------------------------- */

/* THE REALM THE ELEMENT'S OWN DOCUMENT IS THE ACTIVE DOCUMENT OF, or NULL — which is what a relative unit
   absolutizes against, and it is the ELEMENT's and never the running flow's: an iframe's initial containing
   block is 300 CSS pixels wide and the top-level traversable's is 1280, so a `50vw` in one document is a
   different number from a `50vw` in the other and a remembered realm would answer both with whichever asked
   first (CLAUDE.md §per-realm). NULL is a real answer — a DOMParser document is presented by nothing — and it
   is css_length.h's arms that crash on it, because only a relative unit needs it. */
static JSContext *css_cv_realm(lxb_dom_element_t *el)
{
    const lxb_dom_node_t *n = lxb_dom_interface_node(el);

    DCHECK(n->owner_document != NULL,
           "a computed value was derived for an element whose node has no owner document — every node this "
           "engine mints belongs to the document that created it");
    if (n->owner_document == NULL) return NULL;
    return document_active_realm_of(lxb_dom_interface_node(n->owner_document));
}

/* A computed value that IS an absolute length, with every other arm's field left in the one state a reader of
   the wrong arm would have to be reading by mistake. */
static CssLength css_cv_px(CssPx px)
{
    CssLength out = { CSS_LENGTH_ABSOLUTE, css_px(0.0), 0.0, { '\0' } };

    out.px = px;
    return out;
}

/* ---- css-values-4 §6.1.1's `em` and `rem`, ANSWERED FROM THE TREE ------------------------------------------ */

/* WHICH ELEMENT'S COMPUTED `font-size` A FONT-RELATIVE UNIT MEANS, which is the half css_length.h cannot answer
   for itself: both units are defined as a computed `font-size`, and a computed `font-size` is CSS Cascade §7.2's
   inheritance walk over the flattened element tree. `affecting` is css-values-4 §6.1.1's font-affecting rule
   pre-decided from the PROPERTY, because the property is a fact about the declaration this length came out of
   and not about the element the walk reaches. */
typedef struct {
    lxb_dom_element_t *el;
    bool               affecting;
} CssCvFontCtx;

/* THE PARENT'S COMPUTED `font-size`, which is FOUR spec sentences and one walk. CSS Cascade §7.2 Inheritance
   makes it the inherited value; css-fonts-4 §2.5's `Percentages:` line makes it what a percentage font size
   refers to; §2.5's `<relative-size>` makes it what `larger` and `smaller` are relative to; and css-values-4
   §6.1.1's font-affecting rule makes it what an `em` inside a font-affecting property resolves against. Asking
   it four ways would be four chances to disagree about the base case, which all four state identically:
   §7.2's "for the root element, which has no parent element, the inherited value is the initial value of the
   property" and §6.1.1's "the computed metrics corresponding to the initial values of the font and line-height
   properties, if the element has no parent" are the same number, and §2.5's `Initial:` line names it. */
static CssPx css_cv_parent_font_size(lxb_dom_element_t *el)
{
    lxb_dom_element_t *parent = css_cv_parent_element(el);
    CssLength len;

    if (parent == NULL) return css_default_font_size(css_cv_realm(el));
    len = css_computed_length(parent, "font-size");
    DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
           "a parent element's computed `font-size` came back as something other than an ABSOLUTE LENGTH. "
           "css-fonts-4 §2.5 (Font size: the font-size property) states its `Computed value:` line as `an "
           "absolute length` and admits nothing else past the computed value — a percentage refers to the "
           "parent's size and is resolved there, and every keyword is a table entry or a ratio — so this is "
           "the entry below having left an arm unresolved");
    return len.px;
}

/* THE ROOT ELEMENT of this element's document — DOM §4.5's document element, which is what CSS calls the root
   element and what §6.1.1's `rem` is measured on. NULL is a real answer and the spec states its arm: "when
   specified in a document with no root element, the root font-relative lengths are resolved assuming the
   initial values of the font and line-height properties." */
static lxb_dom_node_t *css_cv_root_element(lxb_dom_element_t *el)
{
    const lxb_dom_node_t *n = lxb_dom_interface_node(el);

    if (n->owner_document == NULL) return NULL;
    return document_document_element_of(lxb_dom_interface_node(n->owner_document));
}

/* css-values-4 §6.1.1's ASSUMED "0" GLYPH IS "0.5em WIDE BY 1em TALL", and which of its two dimensions the
   `ch` unit takes is not a fact about the font: §6.1.1 defines the advance measure of a glyph as "its advance
   width or height, WHICHEVER IS IN THE INLINE AXIS of the element", and states the consequence itself — the
   unit "falls back to 0.5em in the general case, and to 1em when it would be typeset upright (i.e.
   writing-mode is vertical-rl or vertical-lr AND text-orientation is upright)".
   THE PARENTHETICAL IS THE TEST AND IT NAMES TWO OF THE FIVE WRITING MODES. `horizontal-tb` puts the inline
   axis on the horizontal, so the advance is the WIDTH; `sideways-rl` and `sideways-lr` are vertical modes in
   which text is never typeset upright, so §6.1.1's general case covers them too and the answer is the same
   width. Only `vertical-rl` and `vertical-lr` can reach the other branch, and which one they take is
   `text-orientation` — a property core/css/css_computed_value.c models no entry for, so that arm crashes.
   THE ELEMENT IS THE ONE THE UNIT IS USED ON and not the one whose METRICS are borrowed. §6.1.1's
   font-affecting clause redirects a unit inside a font-affecting property to "the computed METRICS of the
   parent element", and an inline axis is not a metric — it is the axis of the box the advance is measured
   along, which the section names as the element's own. `el` is NULL for §6.1.1's no-root-element case, where
   the initial values apply and `writing-mode`'s initial value is `horizontal-tb`. */
static FontMetricsEmRatio css_cv_zero_advance(lxb_dom_element_t *el)
{
    char *wm;
    bool upright_possible;

    if (el == NULL) return FONT_METRICS_ZERO_ADVANCE_WIDTH;
    wm = css_computed_value(el, "writing-mode");
    DCHECK(wm != NULL, "the cascade produced no computed `writing-mode` — css-writing-modes-4 §3.2 gives the "
                       "property an `Initial:` line of `horizontal-tb` and lexbor's registry carries it, so "
                       "the last layer always answers");
    upright_possible = strcmp(wm, "vertical-rl") == 0 || strcmp(wm, "vertical-lr") == 0;
    free(wm);
    if (upright_possible)
        DFAIL("css-values-4 §6.1.1's `ch` unit was resolved on an element whose computed `writing-mode` is "
              "`vertical-rl` or `vertical-lr`, which are the two modes in which text CAN be typeset upright — "
              "and whether it IS decides which dimension of the assumed \"0\" glyph is the advance measure, "
              "0.5em wide or 1em tall. §6.1.1 names the deciding property outright: the unit falls back \"to "
              "1em when it would be typeset upright (i.e. writing-mode is vertical-rl or vertical-lr and "
              "TEXT-ORIENTATION IS UPRIGHT)\". css-writing-modes-4 §5.1 \"Orienting Text: the text-orientation "
              "property\" gives it `Value: mixed | upright | sideways`, `Initial: mixed`, `Inherited: yes` and "
              "`Computed value: specified value`, and lexbor's property registry carries it — so this is ONE "
              "ROW of css_computed_models' as-specified arm plus one of css_shorthand_complete_for, exactly as "
              "`writing-mode` and `direction` were, and this crash then becomes the branch between the two "
              "ratios core/css/font_metrics.h already holds");
    return FONT_METRICS_ZERO_ADVANCE_WIDTH;
}

/* IS THIS UNIT MEASURED ON THE ROOT ELEMENT — one of the two questions §6.1.1 splits its twelve units by, and
   the only one that is a property of the unit's NAME rather than of the element. §6.1.1 defines each
   `r`-prefixed unit as "the value of the <unit> unit on the root element", so the answer selects which font
   size the ratio below is a multiple of and, for `ch`, which element's inline axis decides the ratio. */
static bool css_cv_metric_is_root(CssFontMetric which)
{
    return which == CSS_FONT_METRIC_REM || which == CSS_FONT_METRIC_REX ||
           which == CSS_FONT_METRIC_RCH || which == CSS_FONT_METRIC_RIC ||
           which == CSS_FONT_METRIC_RCAP;
}

static CssPx css_cv_font_metric(void *p, CssFontMetric which)
{
    CssCvFontCtx *f = p;
    lxb_dom_node_t *root;
    CssLength len;

    DCHECK(f != NULL && f->el != NULL,
           "css-values-4 §6.1.1's font-metric callback was invoked with no element — the pair is handed to "
           "css_length_parse by the entries below and by nothing else, so an absent element is a metrics "
           "struct assembled field-by-field past them");
    /* §6.1.1's EIGHT FONT-METRIC UNITS ARE A METRIC OVER ONE OF THE TWO FONT SIZES BELOW, so each is answered
       by recursing into this same callback for its base — which is what makes `ex` and `rex` one derivation
       with one difference (`rem` rather than `em`) rather than two that can drift apart. Six of them scale by
       a ratio §6.1.1 fixes; `cap` and `rcap` take core/css/font_metrics.h's picked ascent instead, and the
       difference in signature is the difference in kind (font_metrics.h). For `ch` the ratio is the axis
       question above. */
    if (which != CSS_FONT_METRIC_EM && which != CSS_FONT_METRIC_REM) {
        bool root_relative = css_cv_metric_is_root(which);
        CssPx base = css_cv_font_metric(p, root_relative ? CSS_FONT_METRIC_REM : CSS_FONT_METRIC_EM);
        FontMetricsEmRatio ratio;

        /* §6.1.1's `cap` IS THE ONE THAT IS NOT A SPEC RATIO, so it leaves through its own component entry
           rather than through the table below: "in the cases where it is impossible or impractical to
           determine the cap-height, THE FONT'S ASCENT must be used", and that ascent is a metric of the
           modelled face rather than a number §6.1.1 fixes. core/css/font_metrics.h forms the product so the
           reader's font size and this user agent's face are unioned there and not here. */
        if (which == CSS_FONT_METRIC_CAP || which == CSS_FONT_METRIC_RCAP)
            return font_metrics_ascent_px(css_cv_realm(f->el), base);
        switch (which) {
        case CSS_FONT_METRIC_EX:
        case CSS_FONT_METRIC_REX: ratio = FONT_METRICS_X_HEIGHT; break;
        case CSS_FONT_METRIC_IC:
        case CSS_FONT_METRIC_RIC: ratio = FONT_METRICS_WATER_ADVANCE; break;
        default:
            DCHECK(which == CSS_FONT_METRIC_CH || which == CSS_FONT_METRIC_RCH,
                   "css_length.h's font-metric enumeration named a unit this component answers no arm for — "
                   "the two are one list and have come apart");
            /* §6.1.1's `rch` is "the value of the ch unit ON THE ROOT ELEMENT", so the whole computation moves
               there, the inline axis included. A document with no root element takes §6.1.1's own clause for
               that case and the initial values with it, which `css_cv_zero_advance` reads as its NULL. */
            root = root_relative ? css_cv_root_element(f->el) : lxb_dom_interface_node(f->el);
            ratio = css_cv_zero_advance(root == NULL ? NULL : lxb_dom_interface_element(root));
            break;
        }
        return css_px_scale(base, font_metrics_em_ratio(ratio));
    }
    /* §6.1.1: `em` is "equal to the computed value of the font-size property of the element on which it is
       used" — EXCEPT that "when used in the value of any font-affecting property on the element they refer to,
       the font-relative lengths resolve against the computed metrics of the parent element—or against the
       computed metrics corresponding to the initial values of the font and line-height properties, if the
       element has no parent". `font-size` IS a font-affecting property and the element it refers to is the one
       it is declared on, so `div { font-size: 1.2em }` is 1.2 times the INHERITED size. That is not a nicety:
       resolving it against the element's own computed `font-size` would make this walk a definition of itself
       and it would not terminate. */
    if (which == CSS_FONT_METRIC_EM) {
        if (f->affecting) return css_cv_parent_font_size(f->el);
        len = css_computed_length(f->el, "font-size");
        DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
               "an element's own computed `font-size` came back as something other than an ABSOLUTE LENGTH, "
               "which css-fonts-4 §2.5's `Computed value:` line admits nothing else than");
        return len.px;
    }
    DCHECK(which == CSS_FONT_METRIC_REM,
           "css_length.h's font-metric enumeration named a metric this component answers no arm for — the two "
           "are one list and have come apart. The two of css-values-4 §6.1.1's units that this callback does "
           "NOT carry a row for (`lh`, `rlh`) are the pair §6.1.1 states in terms of a PROPERTY rather than a "
           "metric, and they crash in css_length.c naming `line-height`'s computed value and §10.8.1's depth "
           "below the baseline rather than reaching a third arm here");
    root = css_cv_root_element(f->el);
    /* §6.1.1: `rem` is "equal to the computed value of the em unit on the root element". The element it REFERS
       TO is therefore the root, so the font-affecting clause above bites only when the declaration is on the
       root itself — `html { font-size: 2rem }` is 2 x the INITIAL font size (the root has no parent element),
       while `div { font-size: 2rem }` is 2 x the root's own computed size and `html { margin: 2rem }` is too,
       because a margin is not a font-affecting property. */
    if (root == NULL || (f->affecting && root == lxb_dom_interface_node(f->el)))
        return css_default_font_size(css_cv_realm(f->el));
    len = css_computed_length(lxb_dom_interface_element(root), "font-size");
    DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
           "the ROOT element's computed `font-size` came back as something other than an ABSOLUTE LENGTH, "
           "which css-fonts-4 §2.5's `Computed value:` line admits nothing else than");
    return len.px;
}

/* THE PAIR, BOUND TO ONE ELEMENT AND ONE PROPERTY. `slot` is the caller's own storage and must outlive the
   parse: the metrics are consulted DURING it and only on the arm that found an `em` or a `rem`. */
static CssFontMetrics css_cv_font_metrics(CssCvFontCtx *slot, lxb_dom_element_t *el, const char *name)
{
    CssFontMetrics m;

    slot->el = el;
    slot->affecting = css_font_affecting_property(name);
    m.resolve = css_cv_font_metric;
    m.ctx = slot;
    return m;
}

/* ONE `Computed value:` LINE, WRITTEN THE SAME WAY IN TEN PROPERTY DEFINITIONS. CSS 2.1 §8.3 and §8.4 give the
   margins and the paddings "the percentage as specified or THE ABSOLUTE LENGTH"; §10.2 and §10.5 give `width`
   and `height` "the percentage or 'auto' as specified or the absolute length"; css-sizing says the same for the
   four min/max limits. So the whole rule is: ABSOLUTIZE a length and leave everything else alone — a percentage
   cannot be resolved here because it refers to the containing block, which is a USED value and belongs to
   core/layout/used_value.h, and `auto` is a keyword no cascade step turns into a number.
   THE ABSOLUTIZATION IS WHERE THE FONT AND THE VIEWPORT ENTER, and core/css/css_length.h is the one component
   that knows it: `50vw` becomes a number out of §10.1's initial containing block and carries that rectangle's
   environment fact the whole way to the page, and `2em` becomes one out of the computed `font-size` the
   metrics above walk to, carrying the reader's own default font size the same way. */
static CssLength computed_length(lxb_dom_element_t *el, const char *name, char *spec)
{
    CssCvFontCtx slot;
    CssFontMetrics font = css_cv_font_metrics(&slot, el, name);
    CssLength len = css_length_parse(css_cv_realm(el), &font, spec);

    free(spec);
    return len;
}

/* ---- CSS Backgrounds §3.2 and §3.3's BORDER LONGHANDS ----------------------------------------------------- */

/* The four sides in the order every four-side rule in CSS states them, so a width's index IS its style's. */
static const char *const CSS_BORDER_SIDES[] = { "top", "right", "bottom", "left" };

/* Which side `border-<side>-<part>` names, or -1. The SIDE is the whole reason this exists: `border-top-width`
   has to read `border-top-style` and not some fixed one, so the sibling's name is DERIVED from this property's
   rather than tabulated a second time. */
static int css_border_side_of(const char *name, const char *part)
{
    char probe[32];
    unsigned i;

    for (i = 0; i < sizeof(CSS_BORDER_SIDES) / sizeof(CSS_BORDER_SIDES[0]); i++) {
        snprintf(probe, sizeof probe, "border-%s-%s", CSS_BORDER_SIDES[i], part);
        if (strcmp(probe, name) == 0) return (int)i;
    }
    return -1;
}

/* css-backgrounds-3 §3.3: "The thin, medium, and thick keywords are equivalent to 1px, 3px, and 5px,
   respectively." CSS 2.1 §8.5.1 left the three UA-dependent and required only `thin <= medium <= thick`; the
   level-3 sentence PINS them, so these are the spec's numbers and not this UA's preference. */
static const struct { const char *kw; double px; } CSS_LINE_WIDTH[] = {
    { "thin", 1.0 }, { "medium", 3.0 }, { "thick", 5.0 },
};

/* CSS 2.1 §8.5.1's `Computed value:` line, entire: "absolute length; '0' if the border style is 'none' or
   'hidden'". css-backgrounds-3 §3.3 states the same line as "absolute length, snapped as a border width" and
   moves the none/hidden rule to the USED value ("if the border-style corresponding to a given border-width is
   none or hidden, then the used width is 0", plus the note that the used INITIAL width is therefore 0).
 * THE TWO SPECS PUT ONE RULE AT TWO DIFFERENT STAGES AND THE DIFFERENCE IS OBSERVABLE, so the choice is made
 * here rather than left to whichever caller asks first. CSSOM VIEW §6's `clientTop` returns "the unscaled
 * COMPUTED value of the border-top-width property" — so under the level-3 split a `border-style: none` box
 * would report its declared width as `clientTop`, which is the answer no user agent gives. Applying the rule
 * at the COMPUTED value satisfies every normative sentence in both documents instead of choosing between them:
 * 0 IS an absolute length, so §3.3's computed-value line holds; the used value is then 0 as well, so §3.3's
 * used-value sentence and its note hold; and css-backgrounds-3 makes the RESOLVED value the used value, which
 * is that same number, so getComputedStyle answers `0px` either way. The reverse choice satisfies only one.
 * THE STYLE IS THEREFORE READ FIRST, which is why these two longhands cannot be derived independently and why
 * `border-style` had to be modelled before `border-width` could be. */
static bool css_border_width_off(lxb_dom_element_t *el, int side)
{
    char sibling[32];
    char *style;
    bool off;

    DCHECK(side >= 0, "the `border-*-width` computed-value rule was asked for a property that is not one of "
                      "the four — css_computed_models and this switch are one list and have come apart");
    snprintf(sibling, sizeof sibling, "border-%s-style", CSS_BORDER_SIDES[side < 0 ? 0 : side]);
    style = css_computed_value(el, sibling);
    off = css_cv_is(style, "none") || css_cv_is(style, "hidden");
    free(style);
    return off;
}

/* §8.5.1's SECOND CLAUSE applied to a width that is already a number — the half of the rule that is about THIS
   element rather than about the declaration, which is why an INHERITED width takes it too: a child of a
   `border-top-width: 4px` parent that declares `border-top-style: none` computes 0, and the parent's 4 is not
   what it inherits. The snap is not re-applied: the parent's value is already a whole number of device pixels
   at the ratio of the document the two share (a parent and a child are one document by construction), and
   §6's algorithm is the identity on a length that has been through it. */
static CssLength css_border_width_inherited(lxb_dom_element_t *el, int side, CssLength inherited)
{
    if (css_border_width_off(el, side)) return css_cv_px(css_px(0.0));
    DCHECK(inherited.kind == CSS_LENGTH_ABSOLUTE,
           "a `border-*-width` inherited a computed value that is not an ABSOLUTE LENGTH — §3.3's grammar "
           "admits no percentage and no keyword past the computed value, and the arm below produces one of "
           "§3.3's three pinned numbers or an absolutized length and nothing else");
    return inherited;
}

static CssLength computed_border_width(lxb_dom_element_t *el, const char *name, char *spec)
{
    int side = css_border_side_of(name, "width");
    CssLength len;
    unsigned i;

    /* §8.5.1's second clause is a NUMBER and not a snapped one: "0 if the border style is none or hidden" —
       zero is an integer number of device pixels at every ratio, so it is the same 0 on every display and it
       derives from no environment fact. That is the initial state of almost every element, which is why the
       device pixel ratio does not reach most of the tree. */
    if (css_border_width_off(el, side)) { free(spec); return css_cv_px(css_px(0.0)); }
    for (i = 0; i < sizeof(CSS_LINE_WIDTH) / sizeof(CSS_LINE_WIDTH[0]); i++) {
        if (strcmp(CSS_LINE_WIDTH[i].kw, spec) != 0) continue;
        free(spec);
        return css_cv_px(css_length_snap_line_width(css_cv_realm(el), css_px(CSS_LINE_WIDTH[i].px)));
    }
    len = computed_length(el, name, spec);
    DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
           "a `border-*-width` cascaded to a value that is neither one of §3.3's three keywords nor a length. "
           "`<line-width> = <length [0,∞]> | thin | medium | thick` admits nothing else — no percentage and no "
           "`auto` — so this is a declaration that reached the cascade without its grammar, which is exactly "
           "what css_shorthand.c validates for the two shorthands lexbor's registry does not carry");
    DCHECK(len.px.px >= 0.0,
           "a NEGATIVE `border-*-width` reached the computed value. css-backgrounds-3 §3.3 states it outright "
           "— 'Negative values are invalid' — so the declaration should have been DROPPED by the grammar that "
           "admitted it rather than absolutized here");
    len.px = css_length_snap_line_width(css_cv_realm(el), len.px);
    return len;
}

/* The ten physical box-model lengths plus the four sizing limits — one list, because every one of them takes
   the same computed-value rule above and because used_value.c reads the limits to decide §10.4's clamp. */
static bool css_models_length(const char *name)
{
    static const char *const LENGTHS[] = {
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding-top", "padding-right", "padding-bottom", "padding-left",
        "width", "height", "min-width", "max-width", "min-height", "max-height",
    };
    unsigned i;

    for (i = 0; i < sizeof(LENGTHS) / sizeof(LENGTHS[0]); i++)
        if (strcmp(LENGTHS[i], name) == 0) return true;
    return false;
}

/* ---- css-fonts-4 §2.5 "Font size: the font-size property" ------------------------------------------------- */

/* §2.5's `Computed value:` LINE IS `an absolute length` WITH NO SECOND ARM, which is what makes this property
 * different in kind from every other length above and is the whole reason it needed its own rule. The ten
 * box-model lengths say "the percentage AS SPECIFIED or the absolute length" because their percentage refers to
 * the containing block, which no cascade step knows; §2.5's `Percentages:` line says "refer to PARENT ELEMENT'S
 * FONT SIZE", which the cascade knows exactly, so the percentage resolves HERE and never survives to the
 * computed value. Neither does a keyword: §2.5.1's eight `<absolute-size>` entries are the user agent's own
 * table of font sizes (core/css/font_size_functions.h) and §2.5's two `<relative-size>` keywords are a ratio of
 * the parent's number. So EVERY arm ends in an absolute length, and the assert below is that sentence.
 *
 * IT IS ALSO THE BASE OF css-values-4 §6.1.1's `em` AND `rem`, and the recursion through `computed_length`
 * terminates for a reason worth naming: `font-size` is a FONT-AFFECTING property, so an `em` inside it resolves
 * against the PARENT (the metrics above), and the walk therefore always moves one node up the flattened tree
 * and stops at the root with §7.2's initial value. Answering that predicate false would make it a definition of
 * itself, which is why this asserts it rather than assuming it. */
static CssLength computed_font_size(lxb_dom_element_t *el, char *spec)
{
    CssLength len;

    DCHECK(css_font_affecting_property("font-size"),
           "css-values-4 §6.1.1's font-affecting predicate no longer answers TRUE for `font-size`, so an `em` "
           "in a font-size declaration would resolve against the ELEMENT'S OWN computed font-size — the value "
           "being computed. §6.1.1 states the rule the other way ('when used in the value of any "
           "font-affecting property on the element they refer to, the font-relative lengths resolve against "
           "the computed metrics of the parent element'), and the difference here is not a wrong number but an "
           "unbounded walk: restore the row in core/css/font_size_functions.c");
    if (css_absolute_size_keyword(spec, strlen(spec))) {
        CssPx px = css_absolute_size_px(css_cv_realm(el), spec);

        free(spec);
        return css_cv_px(px);
    }
    /* §2.5's `<relative-size>`: "interpreted relative to the computed font-size of the parent element". The
       PARENT's, on an element with no parent element too — §7.2's initial value is what the metrics answer
       there, so `html { font-size: larger }` is a ratio of `medium` rather than of itself. */
    if (css_cv_kw_is(spec, "larger") || css_cv_kw_is(spec, "smaller")) {
        bool larger = css_cv_kw_is(spec, "larger");
        CssPx px = css_relative_size_px(css_cv_parent_font_size(el), larger);

        free(spec);
        return css_cv_px(px);
    }
    if (css_cv_kw_is(spec, "math")) {
        free(spec);
        DFAIL("css-fonts-4 §2.5 (Font size: the font-size property)'s `math` value reached the computed value: "
              "\"special mathematical scaling rules must be applied when determining the computed value of the "
              "font-size property\". Those rules are MathML Core §4.5 (The math-depth property), which states "
              "the whole procedure — the computed value is the INHERITED font-size times a scale factor "
              "derived from the inherited and computed `math-depth`, from `math-style`, and from the inherited "
              "first available font's OpenType MATH table (`scriptPercentScaleDown` and "
              "`scriptScriptPercentScaleDown`, with a fallback constant of 0.71 per level otherwise). THREE "
              "things are missing and they are not one absence: `math-depth` is a property with its own "
              "computed-value rule (`auto-add | add(<integer>) | <integer>`, resolved against the INHERITED "
              "value and against `math-style`), `math-style` is a `Computed value: specified keyword` line "
              "that is one row of css_computed_models' as-specified arm plus one of css_shorthand_complete_for "
              "— and lexbor's property registry carries NEITHER, so §6's cascade answers nothing for either "
              "and §7.1 has no initial value to fall to. The MATH table is the FONT RECORD css_length.c's "
              "font-metric crash names. BUILD the two properties first: with no OpenType MATH table §4.5's "
              "procedure still answers, out of its own 0.71 constant");
        return css_cv_px(css_px(0.0));
    }
    len = computed_length(el, "font-size", spec);
    /* §2.5's `Percentages: refer to parent element's font size` — resolved HERE, because that is a value the
       cascade has and not a containing block a layout would have to produce. A percentage of the parent is
       what makes `em` and `%` two spellings of one thing on this property, which §2.5's own example set says
       (`em { font-size: 150% }` beside `em { font-size: 1.5em }`).
       AND `font-size` IS css-values-4 §10.11 "Computed Value"'s OWN WORKED EXAMPLE of the property that
       resolves them here rather than leaving them in the function: "whereas font-size computes percentage
       values at computed value time so that font-relative length units can be computed, background-position has
       layout-dependent behavior for percentage values, and thus does not resolve percentages until used-value
       time … font-size will compute such expressions directly into a length." So `calc(150% - 2px)` on this
       property is ONE absolute length, resolved against the same basis §2.5 gives a bare percentage, and the
       two arms are one because §10.11 makes them one value. */
    if (len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED)
        len = css_cv_px(css_length_resolve_pct(len, css_cv_parent_font_size(el)));
    DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
           "a `font-size` cascaded to a value that is neither one of css-fonts-4 §2.5's keywords nor a "
           "`<length-percentage>`. §2.5's `Value:` line is `<absolute-size> | <relative-size> | "
           "<length-percentage [0,∞]> | math` and admits nothing else — no `auto`, no `normal`, no bare "
           "`<number>` — so this is a declaration that reached the cascade without its grammar, which lexbor's "
           "own `font-size` state machine and core/css/css_font_shorthand.c's `<'font-size'>` both refuse");
    /* css-values-4 §9.1 "Numeric Functions"'S CLAMP, AND WHY IT IS A CLAMP RATHER THAN THE ASSERT THAT STOOD
       HERE. §2.5's range is `[0,∞]` and a negative LITERAL is a dropped declaration — §5.1 "Range Restrictions
       and Range Definition Notation": "if the value is outside the allowed range, then unless otherwise
       specified, the declaration is invalid and must be ignored" — which is what the assert asserted and is
       still true. What is no longer true is that nothing else can be negative: §9.1 states the opposite rule
       for a math function, "as the value of a numeric function can't, generally, be known at parse time when
       range restrictions are enforced, numeric functions returning out-of-range values NEVER cause a
       declaration to become invalid. Instead, the value of a numeric function is CLAMPED to the range allowed
       in the context it is used at computed value time if possible". `font-size: calc(1rem - 20px)` is
       therefore a valid declaration computing to 0, and asserting it away would report a page's own CSS as an
       engine invariant. The two are told apart by the derivation and not by a flag: a negative literal cannot
       reach here, so a negative that does is a top-level calculation's result.
       IT IS `css_px_max` AND NOT AN `if`, because the clamped-away value's environment facts are the value's:
       `calc(1rem - 20px)` is a function of the reader's DEFAULT FONT SIZE at every viewport, including the ones
       where it lands at the floor, and taking the bare 0 would delete the arm where the reader's own preference
       makes it positive. */
    len.px = css_px_max(len.px, css_px(0.0));
    return len;
}

/* ---- the computed value ----------------------------------------------------------------------------------- */

bool css_computed_models_length(const char *name)
{
    DCHECK(name != NULL, "the computed-value model question was asked about a NULL property name");
    /* `font-size` IS length-valued and is deliberately NOT in the list above: css-fonts-4 §2.5's computed
       value is `an absolute length` with no percentage arm and no keyword arm, which is a DIFFERENT rule from
       the one every member of that list shares, and folding it in would hand it to `computed_length` whole. */
    return css_models_length(name) || css_border_side_of(name, "width") >= 0 ||
           strcmp(name, "font-size") == 0;
}

bool css_computed_models(const char *name)
{
    DCHECK(name != NULL, "the computed-value model question was asked about a NULL property name");
    return strcmp(name, "overflow-x") == 0 || strcmp(name, "overflow-y") == 0 ||
           strcmp(name, "display") == 0 || strcmp(name, "float") == 0 || strcmp(name, "position") == 0 ||
           strcmp(name, "box-sizing") == 0 || strcmp(name, "white-space") == 0 ||
           strcmp(name, "direction") == 0 || strcmp(name, "writing-mode") == 0 ||
           css_computed_models_length(name) ||
           css_border_side_of(name, "style") >= 0;
}

/* THE THREE CONDITIONS EVERY COMPUTED-VALUE RULE BELOW IS DERIVED UNDER. Both entries go through it, so
   neither can be reached with a property this component does not model or whose shorthands are unexpanded. */
static void css_cv_modelled(lxb_dom_element_t *el, const char *name)
{
    DCHECK(el != NULL && name != NULL, "a computed value was asked for with no element or no property name");
    DCHECK(css_computed_models(name),
           "a computed value was asked for a property this component does not derive. It answers a NAMED set "
           "(css_computed_models) and crashes outside it rather than handing back a specified value under the "
           "word `computed` — the two differ for every length-valued property, and the caller asking is a spec "
           "algorithm that reads the computed one. BUILD the property's own `Computed value:` line here, and "
           "record the shorthands that can set it in css_shorthand.c");
    DCHECK(css_shorthand_complete_for(name),
           "a computed value was derived for a property whose SHORTHANDS are not all expanded by "
           "css_shorthand.c, so the cascade it reads may never have looked at the declaration that set it — a "
           "`margin: 0` two lines above a `margin-top` read is invisible, and the answer is a real number with "
           "nothing to say it is the initial value. Record the complete set in css_shorthand_complete_for");
}

CssLength css_computed_length(lxb_dom_element_t *el, const char *name)
{
    int side;
    char *cascaded;

    DCHECK(css_computed_models_length(name),
           "css_computed_length was asked for a property whose `Computed value:` line is a KEYWORD and not a "
           "length — `display`, `float`, `position`, `box-sizing`, an overflow axis or a `border-*-style`. "
           "There is nothing to absolutize and nothing for a `CssPx` to carry, so the entry that answers those "
           "is css_computed_value, and asking this one would report a keyword as the number zero");
    css_cv_modelled(el, name);
    side = css_border_side_of(name, "width");
    cascaded = cssom_cascaded_value(el, name);
    switch (css_defaulting_of(name, cascaded)) {
    case CSS_DEFAULTING_DECLARED:
        break;
    case CSS_DEFAULTING_INHERITED: {
        /* §7.3.2: "the property's specified and computed values are the inherited value" — and the inherited
           value is a `CssLength` that has ALREADY been absolutized against the environment it derives from, so
           it is taken WHOLE rather than serialized and re-parsed. The box-model lengths' computed-value line is
           "the percentage as specified or the absolute length", which is the identity over a value that is
           already one of those, and css-fonts-4 §2.5's is `an absolute length`, which is the identity over one
           too — an inherited `font-size` is a NUMBER and §2.5's keyword and percentage arms were resolved one
           node up, where their base was. The exception is a border width, whose rule reads THIS element's own
           `border-*-style` and is therefore re-applied. */
        lxb_dom_element_t *parent = css_cv_parent_element(el);

        free(cascaded);
        if (parent != NULL) {
            CssLength inherited = css_computed_length(parent, name);

            return side >= 0 ? css_border_width_inherited(el, side, inherited) : inherited;
        }
        cascaded = NULL;
        break;
    }
    case CSS_DEFAULTING_INITIAL:
        free(cascaded);
        cascaded = NULL;
        break;
    }
    /* §7.1's initial value, which is also §7.2's answer for the root element: "for the root element, which has
       no parent element, the inherited value is the initial value of the property". */
    if (cascaded == NULL) cascaded = cssom_initial_value(name);
    DCHECK(cascaded != NULL,
           "§7.1 produced no INITIAL value for a length-valued property this component models — every one of "
           "them is in lexbor's registry with an initial value, or in css_style_declaration.c's table of the "
           "eight border longhands the registry does not carry");
    if (side >= 0) return computed_border_width(el, name, cascaded);
    if (strcmp(name, "font-size") == 0) return computed_font_size(el, cascaded);
    return computed_length(el, name, cascaded);
}

char *css_computed_value(lxb_dom_element_t *el, const char *name)
{
    char *spec;

    DCHECK(!css_computed_models_length(name),
           "css_computed_value was asked for a property whose `Computed value:` line is `the percentage as "
           "specified or THE ABSOLUTE LENGTH`. An absolute length is where a `50vw` is resolved against the "
           "INITIAL CONTAINING BLOCK and a `border-*-width` is snapped to a DEVICE PIXEL, and both of those "
           "rectangles are PICKED environment facts (core/frame/viewport.h) — so the answer is a `CssPx` and "
           "text would carry the number while dropping the domain behind it, which is the fork "
           "`getComputedStyle(el).width < 768` shares with `innerWidth < 768`. Ask css_computed_length");
    css_cv_modelled(el, name);
    spec = css_cv_specified(el, name);
    DCHECK(spec != NULL,
           "§7's defaulting produced no SPECIFIED value for a keyword-valued property this component models — "
           "every one of them is in lexbor's registry with an initial value, or in css_style_declaration.c's "
           "table of the eight border longhands the registry does not carry");
    if (strcmp(name, "overflow-x") == 0 || strcmp(name, "overflow-y") == 0)
        return computed_overflow(el, name, spec);
    if (strcmp(name, "display") == 0)
        return computed_display(el, spec);
    /* `float` (CSS2 §9.5.1), `position` (css-position §2) and `box-sizing` (css-sizing §5) all state "Computed
       value: as specified" (`box-sizing`'s line is "specified keyword"), and a keyword has no absolutization to
       do — so the specified value IS the answer here rather than a stand-in for one. css-backgrounds-3 §3.2
       gives `border-*-style` the same line ("specified keyword"), which is what lets `border-top-width`'s rule
       above read its sibling with no second derivation in between. css-text-3's `white-space` carries the same
       line ("Computed value: specified keyword") and is asked for by CSS 2.1 §9.2.2.1's question about which
       inter-element white space is collapsed away — the one the block-flow walk asks of a text child. css-text-4
       redefines it as a shorthand of `white-space-collapse`/`text-wrap-mode`, neither of which lexbor's property
       registry carries, so §16.6's longhand is the one this engine can answer.
       css-writing-modes-4 §2.1 "Specifying Directionality: the direction property" and §3.2 "Block Flow
       Direction: the writing-mode property" both state `Computed value: specified value` over a keyword-only
       `Value:` line (`ltr | rtl`, and `horizontal-tb | vertical-rl | vertical-lr | sideways-rl | sideways-lr`),
       so the as-specified arm is the whole of their rule too. THEY ARE HERE BECAUSE THREE ALGORITHMS ASK FOR
       ONE OF THEM BY NAME and could not: CSS 2.1 §10.3.3's over-constrained case ignores `margin-right` "if the
       'direction' property of the containing block has the value 'ltr'" and `margin-left` if it is `rtl`, CSS 2
       §9.4.1 touches a box's LEFT outer edge to its containing block's left edge "(for right-to-left
       formatting, right edges touch)", and CSSOM VIEW §2's scrolling area is stated over a scrolling box's
       OVERFLOW DIRECTIONS, which css-writing-modes-4 §6.2 "Flow-relative Directions" derives from the two
       together — "determining the inline-start and inline-end sides of a box depends not only on the
       writing-mode property but also the direction property". Each of those three crashed for want of this one
       row. */
    DCHECK(strcmp(name, "float") == 0 || strcmp(name, "position") == 0 || strcmp(name, "box-sizing") == 0 ||
               strcmp(name, "white-space") == 0 || strcmp(name, "direction") == 0 ||
               strcmp(name, "writing-mode") == 0 || css_border_side_of(name, "style") >= 0,
           "a property this component claims to model reached the as-specified arm without a `Computed value: "
           "as specified` line to justify it — css_computed_models and this switch are one list and have come "
           "apart");
    return spec;
}

/* ---- CSSOM §9's resolved value ---------------------------------------------------------------------------- */

CssResolvedKind css_resolved_kind(const char *name)
{
    /* §9's own table, in its own order. Each list is the spec's, including the logical-property spellings —
       a page reads `marginInlineStart` exactly as it reads `marginLeft`, and a list that carries one and not
       the other answers the same question two ways. */
    static const char *const USED[] = {
        "background-color", "border-block-end-color", "border-block-start-color", "border-bottom-color",
        "border-inline-end-color", "border-inline-start-color", "border-left-color", "border-right-color",
        "border-top-color", "box-shadow", "caret-color", "color", "outline-color",
    };
    static const char *const USED_IF_RENDERED[] = {
        "block-size", "height", "inline-size", "margin-block-end", "margin-block-start", "margin-bottom",
        "margin-inline-end", "margin-inline-start", "margin-left", "margin-right", "margin-top",
        "padding-block-end", "padding-block-start", "padding-bottom", "padding-inline-end",
        "padding-inline-start", "padding-left", "padding-right", "padding-top", "width",
        /* css-transforms §4: "The transform-origin property is a resolved value special case property like
           height." */
        "transform-origin",
    };
    static const char *const USED_IF_POSITIONED[] = {
        "bottom", "left", "inset-block-end", "inset-block-start", "inset-inline-end", "inset-inline-start",
        "right", "top",
    };
    unsigned i;

    DCHECK(name != NULL, "CSSOM §9's classification was asked about a NULL property name");
    for (i = 0; i < sizeof(USED) / sizeof(USED[0]); i++)
        if (strcmp(USED[i], name) == 0) return CSS_RESOLVED_USED;
    for (i = 0; i < sizeof(USED_IF_RENDERED) / sizeof(USED_IF_RENDERED[0]); i++)
        if (strcmp(USED_IF_RENDERED[i], name) == 0) return CSS_RESOLVED_USED_IF_RENDERED;
    for (i = 0; i < sizeof(USED_IF_POSITIONED) / sizeof(USED_IF_POSITIONED[0]); i++)
        if (strcmp(USED_IF_POSITIONED[i], name) == 0) return CSS_RESOLVED_USED_IF_POSITIONED;
    if (strcmp(name, "line-height") == 0) return CSS_RESOLVED_LINE_HEIGHT;
    if (strcmp(name, "transform") == 0) return CSS_RESOLVED_TRANSFORM;
    return CSS_RESOLVED_COMPUTED;
}

/* CSSOM §6.7.2's serialization of a length, and the ONE place a length crosses into a page. The string is the
   EXAMPLE; `viewport_env_derived` decides whether it crosses as itself or as the example of a concolic whose
   domain is the environment's, from the fact the length carries (css_length.h). BOTH of §9's answers come
   through here — a USED width off §10.3.3's equation and a COMPUTED `border-*-width` off §6's snap are derived
   from different facts and are the same kind of answer, and a second serializer for one of them is a second
   place for the fact to be dropped. */
static JSValue css_resolved_px(JSContext *ctx, CssPx len)
{
    char *text = css_length_serialize_px(len.px);
    JSValue v = JS_NewString(ctx, text);

    free(text);
    return viewport_env_derived(len, v);
}

/* The computed value, for a property whose resolved value §9 says the computed value IS. A length-valued one
   is serialized from the `CssPx` its own entry answers rather than from text, which is what carries the
   environment fact across: `getComputedStyle(el).borderTopWidth` is a device-pixel-ratio question and
   `getComputedStyle(el).width` on a `50vw` box is a viewport question, and neither is a number the author's
   own declarations determined. */
static JSValue css_resolved_computed(JSContext *ctx, lxb_dom_element_t *el, const char *name)
{
    char *v;
    JSValue out;

    if (css_computed_models_length(name)) {
        CssLength len = css_computed_length(el, name);

        if (len.kind == CSS_LENGTH_ABSOLUTE)   return css_resolved_px(ctx, len.px);
        if (len.kind == CSS_LENGTH_PERCENTAGE) {
            /* "The percentage AS SPECIFIED" — §8.3, §8.4 and §10.2's computed value for one, serialized by
               §6.7.2's own rule for a `<percentage>`. It resolves against the containing block, and that is a
               USED value this arm has already established §9 does not ask for. */
            char *pct = css_length_serialize_pct(len.pct);

            out = JS_NewString(ctx, pct);
            free(pct);
            return out;
        }
        if (len.kind == CSS_LENGTH_CALCULATED) {
            char *calc;

            /* css-values-4 §10.13 "Serialization"'s Sum branch — see css_length.h for the shape and for
               §10.13's own `calc(20px + 0%)` → `calc(0% + 20px)` example that pins it.
               THE ONE RESIDUE IT CANNOT NAME IS THE ONE WITH NO LENGTH TERM. §10.10.1 "Simplification" ends a
               Sum with "if root has only a single child at this point, return the child", so `calc(50%)`
               simplifies to a bare `<percentage>` and serializes as `50%`, while `calc(50% + 0px)` keeps both
               terms because "zero-valued terms cannot be simply removed from a Sum" — and the pair that arrives
               here is identical in both cases. A carried ENVIRONMENT FACT settles it whenever there is one
               (only a `<length>` leaf can put one there, so its presence proves the dimension term is), and a
               plain zero with no fact does not.
               THIS IS THE ONLY PLACE THE DISTINCTION IS OBSERVABLE — every used value resolves the two to the
               same number against every basis — which is why it crashes HERE rather than being carried by
               every `CssLength` in the engine. */
            if (len.px.px == 0.0 && len.px.env == CSS_ENV_NONE)
                DFAIL("css-values-4 §10.13 \"Serialization\" was asked for a math function whose residue is a "
                      "percentage and a ZERO length carrying no environment fact, and the two values that "
                      "produce it serialize DIFFERENTLY: `calc(50%)` is §10.10.1's single-child Sum, which "
                      "\"returns the child\", so its computed value is a bare `<percentage>` and §10.13's step "
                      "for a root that \"is a numeric value (number, percentage, or dimension)\" writes `50%`; "
                      "`calc(50% + 0px)` keeps both terms, because \"zero-valued terms cannot be simply removed "
                      "from a Sum\", and writes `calc(0% + 0px)`. `CssMathValue` records `pct_term` and has no "
                      "`num_term`, so the Sum's dimension child is not known to have existed. BUILD the "
                      "symmetric flag in core/css/css_math.c — the same §10.10.1 sentence justifies both, and "
                      "the producers are the ones that already write `pct_term` — then `css_length_parse` "
                      "answers CSS_LENGTH_PERCENTAGE for the single-child Sum and this arm has no ambiguity "
                      "left to crash on");
            calc = css_length_serialize_calc(len.pct, len.px.px);
            out = JS_NewString(ctx, calc);
            free(calc);
            /* The percentage is unresolved, so what this string DERIVES from is the length term's facts — a
               `calc(100% - 2rem)` reports a different `calc(100% - Npx)` to a reader whose default font size
               differs, and that is the same fork `css_resolved_px` mints for a bare `2rem`. */
            return viewport_env_derived(len.px, out);
        }
        DCHECK(len.kind == CSS_LENGTH_KEYWORD,
               "a computed length is none of the four kinds css_length.h defines — the parse answers exactly "
               "one of them and crashes rather than inventing a fifth");
        return JS_NewString(ctx, len.keyword);
    }
    v = css_computed_models(name) ? css_computed_value(el, name) : css_cv_specified(el, name);
    /* A property no cascade layer answers at all — a custom property nobody set — is the EMPTY STRING, which
       is §6.6.1's own answer for one that is not set rather than a default standing in for a value. */
    out = v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
    free(v);
    return out;
}

/* CSS Color 4's `currentcolor`, RESOLVED — "the used value of the `currentcolor` keyword is the used value of
   the `color` property on the same element", and "if the `currentcolor` keyword is set on the `color` property
   itself, it is treated as `color: inherit`".
   BOTH ARMS ARE HERE AND NOT IN css_color.c, because both are questions about the ELEMENT and the TREE. That
   component's parse is called with a string and no context element (its own header says so), so it resolves
   the keyword against the `color` property's INITIAL value — which is exactly right for HTML §4.10.5.1.14's
   colour well control, and would be an element's colour answered from nowhere here.
   THE RECURSION TERMINATES AT THE ROOT, where §7.2's inherited value is the initial value and the initial value
   of `color` is not `currentcolor` — asserted, because that is the only shape that would make it a definition
   of itself. OWNED. */
static char *css_cv_used_color_value(lxb_dom_element_t *el, const char *name)
{
    char *spec = css_cv_specified(el, name);
    lxb_dom_element_t *parent;

    if (!css_cv_kw_is(spec, "currentcolor")) return spec;
    free(spec);
    if (strcmp(name, "color") != 0) return css_cv_used_color_value(el, "color");
    parent = css_cv_parent_element(el);
    if (parent != NULL) return css_cv_used_color_value(parent, "color");
    spec = cssom_initial_value("color");
    DCHECK(!css_cv_kw_is(spec, "currentcolor"),
           "`color`'s own INITIAL value came back as `currentcolor`, which makes the root element's used colour "
           "a definition of itself. CSS Color 4 gives the property an initial value of `canvastext`, so this is "
           "lexbor's registry answering something else and the walk above has no base case");
    return spec;
}

/* §9's two escapes read "the resolved value of the display property", which is the computed one — `display` is
   in "any other property". */
static bool resolved_display_generates_a_box(lxb_dom_element_t *el)
{
    char *d = css_computed_value(el, "display");
    bool box = !css_cv_is(d, "none") && !css_cv_is(d, "contents");

    free(d);
    return box;
}

/* §6.6.1's getPropertyValue SHORTHAND STEP, over RESOLVED values — "for each longhand property longhand that
   property maps to, IN CANONICAL ORDER ... if declaration is null, then return the empty string ... return the
   serialization of list". A COMPUTED block reaches it here rather than through the block's own declarations
   because §7.2's declarations are not stored: they are "the resolved value of every longhand property", derived
   per read, so the list this step builds is built by asking for each longhand's resolved value.
   THE CASCADE IS OVER LONGHANDS, so a shorthand that fell past this would reach cssom_cascaded_value, which
   asserts against exactly that: no layer declares a shorthand, and the answer would be the property's initial
   value with nothing to say the longhands that DID set it were never looked at. */
static JSValue css_resolved_shorthand(JSContext *ctx, lxb_dom_element_t *el, const char *name,
                                      const char *const *lh, unsigned n)
{
    JSValue parts[CSS_SHORTHAND_MAX_LONGHANDS];
    const char *values[CSS_SHORTHAND_MAX_LONGHANDS];
    unsigned i, held = 0, converted = 0;
    bool all = true;
    char *value = NULL;
    JSValue out;

    CHECK(n <= CSS_SHORTHAND_MAX_LONGHANDS,
          "cssom: a shorthand's longhand list outgrew the array §9's resolved value sized from it");
    for (i = 0; i < n && all; i++) {
        parts[i] = css_resolved_value(ctx, el, lh[i]);
        if (JS_IsException(parts[i])) { all = false; break; }
        held = i + 1;
        /* A RESOLVED VALUE THAT IS NOT A PLAIN STRING carries a DOMAIN — a used value derived from the viewport
           or the device pixel ratio — and §6.7.2's consolidation is a joint function of all of them, so the
           result would have to carry every operand's fact the way css_px_combine does. Serializing the example
           out of it here would drop the fork instead. */
        DCHECK(JS_IsString(parts[i]),
               "a shorthand's longhand resolved to a value carrying a DOMAIN rather than a plain string, and "
               "§6.7.2's serialize-a-CSS-value over a list has no way to combine four of those into one. THE "
               "SHORTHAND THAT REACHES THIS FIRST IS `font`, and it reaches it for almost every element rather "
               "than for a declared few: css-fonts-4 §2.5's computed `font-size` is a function of "
               "CSS_ENV_DEFAULT_FONT_SIZE whenever it was not declared in an absolute unit, and `medium` — the "
               "initial value, and therefore the root element's inherited one — IS that fact. A "
               "`border-*-width` only carried a domain when a border was actually declared, which is why this "
               "assert stood so long without firing. BUILD the combination in core/css/css_length.h's terms — "
               "css_px_combine is the shape: the result carries every operand's environment fact — and let "
               "this step produce a concolic string rather than a concrete one. The join is over the FACT SETS "
               "and not over the strings, so what this step needs is each part's `CssPx` beside its "
               "serialization; reading a set back out of a finished JSValue is the wrong direction");
        values[i] = JS_ToCString(ctx, parts[i]);
        if (!values[i]) { all = false; break; }
        converted = i + 1;
        /* "If declaration is null, then return the empty string" — which is the answer for a longhand this
           build resolves no value for, and therefore for every shorthand one of whose longhands it does not. */
        if (*values[i] == '\0') all = false;
    }
    if (all) value = css_shorthand_serialize_value(name, (const char *const *)values);
    for (i = 0; i < converted; i++) JS_FreeCString(ctx, values[i]);
    for (i = 0; i < held; i++) JS_FreeValue(ctx, parts[i]);
    out = value ? JS_NewString(ctx, value) : JS_NewStringLen(ctx, "", 0);
    free(value);
    return out;
}

JSValue css_resolved_value(JSContext *ctx, lxb_dom_element_t *el, const char *name)
{
    const char *const *lh;
    unsigned nlh;
    JSValue out;

    DCHECK(ctx != NULL, "a resolved value was asked for with no realm to answer it in — the string is created "
                        "there, and a used value derived from the viewport mints its domain in the element's "
                        "document's realm, which is a different one and is read from the element itself");
    DCHECK(el != NULL && name != NULL, "a resolved value was asked for with no element or no property name");
    lh = css_shorthand_longhands(name, &nlh);
    if (lh) return css_resolved_shorthand(ctx, el, name, lh, nlh);
    switch (css_resolved_kind(name)) {
    case CSS_RESOLVED_COMPUTED:
        break;
    case CSS_RESOLVED_USED_IF_RENDERED:
        /* "If the property applies to the element and the resolved value of the display property is not none
           or contents, then the resolved value is the used value. Otherwise the resolved value is the computed
           value." BOTH conjuncts are real branches and both are taken. The first is the property's own
           `Applies to:` line (core/css/css_property_applies.h) and it is what answers `width` on a
           non-replaced inline element — `auto`, in every user agent, and not a used value that does not exist.
           The second is `display: none`. Past them, the used value is CSS 2.1 §10's, and
           core/layout/used_value.h computes it — §10.1's containing block and §10.3.3's equation included —
           crashing by section for the arms that need an intrinsic size. */
        if (!css_property_applies(el, name) || !resolved_display_generates_a_box(el)) break;
        return css_resolved_px(ctx, used_value_px(el, name));
    case CSS_RESOLVED_USED_IF_POSITIONED:
        /* "If the property applies to a positioned element and the resolved value of the display property is
           not none or contents, and the property is not over-constrained, then the resolved value is the used
           value. Otherwise the resolved value is the computed value." The first conjunct IS "positioned" —
           CSS 2.1 §9.3.2's `Applies to:` line for the insets is "positioned elements" — so it is asked
           through the same entry as every other applies-to line rather than re-derived here from `position`,
           which is what this branch used to do. A STATICALLY positioned element is the common case and is
           answered. */
        if (!css_property_applies(el, name) || !resolved_display_generates_a_box(el)) break;
        DFAIL("CSSOM §9 makes an inset property's resolved value the USED value for a POSITIONED element that "
              "generates a box, and this element is one. THE CONTAINING BLOCK IS NO LONGER THE BLOCKER — "
              "core/layout/used_value.c answers §10.1's width now, which is what a percentage inset resolves "
              "against — and what is left is three things this component has not been asked for. (1) The four "
              "insets are not among the ten PHYSICAL BOX-MODEL LENGTHS `used_value_px` carries; adding them is "
              "adding a group, not a case. (2) CSS 2.1 §9.4.3 makes a RELATIVELY positioned box's `left` and "
              "`right` a PAIR rather than two values — both `auto` makes both 0, one `auto` makes it the "
              "negation of the other — so the entry has to be asked about the pair. (3) The over-constrained "
              "case of that pair, and §9's own THIRD conjunct which is stated over it, both turn on the "
              "containing block's `direction`, which the cascade inherits now (core/css/css_defaulting.h) and "
              "which css_computed_value models no entry for. An "
              "ABSOLUTELY positioned box is a fourth thing again: §10.3.7 solves its insets from the same "
              "constraint equation as its width, against the PADDING EDGE of its nearest positioned ancestor, "
              "which used_value.c's §10.1 fourth case crashes for and names in full");
        break;
    case CSS_RESOLVED_USED: {
        /* "The resolved value is the used value" — unconditionally, with no `Applies to:` conjunct and no
           `display` escape, which is why this arm asks neither. The used value of a COLOR is CSS Color 4's:
           the computed value with `currentcolor` resolved, converted to sRGB by §11 and serialized by §16.2 in
           the LEGACY comma form, which is what `rgb(255, 0, 0)` is and what a page comparing against
           getComputedStyle reads. */
        char *spec;
        CssColor color;
        char text[CSS_COLOR_FUNCTION_MAX];

        if (strcmp(name, "box-shadow") == 0) {
            DFAIL("CSSOM §9 puts `box-shadow` in the same unconditional used-value list as the colors, and it "
                  "is the one member that is not a `<color>`: css-backgrounds-3 §7.2 makes it a comma-separated "
                  "list of shadows, each two to four LENGTHS plus a colour plus an optional `inset`, and the "
                  "used value resolves the colour of each (a shadow with no colour is `currentcolor`) and "
                  "absolutizes each length. Every piece it needs exists — `css_cv_used_color_value` above "
                  "resolves the colour and css_length.h absolutizes the lengths — and what does not is the "
                  "GRAMMAR: `<shadow>#` is a list this file does not parse and lexbor's registry hands back as "
                  "one serialized string. BUILD the `<shadow>` list as its own component, since css-backgrounds "
                  "§7.2's serialization order (colour, offsets, blur, spread, `inset`) is a rule of its own");
            break;
        }
        DCHECK(css_shorthand_complete_for(name),
               "a used COLOR was derived for a property whose SHORTHANDS are not all expanded by "
               "css_shorthand.c, so the cascade it reads may never have looked at the declaration that set it "
               "— a `background: red` two lines above a `backgroundColor` read is invisible, and the answer "
               "would be the property's initial value with nothing to say so. THREE of §9's colors are in this "
               "state and each names one missing shorthand: `background-color` needs `background`, "
               "`outline-color` needs `outline`, and `caret-color` needs css-ui-4's `caret`. BUILD the "
               "shorthand's row in css_shorthand.c's table and record the longhand in "
               "css_shorthand_complete_for");
        spec = css_cv_used_color_value(el, name);
        if (spec == NULL) {
            DFAIL("CSSOM §9's used-value list carries the LOGICAL colour spellings beside the physical ones — "
                  "`border-block-start-color`, `border-inline-end-color` and the other two — and this engine "
                  "has no value for one at all: lexbor's property registry does not carry them, so §6's "
                  "cascade answers nothing and §7.1 has no initial value to fall to. They are not aliases: "
                  "css-logical §2 maps each to a PHYSICAL side through the element's computed `writing-mode` "
                  "and `direction`, which is the same mapping css_property_applies.c and css_length.c's "
                  "`vi`/`vb` arm name. BUILD the logical-to-physical mapping, and each of these becomes the "
                  "physical longhand it resolves to on this element");
            break;
        }
        if (!css_color_parse(spec, strlen(spec), &color)) {
            DFAIL("a colour-valued property's specified value is not a `<color>` CSS Color 4's grammar accepts. "
                  "The value that reaches here past `currentcolor` and past §7's CSS-wide keywords is either "
                  "css-ui-4's `outline-color: invert` — a real keyword whose used value is a COLOUR INVERSION "
                  "of what is behind the outline, which no serialization can name and which every user agent "
                  "answers as the computed keyword instead — or a `color-mix()`, a `light-dark()` or a "
                  "relative-colour form the parse does not have. BUILD the missing production in "
                  "core/css/css_color.c, and give `invert` §9's computed-value escape, which is the one place "
                  "a used colour has no number");
            free(spec);
            break;
        }
        free(spec);
        /* §11's conversion, which is what makes the serialization below sRGB's: a `lab()` or an `oklch()`
           declaration is a colour in another space until this runs, and §16.2's form is stated for sRGB. */
        css_color_convert(&color, CSS_COLOR_SPACE_SRGB);
        css_color_serialize_srgb(&color, text);
        return JS_NewString(ctx, text);
    }
    case CSS_RESOLVED_LINE_HEIGHT: {
        /* "The resolved value is normal if the computed value is normal, or the used value otherwise." The
           computed value is asked for through §7's defaulting because `line-height` is an INHERITED property:
           a child of a `line-height: 2` parent that declares none has a computed value of 2, not `normal`. */
        char *v = css_cv_specified(el, name);

        if (css_cv_is(v, "normal")) { out = JS_NewString(ctx, v); free(v); return out; }
        free(v);
        DFAIL("CSSOM §9 makes `line-height`'s resolved value the USED value whenever the computed value is not "
              "`normal` — the absolute length a LAYOUT laid the line boxes out with, which for a number or a "
              "percentage is that factor times the used font size. THE FONT SIZE CHAIN IS NO LONGER THE "
              "BLOCKER: css-fonts-4 §2.5's computed `font-size` is derived above and `css_computed_length(el, "
              "\"font-size\")` answers the absolute length this factor multiplies. What is missing is "
              "`line-height` itself as a modelled computed value — css-inline-3 §5.1 (Line Spacing: the "
              "line-height property) gives it a `Value:` of `normal | <number [0,∞]> | <length-percentage "
              "[0,∞]>`, a `Computed value:` of 'the specified keyword, a number, or a computed <length> "
              "value', and a `Percentages:` line of 'computed relative to 1em' — so a percentage becomes a "
              "LENGTH at computed-value time out of the same font size, which is a THIRD shape beside this "
              "file's two and needs its own arm rather than a row of css_models_length. BUILD it beside "
              "`font-size`: it is an inherited property whose shorthand set css_shorthand_complete_for already "
              "records, and past it the only thing left between the computed value and §9's used one is "
              "css-fonts-4 §2.5's own note that the used font size 'can differ from its computed value due to "
              "font-size-adjust'");
        break;
    }
    case CSS_RESOLVED_TRANSFORM: {
        /* css-transforms §3.2: "When the computed value is a <transform-list>, the resolved value is one
           matrix() function … For other computed values, the resolved value is the computed value." */
        char *v = css_cv_specified(el, name);

        if (css_cv_is(v, "none")) { out = JS_NewString(ctx, v); free(v); return out; }
        free(v);
        DFAIL("css-transforms §3.2 makes `transform` a resolved value special case: a <transform-list> resolves "
              "to ONE matrix() function, post-multiplying every function in the list into a 4x4 matrix and "
              "serializing that. Two pieces are missing — the transform-function grammar this file does not "
              "parse, and the TRANSFORM REFERENCE BOX (css-transforms §5) a percentage translation resolves "
              "against, which is the element's border box and therefore a layout. BUILD the function list and "
              "its matrix reduction; a list with no percentage needs no box, so that half lands first");
        break;
    }
    }
    /* §9's "any other property", and the value every escape above breaks to: the COMPUTED value. */
    return css_resolved_computed(ctx, el, name);
}
