/* css-text-3 §4.1's white space processing and §5's line breaking, over core/layout/line_break.h's [UAX14]
   rules and the advance measures core/css/font_metrics.h reads off the first available font. See text_run.h for
   the two answers this produces, for why it is an accumulator, for why it collects before it measures, and for
   the one rule §4.1.2's trimming becomes once the lines are known. */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/encoding/decode.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/line_break.h"
#include "core/layout/text_run.h"

/* ---- the `white-space` value this component measures ------------------------------------------------------
   css-text-3 §3 "White Space and Wrapping: the white-space property" is a longhand in this level (css-text-4
   makes it a shorthand of `white-space-collapse` and `text-wrap-mode`, which lexbor's registry does not carry),
   and its six values answer three independent questions §4.1 asks: whether spaces and tabs collapse (§4.1.1's
   two bullets), whether segment breaks are preserved (§4.1.3 "Segment Break Transformation Rules"), and
   whether lines wrap at soft wrap opportunities. */

static char *tr_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

typedef struct {
    bool collapse;          /* §4.1.1: "white space characters are considered collapsible" */
    bool preserve_breaks;   /* §4.1.3: "segment breaks are not collapsible" */
    bool wraps;             /* §3: "lines may wrap at allowed soft wrap opportunities" */
} TrWhiteSpace;

/* §3's value on `el`, as those three answers. `normal` collapses and wraps; `nowrap` "collapses white space;
   but like pre, it does not allow wrapping"; `pre-line` collapses spaces and tabs while §4.1.3 preserves its
   segment breaks, and wraps; `pre` "prevents user agents from collapsing sequences of white space", preserves
   segment breaks "as forced line breaks", and "lines only break at forced line breaks".
   NAMED RESIDUAL. Not covered: `pre-wrap` and `break-spaces`, whose preserved spaces wrap — §4.1.1's soft wrap
   opportunity "at the end of a sequence of spaces and/or tabs" or "after every space and every tab" — and whose
   trailing spaces §4.1.2 hangs, unconditionally or conditionally, so they are left out of a line's fit. Next
   diff builds: those two opportunities and §4.1.2's hanging in the line fill. Absence shows as this crash on
   the first `pre-wrap` element a page lays out. */
static TrWhiteSpace tr_white_space(lxb_dom_element_t *el)
{
    char *ws = tr_computed(el, "white-space");
    TrWhiteSpace w = { true, false, true };
    bool known = true;

    if (strcmp(ws, "nowrap") == 0) w.wraps = false;
    else if (strcmp(ws, "pre-line") == 0) w.preserve_breaks = true;
    else if (strcmp(ws, "pre") == 0) { w.collapse = false; w.preserve_breaks = true; w.wraps = false; }
    else if (strcmp(ws, "normal") != 0) known = false;
    /* Freed before the crash, not inside it: a DFAIL is compiled out in release and control falls through it,
       so a `free` on each side would be a double free only a release build reaches. */
    free(ws);
    if (!known)
        DFAIL("css-text-3 §3's `pre-wrap` or `break-spaces` reached the white space processing rules. Both "
              "preserve spaces that WRAP: §4.1.1 puts a soft wrap opportunity \"at the end of a sequence of "
              "spaces and/or tabs\" for pre-wrap and \"after every space and every tab\" for break-spaces, and "
              "§4.1.2 hangs pre-wrap's trailing spaces. BUILD those opportunities and the hanging in the line "
              "fill (see tr_white_space's residual)");
    return w;
}

/* The three properties that would change this measurement's sum and that the cascade computes no value for:
   css-text-3 §7.2 "Tracking: the letter-spacing property" adds its length "between each typographic character
   unit", §7.1 "Word Spacing: the word-spacing property" adds its length "at each word separator character", and
   §2.1 "Case Transforms: the text-transform property" changes which characters there are to measure. None is
   modelled by core/css/css_computed_value.c, so a declaration reaches no consumer. This asserts that, so the
   commit that models one fails here naming the term it must become; it answers off a static list and is
   side-effect-free. */
static void tr_require_no_spacing_model(void)
{
    DCHECK(!css_computed_models("letter-spacing") && !css_computed_models("word-spacing") &&
               !css_computed_models("text-transform"),
           "core/css/css_computed_value.c now derives a computed value for `letter-spacing`, `word-spacing` or "
           "`text-transform`, and this measurement does not read it. css-text-3 §7.2 makes tracking an addition "
           "\"between each typographic character unit\" — including, by its own note, after the last one — §7.1 "
           "makes word spacing an addition \"at each word separator character\", and §2.1's case transform "
           "changes the codepoint whose advance is measured. ADD each to the per-character sum below (and, for "
           "letter-spacing, to the line as well as to the whole, since it survives at a break the same way the "
           "trailing space does not) rather than deleting this assert");
}

/* The four properties that would change where it breaks: css-text-3 §5's controls over [UAX14]'s baseline ("the
   line-break property allows choosing various levels of 'strictness'", "the word-break property controls what
   types of letters are glommed together to form unbreakable 'words'", "the overflow-wrap property allows the UA
   to take a break anywhere in otherwise-unbreakable strings", "the hyphens property controls whether automatic
   hyphenation is allowed"); UAX14 §8 "Customization" is the annex's side of the same statement.
   It is a separate assert from the one above because its remedy is elsewhere: engine/gen_line_break.mjs applies
   LB1's default resolution of CJ to NS at generation time, sound only "in the absence of such criteria", and
   `line-break: loose` resolves CJ to ID instead. */
static void tr_require_no_line_break_tailoring(void)
{
    DCHECK(!css_computed_models("line-break") && !css_computed_models("word-break") &&
               !css_computed_models("overflow-wrap") && !css_computed_models("hyphens"),
           "core/css/css_computed_value.c now derives a computed value for `line-break`, `word-break`, "
           "`overflow-wrap` or `hyphens`, and this measurement runs [UAX14]'s UNTAILORED default. css-text-3 §5 "
           "names these four as the controls CSS provides over the annex's baseline and UAX14 §8 "
           "\"Customization\" is how they are expressed. `line-break` is the one that reaches OUTSIDE this "
           "file: engine/gen_line_break.mjs pre-applies LB1's default resolution of CJ to NS, which UAX14 "
           "permits only \"in the absence of\" out-of-band criteria, and `line-break: loose` resolves CJ to ID "
           "— so that generator has to emit the ORIGINAL class beside the resolved one before this property can "
           "be honoured. `word-break: break-all` and `overflow-wrap: anywhere` add opportunities inside words "
           "and `hyphens` adds them at hyphenation points; each is a tailoring of core/layout/line_break.c's "
           "rule chain and not a term in the sum here");
}

/* ---- css-text-3 §4.1.1's collapsible characters ------------------------------------------------------------- */

/* Whether §4.1.1 collapses this character, for an element whose white space collapses. The four have
   different reasons:
     U+0020 SPACE: §4.1.1's "collapsible spaces".
     U+0009 TAB: §4.1.1 step 3, "every collapsible tab is converted to a collapsible space (U+0020)".
     U+000A LF: the segment break. css-text-3 §4 "White Space Processing & Control Characters": "each line feed
       (U+000A) is treated as a segment break"; step 1 and §4.1.3 then reduce it to a space.
     U+000D CR: not a segment break ("they are not treated as segment breaks") but "treated identically to
       spaces (U+0020) in all respects" (§4); reachable only through the DOM API, since HTML §13.2.3.5
       "Preprocessing the input stream" normalises it away in a parse.
   U+000C FORM FEED is content: §4's other control characters "must be rendered as a visible glyph", and its
   [UAX14] class BK makes the break after it forced (§5.5). This is not a Line_Break class test because the two
   questions differ per character: TAB is collapsible and class BA, CR collapsible and class CR, FF content and
   class BK. */
static bool tr_is_collapsible_white_space(uint32_t cp)
{
    return cp == 0x0020 || cp == 0x0009 || cp == 0x000A || cp == 0x000D;
}

/* ---- the collection ----------------------------------------------------------------------------------------- */

void text_run_measure_init(TextRunMeasure *m)
{
    DCHECK(m != NULL, "a text run measurement was begun with nowhere to keep it");
    m->items = NULL;
    m->count = 0;
    m->capacity = 0;
    m->in_white_space_run = false;
    m->actions = NULL;
    m->item_of_cp = NULL;
    m->ncps = 0;
    m->max_content = css_px(0.0);
    m->min_content = css_px(0.0);
    m->splits = false;
    m->atomic_sizes_differ = false;
    m->faces_of = NULL;
    m->faces.n = 0;
    m->advance_of = NULL;
    m->finished = false;
    m->prep = 0;
    m->cursor = 0;
    m->copied = 0;
    m->walk = 0;
    m->cps = NULL;
    m->answered = false;
    m->released = false;
}

/* One item's storage, with every field of every kind written, so an edge never carries a stale code point a
   walk could measure as text; the accessors below refuse the fields of another kind. */
static TextRunItem *tr_append_item(TextRunMeasure *m, TextRunItemKind kind, lxb_dom_element_t *style)
{
    TextRunItem *it;

    if (m->count == m->capacity) {
        size_t capacity = m->capacity != 0 ? m->capacity * 2 : 32;
        TextRunItem *grown = realloc(m->items, capacity * sizeof *grown);

        CHECK(grown != NULL, "out of memory collecting one inline formatting context's items for css-sizing-3 "
                             "§5.1's intrinsic inline sizes. The collection is the run's own text and its "
                             "boxes' edges and nothing more, so a failure here is the physical floor");
        m->items = grown;
        m->capacity = capacity;
    }
    it = &m->items[m->count++];
    it->kind = kind;
    it->style = style;
    it->cp = 0;
    it->face = INSTALLED_FONT_COUNT;
    it->advance = css_px(0.0);
    it->wraps = false;
    it->collapsible_space = false;
    it->size[TEXT_RUN_SIZE_MIN_CONTENT] = css_px(0.0);
    it->size[TEXT_RUN_SIZE_MAX_CONTENT] = css_px(0.0);
    return it;
}

