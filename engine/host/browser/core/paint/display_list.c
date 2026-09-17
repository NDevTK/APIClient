/* THE INK — the mark vocabulary and the ordered sequence. See display_list.h for why the order is the list's
   whole statement, why a rectangle carries the environment facts it is a function of, why a colour is not
   quantized here, and why the canvas is a KIND rather than a rectangle with a flag on it. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_length.h"
#include "core/paint/display_list.h"

/* IS THIS ONE OF THE KINDS THIS COMPONENT DEFINES — A SWITCH AND NOT A RANGE TEST, SO THE COMPILER ASKS THE
   QUESTION FIRST. `DisplayMarkKind` is a closed vocabulary this engine writes, and a `kind <= LAST` comparison
   would silently ADMIT the next member somebody adds: the append below would take a mark whose meaning nothing
   here states, and the first thing that noticed would be a surface drawing the wrong shape. Written as a
   switch with NO `default:` label, `-Wswitch` names THIS FILE at the moment the enum grows — which is the same
   mechanism core/paint/box_paint.c relies on over `PaintStep`, and the fix is an ARM here and in every
   consumer, never a `default:`.
   IN A RELEASE BUILD THIS IS REFERENCED AND NEVER CALLED, and that is the DCHECK doing what check.h says it
   does rather than a leftover: the release spelling is `((void)sizeof(cond))`, so the condition is
   TYPE-CHECKED and not evaluated. `clang -Wall -DAPICLIENT_DEV=0` therefore reports it as not needed and not
   emitted; a reader meeting that has found the assert compiled out and not a dead predicate, and the answer
   is neither to drop the helper nor to call it outside the DCHECK — the compile-time question above is the
   whole point of it, and it is asked at every setting. */
static bool dl_kind_is_defined(DisplayMarkKind kind)
{
    switch (kind) {
    case DISPLAY_MARK_FILL_RECT:
    case DISPLAY_MARK_FILL_CANVAS:
    case DISPLAY_MARK_BORDER:
        return true;
    }
    return false;
}

/* WHICH COLOURS A MARK HAS IS ITS KIND'S QUESTION, so the two colour invariants below are asked over a SWITCH
   and not over the `color` field. A `DISPLAY_MARK_BORDER` does not use `color` at all — CSS 2.1 §8.5
   "Border properties" gives a border FOUR colours, one per side — so a pair of asserts written over `color`
   alone would hold for every border mark whatever its four sides carried, which is an assert whose two sides
   cannot disagree. Both switches carry no `default:`, so a kind added without saying which colours it uses is
   named HERE at compile time rather than by a surface compositing an unset one. */
static bool dl_colors_are_srgb(const DisplayMark *m)
{
    unsigned i;

    switch (m->kind) {
    case DISPLAY_MARK_FILL_RECT:
    case DISPLAY_MARK_FILL_CANVAS:
        return m->color.space == CSS_COLOR_SPACE_SRGB;
    case DISPLAY_MARK_BORDER:
        for (i = 0; i < 4; i++)
            if (m->side[i].color.space != CSS_COLOR_SPACE_SRGB) return false;
        return true;
    }
    return false;
}

static bool dl_alphas_are_in_range(const DisplayMark *m)
{
    unsigned i;

    switch (m->kind) {
    case DISPLAY_MARK_FILL_RECT:
    case DISPLAY_MARK_FILL_CANVAS:
        return m->color.a >= 0.0 && m->color.a <= 1.0;
    case DISPLAY_MARK_BORDER:
        for (i = 0; i < 4; i++)
            if (!(m->side[i].color.a >= 0.0 && m->side[i].color.a <= 1.0)) return false;
        return true;
    }
    return false;
}

/* THE FOUR USED WIDTHS ARE NON-NEGATIVE. This is a value core/layout/used_value.c DERIVED and not one a
   document declared — css-backgrounds-3 §3.3's `<line-width>` is a non-negative `<length>` so lexbor drops a
   negative declaration, and CSS 2.1 §17.6.2 "The collapsing border model"'s halves are halves of a
   non-negative number — so a negative here is arithmetic this engine performed, which is exactly what a
   DCHECK may stand on. */
static bool dl_border_widths_are_non_negative(const DisplayMark *m)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        if (!(m->side[i].width.px >= 0.0)) return false;
    return true;
}

