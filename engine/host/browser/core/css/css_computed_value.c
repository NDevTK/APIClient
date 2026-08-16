/* CSS Cascade §Computed Value + CSSOM §9's resolved value. See css_computed_value.h for the split. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_property_applies.h"
#include "core/css/css_shorthand.h"
#include "core/css/css_style_declaration.h"
#include "core/dom/document.h"
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

/* CSS Cascade §7.3's CSS-WIDE KEYWORDS. See css_computed_value.h for the contract and for the second caller. */
bool css_wide_keyword(const char *v)
{
    static const char *const WIDE[] = { "inherit", "initial", "unset", "revert", "revert-layer" };
    unsigned i;
    size_t n;

    while (*v && isspace((unsigned char)*v)) v++;
    for (n = strlen(v); n > 0 && isspace((unsigned char)v[n - 1]); n--) { }
    for (i = 0; i < sizeof(WIDE) / sizeof(WIDE[0]); i++) {
        size_t k = strlen(WIDE[i]);
        size_t j;
        if (k != n) continue;
        for (j = 0; j < k; j++)
            if ((char)tolower((unsigned char)v[j]) != WIDE[i][j]) break;
        if (j == k) return true;
    }
    return false;
}

/* "The ROOT ELEMENT" — the element whose parent is the Document itself. CSS Display §2.8 gives it its own
   computed-value rules, and CSSOM VIEW §6 asks the same question of the same node. */