static void tr_append(TextRunMeasure *m, uint32_t cp, lxb_dom_element_t *style, bool wraps, bool space)
{
    TextRunItem *c = tr_append_item(m, TEXT_RUN_ITEM_CHAR, style);

    if (m->faces_of != style) {
        css_font_fallback_list(style, &m->faces);
        m->faces_of = style;
    }
    if (m->advance_of != style) {
        m->advance_basis = css_font_advance_basis(style);
        m->advance_of = style;
    }
    c->cp = cp;
    c->face = font_matching_font_for_character(&m->faces, cp);
    if (cp != 0x0009) {
        c->advance = css_font_advance_measure_basis_px(&m->advance_basis, c->face, cp);
        DCHECK(c->advance.px >= 0.0,
               "the first available font reported a NEGATIVE advance measure for a character. OpenType 'hmtx' — "
               "Horizontal Metrics Table's advanceWidth is a UFWORD, .notdef's included, and a computed "
               "`font-size` is non-negative — so this is a derivation that lost a sign");
    }
    c->wraps = wraps;
    c->collapsible_space = space;
}

/* Whether this kind contributes a code point to [UAX14]: the partition `finish` sizes its arrays by, fills them
   by and asserts agreement over. An inline box edge is outside it (css-text-3 §5.5 "Line Breaking Details":
   "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the flow"); a forced
   break (HTML §15.3.4's `display-outside: newline`) and an atomic inline are inside it (see text_run.h). */
static bool tr_kind_has_code_point(TextRunItemKind kind)
{
    return kind == TEXT_RUN_ITEM_CHAR || kind == TEXT_RUN_ITEM_FORCED_BREAK ||
           kind == TEXT_RUN_ITEM_ATOMIC;
}

/* Whether this kind occupies a stored inline size on its line. It is not the complement of the partition above:
   an edge has a size and no code point, a forced break a code point and no size, an atomic inline both, and a
   character's width is css-values-4 §6.1.1's advance measure of its code point, read from the font rather than
   stored. The two questions cross, so they are two functions. */
static bool tr_kind_has_size(TextRunItemKind kind)
{
    return kind == TEXT_RUN_ITEM_EDGE || kind == TEXT_RUN_ITEM_ATOMIC;
}

/* Whether this item is content that css-text-3 §4.1.2 "Phase II: Trimming and Positioning" stops at when it
   removes "a sequence of collapsible spaces at the beginning of a line" and at the end. An atomic inline is
   content and an edge is not: an inline box opened before a line's leading space leaves that space at the
   beginning of the line, while `<div> <img> a</div>` keeps the space between the image and the `a` and trims
   only the one before the image. */
static bool tr_item_stops_trim(const TextRunItem *it)
{
    if (it->kind == TEXT_RUN_ITEM_ATOMIC) return true;
    return it->kind == TEXT_RUN_ITEM_CHAR && !it->collapsible_space;
}

/* The fields of one kind, reached through a check of that kind. Every read of `cp`, `wraps`,
   `collapsible_space` or `size` checks the kind first, through these or at the site (`tr_item_stops_trim`,
   `tr_opportunity_enabled`, `text_run_measure_item_cp`), so a walk that lost track of the kind aborts at the
   read. */
static const TextRunItem *tr_char_at(const TextRunMeasure *m, size_t i)
{
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    DCHECK(m->items[i].kind == TEXT_RUN_ITEM_CHAR,
           "a TEXT item's fields were read from an item that is an inline box EDGE or a FORCED LINE BREAK. "
           "css-text-3 §5.5 \"Line Breaking Details\" makes all three different things at the same kind of "
           "position — an edge is a width and carries no code point, a forced break carries HTML §15.3.4's "
           "newline and no width, and only a character is measured by css-values-4 §6.1.1's advance measure — "
           "so this is a walk that stopped distinguishing them, and the value it would have read is a zero this "
           "component wrote or a code point no document contains rather than anything the document has");
    return &m->items[i];
}

static const TextRunItem *tr_sized_at(const TextRunMeasure *m, size_t i)
{
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    DCHECK(tr_kind_has_size(m->items[i].kind),
           "a STORED inline size was read from an item that is a CHARACTER or a FORCED LINE BREAK — a "
           "character's contribution is css-values-4 §6.1.1's advance measure of its own code point and a "
           "forced break's is nothing at all, which are two different quantities read two different ways. The "
           "two kinds that DO carry one are an inline box EDGE (css-sizing-3 §2.2's outer size at one boundary) "
           "and an ATOMIC INLINE (CSS 2 §8.1's whole margin box), and the per-line sum adds either at the "
           "position its item sits at");
    return &m->items[i];
}

/* One sized item's contribution under one of css-sizing-3 §2.1's two constraints. The basis is an operand a
   caller can get wrong silently (both members are a `CssPx` of the same box), so the one read of the array
   asserts it is one of the two the enum names. */
static CssPx tr_sized_px(const TextRunMeasure *m, size_t i, TextRunSizeBasis basis)
{
    DCHECK(basis == TEXT_RUN_SIZE_MIN_CONTENT || basis == TEXT_RUN_SIZE_MAX_CONTENT,
           "a line was summed under a css-sizing-3 §2.1 constraint that is neither of the two the section "
           "names. `TextRunSizeBasis` has exactly two members and indexes an item's two sizes, so a third "
           "value here reads past the pair and reports a line width out of whatever followed it in the "
           "item record");
    return tr_sized_at(m, i)->size[basis];
}

/* The code point [UAX14] is handed for this item. Separate from `tr_char_at` because the answer sets differ: an
   advance is asked only of a character, the break pass of a character, a forced break and an atomic inline. */
static uint32_t tr_code_point_at(const TextRunMeasure *m, size_t i)
{
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    DCHECK(tr_kind_has_code_point(m->items[i].kind),
           "a code point was read from an inline box EDGE, which is the one kind of the four that has none. "
           "css-text-3 §5.5 \"Line Breaking Details\" says an "
           "inline box boundary \"do[es] not introduce a forced line break or soft wrap opportunity in the "
           "flow\", so handing one to [UAX14] would create or forbid a break at exactly the position the "
           "sentence says to leave alone — and the code point it would hand over is the zero `tr_append_item` "
           "wrote, whose Line_Break class is CM");
    return m->items[i].cp;
}

void text_run_measure_add_box_edge(TextRunMeasure *m, lxb_dom_element_t *style, CssPx size)
{
    DCHECK(m != NULL && style != NULL,
           "an inline box edge was added with no accumulator or no element — css-sizing-3 §2.2's outer size is "
           "a fact about one box, so there is no edge without the box it belongs to");
    DCHECK(!m->finished,
           "an inline box edge was added to a measurement that has already produced its answers. "
           "text_run_measure_finish_begin closed the collection those answers are derived from, so this is one "
           "accumulator being used for two runs");
    {
        /* One number under both of css-sizing-3 §2.1's constraints, written here so no caller can state a
           second: css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes" resolves an edge's
           lengths identically for both ("For the min size properties, as well as for margins and paddings (and
           gutters), a cyclic percentage is resolved against zero for determining intrinsic size
           contributions"), and on a real line it is core/layout/intrinsic_size.h's `used_inline_box_edge_px`,
           one used value. */
        TextRunItem *it = tr_append_item(m, TEXT_RUN_ITEM_EDGE, style);

        it->size[TEXT_RUN_SIZE_MIN_CONTENT] = size;
        it->size[TEXT_RUN_SIZE_MAX_CONTENT] = size;
    }
    /* An edge does not close a collapsible white-space run: §4.1.1 collapses over the inline formatting context,
       "even one outside the boundary of the inline containing that space", so spaces either side of a box
       boundary collapse to one U+0020. */
}

/* HTML §15.3.4's newline, as the one code point [UAX14] needs to decide it. css-text-3 §4 "White Space
   Processing & Control Characters": "in the case of HTML, newlines are normalized to line feed characters
   (U+000A) for representation in the DOM, so when an HTML document is represented as a DOM tree each line feed
   (U+000A) is treated as a segment break", and §5.5 "Line Breaking Details" makes a preserved segment break a
   forced line break. `display-outside: newline` declares that the box is one, so the code point comes from the
   declaration. A constant rather than an argument, so callers cannot give two answers to what a `br` is. */
#define TR_HTML_NEWLINE 0x000AU

/* A forced break at the current position, from a `br` or a preserved segment break: one item, one U+000A. */
static void tr_append_forced_break(TextRunMeasure *m, lxb_dom_element_t *style)
{
    TextRunItem *it = tr_append_item(m, TEXT_RUN_ITEM_FORCED_BREAK, style);

    it->cp = TR_HTML_NEWLINE;
    /* A forced break closes a collapsible white-space run: §4.1.1 step 4 collapses "any collapsible space
       immediately following another collapsible space", and a break between two spaces separates them. The
       choice is unobservable in the measurement: a forced break ends its line box, so a space beside it is at a
       line's edge, which §4.1.2 "Phase II: Trimming and Positioning" removes (as does §4.1.1 step 1, "any
       sequence of collapsible spaces and tabs immediately preceding or following a segment break is removed").
       The flag follows step 4's wording. */
    m->in_white_space_run = false;
}

void text_run_measure_add_forced_break(TextRunMeasure *m, lxb_dom_element_t *style)
{
    DCHECK(m != NULL && style != NULL,
           "a forced line break was added with no accumulator or no element. CSS 2.2 §10.8's step 1 is over "
           "every inline-level box on the line and this is one of them, so its own `line-height` and first "
           "available font are operands of the line's height — there is no break without the box it is");
    DCHECK(!m->finished,
           "a forced line break was added to a measurement that has already produced its answers. [UAX14] ran "
           "over the code points as they stood, so this is one accumulator being used for two runs");
    /* The [UAX14] pass will include this position, so the unhonoured tailoring is asserted here too: a context
       of nothing but `<br>`s adds no text node. The spacing model is not asserted: §7.2's tracking and §7.1's
       word spacing sit beside character units and word separators, and a forced break is neither and has no
       advance. */
    tr_require_no_line_break_tailoring();
    tr_append_forced_break(m, style);
}

/* css-text-3 §5.5's atomic inline, as the one code point [UAX14] needs to decide its two boundaries: U+FFFC
   OBJECT REPLACEMENT CHARACTER has Line_Break class CB, and LB20 "Break before and after unresolved CB" is §5.5's
   "there is a soft wrap opportunity before and after each replaced element or other atomic inline" (text_run.h
   says why the rest of that sentence needs no rule and its named exception does). A constant for
   `TR_HTML_NEWLINE`'s reason. */