/* EACH OF THE FOUR STYLES IS ONE OF CSS 2.1 §8.5.3's TEN — a SWITCH for `dl_kind_is_defined`'s own reason, so
   that a `<border-style>` value added to the vocabulary is named here rather than admitted by a range test. */
static bool dl_border_style_is_defined(DisplayBorderStyle style)
{
    switch (style) {
    case DISPLAY_BORDER_STYLE_NONE:
    case DISPLAY_BORDER_STYLE_HIDDEN:
    case DISPLAY_BORDER_STYLE_DOTTED:
    case DISPLAY_BORDER_STYLE_DASHED:
    case DISPLAY_BORDER_STYLE_SOLID:
    case DISPLAY_BORDER_STYLE_DOUBLE:
    case DISPLAY_BORDER_STYLE_GROOVE:
    case DISPLAY_BORDER_STYLE_RIDGE:
    case DISPLAY_BORDER_STYLE_INSET:
    case DISPLAY_BORDER_STYLE_OUTSET:
        return true;
    }
    return false;
}

static bool dl_border_styles_are_defined(const DisplayMark *m)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        if (!dl_border_style_is_defined(m->side[i].style)) return false;
    return true;
}

/* EVERY NUMBER OF A MARK'S RECTANGLE IS FINITE. All four are arithmetic THIS ENGINE performed and none is a
   number a document stated, which is the line CLAUDE.md draws for what a DCHECK may stand on: a
   `DISPLAY_MARK_FILL_RECT`'s and a `DISPLAY_MARK_BORDER`'s come from core/dom/element_view.c's border area —
   a used extent off core/layout/used_value.h plus a box origin off core/layout/flow_position.h, less the
   window scroll — and a `DISPLAY_MARK_FILL_CANVAS`'s from core/frame/viewport.h's initial containing block. A
   page may DECLARE `width: 1e400px`; what reaches here is the used value this engine derived from it, so a
   NaN or an infinity is a derivation that lost an operand. It is asked of ALL FOUR because a position is as
   much this engine's arithmetic as an extent is. */
static bool dl_rect_is_finite(const DisplayMark *m)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        if (!isfinite(m->rect[i].px)) return false;
    return true;
}

/* AND THE TWO EXTENTS ARE NON-NEGATIVE — ASKED OF THE EXTENTS ALONE, because the four numbers are TWO KINDS
   and one rule over all four would be wrong in one direction. `rect[0]` and `rect[1]` are CSSOM VIEW §6
   "Extensions to the Element Interface"' CLIENT coordinates, which core/dom/element_view.c derives by
   subtracting the window scroll from a position in the initial containing block's space — so a box scrolled
   above the viewport reports a NEGATIVE `y`, and a rule over all four would hand any page that calls
   `scrollTo` an abort switch for this engine. `rect[2]` and `rect[3]` are EXTENTS: the block arm's are
   `used_value_border_edge_px`, a content box core/layout/used_value.c asserts non-negative plus a
   non-negative surround; the inline arm's are a `FlowRect`, which core/layout/flow_position.h declares as "a
   position and the two extents that go with it" over a BORDER AREA; and the canvas kind's are the initial
   containing block's dimensions. core/dom/element_view.h states the same rule for the six §6 members that are
   also extents — they "are distances between parallel edges and cannot be negative" — and a border area's two
   are that sentence on a different code path rather than a second rule. */
static bool dl_rect_extents_are_non_negative(const DisplayMark *m)
{
    return m->rect[2].px >= 0.0 && m->rect[3].px >= 0.0;
}

void display_list_init(DisplayList *dl)
{
    DCHECK(dl != NULL, "a display list was initialised through no list");
    dl->v = NULL;
    dl->n = dl->cap = 0;
}

void display_list_free(DisplayList *dl)
{
    DCHECK(dl != NULL, "a display list was released through no list");
    free(dl->v);
    dl->v = NULL;
    dl->n = dl->cap = 0;
}