static bool css_is_root_element(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
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
    char *other = cssom_cascaded_value(el, xaxis ? "overflow-y" : "overflow-x");
    const char *self_v, *other_v;
    char *out;

    DCHECK(other != NULL,
           "the cascade produced no value for the other overflow axis — lexbor's property registry carries "
           "`overflow-x` and `overflow-y` with an initial value of `visible`, so the cascade's last layer "
           "always answers and a NULL here is a cascade that stopped early");
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
        char *d = cssom_cascaded_value(lxb_dom_interface_element((lxb_dom_node_t *)p), "display");

        DCHECK(d != NULL, "the cascade produced no `display` for an ancestor element — the UA layer answers "
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

/* ONE `Computed value:` LINE, WRITTEN THE SAME WAY IN TEN PROPERTY DEFINITIONS. CSS 2.1 §8.3 and §8.4 give the
   margins and the paddings "the percentage as specified or THE ABSOLUTE LENGTH"; §10.2 and §10.5 give `width`
   and `height` "the percentage or 'auto' as specified or the absolute length"; css-sizing says the same for the
   four min/max limits. So the whole rule is: ABSOLUTIZE a length and leave everything else alone — a percentage
   cannot be resolved here because it refers to the containing block, which is a USED value and belongs to
   core/layout/used_value.h, and `auto` is a keyword no cascade step turns into a number.
   THE ABSOLUTIZATION IS WHERE THE FONT AND THE VIEWPORT ENTER, and core/css/css_length.h is the one component
   that knows it: `50vw` becomes a number out of §10.1's initial containing block and carries that rectangle's
   environment fact the whole way to the page, and `2em` crashes naming the inheritance step it needs. */
static CssLength computed_length(JSContext *realm, char *spec)
{
    CssLength len = css_length_parse(realm, spec);

    free(spec);
    return len;
}

/* A computed value that IS an absolute length, with every other arm's field left in the one state a reader of
   the wrong arm would have to be reading by mistake. */
static CssLength css_cv_px(CssPx px)
{
    CssLength out = { CSS_LENGTH_ABSOLUTE, { 0.0, CSS_ENV_NONE, NULL }, 0.0, { '\0' } };

    out.px = px;
    return out;
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
static CssLength computed_border_width(lxb_dom_element_t *el, const char *name, char *spec)
{
    char sibling[32];
    int side = css_border_side_of(name, "width");
    char *style;
    bool off;
    CssLength len;
    unsigned i;

    DCHECK(side >= 0, "the `border-*-width` computed-value rule was asked for a property that is not one of "
                      "the four — css_computed_models and this switch are one list and have come apart");
    snprintf(sibling, sizeof sibling, "border-%s-style", CSS_BORDER_SIDES[side < 0 ? 0 : side]);
    style = css_computed_value(el, sibling);
    off = css_cv_is(style, "none") || css_cv_is(style, "hidden");
    free(style);
    /* §8.5.1's second clause is a NUMBER and not a snapped one: "0 if the border style is none or hidden" —
       zero is an integer number of device pixels at every ratio, so it is the same 0 on every display and it
       derives from no environment fact. That is the initial state of almost every element, which is why the
       device pixel ratio does not reach most of the tree. */
    if (off) { free(spec); return css_cv_px(css_px(0.0)); }
    for (i = 0; i < sizeof(CSS_LINE_WIDTH) / sizeof(CSS_LINE_WIDTH[0]); i++) {
        if (strcmp(CSS_LINE_WIDTH[i].kw, spec) != 0) continue;
        free(spec);
        return css_cv_px(css_length_snap_line_width(css_cv_realm(el), css_px(CSS_LINE_WIDTH[i].px)));
    }
    len = computed_length(css_cv_realm(el), spec);
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

/* ---- the computed value ----------------------------------------------------------------------------------- */

bool css_computed_models_length(const char *name)
{
    DCHECK(name != NULL, "the computed-value model question was asked about a NULL property name");
    return css_models_length(name) || css_border_side_of(name, "width") >= 0;
}

bool css_computed_models(const char *name)
{
    DCHECK(name != NULL, "the computed-value model question was asked about a NULL property name");
    return strcmp(name, "overflow-x") == 0 || strcmp(name, "overflow-y") == 0 ||
           strcmp(name, "display") == 0 || strcmp(name, "float") == 0 || strcmp(name, "position") == 0 ||
           strcmp(name, "box-sizing") == 0 || css_computed_models_length(name) ||
           css_border_side_of(name, "style") >= 0;
}

/* THE CASCADE'S WINNER, plus the three conditions every computed-value rule below is derived under. Both
   entries go through it, so neither can be reached with a property whose shorthands are unexpanded or with a
   CSS-wide keyword nothing can default. OWNED: the rule that takes it frees it. */
static char *css_cv_specified(lxb_dom_element_t *el, const char *name)
{
    char *spec;

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
    spec = cssom_cascaded_value(el, name);
    DCHECK(spec != NULL,
           "the cascade produced nothing for a property this component models — every one of them is in "
           "lexbor's registry with an initial value, so the cascade's last layer always answers");
    DCHECK(!css_wide_keyword(spec),
           "the cascade's winner is a CSS-WIDE KEYWORD (inherit / initial / unset / revert), and CSS Cascade "
           "§7's DEFAULTING step is what turns one into a value: `inherit` takes the parent element's computed "
           "value, `initial` the property's initial value, `unset` whichever of those the property's "
           "inheritance says. css_style_declaration.c's cascade has NO inheritance step at all — not for the "
           "keyword and not for an inherited property nobody declared — so there is nothing here to default "
           "with. BUILD the defaulting step: the property's inherited-ness comes from its own spec "
           "definition (@webref/css publishes it beside the `computedValue` line), and the parent's computed "
           "value is this same entry one node up");
    return spec;
}

CssLength css_computed_length(lxb_dom_element_t *el, const char *name)
{
    char *spec;

    DCHECK(css_computed_models_length(name),
           "css_computed_length was asked for a property whose `Computed value:` line is a KEYWORD and not a "
           "length — `display`, `float`, `position`, `box-sizing`, an overflow axis or a `border-*-style`. "
           "There is nothing to absolutize and nothing for a `CssPx` to carry, so the entry that answers those "
           "is css_computed_value, and asking this one would report a keyword as the number zero");
    spec = css_cv_specified(el, name);
    if (css_border_side_of(name, "width") >= 0)
        return computed_border_width(el, name, spec);
    return computed_length(css_cv_realm(el), spec);
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
    spec = css_cv_specified(el, name);
    if (strcmp(name, "overflow-x") == 0 || strcmp(name, "overflow-y") == 0)
        return computed_overflow(el, name, spec);
    if (strcmp(name, "display") == 0)
        return computed_display(el, spec);
    /* `float` (CSS2 §9.5.1), `position` (css-position §2) and `box-sizing` (css-sizing §5) all state "Computed
       value: as specified" (`box-sizing`'s line is "specified keyword"), and a keyword has no absolutization to
       do — so the specified value IS the answer here rather than a stand-in for one. css-backgrounds-3 §3.2
       gives `border-*-style` the same line ("specified keyword"), which is what lets `border-top-width`'s rule
       above read its sibling with no second derivation in between. */
    DCHECK(strcmp(name, "float") == 0 || strcmp(name, "position") == 0 || strcmp(name, "box-sizing") == 0 ||
               css_border_side_of(name, "style") >= 0,
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
        DCHECK(len.kind == CSS_LENGTH_KEYWORD,
               "a computed length is none of the three kinds css_length.h defines — the parse answers exactly "
               "one of them and crashes rather than inventing a fourth");
        return JS_NewString(ctx, len.keyword);
    }
    v = css_computed_models(name) ? css_computed_value(el, name) : cssom_cascaded_value(el, name);
    /* A property no cascade layer answers at all — a custom property nobody set — is the EMPTY STRING, which
       is §6.6.1's own answer for one that is not set rather than a default standing in for a value. */
    out = v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
    free(v);
    return out;
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
               "§6.7.2's serialize-a-CSS-value over a list has no way to combine four of those into one. BUILD "
               "the combination in core/css/css_length.h's terms — css_px_combine is the shape: the result "
               "carries every operand's environment fact — and let this step produce a concolic string rather "
               "than a concrete one");
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
              "containing block's `direction`, which is INHERITED by a cascade with no inheritance step. An "
              "ABSOLUTELY positioned box is a fourth thing again: §10.3.7 solves its insets from the same "
              "constraint equation as its width, against the PADDING EDGE of its nearest positioned ancestor, "
              "which used_value.c's §10.1 fourth case crashes for and names in full");
        break;
    case CSS_RESOLVED_USED:
        DFAIL("CSSOM §9 makes this property's resolved value the USED value unconditionally — it is one of the "
              "COLOR properties, whose used value is the computed value with `currentcolor` resolved against "
              "the element's own computed `color`, serialized as an absolute color per CSS Color §serializing "
              "(`red` resolves to `rgb(255, 0, 0)`, which is what a page comparing against getComputedStyle "
              "expects). Neither half exists here: css_style_declaration.c's cascade has NO INHERITANCE, and "
              "`color` is an inherited property, so an element that declares none has no computed color for "
              "`currentcolor` to resolve against in the first place. BUILD the cascade's defaulting and "
              "inheritance step, then the absolute-color serialization (core/css/css_color.c already parses "
              "and converts the color spaces)");
        break;
    case CSS_RESOLVED_LINE_HEIGHT: {
        /* "The resolved value is normal if the computed value is normal, or the used value otherwise." */
        char *v = cssom_cascaded_value(el, name);

        if (css_cv_is(v, "normal")) { out = JS_NewString(ctx, v); free(v); return out; }
        free(v);
        DFAIL("CSSOM §9 makes `line-height`'s resolved value the USED value whenever the computed value is not "
              "`normal` — the absolute length a LAYOUT laid the line boxes out with, which for a number or a "
              "percentage is that factor times the used font size. This engine has no font size chain and no "
              "line boxes. BUILD the font-size cascade (the computed value of a `<number>` line-height is the "
              "number itself; the used value is the number times the element's computed font-size)");
        break;
    }
    case CSS_RESOLVED_TRANSFORM: {
        /* css-transforms §3.2: "When the computed value is a <transform-list>, the resolved value is one
           matrix() function … For other computed values, the resolved value is the computed value." */
        char *v = cssom_cascaded_value(el, name);

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