#define TR_OBJECT_REPLACEMENT 0xFFFCU

void text_run_measure_add_atomic(TextRunMeasure *m, lxb_dom_element_t *style, CssPx min_content,
                                 CssPx max_content)
{
    TextRunItem *it;

    DCHECK(m != NULL && style != NULL,
           "an atomic inline was added with no accumulator or no element. CSS 2.2 §10.8's step 1 takes \"the "
           "height of their margin box\" for exactly this box, and CSS 2.2 §9.4.2 puts that box's horizontal "
           "margins, borders and padding on the line — both are facts about ONE element, so there is no atomic "
           "inline without the box it is");
    DCHECK(!m->finished,
           "an atomic inline was added to a measurement that has already produced its answers. [UAX14] ran over "
           "the code points as they stood, so this is one accumulator being used for two runs");
    /* The [UAX14] pass will include this position, so the unhonoured tailoring is asserted here too (a context
       holding only an `<img>` adds no text node). The spacing model is asserted here, unlike at a forced break,
       because this item has an inline extent: §7.2's "between each typographic character unit" puts a tracking
       term at each side of it. */
    tr_require_no_spacing_model();
    tr_require_no_line_break_tailoring();
    DCHECK(line_break_class_of(TR_OBJECT_REPLACEMENT) == LB_CLASS_CB,
           "U+FFFC OBJECT REPLACEMENT CHARACTER's [UAX14] Line_Break class is not CB in "
           "core/layout/line_break_class.c, so LB20 \"Break before and after unresolved CB\" no longer puts "
           "css-text-3 §5.5's soft wrap opportunity before and after this box. That sentence is what this item "
           "kind IS, and the code point is how it is expressed to the annex — a different class here means the "
           "generated table and this constant have come apart, and the run would break around an image "
           "according to whatever rule that other class reaches");
    /* The pair is ordered, which is what makes two adjacent `CssPx` arguments safe to take positionally:
       css-sizing-3 §2.1 "Auto Box Sizes" takes more soft wrap opportunities for min-content, so a transposed
       pair either trips this or is two equal numbers. A DCHECK rather than a refusal because both operands are
       this codebase's (core/layout/intrinsic_size.h's §5.2 contribution or core/layout/used_value.h's used
       width); §2.2's floor repairs a negative margin's inversion before either number arrives. */
    DCHECK(min_content.px <= max_content.px,
           "an atomic inline was added whose MIN-CONTENT contribution is WIDER than its max-content one. "
           "css-sizing-3 §2.1 \"Auto Box Sizes\" states the first over the same content with more soft wrap "
           "opportunities taken, and css-sizing-3 §2.2 \"Intrinsic Size Contributions\" repairs the one case "
           "that can invert them before a contribution leaves its producer — \"if the ideal max-content "
           "contribution would be smaller than the min-content contribution (e.g. due to the use of negative "
           "margins), the effective max-content contribution is floored by the min-content contribution\". So "
           "this is either a producer that skipped §2.2's floor or a caller that passed the pair in the other "
           "order, and the two arguments are the same type, so nothing else would have caught it");
    it = tr_append_item(m, TEXT_RUN_ITEM_ATOMIC, style);
    it->cp = TR_OBJECT_REPLACEMENT;
    it->size[TEXT_RUN_SIZE_MIN_CONTENT] = min_content;
    it->size[TEXT_RUN_SIZE_MAX_CONTENT] = max_content;
    /* A run holding an atomic with two sizes is an intrinsic measurement and cannot be filled (text_run.h's
       `atomic_sizes_differ`); recorded here because the fill asks once per placed box. The whole `CssPx` is
       compared, not only its example: core/css/css_length.h carries the environment facts a length is a joint
       function of, and two contributions derived from different facts are two answers even where this
       viewport makes their examples agree. */
    if (min_content.px != max_content.px || min_content.env != max_content.env ||
        min_content.realm != max_content.realm)
        m->atomic_sizes_differ = true;
    /* An atomic inline closes a collapsible white-space run, observably: it renders, so it separates the spaces
       on its sides under §4.1.1 step 4 ("any collapsible space immediately following another collapsible
       space"), and `<div>a <img> b</div>` keeps a space on each side of the image. */
    m->in_white_space_run = false;
}

void text_run_measure_text_begin(TextRunMeasure *m, lxb_dom_element_t *style, const lxb_dom_node_t *text,
                                 TextRunText *t)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)text;
    TrWhiteSpace ws;

    DCHECK(m != NULL && style != NULL && text != NULL && t != NULL,
           "a text run was measured with no accumulator, no style element, no node or nowhere to hold it");
    DCHECK(!m->finished, "a text node was added to a measurement that has already produced its two answers. "
                         "text_run_measure_finish_begin closed the collection those answers are derived from, "
                         "so this is one accumulator being used for two runs");
    DCHECK(text->type == LXB_DOM_NODE_TYPE_TEXT,
           "something that is not a TEXT node was handed to css-text-3 §4.1's white space processing. The rules "
           "are stated over character data, and CSS 2.2 §9.2's box generation makes an element a BOX rather "
           "than a run — a caller holding one is at the wrong arm of its own child walk");
    DCHECK(text->parent == lxb_dom_interface_node(style),
           "the element whose computed properties a text run was measured with is not the run's own parent. "
           "css-text-3 §4.1.1 processes white space \"for each inline (including anonymous inlines)\" and "
           "css-values-4 §6.1.1's advance measure is stated over \"the element on which it is used\", so a run "
           "measured against another element's `font-size` and `white-space` is one document's text under "
           "another one's typography");
    tr_require_no_spacing_model();
    tr_require_no_line_break_tailoring();
    /* `white-space` is asked once per node, not once per white-space character: asking only where a space
       appears would accept a `pre-wrap` run that happens to contain none, a correctness owed to the string
       rather than to this component. The value is a fact about the inline. */
    ws = tr_white_space(style);
    t->style = style;
    t->collapse = ws.collapse;
    t->preserve_breaks = ws.preserve_breaks;
    t->wraps = ws.wraps;
    /* An empty text node (DOM §4.10's `data` may be the empty string) contributes nothing and leaves the
       collapsing state as it was, so a collapsible run split across it collapses as one run. */
    t->p = cd->data.data;
    t->end = cd->data.data == NULL ? NULL : cd->data.data + cd->data.length;
}

bool text_run_measure_text_done(const TextRunText *t)
{
    DCHECK(t != NULL, "a text node's progress was asked about with no node held");
    return t->p == t->end;
}

void text_run_measure_text_step(TextRunMeasure *m, TextRunText *t)
{
    uint32_t cp;

    DCHECK(!m->finished && !text_run_measure_text_done(t),
           "a text node was stepped past its last character or into a closed measurement");
    cp = (uint32_t)lxb_encoding_decode_valid_utf_8_single(&t->p, t->end);
    /* Lexbor's decoder, rather than another hand-rolled UTF-8 walk. It requires well-formed bytes, which a DOM
       string has: lexbor's parser replaces malformed input and quickjs's encoder emits the three-byte form even
       for an unpaired surrogate, which decodes to that code point and is stopped by the advance measure's own
       scalar-value assert. */
    DCHECK(cp != (uint32_t)LXB_ENCODING_DECODE_ERROR,
           "a DOM string held bytes that are not well-formed UTF-8. Lexbor's parser replaces malformed "
           "input and every other writer of character data in this engine goes through quickjs's own "
           "encoder, so a tree containing one was built by something that did neither");
    /* §4.1.3: under `pre` and `pre-line` "segment breaks are not collapsible and are instead transformed into a
       preserved line feed (U+000A)", which §3 makes a forced line break — the same item HTML §15.3.4's `br`
       adds. Spaces beside it are collapsible under `pre-line`, and §4.1.1 step 1's removal of them is §4.1.2's
       trimming at the two line edges the break makes. */
    if (cp == 0x000A && t->preserve_breaks) {
        tr_append_forced_break(m, t->style);
        return;
    }
    /* §4.1.1's second bullet: under `pre` "any sequence of spaces is treated as a sequence of non-breaking
       spaces", each one measured, and a tab is preserved for §4.1.2 to position. A CR is "treated identically
       to spaces (U+0020) in all respects" (§4). None is collapsible, so §4.1.2 trims none. */
    if (!t->collapse && tr_is_collapsible_white_space(cp)) {
        m->in_white_space_run = false;
        tr_append(m, cp == 0x0009 ? 0x0009 : 0x0020, t->style, t->wraps, false);
        return;
    }
    if (tr_is_collapsible_white_space(cp)) {
        /* §4.1.1's one surviving space of a run, opened at its first collapsible character and not reopened by
           the ones after it (step 4: "any collapsible space immediately following another collapsible space …
           is collapsed to have zero advance width"). It is recorded as U+0020, what every path through §4.1.1
           leaves (step 3's tab, §4.1.3's segment break, §4's CR), and carries the inline of the first
           white-space character, the box css-text-3 §5.5 says controls the break it creates. */
        if (!m->in_white_space_run) {
            tr_append(m, 0x0020, t->style, t->wraps, true);
            m->in_white_space_run = true;
        }
        return;
    }
    m->in_white_space_run = false;
    tr_append(m, cp, t->style, t->wraps, false);
}

/* ---- the measurement ---------------------------------------------------------------------------------------- */

/* css-text-3 §4.1.2's preserved tab at `pos` along its line: "each preserved tab is rendered as a horizontal shift
   that lines up the start edge of the next glyph with the next tab stop. If this distance is less than 0.5ch,
   then the subsequent tab stop is used instead. Tab stops occur at points that are multiples of the tab size";
   "if the tab size is zero, preserved tabs are not rendered". §4.2 makes a `<number>` tab size "a multiple of the
   advance width of the space character (U+0020) of the nearest block container ancestor".
   NAMED RESIDUAL. Not covered: §4.1.2 measures stops "from the starting content edge" of that block container,
   and this measures them from the line box's start, which is that edge only for a line no float shortens.
   Next diff builds: the line's offset from the content edge as an operand of the prefix sum. Absence shows as a
   tab on a float-shortened `pre` line landing one stop off Chrome's. */