void display_list_append(DisplayList *dl, const DisplayMark *mark)
{
    DCHECK(dl != NULL, "a mark was appended to no display list");
    DCHECK(mark != NULL, "a display list was appended to with no mark — an append is a COPY of the caller's "
                         "mark, so there is no arm here under which the absence of one is a statement");
    DCHECK(dl_kind_is_defined(mark->kind),
           "a display mark carries a kind this component does not define. The enum is a closed VOCABULARY this "
           "engine writes and the predicate above is a switch, so a value that fails it is this engine's own "
           "uninitialised memory rather than a document's — a kind somebody ADDED would have been named at "
           "compile time by -Wswitch instead. See display_list.h's residual for which kinds CSS 2.1 §E.2 "
           "\"Painting order\"'s sub-lists still name and which diff builds each");
    /* THE SPACE AND THE ALPHA ARE THIS ENGINE'S OWN AND ARE THEREFORE ASSERTABLE, which is the line
       CLAUDE.md draws and which a colour read out of a page makes worth drawing explicitly. The page states
       the SPECIFIED colour and this engine states the used one: the space is sRGB because the painter ran CSS
       Color 4 §11 "Converting Colors" on it, and the alpha is in range because CSS Color 4 §16.1.2 puts it
       there — core/css/css_color.h records that sentence in its own words, "<alpha-value>s which were
       specified outside the valid range are clamped at parse time". Neither is a claim about what the
       document declared. */
    DCHECK(dl_colors_are_srgb(mark),
           "a display mark's colour is not in sRGB. CSS Color 4 §11 \"Converting Colors\" is the painter's to "
           "run, once, where the cascade is read; a mark whose space were free to vary would make every "
           "consumer of a list ask a question the painter already answered, and two answers to one question is "
           "the shape that drifts. For a `DISPLAY_MARK_BORDER` this is asked of all FOUR of CSS 2.1 §8.5 "
           "\"Border properties\"' colours, one per side, and not of the unused `color` field");
    DCHECK(dl_alphas_are_in_range(mark),
           "a display mark's alpha is outside [0, 1]. CSS Color 4 §16.1.2 clamps an <alpha-value> at parse "
           "time, so a value outside the range did not come from a declaration — it came from arithmetic this "
           "engine performed on one");
    DCHECK(mark->kind != DISPLAY_MARK_BORDER || dl_border_widths_are_non_negative(mark),
           "a border mark carries a NEGATIVE used border width. css-backgrounds-3 §3.3 \"Line Thickness: the "
           "border-width properties\"' `<line-width>` is a non-negative `<length>` so lexbor drops a negative "
           "declaration, and CSS 2.1 §17.6.2 \"The collapsing border model\"'s widths are halves of "
           "non-negative numbers — so a negative here is a used value core/layout/used_value.c derived rather "
           "than one an author wrote, and the border area it describes would extend OUTSIDE the box's border "
           "box");
    DCHECK(mark->kind != DISPLAY_MARK_BORDER || dl_border_styles_are_defined(mark),
           "a border mark carries a line style outside CSS 2.1 §8.5.3 \"Border style: 'border-top-style', "
           "'border-right-style', 'border-bottom-style', 'border-left-style', and 'border-style'\"' ten. That "
           "value type is a closed list and core/css/css_shorthand.c validates every declaration against it, "
           "dropping the ones that do not match — so a style here that this vocabulary does not define is this "
           "engine's grammar and this engine's vocabulary having come apart, never a keyword a document wrote");
    /* THE GEOMETRY, ASSERTED HERE BECAUSE NOTHING UPSTREAM OF THIS DOOR ASSERTS IT. The paragraph that stood
       here REFUSED these two invariants as "a second copy of one rule", naming core/dom/element_view.h's
       assert "at `ev_length_long` where §6's six extents are produced" as the first copy. It is REWRITTEN
       RATHER THAN DELETED because its reasoning is the one a reader re-derives — an invariant belongs at its
       producer — and only its PREMISE is wrong: there is no first copy on either path. `ev_length_long` has
       THREE callers and every one is a `long` IDL member, which is exactly the six extents element_view.h
       names beside it; a MARK'S RECTANGLE REACHES NONE OF THEM. `element_view_bounding_box_px` answers in
       `CssPx` through `ev_border_area_px`, and core/paint/box_paint.c's `bp_canvas_region` answers out of
       core/frame/viewport.h's initial containing block — a different member, a different type and a different
       call. WHETHER THE PREMISE WAS EVER TRUE IS NOT ESTABLISHABLE FROM THIS CHECKOUT, which is a shallow
       clone; that it is false at `origin/main` is what a reader can check, by reading who calls
       `ev_length_long`.
       THEY ARE THIS COMPONENT'S BECAUSE THIS IS THE ONE DOOR. display_list.h states that an append is "the
       ONLY way ink enters a list", so an invariant asked here covers every mark of every kind and every
       producer, present and future, where a producer-side one covers the producer somebody remembered — and
       the canvas kind's rectangle is produced by a function in another component that this one never sees.
       AND THEY ARE THE FIRST CONSUMER'S PRECONDITION rather than tidiness: a surface that turns this
       rectangle into scanlines turns `rect[2]` and `rect[3]` into a loop bound and an allocation size, where
       a NaN is UNDEFINED BEHAVIOUR at the cast to an integer and a negative extent is a byte count that is
       not one. */
    DCHECK(dl_rect_is_finite(mark),
           "a display mark's rectangle carries a coordinate that is not FINITE. Every one of the four is a "
           "used value this engine derived — core/dom/element_view.c's border area for the two box kinds, "
           "core/frame/viewport.h's initial containing block for the canvas — so a NaN or an infinity is a "
           "derivation that lost an operand and never a number a document stated. FIX IT AT THE PRODUCER the "
           "mark's kind names, not here: this is the door every mark enters by and therefore the place the "
           "loss is NOTICED, which is not the place it happened");
    DCHECK(dl_rect_extents_are_non_negative(mark),
           "a display mark's rectangle carries a NEGATIVE width or height. core/dom/element_view.h states the "
           "rule for CSSOM VIEW §6 \"Extensions to the Element Interface\"' six `long` extents — they \"are "
           "distances between parallel edges and cannot be negative\" — and a border area's two are the same "
           "sentence: a content box core/layout/used_value.c asserts non-negative, plus a surround CSS 2.1 "
           "§8.4 and css-backgrounds-3 §3.3 both forbid to be negative. This is asked of `rect[2]` and "
           "`rect[3]` ALONE and never of `rect[0]` or `rect[1]`, which are CLIENT coordinates and are "
           "legitimately negative for a box scrolled above the viewport");
    if (dl->n == dl->cap) {
        size_t cap = dl->cap ? dl->cap * 2 : 8;
        DisplayMark *v = realloc(dl->v, cap * sizeof *v);

        CHECK(v != NULL, "paint: OOM growing a display list — a dropped mark is ink that is silently absent "
                         "from a sequence whose whole statement is that it is complete and in CSS 2.1 §E.2 "
                         "\"Painting order\"'s order, and no consumer could tell that list from a document "
                         "with less in it");
        dl->v = v;
        dl->cap = cap;
    }
    dl->v[dl->n++] = *mark;
}