static CssPx tr_tab_advance(lxb_dom_element_t *style, CssPx pos)
{
    CssTabSize ts = css_computed_tab_size(style);
    lxb_dom_element_t *container = style;
    CssPx size, half_ch, args[3];
    double stop;

    while (container != NULL && !block_flow_display_is_block_container(css_computed_display(container)))
        container = css_parent_element(container);
    DCHECK(container != NULL,
           "a preserved tab has no block container ancestor. Text is laid out in an inline formatting context, "
           "which CSS 2.2 §9.4.2 says a block container establishes, so the walk up from its inline box ends at "
           "one");
    if (ts.is_number) {
        FontFallbackList faces;

        css_font_fallback_list(container, &faces);
        size = css_px_scale(css_font_advance_measure_px(container, font_matching_font_for_character(&faces, 0x20),
                                                        0x0020),
                            ts.number);
    } else {
        size = ts.px;
    }
    if (size.px <= 0.0) return css_px(0.0);
    half_ch = css_px_scale(css_font_ch_px(style), 0.5);
    stop = (floor(pos.px / size.px) + 1.0) * size.px;
    if (stop - pos.px < half_ch.px) stop += size.px;
    args[0] = pos;
    args[1] = size;
    args[2] = half_ch;
    return css_px_atom("css-text-3 tab stop", args, 3, stop - pos.px);
}

/* What follows character `at` in its shaping run for pair kerning: the characters after it on the line in the
   same element and face, up to a box edge, an atomic inline, a forced break or `end`. `reached_end` records an
   answer that stopped at `end` rather than at the run, which a longer line could change. */
typedef struct { const TextRunMeasure *m; size_t at, end; bool reached_end; } TrKernRun;

static int32_t tr_kern_after(void *arg, size_t k)
{
    TrKernRun *r = arg;
    size_t j = r->at + 1 + k;
    const TextRunItem *left = &r->m->items[r->at];

    if (j >= r->end) {
        r->reached_end = true;
        return -1;
    }
    if (r->m->items[j].kind != TEXT_RUN_ITEM_CHAR || r->m->items[j].style != left->style ||
        r->m->items[j].face != left->face)
        return -1;
    return (int32_t)r->m->items[j].cp;
}

/* Character `p`'s pair kerning against what follows it before `end`, under css-fonts-4 §6.3 "Kerning: the
   font-kerning property", which "controls metric kerning, kerning that utilizes adjustment data contained in
   the font". `*settled` is whether the answer stands however far the line grows. A preserved tab has none. */
static CssPx tr_kerning(const TextRunMeasure *m, size_t p, size_t end, bool *settled)
{
    const TextRunItem *c = tr_char_at(m, p);
    TrKernRun kr;
    CssPx k;

    if (c->cp == 0x0009) {
        *settled = true;
        return css_px(0.0);
    }
    kr.m = m;
    kr.at = p;
    kr.end = end;
    kr.reached_end = false;
    k = css_font_kerning_px(c->style, c->face, c->cp, tr_kern_after, &kr);
    *settled = !kr.reached_end;
    return k;
}

/* A shaping run's inline size, as real Chrome snaps it: Chromium 0842759f
   third_party/blink/renderer/core/layout/inline/line_breaker.cc sets an item's inline size to its shape
   result's SnappedWidth(), which platform/fonts/shaping/shape_result.h defines as
   `LayoutUnit::FromFloatCeil(width_)`, the advances' sum rounded up to the next 1/64 px. */
static double tr_snapped_px(double width)
{
    return ceil(width * 64.0) / 64.0;
}

static CssPx tr_snapped_width(CssPx width)
{
    /* The snap only rounds up, by less than 1/64 px: `width <= ceil(64 * width) / 64 < width + 1/64`. */
    return css_px_atom_enclosed("Blink ShapeResult::SnappedWidth: ceil(width * 64) / 64", width, 0.0, 1.0 / 64.0,
                                tr_snapped_px(width.px));
}

static void tr_end_open(TextRunLineEnd *e, size_t lo)
{
    css_px_sum_init(&e->closed);
    css_px_sum_init(&e->run);
    e->run_style = NULL;
    e->kern_lo = e->end = lo;
}

/* The open run's kernings that can no longer change, given what lies before `end`, join `run` in order. */
static void tr_end_settle(const TextRunMeasure *m, TextRunLineEnd *e, size_t end)
{
    while (e->kern_lo < e->end) {
        bool settled;
        CssPx k = tr_kerning(m, e->kern_lo, end, &settled);

        if (!settled) return;
        css_px_sum_add(&e->run, k);
        e->kern_lo++;
    }
}

/* The open run's unsettled kernings, read against what lies before `end`. */
static CssPx tr_end_pending(const TextRunMeasure *m, const TextRunLineEnd *e, size_t end)
{
    CssPxSum s;
    size_t p;
    bool settled;

    css_px_sum_init(&s);
    for (p = e->kern_lo; p < e->end; p++) css_px_sum_add(&s, tr_kerning(m, p, end, &settled));
    return css_px_sum_value(&s);
}

/* The open run ends: its kernings are read against the line as it stands and its width is snapped once. */
static void tr_end_close(const TextRunMeasure *m, TextRunLineEnd *e)
{
    if (e->run_style != NULL) {
        css_px_sum_add(&e->run, tr_end_pending(m, e, e->end));
        css_px_sum_add(&e->closed, tr_snapped_width(css_px_sum_value(&e->run)));
        css_px_sum_init(&e->run);
        e->run_style = NULL;
    }
    e->kern_lo = e->end;
}

/* Character `i` joins the open run, or opens one in another element. The run's earlier kernings are settled
   against it before a preserved tab is positioned, so the tab stop is measured from where the tab begins. */
static void tr_end_add_char(const TextRunMeasure *m, TextRunLineEnd *e, size_t i)
{
    const TextRunItem *c = tr_char_at(m, i);
    CssPx advance;

    if (e->run_style != c->style) {
        tr_end_close(m, e);
        e->run_style = c->style;
        e->kern_lo = e->end = i;
    }
    DCHECKF(e->end == i, "item %zu joined a shaping run that ends at item %zu, so a character between them is "
            "on the line and in no run", i, e->end);
    tr_end_settle(m, e, i + 1);
    if (c->cp == 0x0009) {
        CssPx at = css_px_add(css_px_add(css_px_sum_value(&e->closed), css_px_sum_value(&e->run)),
                              tr_end_pending(m, e, i + 1));

        advance = tr_tab_advance(c->style, at);
    } else {
        advance = c->advance;
    }
    css_px_sum_add(&e->run, advance);
    e->end = i + 1;
}

/* Where the open run stands if it continues past the end of the sum: unsnapped, with its kernings read
   against what lies before `kend`. */
static CssPx tr_end_offset(const TextRunMeasure *m, const TextRunLineEnd *e, size_t kend)
{
    return css_px_add(css_px_add(css_px_sum_value(&e->closed), css_px_sum_value(&e->run)),
                      tr_end_pending(m, e, kend));
}

/* The sum's inline size with its open run ending where the sum does. */
static CssPx tr_end_size(const TextRunMeasure *m, const TextRunLineEnd *e)
{
    if (e->run_style == NULL) return css_px_sum_value(&e->closed);
    return css_px_add(css_px_sum_value(&e->closed),
                      tr_snapped_width(css_px_add(css_px_sum_value(&e->run), tr_end_pending(m, e, e->end))));
}

/* The same size as a number alone, for a comparison that interns no form. */
static double tr_end_size_px(const TextRunMeasure *m, const TextRunLineEnd *e)
{
    if (e->run_style == NULL) return e->closed.v.px;
    return e->closed.v.px + tr_snapped_px(e->run.v.px + tr_end_pending(m, e, e->end).px);
}

static void tr_line_sum_open(TextRunLineSum *s, size_t lo)
{
    s->lo = lo;
    s->at = lo;
    s->content = false;
    tr_end_open(&s->sum, lo);
    tr_end_open(&s->size, lo);
}

/* Add item `s->at` to the line. A sized item closes the open run and is summed wherever it lands; a forced
   break closes it and adds nothing. A character before the first content item is §4.1.2's leading trim, and
   one after the last is summed into `sum` alone, because §4.1.2 removes it from this line and keeps it if
   content follows, where a preserved tab is positioned against everything before it. */
static void tr_line_sum_add(const TextRunMeasure *m, TextRunLineSum *s, TextRunSizeBasis basis)
{
    size_t i = s->at++;
    const TextRunItem *it;

    DCHECK(i < m->count, "a line was summed past the end of the collected run");
    it = &m->items[i];
    if (tr_kind_has_size(it->kind)) {
        CssPx px = tr_sized_px(m, i, basis);

        tr_end_close(m, &s->sum);
        css_px_sum_add(&s->sum.closed, px);
        if (tr_item_stops_trim(it)) {
            s->size = s->sum;
            s->content = true;
        } else {
            tr_end_close(m, &s->size);
            css_px_sum_add(&s->size.closed, px);
        }
        return;
    }
    if (it->kind == TEXT_RUN_ITEM_FORCED_BREAK) {
        tr_end_close(m, &s->sum);
        tr_end_close(m, &s->size);
        return;
    }
    if (!tr_item_stops_trim(it) && !s->content) return;
    tr_end_add_char(m, &s->sum, i);
    if (tr_item_stops_trim(it)) {
        s->size = s->sum;
        s->content = true;
    }
}

/* Sum the line forward to item boundary `at`; answers the items added. */
static size_t tr_line_sum_to(const TextRunMeasure *m, TextRunLineSum *s, size_t at, TextRunSizeBasis basis)
{
    size_t k = 0;

    DCHECK(s->at <= at && at <= m->count, "a line was summed to a boundary behind it or past the run's end");
    while (s->at < at) { tr_line_sum_add(m, s, basis); k++; }
    return k;
}

/* The line `[s->lo, s->at)`'s inline size: css-text-3 §4.1.2's trailing trim is applied. */
static CssPx tr_line_sum_size(const TextRunMeasure *m, const TextRunLineSum *s)
{
    return tr_end_size(m, &s->size);
}

/* How far along line `[lo, hi)` item `upto` begins: the sum of the sizes and advances of `[lo, upto)`, less
   css-text-3 §4.1.2 "Phase II: Trimming and Positioning"'s ends ("a sequence of collapsible spaces at the
   beginning of a line is removed", "a sequence of collapsible spaces at the end of a line is removed"). It is
   the line sum taken to `upto`; past the line's last content item it is that sum's trimmed size. A position
   inside a shaping run that continues on the line is unsnapped, with the pair it forms with `upto` kerned.
   `upto == hi` is the line's size, where its last item ends (a fragment's far edge). */
static CssPx tr_line_prefix(const TextRunMeasure *m, size_t lo, size_t hi, size_t upto,
                            TextRunSizeBasis basis)
{
    TextRunLineSum s;
    size_t i, keep_hi = lo;
    bool continues;

    DCHECK(lo <= hi && hi <= m->count, "a line was measured over a range that is not inside the collected run");
    DCHECK(lo <= upto && upto <= hi,
           "css-text-3 §4.1.2's per-line sum was asked for a prefix that ends outside the line it is a prefix "
           "of. The trim range is the line's, so a bound past `hi` would sum items §4.1.2 never looked at and "
           "a bound before `lo` would sum a negative span");
    for (i = hi; i > lo; i--)
        if (tr_item_stops_trim(&m->items[i - 1])) { keep_hi = i; break; }
    tr_line_sum_open(&s, lo);
    tr_line_sum_to(m, &s, upto, basis);
    if (upto > keep_hi) return tr_line_sum_size(m, &s);
    continues = upto < keep_hi && m->items[upto].kind == TEXT_RUN_ITEM_CHAR &&
                m->items[upto].style == s.sum.run_style;
    return continues ? tr_end_offset(m, &s.sum, keep_hi) : tr_end_size(m, &s.sum);
}

/* One line's inline size: the prefix above taken to the line's end, which CSS 2.2 §9.4.2's distribution
   compares against an available width. */
static CssPx tr_line_size(const TextRunMeasure *m, size_t lo, size_t hi, TextRunSizeBasis basis)
{
    return tr_line_prefix(m, lo, hi, hi, basis);
}

/* Whether the soft wrap opportunity at a boundary is enabled. css-text-3 §5.5 "Line Breaking Details" has two
   cases:
     "For soft wrap opportunities created by characters that disappear at the line break (e.g. U+0020 SPACE),
      properties on the box directly containing that character control the line breaking at that opportunity."
      [UAX14] LB18 `SP ÷` puts the opportunity after the space, so the space to the left is that character.
     "For soft wrap opportunities defined by the boundary between two characters …, the white-space property on
      the nearest common ancestor of the two characters controls breaking." The walk below answers this one. */

/* The nearest common ancestor of the two inline boxes a boundary falls between: the element css-text-3 §5.5
   hands the decision to for "soft wrap opportunities defined by the boundary between two characters or atomic
   inlines". It is a walk per boundary, not a field per character, because the question is about a pair.
   The answer is an element by the caller's contract: CSS 2.2 §9.4.2 establishes one inline formatting context
   with "a block container box", one element containing both; a Document, a fragment or no common ancestor
   means two contexts were accumulated through one measurement. */
static lxb_dom_element_t *tr_nearest_common_ancestor(lxb_dom_element_t *a, lxb_dom_element_t *b)
{
    lxb_dom_node_t *x, *y;

    DCHECK(a != NULL && b != NULL,
           "css-text-3 §5.5's nearest common ancestor was asked for with one of the two inline boxes missing. "
           "Every character this accumulator collects carries the element whose computed properties it has, "
           "written by `tr_append`, so a NULL here is a collection entry that was never filled");
    if (a == b) return a;
    for (x = lxb_dom_interface_node(a); x != NULL; x = x->parent)
        for (y = lxb_dom_interface_node(b); y != NULL; y = y->parent)
            if (x == y) {
                DCHECK(x->type == LXB_DOM_NODE_TYPE_ELEMENT,
                       "css-text-3 §5.5's nearest common ancestor of two characters in ONE inline formatting "
                       "context is not an ELEMENT. CSS 2.2 §9.4.2 establishes that context with \"a block "
                       "container box\", which is one element containing both characters — so a Document or a "
                       "fragment here means two formatting contexts were accumulated through one measurement, "
                       "and every boundary between them is a break decided over text that shares no line");
                return lxb_dom_interface_element(x);
            }
    DFAIL("two characters of ONE inline formatting context have NO common ancestor, so they are in different "
          "trees. css-text-3 §5.5 gives a boundary between two characters to \"the white-space property on the "
          "nearest common ancestor\", and there is none to read — which is not a missing capability but the "
          "accumulator's contract broken by its caller: core/layout/text_run.h states that the nodes of ONE "
          "formatting context are added in document order, and a node from a second tree cannot be in that "
          "order. Find the walk that added it");
    return a;
}

/* Neither side of an opportunity is a forced break, by [UAX14]: HTML §15.3.4's newline is U+000A, class LF,
   and LB6 `× ( BK | CR | LF | NL )` prohibits the boundary before it while LB5 `LF !` makes the one after
   mandatory. So a break item's `wraps` and `collapsible_space` are never read, and
   `text_run_measure_add_forced_break` does not compute them.
   An atomic inline can be on either side, by §5.5's own wording ("the boundary between two characters or
   atomic inlines"), so a `white-space: nowrap` ancestor suppresses an opportunity beside an image as beside a
   letter. It is never §5.5's first case ("characters that disappear at the line break"), since it renders, so
   that arm asks only about a collapsible space. */
static bool tr_opportunity_enabled(const TextRunMeasure *m, size_t left_item, size_t right_item)
{
    const TextRunItem *left, *right;
    lxb_dom_element_t *nca;
    bool wraps;

    DCHECK(left_item < m->count && right_item < m->count,
           "css-text-3 §5.5's soft wrap opportunity was asked about an item past the end of the collected run");
    left = &m->items[left_item];
    right = &m->items[right_item];
    DCHECK((left->kind == TEXT_RUN_ITEM_CHAR || left->kind == TEXT_RUN_ITEM_ATOMIC) &&
               (right->kind == TEXT_RUN_ITEM_CHAR || right->kind == TEXT_RUN_ITEM_ATOMIC),
           "css-text-3 §5.5's soft wrap opportunity was asked about a boundary one of whose sides is an inline "
           "box EDGE or a FORCED LINE BREAK. §5.5 states the question over \"two characters or atomic inlines\" "
           "and neither other kind can be a side of one: an edge contributes no code point at all, so [UAX14] "
           "never decided a boundary at it, and a forced break is a U+000A that LB6 makes PROHIBITED before and "
           "LB5 makes MANDATORY after — so a boundary touching one is never an OPPORTUNITY and this function is "
           "unreachable for it");
    DCHECK(right->kind != TEXT_RUN_ITEM_CHAR || !right->collapsible_space,
           "[UAX14] LB7 \"do not break before spaces or zero width space\" was asked to allow a break before a "
           "collapsible space. core/layout/line_break.c returns PROHIBITED there unconditionally, so this is "
           "the rule chain and this file disagreeing about which side of a space the opportunity is on");
    /* §5.5's first case, about one character: "properties on the box directly containing that character
       control the line breaking at that opportunity." */
    if (left->kind == TEXT_RUN_ITEM_CHAR && left->collapsible_space) return left->wraps;
    nca = tr_nearest_common_ancestor(left->style, right->style);
    wraps = tr_white_space(nca).wraps;
    /* Where both characters are in one inline box, that box is the nearest common ancestor, so this fresh read
       of `white-space` must equal the flag `tr_append` recorded at collection; a disagreement is a per-flow
       computed value read outside the flow that collected it, which would silently move a break. It is asked
       only of a character, the one kind `tr_append` records a flag for: an atomic's `wraps` is the false
       `tr_append_item` wrote, and an atomic is never its boundary's nearest common ancestor because
       core/layout/line_box.c does not descend into one. */
    DCHECK(left->kind != TEXT_RUN_ITEM_CHAR || nca != left->style || wraps == left->wraps,
           "css-text-3 §5's `white-space` read for one element answered differently at collection time and at "
           "measurement time. The characters of an inline formatting context are collected and then measured "
           "within one flow, so the cascade cannot have changed between them; this is a computed value being "
           "read against a different flow's DOM than the one the run was collected from");
    return wraps;
}

/* Where the two index spaces meet: a break before code point `k` becomes the item boundary that ends a line.
   [UAX14] sees code points only (css-text-3 §5.5: an inline box boundary must not create or forbid a break),
   while a line is a contiguous run of items whose sizes are summed.
   The boundary is immediately after the item holding code point k-1. Taking the item holding k instead would put
   every edge between the two characters on the earlier line: `<div>aaa <span>bbb</span></div>` breaking at its
   space would leave the span's opening edge on the first line and its text on the second. This mapping puts
   that edge on the second line and keeps the empty span of `<div>aaa<span></span> bbb</div>` on the first,
   from the order the caller emitted items in, which is why `text_run_measure_add_box_edge` takes no side
   argument. `k == 0` is the start of text (LB2 never breaks there) and maps to item boundary 0. */
static size_t tr_item_boundary_of_cp(const size_t *item_of_cp, size_t k)
{
    return k == 0 ? 0 : item_of_cp[k - 1] + 1;
}

/* css-text-3 §5.5's one tailoring of [UAX14]: U+00A0 NO-BREAK SPACE beside an atomic inline, the only boundary
 * where the section and the annex differ. §5.5 states:
 *   "For Web-compatibility there is a soft wrap opportunity before and after each replaced element or other
 *    atomic inline, even when adjacent to a character that would normally suppress them, including U+00A0
 *    NO-BREAK SPACE."
 *   "However, with the exception of U+00A0 NO-BREAK SPACE, there must be no soft wrap opportunity between
 *    atomic inlines and adjacent characters belonging to the Unicode GL, WJ, or ZWJ line breaking classes."
 * LB20 `÷ CB`, `CB ÷` is the first; the second is what LB8a, LB11, LB12 and LB12a already answer, since they
 * precede LB20. U+00A0 is GL, so there the annex suppresses an opportunity §5.5 requires.
 * It is a tailoring UAX14 §8 "Customization" provides for, applied to the one array before any partition reads
 * it, so every boundary still has one answer. A mandatory action is never overwritten (§5.5's forced breaks
 * and LB3's eot are stronger than an opportunity); the loop bounds keep eot out and the assert the rest. */