CssEnvSet display_list_env(const DisplayList *dl)
{
    CssEnvSet env = CSS_ENV_NONE;
    size_t i;
    unsigned k;

    DCHECK(dl != NULL, "the environment set of no display list was asked for");
    /* THE UNION OVER EVERY COORDINATE OF EVERY MARK, which is css_length.h's own rule for a sum read over a
       sequence instead of over two operands: "the sum of two lengths derived from DIFFERENT facts is a
       function of BOTH, which is the union". The COLOUR contributes nothing — a used colour is a function of
       the cascade and of `currentcolor`'s walk up the tree, and neither is one of core/frame/viewport.h's
       picked facts — so a list of ink whose geometry is determined is a list with no arm to explore however
       many colours are in it. */
    for (i = 0; i < dl->n; i++) {
        for (k = 0; k < 4; k++) env |= dl->v[i].rect[k].env;
        /* AND THE LENGTHS A KIND CARRIES BESIDE ITS RECTANGLE, asked over a SWITCH so that the union cannot
           silently stop covering a mark. A border's four USED WIDTHS are such lengths and they are not
           determined in general: core/layout/used_value.h states that a `border: 1px solid` "arrives carrying
           the DEVICE PIXEL RATIO's" fact, because css-values §6 snaps a border width to a whole number of
           device pixels. A union over `rect` alone would therefore report CSS_ENV_NONE for a list whose only
           ink is a bordered box at determined coordinates — the POSITIVE statement that this ink is the same
           ink under every arm, made about ink that moves, which is the identical defect carrying the canvas
           kind's extent exists to prevent. The fill kinds are a deliberate `break` and not an omission: they
           have no length but the rectangle, and `side` is a field their kind does not use, so reading it here
           would union memory no builder of theirs wrote. */
        switch (dl->v[i].kind) {
        case DISPLAY_MARK_FILL_RECT:
        case DISPLAY_MARK_FILL_CANVAS:
            break;
        case DISPLAY_MARK_BORDER:
            for (k = 0; k < 4; k++) env |= dl->v[i].side[k].width.env;
            break;
        }
    }
    return env;
}