static void tr_atomic_nbsp_tailor_at(TextRunMeasure *m, size_t i)
{
    bool left_atomic = m->items[m->item_of_cp[i - 1]].kind == TEXT_RUN_ITEM_ATOMIC;
    bool right_atomic = m->items[m->item_of_cp[i]].kind == TEXT_RUN_ITEM_ATOMIC;
    uint32_t other = left_atomic ? m->cps[i] : m->cps[i - 1];

    DCHECK(i >= 1 && i < m->ncps, "css-text-3 §5.5's tailoring was asked at a boundary with no character beside it");
    if (left_atomic == right_atomic) return;   /* neither side, or both — no adjacent character */
    if (other != 0x00A0) return;
    DCHECK(line_break_class_of(0x00A0) == LB_CLASS_GL,
           "U+00A0 NO-BREAK SPACE's [UAX14] Line_Break class is not GL, so LB12 `GL ×` and LB12a "
           "`[^SP BA HY] × GL` are no longer what suppresses the opportunity beside it and this tailoring "
           "is correcting a rule that no longer fires. css-text-3 §5.5's exception names this character by "
           "code point, so the class is the thing that moved and core/layout/line_break_class.c is where");
    DCHECK(m->actions[i] != LINE_BREAK_MANDATORY,
           "css-text-3 §5.5's U+00A0 exception was about to downgrade a MANDATORY break to a soft wrap "
           "opportunity. No rule makes a boundary between U+FFFC (class CB) and U+00A0 (class GL) "
           "mandatory — LB4, LB5 and LB6 are stated over BK, CR, LF and NL and LB3 over eot, which this "
           "loop does not reach — so a mandatory action here is the rule chain and this derivation having "
           "come apart, and taking the opportunity would move a forced line break");
    m->actions[i] = LINE_BREAK_OPPORTUNITY;
}

/* css-sizing-3 §2.1 "Auto Box Sizes": "If the ideal max-content size would be smaller than the min-content
   size (e.g. due to the use of negative margins) the effective max-content size is floored by the min-content
   size." A negative margin box (an atomic inline's, or an inline box edge) lowers a max-content line's sum and
   not the widest min-content segment, so the floor is live on page input. Both answers are maxima over lines
   from text_run_measure_init's zero, so a line of negative extent answers zero. */
static void tr_floor_max_content(TextRunMeasure *m)
{
    m->max_content = css_px_max(m->max_content, m->min_content);
}

/* The phases between the closed collection and §2.1's walk, one item or position per step. */
typedef enum {
    TR_PREP_COUNT = 0,   /* the code-point-bearing items, counted */
    TR_PREP_EDGES,       /* a run of box edges only, summed as its one line */
    TR_PREP_COPY,        /* the code points and their items, copied out for [UAX14] */
    TR_PREP_BREAKS,      /* core/layout/line_break.h's pass */
    TR_PREP_TAILOR,      /* css-text-3 §5.5's U+00A0 tailoring */
    TR_PREP_WALK         /* §2.1's walk */
} TrPrep;

void text_run_measure_finish_begin(TextRunMeasure *m)
{
    DCHECK(m != NULL, "a text run measurement was finished with nothing to finish");
    DCHECK(!m->finished, "a text run measurement was finished twice — the second call would run [UAX14] over a "
                         "collection the first one released");
    m->finished = true;
    m->prep = TR_PREP_COUNT;
    m->cursor = 0;
    m->copied = 0;
    if (m->count == 0) {
        /* An inline formatting context with no items: css-sizing-3 §2.1's two sizes are the size that "fits
           around its contents", and there are none, so both are zero. */
        DCHECK(m->items == NULL, "an empty run holds a collection it never appended to");
        m->answered = true;
    }
}

bool text_run_measure_finish_done(const TextRunMeasure *m)
{
    DCHECK(m != NULL && m->finished, "a text run measurement's walk was asked about before its collection closed");
    return m->answered;
}

/* One step before §2.1's walk; answers the items it took. */
static size_t tr_prep_step(TextRunMeasure *m)
{
    switch ((TrPrep)m->prep) {
    case TR_PREP_COUNT:
        if (tr_kind_has_code_point(m->items[m->cursor].kind)) m->ncps++;
        if (++m->cursor < m->count) return 1;
        m->cursor = 0;
        if (m->ncps == 0) {
            /* A run of box edges only (empty inline boxes). [UAX14] is not run: css-text-3 §5.5's "inline box
               boundaries do not introduce a forced line break or soft wrap opportunity in the flow" leaves
               nowhere to break, so there is one line and both of css-sizing-3 §2.1's answers are its size,
               independent of any width. `splits` stays false, which tells the fill's caller it has no available
               width to derive. */
            tr_line_sum_open(&m->max_line, 0);
            tr_line_sum_open(&m->min_line, 0);
            m->prep = TR_PREP_EDGES;
            return 1;
        }
        m->cps = malloc(m->ncps * sizeof *m->cps);
        m->actions = malloc((m->ncps + 1) * sizeof *m->actions);
        m->item_of_cp = malloc(m->ncps * sizeof *m->item_of_cp);
        CHECK(m->cps != NULL && m->actions != NULL && m->item_of_cp != NULL,
              "out of memory running [UAX14]'s line breaking over one inline formatting context. Every allocation "
              "is one entry per code point of the run — its own text, its forced breaks and its atomic inlines — "
              "so a failure here is the physical floor");
        m->prep = TR_PREP_COPY;
        return 1;
    case TR_PREP_EDGES:
        tr_line_sum_add(m, &m->max_line, TEXT_RUN_SIZE_MAX_CONTENT);
        tr_line_sum_add(m, &m->min_line, TEXT_RUN_SIZE_MIN_CONTENT);
        if (++m->cursor < m->count) return 1;
        m->max_content = css_px_max(m->max_content, tr_line_sum_size(m, &m->max_line));
        m->min_content = css_px_max(m->min_content, tr_line_sum_size(m, &m->min_line));
        tr_floor_max_content(m);
        m->answered = true;
        return 1;
    case TR_PREP_COPY: {
        size_t i = m->cursor++;

        if (tr_kind_has_code_point(m->items[i].kind)) {
            DCHECK(m->copied < m->ncps,
                   "the copy found more CODE-POINT-BEARING items than the count did, so the [UAX14] arrays are "
                   "sized for a run that is not the one being copied into them; nothing may append after the "
                   "collection closes");
            m->cps[m->copied] = tr_code_point_at(m, i);
            m->item_of_cp[m->copied] = i;
            m->copied++;
        }
        if (m->cursor < m->count) return 1;
        DCHECK(m->copied == m->ncps,
               "the copy found fewer CODE-POINT-BEARING items than the count did, so the [UAX14] arrays are "
               "sized for a run that is not the one copied into them");
        line_break_pass_begin(&m->breaks, m->cps, m->ncps, m->actions);
        m->prep = TR_PREP_BREAKS;
        return 1;
    }
    case TR_PREP_BREAKS:
        line_break_pass_step(&m->breaks);
        if (!line_break_pass_done(&m->breaks)) return 1;
        m->cursor = 1;
        m->prep = TR_PREP_TAILOR;
        return 1;
    case TR_PREP_TAILOR:
        /* css-text-3 §5.5's tailoring, applied to the one array before any partition reads it. */
        if (m->cursor < m->ncps) tr_atomic_nbsp_tailor_at(m, m->cursor++);
        if (m->cursor < m->ncps) return 1;
        m->walk = 1;
        tr_line_sum_open(&m->max_line, 0);
        tr_line_sum_open(&m->min_line, 0);
        m->prep = TR_PREP_WALK;
        return 1;
    case TR_PREP_WALK:
        break;
    }
    DFAIL("css-sizing-3 §2.1's preparation was stepped in a phase TrPrep does not name, or in its walk");
    return 0;
}

/* The two answers are one walk over different lines, css-sizing-3 §2.1 "Auto Box Sizes"'s own construction: "if
   none of the soft wrap opportunities within the box were taken" against "if all soft wrap opportunities within
   the box were taken"; an atomic descendant's own opportunities are its answer, not breaks this run takes.
   Max-content lines are cut by forced breaks alone (CSS 2.2 §10.3.5 "Floating, non-replaced elements": "without
   breaking lines other than where explicit line breaks occur"), min-content lines by every break. Both open lines
   are summed forward at every position, so a step costs the items it passes. `actions[ncps]` is LB3's mandatory
   break at eot, so the last step closes each answer's last line and there is no tail case. */
size_t text_run_measure_finish_step(TextRunMeasure *m)
{
    size_t i, at, k;
    bool forced, soft = false;

    DCHECK(m != NULL && m->finished && !m->answered && !m->released,
           "css-sizing-3 §2.1's walk was stepped outside its run: before the collection closed, after its last "
           "position, or after the release");
    if (m->prep != TR_PREP_WALK) return tr_prep_step(m);
    i = m->walk++;
    DCHECKF(i >= 1 && i <= m->ncps, "css-sizing-3 §2.1's walk reached position %zu of a run of %zu code points",
            i, m->ncps);
    /* The line ends at this break, in item coordinates (`tr_item_boundary_of_cp`). At `i == ncps` it runs
       to the end of the collection, because trailing box edges after the last code point are still on the
       last line: LB3 closes the text, not the items. */
    at = i == m->ncps ? m->count : tr_item_boundary_of_cp(m->item_of_cp, i);
    forced = m->actions[i] == LINE_BREAK_MANDATORY;
    k = tr_line_sum_to(m, &m->max_line, at, TEXT_RUN_SIZE_MAX_CONTENT) +
        tr_line_sum_to(m, &m->min_line, at, TEXT_RUN_SIZE_MIN_CONTENT);

    /* At eot there is no right-hand code point and LB3 makes the action mandatory, so the opportunity arm is
       unreachable there. The guard is explicit because the index it protects, `item_of_cp[i]`, is one past
       the end at that position. */
    if (i < m->ncps && m->actions[i] == LINE_BREAK_OPPORTUNITY)
        soft = tr_opportunity_enabled(m, m->item_of_cp[i - 1], m->item_of_cp[i]);
    DCHECK(i < m->ncps || forced,
           "[UAX14] LB3 \"Always break at the end of text\" did not make the action at eot MANDATORY. "
           "core/layout/line_break.h states that the action at position `count` always is, and the walk "
           "has no tail case precisely because of it");
    /* CSS 2.2 §9.4.2's theorem, recorded on the one pass that sees it: a break strictly inside the run. Eot
       is excluded because LB3's action there closes the run; counting it would report a run with nowhere to
       break as splittable. */
    if (i < m->ncps && (forced || soft)) m->splits = true;
    if (forced && i < m->ncps) {
        LineBreakClass cls = line_break_class_of(m->cps[i - 1]);
        TextRunItemKind kind = m->items[m->item_of_cp[i - 1]].kind;

        /* An item's kind decides which class it may reach a mandatory action through, and the two are
           written by different components. A character of a collapsed run reaches one only as BK or NL
           (css-text-3 §5.5's forced classes "regardless of the white-space value"); §4.1.1 collapses CR and
           LF away (§4's CR is "treated identically to spaces (U+0020) in all respects", LF is the segment
           break). A forced break item reaches one only as LF, the code point HTML §15.3.4's `newline`
           names. An atomic inline never does: U+FFFC is class CB, and LB20 is an opportunity on both sides. */
        DCHECK(kind == TEXT_RUN_ITEM_FORCED_BREAK
                   ? cls == LB_CLASS_LF
                   : (kind == TEXT_RUN_ITEM_CHAR && (cls == LB_CLASS_BK || cls == LB_CLASS_NL)),
               "a FORCED line break inside a collapsed run came from an item whose [UAX14] class is not the "
               "one its kind implies. A CHARACTER may reach a mandatory action only as css-text-3 §5.5's BK "
               "or NL, since §4.1.1 collapses CR and LF away before this measurement sees them; a FORCED "
               "BREAK item may reach it only as LF, since HTML §15.3.4's `display-outside: newline` is the "
               "U+000A css-text-3 §4 names; and an ATOMIC INLINE may not reach one at all, since its U+FFFC "
               "is class CB and LB20 breaks before and after it rather than mandating. So this is Phase I's "
               "collapsible set, this file's two code-point constants, and [UAX14]'s classes having come "
               "apart");
    }
    if (forced) {
        m->max_content = css_px_max(m->max_content, tr_line_sum_size(m, &m->max_line));
        tr_line_sum_open(&m->max_line, at);
    }
    if (forced || soft) {
        m->min_content = css_px_max(m->min_content, tr_line_sum_size(m, &m->min_line));
        tr_line_sum_open(&m->min_line, at);
    }
    if (i < m->ncps) return k;
    DCHECK(m->max_line.lo == m->count && m->min_line.lo == m->count,
           "[UAX14] LB3 \"Always break at the end of text\" did not close the last line of one of the two "
           "answers over the whole ITEM collection, so a run's tail was collected and never measured. The last "
           "line runs to `count` rather than to a break position exactly so that a box edge after the final "
           "character is still on a line; a short answer here is that mapping and this walk disagreeing");
    tr_floor_max_content(m);
    /* The code points are not read again: every walk over this run asks the action at a position, and an
       advance is read off the item, which carries the element the code point belongs to. */
    free(m->cps);
    m->cps = NULL;
    m->answered = true;
    return k;
}

void text_run_measure_release(TextRunMeasure *m)
{
    DCHECK(m != NULL, "a text run measurement was released with nothing to release");
    DCHECK(m->finished,
           "a text run measurement was released before [UAX14] ever ran over it, so the collection is being "
           "freed by a caller that never asked it anything. text_run.h states the sequence — collect, finish, "
           "read and fill, release — and a walk that skipped the middle of it measured a document and threw "
           "the measurement away");
    DCHECK(!m->released, "a text run measurement was released twice, and the second call frees pointers the "
                         "first one returned to the allocator");
    m->released = true;
    if (m->prep == TR_PREP_BREAKS) line_break_pass_release(&m->breaks);
    free(m->items);
    free(m->actions);
    free(m->item_of_cp);
    free(m->cps);   /* held only while §2.1's walk is incomplete */
    m->items = NULL;
    m->actions = NULL;
    m->item_of_cp = NULL;
    m->cps = NULL;
    m->count = 0;
    m->capacity = 0;
    m->ncps = 0;
}

/* The relation between the two answers, asserted where they are read because it is about the pair, and
   established by `tr_floor_max_content` on every exit that sets `answered`. CSS 2.2 §10.3.5's
   `min(max(preferred minimum width, available width), preferred width)` and css-sizing-3 §2.1's
   `clamp(min-content, stretch-fit, max-content)` agree only while it holds. */
static void tr_require_answers(const TextRunMeasure *m)
{
    DCHECK(m != NULL, "css-sizing-3 §2.1's intrinsic inline sizes were read from no measurement");
    DCHECK(m->answered,
           "css-sizing-3 §2.1's intrinsic inline sizes were read from a measurement whose walk over its code "
           "point positions has not completed. text_run_measure_finish_step is what produces them, and the "
           "fields before its last position hold a partial maximum or the zero text_run_measure_init wrote — "
           "which is a plausible answer for a run with text in it and is why this is a crash rather than a "
           "default");
    DCHECK(!m->released,
           "a text run measurement was read AFTER `text_run_measure_release`. Every answer this component has "
           "— css-sizing-3 §2.1's two sizes and CSS 2.2 §9.4.2's line boxes — is derived from a collection "
           "that call returned to the allocator, so the numbers still sitting in the struct describe a run "
           "whose items are gone and the fill would walk freed memory. The measurement ENDS at the release");
    DCHECK(m->min_content.px <= m->max_content.px,
           "a text run's MIN-CONTENT inline size came out WIDER than its max-content inline size. css-sizing-3 "
           "§2.1 states the first as the size \"if ALL soft wrap opportunities within the box were taken\" and "
           "the second as the size \"if NONE\" were, and §2.1's floor makes the effective max-content size "
           "at least the min-content one; `tr_floor_max_content` applies it on every exit that sets "
           "`answered`, so this is an exit that skipped it");
}

/* CSS 2.2 §9.4.2's fill and its per-item positions are about a line that exists, so every box on it has one
   used width. An atomic inline carrying css-sizing-3 §5.2's two contributions has none: they are its sizes under
   hypothetical zero-sized and infinitely-sized containing blocks, neither of which is the width being filled,
   and reading either member would place the box at a size no layout assigned it. core/layout/line_box.c states
   each atomic's used margin-box width twice; core/layout/intrinsic_size.c hands over §5.2's pair and never
   fills. So the flag is false for every measurement that may be filled. */
static void tr_require_used_sizes(const TextRunMeasure *m)
{
    DCHECK(!m->atomic_sizes_differ,
           "CSS 2.2 §9.4.2's line boxes were asked of a run holding an ATOMIC INLINE whose two css-sizing-3 "
           "§5.2 \"Intrinsic Contributions\" differ, so that box has no USED width for a line to place it at. "
           "css-sizing-3 §5.1 \"Intrinsic Sizes\" defines the pair as the sizes the box would have under a "
           "hypothetical ZERO-sized and a hypothetical INFINITELY-sized containing block, and a line box is "
           "filled at neither — it is filled at a width the containing block has already determined. "
           "core/layout/line_box.c supplies `used_value_margin_edge_px` for both members precisely so that "
           "this cannot arise; a true flag here is an INTRINSIC measurement from core/layout/intrinsic_size.c "
           "being handed to the fill, and every box on the resulting line would be positioned at a size no "
           "layout ever assigned it");
}

CssPx text_run_measure_max_content(const TextRunMeasure *m)
{
    tr_require_answers(m);
    return m->max_content;
}

CssPx text_run_measure_min_content(const TextRunMeasure *m)
{
    tr_require_answers(m);
    return m->min_content;
}

lxb_dom_element_t *text_run_measure_item_style(const TextRunMeasure *m, size_t i)
{
    tr_require_answers(m);
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    DCHECK(m->items[i].style != NULL,
           "a collected item carries no element. `tr_append_item` writes one on every path for both kinds, so "
           "a NULL here is a record that was reached before it was filled");
    return m->items[i].style;
}

bool text_run_measure_item_is_text(const TextRunMeasure *m, size_t i)
{
    tr_require_answers(m);
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    return m->items[i].kind == TEXT_RUN_ITEM_CHAR;
}

bool text_run_measure_item_is_forced_break(const TextRunMeasure *m, size_t i)
{
    tr_require_answers(m);
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    return m->items[i].kind == TEXT_RUN_ITEM_FORCED_BREAK;
}

bool text_run_measure_item_is_atomic(const TextRunMeasure *m, size_t i)
{
    tr_require_answers(m);
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    return m->items[i].kind == TEXT_RUN_ITEM_ATOMIC;
}

uint32_t text_run_measure_item_cp(const TextRunMeasure *m, size_t i)
{
    tr_require_answers(m);
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    DCHECK(m->items[i].kind == TEXT_RUN_ITEM_CHAR,
           "the CODE POINT of a collected item that is not a CHARACTER was read. Two other kinds carry one and "
           "neither is a character the document contains: a forced break's is the U+000A css-text-3 §4 names "
           "as HTML's newline, which `text_run_measure_add_forced_break` supplies from HTML §15.3.4 \"Phrasing "
           "content\"'s declaration for [UAX14] and for nothing else, and an atomic inline's is the U+FFFC "
           "whose [UAX14] class CB is how css-text-3 §5.5 \"Line Breaking Details\"' \"soft wrap opportunity "
           "before and after\" is expressed. Handing either to a painter draws a glyph for a code point no "
           "author wrote — ask `text_run_measure_item_is_text` first, which is what every walk over a line's "
           "items already asks for CSS 2.2 §9.4.2's zero-height rule");
    return m->items[i].cp;
}

InstalledFont text_run_measure_item_face(const TextRunMeasure *m, size_t i)
{
    tr_require_answers(m);
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    DCHECK(m->items[i].kind == TEXT_RUN_ITEM_CHAR,
           "the FACE of a collected item that is not a character was read — only a character is drawn");
    DCHECK((unsigned)m->items[i].face < (unsigned)INSTALLED_FONT_COUNT,
           "a collected character carries no face. `tr_append` chooses one for every character, so this is a "
           "record reached before it was filled");
    return m->items[i].face;
}

bool text_run_measure_splits(const TextRunMeasure *m)
{
    tr_require_answers(m);
    return m->splits;
}

/* ---- CSS 2.2 §9.4.2's distribution across line boxes ------------------------------------------------------
   "When several inline-level boxes cannot fit horizontally within a single line box, they are distributed among
   two or more vertically-stacked line boxes."
   The iteration is `text_run_measure_finish_step`'s: the same positions, `actions` and `tr_opportunity_enabled`
   question; only which positions are taken differs. `prev` is the last position that still fit, which makes it
   greedy: when a line grows past `available` the break goes back to `prev` and the segment after it opens the
   next line. With no such prefix (`prev == start`) nothing is emitted early, per §9.4.2: "if an inline box
   cannot be split … then the inline box overflows the line box." Every position either closes a line at a
   strictly greater boundary or leaves it alone, so the walk ends at the run's last position.
   The lines are non-empty and partition `[0, count)` (`tr_item_boundary_of_cp` is strictly increasing and the
   last line closes at `count`), which bounds the allocation at `count`. */
void text_run_fill_begin(TextRunFill *f, const TextRunMeasure *m, CssPx available)
{
    tr_require_answers(m);
    tr_require_used_sizes(m);
    DCHECK(f != NULL, "CSS 2.2 §9.4.2's fill was begun with nowhere to hold it");
    f->available = available;
    f->walk = 1;
    f->n = 0;
    f->start = 0;
    f->prev = 0;
    tr_line_sum_open(&f->line, 0);
    tr_line_sum_open(&f->at_prev, 0);
    tr_line_sum_open(&f->seg, 0);
    f->out = NULL;
    f->complete = false;
    if (m->count == 0) {
        /* §9.4.2: "line boxes are created as needed to hold inline-level content within an inline formatting
           context", and there is none: no line box at all, not the zero-height line box the section treats as
           not existing. */
        DCHECK(m->items == NULL, "an empty run holds a collection it never appended to");
        f->complete = true;
        return;
    }
    f->out = malloc(m->count * sizeof *f->out);
    CHECK(f->out != NULL, "out of memory distributing one inline formatting context's content across CSS 2.2 "
                          "§9.4.2's line boxes. The array is one entry per collected item and the lines partition "
                          "them, so a failure here is the physical floor");
    if (m->ncps == 0) {
        /* css-text-3 §5.5 "Line Breaking Details": "out-of-flow boxes and inline box boundaries do not
           introduce a forced line break or soft wrap opportunity in the flow". A run of box edges has nowhere
           to break, so §9.4.2's overflow sentence puts it on one line box. */
        DCHECK(!m->splits, "a run with no CODE POINTS in it reported a break position, and css-text-3 §5.5 says "
                           "an inline box boundary is not one — so `finish` recorded a split from an item that "
                           "carries no code point for [UAX14] to have decided about. A forced break carries one "
                           "and so does an atomic inline, so a `br` or an `<img>` in this run would have been "
                           "counted and this arm not taken");
        f->out[0].from = 0;
        f->out[0].to = m->count;
        f->out[0].size = tr_line_size(m, 0, m->count, TEXT_RUN_SIZE_MAX_CONTENT);
        f->n = 1;
        f->start = m->count;
        f->complete = true;
    }
}

bool text_run_fill_done(const TextRunFill *f)
{
    DCHECK(f != NULL, "CSS 2.2 §9.4.2's fill was asked about with no fill");
    return f->complete;
}

/* One line ending at `to`, of inline size `size`. */
static void tr_fill_emit(TextRunFill *f, const TextRunMeasure *m, size_t to, CssPx size)
{
    DCHECK(f->n < m->count, "CSS 2.2 §9.4.2's fill produced more line boxes than the run has items, and the "
                            "lines partition the items — so a line was emitted empty or a boundary went "
                            "backwards");
    DCHECKF(size.px == tr_line_size(m, f->start, to, TEXT_RUN_SIZE_MAX_CONTENT).px,
            "the line [%zu, %zu) was fitted at %.17g px and measures %.17g px from its start: the forward sum and "
            "the line's own sum came apart", f->start, to, size.px,
            tr_line_size(m, f->start, to, TEXT_RUN_SIZE_MAX_CONTENT).px);
    f->out[f->n].from = f->start;
    f->out[f->n].to = to;
    f->out[f->n].size = size;
    f->n++;
    f->start = to;
}

size_t text_run_fill_step(TextRunFill *f, const TextRunMeasure *m)
{
    size_t i, at, k;
    bool forced, soft = false;

    DCHECK(f != NULL && !f->complete, "CSS 2.2 §9.4.2's fill was stepped after its last position");
    tr_require_answers(m);
    i = f->walk++;
    DCHECKF(i >= 1 && i <= m->ncps, "CSS 2.2 §9.4.2's fill reached position %zu of a run of %zu code points",
            i, m->ncps);
    at = i == m->ncps ? m->count : tr_item_boundary_of_cp(m->item_of_cp, i);
    forced = m->actions[i] == LINE_BREAK_MANDATORY;
    /* The open line and the line that would open at `prev` are both summed forward to every position, so no
       position re-sums a line from its start. */
    k = tr_line_sum_to(m, &f->line, at, TEXT_RUN_SIZE_MAX_CONTENT) +
        tr_line_sum_to(m, &f->seg, at, TEXT_RUN_SIZE_MAX_CONTENT);
    if (i < m->ncps && m->actions[i] == LINE_BREAK_OPPORTUNITY)
        soft = tr_opportunity_enabled(m, m->item_of_cp[i - 1], m->item_of_cp[i]);
    if (forced || soft) {
        DCHECK(at > f->prev, "CSS 2.2 §9.4.2's fill met two break positions at the SAME item boundary, so one of "
                             "them divides nothing. `tr_item_boundary_of_cp` is strictly increasing in the code "
                             "point index and the eot position maps past every item, so this is that mapping "
                             "having stopped being a function of the code point");
        /* "cannot fit horizontally within a single line box", measured over the line as it would be if this
           position were not taken: the comparison is at `at` and the emission at `prev`. `prev > start` is
           §9.4.2's cannot-be-split condition stated positively. */
        if (f->prev > f->start && tr_end_size_px(m, &f->line.size) > f->available.px) {
            tr_fill_emit(f, m, f->prev, tr_line_sum_size(m, &f->at_prev));
            f->line = f->seg;
        }
        if (forced) {
            /* css-text-3 §5.5 makes this break forced "regardless of the white-space value", so the line ends
               here whether or not anything overflowed; at `i == ncps` it is LB3 closing the run. */
            tr_fill_emit(f, m, at, tr_line_sum_size(m, &f->line));
            tr_line_sum_open(&f->line, at);
        }
        f->prev = at;
        f->at_prev = f->line;
        tr_line_sum_open(&f->seg, at);
    }
    if (i < m->ncps) return k;
    DCHECK(f->start == m->count,
           "[UAX14] LB3 \"Always break at the end of text\" did not close CSS 2.2 §9.4.2's last line box over "
           "the whole ITEM collection, so the run's tail is on no line at all. The last line runs to `count` "
           "rather than to a break position exactly so that a box edge after the final character is still on a "
           "line, and a caller reading these lines would place that box nowhere");
    DCHECK(f->n >= 1, "CSS 2.2 §9.4.2 produced NO line box for a run that has code points in it — \"line boxes "
                      "are created as needed to hold inline-level content\" and there is content");
    /* §9.4.2's overflow theorem, closed here: `text_run_measure_splits` lets a caller skip deriving an
       available width, which is safe only because a run with no inside position emits only at eot, one line
       box with `available` never compared. */
    DCHECK(m->splits || f->n == 1,
           "CSS 2.2 §9.4.2's fill divided a run that `text_run_measure_splits` says has nowhere to be divided. "
           "The section's \"if an inline box cannot be split … then the inline box overflows the line box\" is "
           "what makes that predicate a theorem, and a caller trusting it has passed an available width it "
           "derived from nothing");
    f->complete = true;
    return k;
}

size_t text_run_fill_end(TextRunFill *f, const TextRunMeasure *m, TextRunLine **lines)
{
    size_t n;

    DCHECK(f != NULL && f->complete, "CSS 2.2 §9.4.2's fill was ended before its last position was taken");
    DCHECK(lines != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked for with nowhere to put them. The count alone is not the "
           "answer this entry has: a caller asks which line each BOX is on — §10.8's step 3 is two maxima over "
           "the boxes on ONE line — and a number cannot be asked that");
    DCHECK(f->n == 0 || f->out != NULL, "CSS 2.2 §9.4.2's fill reported line boxes and holds none of them");
    tr_require_answers(m);
    *lines = f->out;
    n = f->n;
    f->out = NULL;
    f->n = 0;
    return n;
}

size_t text_run_fill_lines(const TextRunFill *f, const TextRunLine **lines)
{
    DCHECK(f != NULL && lines != NULL, "CSS 2.2 §9.4.2's emitted lines were asked with no fill or nowhere");
    *lines = f->out;
    return f->n;
}

void text_run_fill_set_available(TextRunFill *f, CssPx available)
{
    DCHECK(f != NULL && !f->complete, "CSS 2.2 §9.4.2's open line was narrowed after the fill's last position");
    f->available = available;
}

void text_run_fill_release(TextRunFill *f)
{
    DCHECK(f != NULL, "CSS 2.2 §9.4.2's fill was released with no fill");
    free(f->out);
    f->out = NULL;
}

/* CSS 2.2 §9.4.2's per-item position on one line box: a distance from the line box's start edge, not a
   coordinate (see text_run.h). */
CssPx text_run_measure_line_offset(const TextRunMeasure *m, TextRunLine line, size_t i)
{
    tr_require_answers(m);
    tr_require_used_sizes(m);
    DCHECK(line.from <= line.to && line.to <= m->count,
           "CSS 2.2 §9.4.2's per-item position was asked over a line that is not one of the fill's — the fill's "
           "lines PARTITION the collected items, so a range outside `[0, count]` is a `TextRunLine` some other "
           "measurement produced and the offsets would be into another document's text");
    DCHECK(line.from <= i && i <= line.to,
           "CSS 2.2 §9.4.2's per-item position was asked for an item that is not on the line it was asked "
           "about. A box is on a line exactly when one of its items is (core/layout/line_box.c takes the "
           "intersection), so an index outside the line is a caller that has lost which fragment it is placing "
           "— and the answer would be a real distance measured along the wrong line. `line.to` itself IS "
           "admitted: it is where the line's last item ENDS, which is a fragment's far edge");
    return tr_line_prefix(m, line.from, line.to, i, TEXT_RUN_SIZE_MAX_CONTENT);
}
