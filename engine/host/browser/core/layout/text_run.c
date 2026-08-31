/* css-text-3 §4.1's white space processing and §5's line breaking, over core/layout/line_break.h's [UAX14]
   rules and the advance measures core/css/font_metrics.h reads off the first available font. See text_run.h for
   the two answers this produces, for why it is an accumulator, for why it collects before it measures, and for
   the one rule §4.1.2's trimming becomes once the lines are known. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/encoding/decode.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/line_break.h"
#include "core/layout/text_run.h"

/* ---- the `white-space` group this component measures --------------------------------------------------------
   css-text-3 §3 "White Space and Wrapping: the white-space property" splits the property's values by two
   independent facts, and this component needs BOTH separately: whether white space COLLAPSES (which decides
   what §4.1.1 leaves to measure) and whether the line WRAPS (which decides whether a soft wrap opportunity
   exists at all, and therefore whether the two answers differ). `normal` and `nowrap` share the first and
   differ in the second, which is exactly why they are one arm here with a boolean rather than two arms. */

static char *tr_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

/* DOES A SOFT WRAP OPPORTUNITY EXIST IN THIS INLINE — css-text-3 §5's "when wrapping is enabled (see
   white-space), the UA must minimize the amount of content overflowing a line by wrapping the line at a soft
   wrap opportunity". Under `nowrap` it is not enabled, so the run has no SOFT break in it and its min-content
   size is its max-content size; under `normal` it is. Neither value suppresses a FORCED break — §5.5 "Line
   Breaking Details" says BK and NL are forced line breaks "regardless of the white-space value" — which is why
   this boolean gates only the opportunity and not the mandatory action.
   The value is read here rather than derived from the collapsing arm because §3 makes them independent —
   `pre-wrap` collapses nothing and wraps, `nowrap` collapses and does not — so one boolean standing for both
   would be right for two values and wrong for the other four.
   EVERY OTHER VALUE CRASHES, and the crash is in this one predicate rather than at each use, because what those
   values change is not this component's arithmetic but the SET OF CHARACTERS §4.1.1 leaves it to measure. */
static bool tr_wraps(lxb_dom_element_t *el)
{
    char *ws = tr_computed(el, "white-space");
    bool normal = strcmp(ws, "normal") == 0, nowrap = strcmp(ws, "nowrap") == 0;

    /* FREED BEFORE THE CRASH AND NOT INSIDE IT, because a DFAIL is COMPILED OUT in a release build and control
       falls through it — a `free` on each side of one is a double free that only a release build reaches. */
    free(ws);
    if (!normal && !nowrap)
        DFAIL("css-text-3 §4.1.1 \"Phase I: Collapsing and Transformation\" makes this element's white space "
              "PRESERVED rather than collapsible — the computed `white-space` is one of `pre`, `pre-wrap`, "
              "`pre-line` or `break-spaces`, and §4.1.1's second bullet states what each of them leaves to "
              "measure: \"if white-space is set to pre, pre-wrap, or break-spaces, any sequence of spaces is "
              "treated as a sequence of NON-BREAKING SPACES. However, for pre-wrap, a soft wrap opportunity "
              "exists at the END of a sequence of spaces and/or tabs, while for break-spaces, a soft wrap "
              "opportunity exists AFTER EVERY SPACE and every tab.\" So every one of the four is a different "
              "answer to both of this component's questions, and none of them is the collapsed one. TWO THINGS "
              "ARE NEEDED, and the FORCED-BREAK SPLIT IS NO LONGER ONE OF THEM: §4.1.3 \"Segment Break "
              "Transformation Rules\" makes a preserved segment break a forced line break, and this file "
              "already measures a run that a forced break cuts into pieces (CSS 2.2 §10.3.5's \"formatting the "
              "content without breaking lines OTHER THAN WHERE EXPLICIT LINE BREAKS OCCUR\"), because [UAX14] "
              "LB4's BK reaches it under `normal` through U+000C FORM FEED and LB5's LF reaches it through "
              "HTML §15.3.4's `br`. What is missing is (1) the "
              "PRESERVED SPACE — a sequence of spaces that is a sequence of NON-BREAKING spaces rather than "
              "§4.1.1's one collapsed U+0020, with the per-value soft wrap opportunity above — and (2) §4.1.2's "
              "preserved TAB, which is not an advance at all but \"a horizontal shift that lines up the start "
              "edge of the next glyph with the next TAB STOP\" at multiples of `tab-size` from the nearest "
              "block container ancestor's starting content edge — a position, so it cannot be summed and needs "
              "the run's origin. `tab-size` itself is a third thing: core/css/css_computed_value.c derives no "
              "computed value for it. BUILD the preserved space first; the tab needs an origin this component "
              "is not given and that is a change to its interface");
    return normal;
}

/* THE THREE PROPERTIES THAT WOULD CHANGE THIS MEASUREMENT'S SUM AND THAT THIS ENGINE HAS NO COMPUTED VALUE FOR.
   css-text-3 §7.2 "Tracking: the letter-spacing property" adds its length "between each typographic character
   unit", §7.1 "Word Spacing: the word-spacing property" adds its length "at each word separator character", and
   §2.1 "Case Transforms: the text-transform property" changes which characters there are to measure at all.
   Each would enter the sum below, and none of them is a property core/css/css_computed_value.c models — so the
   cascade produces no value for them anywhere in this engine and a declaration of one reaches no consumer.
   THAT IS ASSERTED AND NOT WRITTEN DOWN, which is the whole point: a comment saying "ask for these when they
   exist" is a follow-up nothing enforces, while this fires on the commit that adds the row and names the term
   it has to become. It is side-effect-free and answers off a static list, which is what makes it a DCHECK. */
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

/* THE FOUR PROPERTIES THAT WOULD CHANGE WHERE IT BREAKS. css-text-3 §5's own list of the controls CSS provides
   over [UAX14]'s baseline is exactly these: "the line-break property allows choosing various levels of
   'strictness'", "the word-break property controls what types of letters are glommed together to form
   unbreakable 'words'", "the overflow-wrap property allows the UA to take a break anywhere in
   otherwise-unbreakable strings", "the hyphens property controls whether automatic hyphenation is allowed".
   UAX14 §8 "Customization" is the other side of the same statement, and §5's own note says the annex "defines a
   baseline behavior … which is expected to be further tailored".
   THIS IS NOT THE SAME ASSERT AS THE ONE ABOVE and folding them together would hide what the failure means.
   `line-break` in particular is what would falsify a decision made in engine/gen_line_break.mjs rather than
   here: that generator applies [UAX14] LB1's DEFAULT resolution of CJ to NS at generation time, which is sound
   only "in the absence of such criteria" — and `line-break: loose` is precisely such a criterion, resolving CJ
   to ID instead. So the day this fires, the fix is upstream of this file. */
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

/* ---- css-text-3 §4.1.1's COLLAPSIBLE CHARACTERS ------------------------------------------------------------- */

/* IS THIS CHARACTER ONE OF THE ONES §4.1.1 COLLAPSES, for an element in the `normal`/`nowrap` group.
   THE FOUR ARE NOT ONE LIST WITH ONE REASON, and the differences are the spec's own:
     U+0020 SPACE — §4.1.1's "collapsible spaces".
     U+0009 TAB   — §4.1.1 step 3, "every collapsible tab is converted to a collapsible space (U+0020)".
     U+000A LF    — the SEGMENT BREAK. css-text-3 §4 "White Space Processing & Control Characters": "in the case
                    of HTML, newlines are normalized to line feed characters (U+000A) for representation in the
                    DOM, so when an HTML document is represented as a DOM tree EACH LINE FEED (U+000A) IS
                    TREATED AS A SEGMENT BREAK." §4.1.1 step 1 then removes the spaces and tabs around it and
                    §4.1.3 transforms what is left into a space.
     U+000D CR    — NOT a segment break, which is the one of the four that is counter-intuitive and is stated
                    outright twice. §4: "unlike HTML, the DOM does not give any particular meaning to carriage
                    returns (U+000D), so they are NOT TREATED AS SEGMENT BREAKS. If carriage returns (U+000D)
                    are inserted into the DOM by means other than HTML parsing, they then get treated as defined
                    below" — and below is "carriage returns (U+000D) are TREATED IDENTICALLY TO SPACES (U+0020)
                    IN ALL RESPECTS". So a CR is a collapsible space and is reachable only through the DOM API,
                    because HTML §13.2.3.5 "Preprocessing the input stream" normalises it away in a parse.
   U+000C FORM FEED IS DELIBERATELY NOT HERE, and it is now MEASURED rather than refused. §4's control-character
   rule covers it — "control characters (Unicode category Cc) OTHER THAN tabs (U+0009), line feeds (U+000A),
   carriage returns (U+000D) and sequences that form a segment break must be rendered as a VISIBLE GLYPH which
   the UA must synthesize" — so it is content; and [UAX14] gives it Line_Break class BK, which css-text-3 §5.5
   makes a FORCED LINE BREAK "regardless of the white-space value". Both facts are honoured below: it is
   collected like any other character and the break after it cuts the max-content answer.
   THAT IS ALSO WHY THIS PREDICATE IS NOT A LINE_BREAK CLASS TEST. The two questions have different answers for
   the same character: U+0009 TAB is collapsible here and [UAX14] class BA, U+000D CR is collapsible here and
   class CR, and U+000C is content here and class BK. §4.1.1's set is css-text-3's and it is enumerated. */
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
    m->finished = false;
    m->released = false;
}

/* ONE ITEM'S STORAGE, with every field of BOTH kinds written. A partially-initialised item is the defect this
   component is least able to notice — an edge whose `cp` held a stale code point would be measured as text by
   any walk that forgot to ask the kind — so the append writes the whole record and the accessors below refuse
   the fields of the other kind. */
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
    it->wraps = false;
    it->collapsible_space = false;
    it->size = css_px(0.0);
    return it;
}

static void tr_append(TextRunMeasure *m, uint32_t cp, lxb_dom_element_t *style, bool wraps, bool space)
{
    TextRunItem *c = tr_append_item(m, TEXT_RUN_ITEM_CHAR, style);

    c->cp = cp;
    c->wraps = wraps;
    c->collapsible_space = space;
}

/* DOES THIS KIND CONTRIBUTE A CODE POINT TO [UAX14] — the partition of the three kinds that the break pass is
   stated over, written once because `finish` sizes its arrays by it, fills them by it, and asserts the two
   walks agreed. css-text-3 §5.5 "Line Breaking Details" is what puts an inline box EDGE outside it ("inline box
   boundaries do not introduce a forced line break or soft wrap opportunity in the flow"), and HTML §15.3.4's
   `display-outside: newline` is what puts a forced break INSIDE it — see text_run.h for the U+000A that
   declaration names and for why [UAX14] rather than this file decides the boundaries around it. */
static bool tr_kind_has_code_point(TextRunItemKind kind)
{
    return kind == TEXT_RUN_ITEM_CHAR || kind == TEXT_RUN_ITEM_FORCED_BREAK ||
           kind == TEXT_RUN_ITEM_ATOMIC;
}

/* DOES THIS KIND OCCUPY AN INLINE SIZE ON ITS LINE — the other partition of the four kinds, and it is NOT the
   complement of the one above. An EDGE has a size and no code point, a FORCED BREAK has a code point and no
   size, an ATOMIC INLINE has BOTH, and a CHARACTER has neither in this sense (its width is css-values-4
   §6.1.1's advance measure of its own code point, read from the font rather than stored). The two questions
   therefore cross, which is exactly why they are two functions and not one flag. */
static bool tr_kind_has_size(TextRunItemKind kind)
{
    return kind == TEXT_RUN_ITEM_EDGE || kind == TEXT_RUN_ITEM_ATOMIC;
}

/* IS THIS ITEM CONTENT css-text-3 §4.1.2 "Phase II: Trimming and Positioning" STOPS AT — the test that finds a
   line's two trim boundaries. §4.1.2 removes "a sequence of collapsible spaces at the beginning of a line" and
   again at the end, so what bounds the sequence is the first thing on the line that is not one of those spaces.
   AN ATOMIC INLINE IS SUCH A THING AND AN EDGE IS NOT, which is the whole reason this is a predicate rather
   than a `kind == CHAR` test at each end. An inline box BOUNDARY is not content — "an inline box opened before
   a line's leading space leaves that space at the beginning of the line" — but a replaced element or other
   atomic inline IS, so `<div> <img> a</div>` keeps the space between the image and the `a` and trims only the
   one before the image. A scan that stopped only at characters would trim the middle space as though it were
   at the line's edge, which is a space every user agent renders. */
static bool tr_item_stops_trim(const TextRunItem *it)
{
    if (it->kind == TEXT_RUN_ITEM_ATOMIC) return true;
    return it->kind == TEXT_RUN_ITEM_CHAR && !it->collapsible_space;
}

/* THE FIELDS OF ONE KIND, REACHED ONLY THROUGH A CHECK OF THAT KIND. Every read of `cp`, `wraps`,
   `collapsible_space` or `size` in this file goes through one of these, so a walk that lost track of which
   kind it is standing on aborts at the read rather than measuring an edge as a U+0000 or summing a
   character's uninitialised `size` into a line. */
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

/* THE CODE POINT [UAX14] IS HANDED FOR THIS ITEM, for the two kinds that have one. It is a separate accessor
   from `tr_char_at` because the two questions have different answer sets: an advance measure is asked only of a
   character, while the break pass is asked of a character AND of a forced break, and one accessor serving both
   would be an assert that no longer says which. */
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
           "text_run_measure_finish releases the collection those answers were derived from, so this is one "
           "accumulator being used for two runs");
    DCHECK(size.px >= 0.0,
           "css-sizing-3 §2.2's outer-size contribution for one side of an inline box is NEGATIVE. A padding "
           "and a border width are non-negative by their own properties' `Value:` lines, and §2.2's \"for this "
           "purpose auto margins are treated as zero\" leaves only a declared negative margin — which is real "
           "CSS, and is a case this sum has not been derived for rather than one to let through");
    tr_append_item(m, TEXT_RUN_ITEM_EDGE, style)->size = size;
    /* AN EDGE DOES NOT CLOSE A COLLAPSIBLE WHITE-SPACE RUN, which is css-text-3 §4.1.1's own scope rather than
       a convenience: the collapsing is stated over the inline formatting CONTEXT and applies "even one outside
       the boundary of the inline containing that space", so a space either side of a box boundary is one run
       and collapses to one U+0020. Leaving `in_white_space_run` alone is what makes that true. */
}

/* HTML §15.3.4's NEWLINE, PUT IN THE RUN AS THE ONE CODE POINT [UAX14] NEEDS TO DECIDE IT. css-text-3 §4
   "White Space Processing & Control Characters" is the sentence that names it — "in the case of HTML, newlines
   are normalized to line feed characters (U+000A) for representation in the DOM, so when an HTML document is
   represented as a DOM tree each line feed (U+000A) is treated as a segment break" — and §5.5 "Line Breaking
   Details" makes a PRESERVED segment break a forced line break. `display-outside: newline` is a declaration
   that this box IS one, so the code point comes from the declaration rather than from character data.
   IT IS A CONSTANT HERE AND NOT AN ARGUMENT for the reason text_run.h gives: two callers free to pass a
   different code point would be two answers to what a `br` is. */
#define TR_HTML_NEWLINE 0x000AU

void text_run_measure_add_forced_break(TextRunMeasure *m, lxb_dom_element_t *style)
{
    TextRunItem *it;

    DCHECK(m != NULL && style != NULL,
           "a forced line break was added with no accumulator or no element. CSS 2.2 §10.8's step 1 is over "
           "every inline-level box on the line and this is one of them, so its own `line-height` and first "
           "available font are operands of the line's height — there is no break without the box it is");
    DCHECK(!m->finished,
           "a forced line break was added to a measurement that has already produced its answers. [UAX14] ran "
           "over the code points as they stood, so this is one accumulator being used for two runs");
    /* THE [UAX14] PASS IS ABOUT TO INCLUDE THIS POSITION, so the tailoring this component does not honour is
       asserted here as well as at a text node — an inline formatting context that is nothing but `<br>`s adds
       no text node at all and would otherwise run the untailored annex with nothing having said so.
       THE SPACING MODEL IS DELIBERATELY NOT ASSERTED HERE. css-text-3 §7.2 "Tracking: the letter-spacing
       property" adds its length "between each typographic character unit" and css-text-3 §7.1 "Word Spacing:
       the word-spacing property" adds its length "at each word separator character"; a forced break is neither,
       and it contributes no advance for either to be added to. Asserting it would make this entry crash for a
       property that cannot change its contribution. */
    tr_require_no_line_break_tailoring();
    it = tr_append_item(m, TEXT_RUN_ITEM_FORCED_BREAK, style);
    it->cp = TR_HTML_NEWLINE;
    /* A FORCED BREAK DOES CLOSE A COLLAPSIBLE WHITE-SPACE RUN, which is the opposite of an edge and for the
       reason the edge comment gives: §4.1.1 step 4 collapses "any collapsible space IMMEDIATELY FOLLOWING
       another collapsible space", and a break between two of them is a character between them, so they are not
       adjacent and the second opens a run of its own.
       IT IS UNOBSERVABLE IN THE MEASUREMENT EITHER WAY, and that is worth stating so the next reader does not
       take this line for a load-bearing choice: a forced break always ENDS its line box, so a collapsible space
       beside one is always at a line's edge, and §4.1.2 "Phase II: Trimming and Positioning" removes exactly
       those. §4.1.1's own step 1 — "any sequence of collapsible spaces and tabs immediately preceding or
       following a segment break is removed" — reaches the same answer by a third route. Three derivations,
       one number; the flag is set to the one that follows from step 4's own wording. */
    m->in_white_space_run = false;
}

/* css-text-3 §5.5's ATOMIC INLINE, PUT IN THE RUN AS THE ONE CODE POINT [UAX14] NEEDS TO DECIDE ITS TWO
   BOUNDARIES. U+FFFC OBJECT REPLACEMENT CHARACTER has Line_Break class CB, and LB20 "Break before and after
   unresolved CB" is §5.5's "there is a soft wrap opportunity before and after each replaced element or other
   atomic inline" — see text_run.h for why the rest of §5.5's sentence needs no rule here and why its NAMED
   EXCEPTION does.
   IT IS A CONSTANT HERE AND NOT AN ARGUMENT, for `TR_HTML_NEWLINE`'s reason: two callers free to pass a
   different code point would be two answers to what an atomic inline is to the annex. */
#define TR_OBJECT_REPLACEMENT 0xFFFCU

void text_run_measure_add_atomic(TextRunMeasure *m, lxb_dom_element_t *style, CssPx size)
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
    /* THE [UAX14] PASS IS ABOUT TO INCLUDE THIS POSITION, so the tailoring this component does not honour is
       asserted here as well as at a text node and at a forced break — an inline formatting context that is
       nothing but a single `<img>` adds no text node at all and would otherwise run the untailored annex with
       nothing having said so.
       THE SPACING MODEL IS ASSERTED HERE AND THE FORCED BREAK DELIBERATELY DOES NOT, and the difference is that
       this item HAS an inline extent for a spacing term to sit beside. css-text-3 §7.2 "Tracking: the
       letter-spacing property" adds its length "between each typographic character unit", and the boundaries
       either side of an atomic inline are two such positions on this line — so the day the cascade models the
       property, this sum acquires a term at exactly this item and the assert is what says so. */
    tr_require_no_spacing_model();
    tr_require_no_line_break_tailoring();
    DCHECK(line_break_class_of(TR_OBJECT_REPLACEMENT) == LB_CLASS_CB,
           "U+FFFC OBJECT REPLACEMENT CHARACTER's [UAX14] Line_Break class is not CB in "
           "core/layout/line_break_class.c, so LB20 \"Break before and after unresolved CB\" no longer puts "
           "css-text-3 §5.5's soft wrap opportunity before and after this box. That sentence is what this item "
           "kind IS, and the code point is how it is expressed to the annex — a different class here means the "
           "generated table and this constant have come apart, and the run would break around an image "
           "according to whatever rule that other class reaches");
    it = tr_append_item(m, TEXT_RUN_ITEM_ATOMIC, style);
    it->cp = TR_OBJECT_REPLACEMENT;
    it->size = size;
    /* AN ATOMIC INLINE CLOSES A COLLAPSIBLE WHITE-SPACE RUN, and unlike the forced break's flag this one is
       OBSERVABLE. css-text-3 §4.1.1 step 4 collapses "any collapsible space IMMEDIATELY FOLLOWING another
       collapsible space", and this box is a character between them in §4.1.1's sense — it renders — so the
       spaces on its two sides are two runs and each keeps its own surviving U+0020. `<div>a <img> b</div>`
       therefore has a space on each side of the image, which is what every user agent renders, and leaving the
       flag set as an EDGE does would silently delete the second one. */
    m->in_white_space_run = false;
}

void text_run_measure_add_text(TextRunMeasure *m, lxb_dom_element_t *style, const lxb_dom_node_t *text)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)text;
    const lxb_char_t *p, *end;
    bool wraps;

    DCHECK(m != NULL && style != NULL && text != NULL,
           "a text run was measured with no accumulator, no style element or no node");
    DCHECK(!m->finished, "a text node was added to a measurement that has already produced its two answers. "
                         "text_run_measure_finish releases the collection those answers were derived from, so "
                         "this is one accumulator being used for two runs");
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
    /* THE `white-space` GROUP IS ASKED ONCE PER NODE AND NOT ONCE PER WHITE-SPACE CHARACTER, which is the
       difference between a contract and an accident. Asking it only where a space appears would answer
       correctly for a `white-space: pre` run that happens to contain none — the four Phase I steps and every
       [UAX14] rule agree on such a run — and that correctness is a property of the STRING rather than of this
       component, so it would come and go with the document. The value is a fact about the inline, so it is
       validated where the inline is. */
    wraps = tr_wraps(style);
    /* An empty text node — DOM §4.10's `data` may be the empty string — has no character to process, and
       §4.1.1's steps are all stated over characters. It is not a case to skip past: it genuinely contributes
       nothing and leaves the collapsing state exactly as it was, which is what keeps a collapsible run split
       across it collapsing as ONE run. */
    if (cd->data.data == NULL || cd->data.length == 0) return;
    p = cd->data.data;
    end = p + cd->data.length;
    while (p < end) {
        uint32_t cp = (uint32_t)lxb_encoding_decode_valid_utf_8_single(&p, end);

        /* THE DECODER IS LEXBOR'S — CLAUDE.md's bind-before-build over a hand-rolled fourth UTF-8 walk in this
           tree. Its contract is that the bytes are well formed, and a DOM string's are: lexbor's parser
           replaces malformed input and quickjs's encoder emits the three-byte form even for an unpaired
           surrogate, which decodes to that code point and is stopped by the advance measure's own scalar-value
           assert rather than being measured. */
        DCHECK(cp != (uint32_t)LXB_ENCODING_DECODE_ERROR,
               "a DOM string held bytes that are not well-formed UTF-8. Lexbor's parser replaces malformed "
               "input and every other writer of character data in this engine goes through quickjs's own "
               "encoder, so a tree containing one was built by something that did neither");
        if (tr_is_collapsible_white_space(cp)) {
            /* §4.1.1's ONE SURVIVING SPACE of a run, opened at the run's FIRST collapsible character and not
               re-opened by the ones after it — step 4's own sentence ("any collapsible space immediately
               following another collapsible space … is collapsed to have zero advance width"), taken as a
               property of the run rather than as a per-character subtraction. The character recorded is
               U+0020, which is what every path through §4.1.1 leaves: step 3 turns a tab into one, §4.1.3 turns
               the surviving segment break into one, and a CR is one "in all respects" (§4). It carries the
               inline of the FIRST white-space character, which is the box css-text-3 §5.5 says controls the
               break at the opportunity this space creates. */
            if (!m->in_white_space_run) {
                tr_append(m, 0x0020, style, wraps, true);
                m->in_white_space_run = true;
            }
            continue;
        }
        m->in_white_space_run = false;
        tr_append(m, cp, style, wraps, false);
    }
}

/* ---- the measurement ---------------------------------------------------------------------------------------- */

/* HOW FAR ALONG THE LINE `[lo, hi)` ITEM `upto` BEGINS — the sum of the advance measures and box edges of
   `[lo, upto)`, less css-text-3 §4.1.2 "Phase II: Trimming and Positioning"'s edges: "a sequence of collapsible
   spaces at the beginning of a line is removed", "a sequence of collapsible spaces at the end of a line is
   removed". The trimming is a SEQUENCE in the spec and a loop here even though §4.1.1 leaves at most one such
   space per run, because the two statements are independent: the day preserved white space is built, a line can
   begin with several.
   THE TRIM RANGE IS THE WHOLE LINE'S AND THE SUM IS THE PREFIX'S, WHICH IS THE ONLY WAY A POSITION AND A SIZE
   CAN AGREE. §4.1.2's two edges are properties of the LINE — which spaces disappear depends on where the line
   ends, not on where the item being located sits — so a prefix that recomputed them over `[lo, upto)` would
   trim a space in the MIDDLE of the line as though it were at the end, and the offsets of a line's items would
   not add up to that line's own size. That is why this is one walk with a third bound rather than two
   functions: `upto == hi` is the SIZE and every smaller `upto` is a POSITION inside it, and the two cannot
   drift apart because there is nothing for them to drift between.
   `upto == hi` IS THEREFORE A REAL ANSWER AND NOT AN EDGE CASE — it is where the line's last item ENDS, which
   is what a caller reporting a fragment's far edge asks for. */
static CssPx tr_line_prefix(const TextRunMeasure *m, size_t lo, size_t hi, size_t upto)
{
    CssPx sum = css_px(0.0);
    size_t i, keep_lo = hi, keep_hi = lo;

    DCHECK(lo <= hi && hi <= m->count, "a line was measured over a range that is not inside the collected run");
    DCHECK(lo <= upto && upto <= hi,
           "css-text-3 §4.1.2's per-line sum was asked for a prefix that ends outside the line it is a prefix "
           "of. The trim range below is the LINE's, so a bound past `hi` would sum items §4.1.2 never looked at "
           "and a bound before `lo` would sum a negative span");
    /* §4.1.2's TWO ENDS ARE FOUND OVER THE ITEMS THAT ARE CONTENT, which `tr_item_stops_trim` decides and which
       an inline box EDGE is not. The section removes "a sequence of collapsible spaces at the BEGINNING of a
       line" and again at the end, and a box boundary is not a space — an inline box opened before a line's
       leading space leaves that space at the beginning of the line, so a scan that halted on the boundary would
       keep a space every user agent drops. An ATOMIC INLINE is on the other side of that test for the same
       reason read the other way: it renders, so a space beyond it is not at the line's edge at all.
       The two ends are found first and nothing is moved, because `lo` and `hi` still delimit the ITEMS whose
       edges are summed: trimming by advancing the range would drop an edge that sits outside the surviving
       text, which is precisely the case an empty inline box at a line's start is. */
    for (i = lo; i < hi; i++)
        if (tr_item_stops_trim(&m->items[i])) { keep_lo = i; break; }
    for (i = hi; i > lo; i--)
        if (tr_item_stops_trim(&m->items[i - 1])) { keep_hi = i; break; }
    for (i = lo; i < upto; i++) {
        CssPx advance;

        /* THE TWO SIZED KINDS ARE SUMMED WHEREVER THEY LAND and neither is subject to §4.1.2's trim, which
           `tr_item_stops_trim` above is the other half of: the section removes a sequence of COLLAPSIBLE SPACES
           at a line's two ends, and neither an inline box boundary nor an atomic inline is one. An EDGE
           contributes css-sizing-3 §2.2's outer size at one boundary of its box and an ATOMIC INLINE the whole
           of CSS 2 §8.1's margin box, which is CSS 2.2 §9.4.2's "horizontal margins, borders, and padding are
           respected between these boxes" for a box the section makes "a single opaque box". */
        if (tr_kind_has_size(m->items[i].kind)) {
            sum = css_px_add(sum, tr_sized_at(m, i)->size);
            continue;
        }
        /* A FORCED BREAK OCCUPIES A POSITION AND NO WIDTH. The U+000A it carries exists for [UAX14] alone — see
           text_run.h — and css-values-4 §6.1.1's advance measure of it would be the first available font's
           .notdef advance, a real number for a box that draws nothing. It is skipped BEFORE §4.1.2's trim test
           because it is outside that test's range by construction: the two ends are found over characters, so a
           break at a line's edge is neither kept nor removed, it is simply not one of the things being summed. */
        if (m->items[i].kind == TEXT_RUN_ITEM_FORCED_BREAK) continue;
        if (i < keep_lo || i >= keep_hi) continue;   /* §4.1.2 removed it from this line */
        advance = css_font_advance_measure_px(tr_char_at(m, i)->style, tr_char_at(m, i)->cp);
        DCHECK(advance.px >= 0.0,
               "the first available font reported a NEGATIVE advance measure for a character on a line. "
               "OpenType 'hmtx' — Horizontal Metrics Table's advanceWidth is a UFWORD, .notdef's included, and "
               "a computed `font-size` is non-negative — so this is a derivation that lost a sign and not a "
               "face");
        sum = css_px_add(sum, advance);
    }
    return sum;
}

/* ONE LINE'S INLINE SIZE — the prefix above taken to the line's own end, which is what CSS 2.2 §9.4.2's
   distribution compares against an available width. */
static CssPx tr_line_size(const TextRunMeasure *m, size_t lo, size_t hi)
{
    return tr_line_prefix(m, lo, hi, hi);
}

/* IS THE SOFT WRAP OPPORTUNITY AT THIS BOUNDARY ENABLED — css-text-3 §5.5 "Line Breaking Details" states the
   question as two cases and this answers both:
     "For soft wrap opportunities created by characters that DISAPPEAR AT THE LINE BREAK (e.g. U+0020 SPACE),
      properties on the box directly containing that character control the line breaking at that opportunity."
      [UAX14] LB18 `SP ÷` puts the opportunity after the space and §4.1.2 is what makes it disappear, so the
      space to the LEFT is that character.
     "For soft wrap opportunities defined by the boundary between two characters …, the white-space property on
      the NEAREST COMMON ANCESTOR of the two characters controls breaking."
   The second is answered by the walk below. */

/* THE NEAREST COMMON ANCESTOR OF THE TWO INLINE BOXES A BOUNDARY FALLS BETWEEN — the element css-text-3 §5.5
   "Line Breaking Details" hands the decision to for "soft wrap opportunities defined by the boundary between
   two characters or atomic inlines".
   IT IS A WALK AND NOT A FIELD ON THE CHARACTER, because the question is asked of a PAIR and answered per
   BOUNDARY: an ancestor stored per character would be one element chosen without knowing which neighbour it
   would later be asked about, which is the wrong arity for the question rather than a cheaper form of it.
   THE ANSWER IS AN ELEMENT, AND THAT IS A THEOREM ABOUT THE CALLER rather than a case checked defensively.
   Both characters belong to ONE inline formatting context, and CSS 2.2 §9.4.2 says such a context "is
   established by a block container box" — one box, an element, containing both. So a common ancestor exists
   and the NEAREST one is at or below it. A Document, a fragment, or no common ancestor at all is two runs from
   two trees accumulated through one measurement, which is the accumulator's own contract broken upstream. */
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

/* NEITHER SIDE OF AN OPPORTUNITY CAN BE A FORCED BREAK, AND THE ASSERT BELOW IS WHERE THAT IS ENFORCED — a
   theorem of [UAX14] rather than a case this function declines to handle, and it is worth naming because the
   crash would otherwise read as an omission. HTML §15.3.4's newline is a U+000A, whose Line_Break class is LF:
   LB6 `× ( BK | CR | LF | NL )` makes the boundary BEFORE it PROHIBITED and LB5 `LF !` makes the boundary AFTER
   it MANDATORY, so no boundary touching one is ever LINE_BREAK_OPPORTUNITY and this function is never reached
   with one. A break item's `wraps` and `collapsible_space` are therefore never read, which is why
   `text_run_measure_add_forced_break` does not compute them.
   AN ATOMIC INLINE CAN BE ON EITHER SIDE AND THAT IS §5.5's OWN WORDING, not a widening: the section hands the
   decision to "the white-space property on the nearest common ancestor" for "soft wrap opportunities defined by
   the boundary between two CHARACTERS OR ATOMIC INLINES". So the ancestor walk is the answer for a boundary
   with an image on one side of it exactly as it is for one between two letters, and a `white-space: nowrap`
   container suppresses both. What an atomic inline can NEVER be is §5.5's first case — a character "that
   disappears at the line break" — since it renders, which is why that arm still asks only about a collapsible
   space and why an atomic's `collapsible_space` slot stays the false `tr_append_item` wrote. */
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
    /* §5.5's FIRST case, which is about ONE character rather than a boundary: "for soft wrap opportunities
       created by characters that disappear at the line break (e.g. U+0020 SPACE), properties on the box
       DIRECTLY CONTAINING THAT CHARACTER control the line breaking at that opportunity." */
    if (left->kind == TEXT_RUN_ITEM_CHAR && left->collapsible_space) return left->wraps;
    nca = tr_nearest_common_ancestor(left->style, right->style);
    wraps = tr_wraps(nca);
    /* TWO-SIDED, AND THE SIDE IT CHECKS IS THE COLLECTION'S. Where the two characters are in the SAME inline
       box that box IS the nearest common ancestor, so this fresh read of `white-space` must produce exactly
       the flag `tr_append` recorded when the character was collected. A disagreement is the cascade having
       answered differently for one element at two times — a per-flow computed value read outside the flow
       that collected it — which would silently move a break rather than fail.
       IT IS ASKED ONLY OF A CHARACTER, because only `tr_append` records a flag to compare against: an ATOMIC
       INLINE's `wraps` slot is the false `tr_append_item` wrote and stands for nothing. That is not a case
       excluded to keep the assert quiet — a boundary whose left side is an atomic inline cannot have that box
       as its nearest common ancestor at all, since core/layout/line_box.c does not descend into an atomic and
       so nothing inside one is ever the boundary's other side. */
    DCHECK(left->kind != TEXT_RUN_ITEM_CHAR || nca != left->style || wraps == left->wraps,
           "css-text-3 §5's `white-space` read for one element answered differently at collection time and at "
           "measurement time. The characters of an inline formatting context are collected and then measured "
           "within one flow, so the cascade cannot have changed between them; this is a computed value being "
           "read against a different flow's DOM than the one the run was collected from");
    return wraps;
}

/* THE ONE PLACE THE TWO INDEX SPACES MEET — a break position expressed over CODE POINTS, turned into the ITEM
   boundary that break puts a line's end at. [UAX14] is handed code points alone (css-text-3 §5.5: an inline box
   boundary must not create or forbid a break), so `actions[k]` is the boundary before the k-th CODE POINT, while
   a line is a contiguous run of ITEMS whose edges are summed into it.
   THE MAPPING IS "IMMEDIATELY AFTER THE ITEM HOLDING CODE POINT k-1", AND THE ALTERNATIVE IS WRONG BY EXAMPLE
   rather than by taste. Taking instead the item holding character k puts every edge between the two characters
   on the EARLIER line, and `<div>aaa <span>bbb</span></div>` breaking at its space then places the span's
   OPENING edge on the first line while all of its text is on the second — a fragment's border rendered on a
   line the fragment is not on. Mapping after character k-1 puts that opening edge on the second line with the
   content it opens, and it is equally right at the other end: in `<div>aaa<span></span> bbb</div>` the empty
   span precedes the space, so it stays on the first line where its position in the run puts it. The rule is
   one sentence, total over every position, and derived from the ORDER the caller emitted the items in — which
   is why `text_run_measure_add_box_edge` takes no side argument.
   `k == 0` IS THE START OF TEXT and maps to item boundary 0 — LB2 never breaks there, so this is the seed of
   the first line rather than a break, and there are no items before it to place. */
static size_t tr_item_boundary_of_cp(const size_t *item_of_cp, size_t k)
{
    return k == 0 ? 0 : item_of_cp[k - 1] + 1;
}

/* css-text-3 §5.5's ONE TAILORING OF [UAX14], applied ONCE over the single pass — the U+00A0 NO-BREAK SPACE
 * beside an atomic inline, which is the only boundary where the section and the annex's own answer differ.
 *
 * WHY THERE IS EXACTLY ONE, AND WHY THAT IS A DERIVATION RATHER THAN A SURVEY. §5.5 states the atomic inline's
 * two opportunities and their suppression in two consecutive sentences:
 *   "For Web-compatibility there is a soft wrap opportunity BEFORE AND AFTER each replaced element or other
 *    atomic inline, EVEN WHEN ADJACENT TO A CHARACTER THAT WOULD NORMALLY SUPPRESS THEM, INCLUDING U+00A0
 *    NO-BREAK SPACE."
 *   "However, WITH THE EXCEPTION OF U+00A0 NO-BREAK SPACE, there must be no soft wrap opportunity between
 *    atomic inlines and adjacent characters belonging to the Unicode GL, WJ, or ZWJ line breaking classes."
 * U+FFFC's class is CB and LB20 `÷ CB`, `CB ÷` is the first sentence; the second sentence is already what the
 * annex answers, because LB8a `ZWJ ×`, LB11 `× WJ`, `WJ ×`, LB12 `GL ×` and LB12a `[^SP BA HY] × GL` are all
 * EARLIER rules than LB20 and therefore win. The two statements agree everywhere except on U+00A0 itself,
 * which IS a GL character — so the annex suppresses the opportunity there and §5.5 requires it. That single
 * boundary is what this function writes, and nothing else.
 *
 * IT IS NOT "TWO RULE SETS OVER ONE RUN", which is the thing text_run.h forbids and the shape this most
 * resembles. The forbidden shape is an action WRITTEN HERE BESIDE actions the annex computed for the same
 * question — two deciders, free to disagree. This is a CSS-level tailoring the annex itself provides for
 * (UAX14 §8 "Customization") and css-text-3 makes normative, applied to the ONE array before any partition
 * reads it: after this call there is still exactly one answer for each boundary, and the max-content walk, the
 * min-content walk and CSS 2.2 §9.4.2's fill all read it.
 *
 * A MANDATORY ACTION IS NEVER OVERWRITTEN, and that is §5.5's own first bullet rather than caution: preserved
 * segment breaks and BK/NL characters "must be treated as FORCED line breaks", which is a stronger statement
 * than an opportunity, and [UAX14] LB3 makes the eot position mandatory as well. Downgrading one to an
 * opportunity would put a `br` beside an image on the same line. The loop's own bounds keep eot out of reach;
 * the test keeps the rest out. */
static void tr_atomic_nbsp_tailoring(TextRunMeasure *m, const uint32_t *cps)
{
    size_t i;

    for (i = 1; i < m->ncps; i++) {
        bool left_atomic = m->items[m->item_of_cp[i - 1]].kind == TEXT_RUN_ITEM_ATOMIC;
        bool right_atomic = m->items[m->item_of_cp[i]].kind == TEXT_RUN_ITEM_ATOMIC;
        uint32_t other = left_atomic ? cps[i] : cps[i - 1];

        if (left_atomic == right_atomic) continue;   /* neither side, or both — no adjacent character */
        if (other != 0x00A0) continue;
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
}

void text_run_measure_finish(TextRunMeasure *m)
{
    uint32_t *cps;
    size_t i, k = 0, max_line = 0, min_line = 0;

    DCHECK(m != NULL, "a text run measurement was finished with nothing to finish");
    DCHECK(!m->finished, "a text run measurement was finished twice — the second call would run [UAX14] over a "
                         "collection the first one released");
    m->finished = true;
    if (m->count == 0) {
        /* An inline formatting context with no items in it. css-sizing-3 §2.1's two sizes are both the size
           that "fits around its contents", and there are none — zero, and not a case this skipped. */
        DCHECK(m->items == NULL, "an empty run holds a collection it never appended to");
        return;
    }
    for (i = 0; i < m->count; i++)
        if (tr_kind_has_code_point(m->items[i].kind)) m->ncps++;
    if (m->ncps == 0) {
        /* A run of BOX EDGES AND NOTHING ELSE — an inline formatting context whose only content is empty inline
           boxes. [UAX14] is not run at all: css-text-3 §5.5's "inline box boundaries do not introduce a forced
           line break or soft wrap opportunity in the flow" means there is nowhere to break, so there is exactly
           ONE line and both of css-sizing-3 §2.1's answers are that line's size. This is core/layout/
           §9.4.2's own count reached from the inline-size side, and it is a THEOREM here for the same reason it
           is one there — independent of every width in the document. `splits` stays FALSE for exactly that
           statement, which is what tells the fill's caller it has no available width to derive. */
        m->max_content = tr_line_size(m, 0, m->count);
        m->min_content = m->max_content;
        return;
    }
    cps = malloc(m->ncps * sizeof *cps);
    m->actions = malloc((m->ncps + 1) * sizeof *m->actions);
    m->item_of_cp = malloc(m->ncps * sizeof *m->item_of_cp);
    CHECK(cps != NULL && m->actions != NULL && m->item_of_cp != NULL,
          "out of memory running [UAX14]'s line breaking over one inline formatting context. Every allocation "
          "is one entry per code point of the run — its own text, its forced breaks and its atomic inlines — "
          "so a failure here is the physical floor");
    for (i = 0; i < m->count; i++) {
        if (!tr_kind_has_code_point(m->items[i].kind)) continue;
        cps[k] = tr_code_point_at(m, i);
        m->item_of_cp[k] = i;
        k++;
    }
    DCHECK(k == m->ncps,
           "the second walk over the collection found a different number of CODE-POINT-BEARING items than the "
           "first one did, so the [UAX14] arrays are sized for a run that is not the one being copied into "
           "them. Nothing may append between the two walks — they are one statement in one function");
    line_break_actions(cps, m->ncps, m->actions);
    /* css-text-3 §5.5's OWN TAILORING, APPLIED TO THE ONE ARRAY BEFORE ANY PARTITION READS IT — see the
       function for which single boundary it is and for why it is not a second rule set. */
    tr_atomic_nbsp_tailoring(m, cps);
    /* THE TWO ANSWERS ARE THE SAME WALK OVER DIFFERENT LINES, which is css-sizing-3 §2.1's own construction:
       the sizes differ by "if NONE of the soft wrap opportunities were taken" against "if ALL" were. So the
       max-content lines are cut by FORCED breaks alone — CSS 2.2 §10.3.5's "without breaking lines other than
       where explicit line breaks occur" — and the min-content lines by every break there is. `actions[count]`
       is LB3's mandatory break at eot, so the last line of each answer is closed by the loop rather than after
       it, and there is no tail case to get wrong. */
    for (i = 1; i <= m->ncps; i++) {
        /* §4.1.2's line ends where this break puts it, in ITEM coordinates — see tr_item_boundary_of_cp for
           why the boundary is taken after code point `i-1` and not before code point `i`. At `i == ncps` the
           line runs to the END OF THE COLLECTION rather than to that boundary, because every trailing box edge
           after the last code point is still on the last line: LB3 closes the text, not the items. */
        size_t at = i == m->ncps ? m->count : tr_item_boundary_of_cp(m->item_of_cp, i);
        bool forced = m->actions[i] == LINE_BREAK_MANDATORY;
        bool soft = false;

        /* AT eot THERE IS NO RIGHT-HAND CODE POINT TO ASK ABOUT, and there is no question either: LB3 makes
           `actions[ncps]` MANDATORY, so the opportunity arm is unreachable there. Written as a guard rather
           than left to `&&`'s short-circuit because the index it protects is `item_of_cp[i]`, which is one
           past the end at that position — a reader must be able to see that it is never taken. */
        if (i < m->ncps && m->actions[i] == LINE_BREAK_OPPORTUNITY)
            soft = tr_opportunity_enabled(m, m->item_of_cp[i - 1], m->item_of_cp[i]);
        DCHECK(i < m->ncps || forced,
               "[UAX14] LB3 \"Always break at the end of text\" did not make the action at eot MANDATORY. "
               "core/layout/line_break.h states that the action at position `count` always is, and the walk "
               "below has no tail case precisely because of it");
        /* CSS 2.2 §9.4.2's theorem, recorded on the ONE pass that can see it: a position strictly inside the
           run at which the fill may divide it. The eot position is excluded by `i < ncps` because LB3's
           mandatory action there CLOSES the run — a fill that treated it as a division would report a run with
           nowhere to break as splittable and send its caller off to derive a width that cannot matter. */
        if (i < m->ncps && (forced || soft)) m->splits = true;
        if (forced && i < m->ncps) {
            LineBreakClass cls = line_break_class_of(cps[i - 1]);
            TextRunItemKind kind = m->items[m->item_of_cp[i - 1]].kind;

            /* WHICH SIDE OF THIS ASSERT AN ITEM IS ON IS DECIDED BY ITS KIND AND CHECKED AGAINST ITS CLASS,
               which is the whole point: the two are written by different components and a disagreement is one
               of them wrong. A CHARACTER of a collapsed run reaches a mandatory action only through BK or NL —
               css-text-3 §5.5 makes exactly those two forced "regardless of the white-space value", and the
               other two mandatory classes are CR and LF, both of which §4.1.1 collapses away before this
               measurement sees them (a CR because §4 makes it "treated identically to spaces (U+0020) in all
               respects", an LF because it is the segment break §4.1.3 transforms into one). A FORCED BREAK item
               reaches it through LF and only LF, because that is the code point HTML §15.3.4's `newline`
               names — so a break item whose class is anything else is `TR_HTML_NEWLINE` and
               core/layout/line_break_class.c having come apart. AN ATOMIC INLINE REACHES IT AT ALL, which is
               why the third arm is `false` rather than a class test: its code point is U+FFFC, class CB, and
               LB20 `÷ CB`, `CB ÷` is an OPPORTUNITY on both sides — no rule of the annex makes a boundary at a
               CB mandatory, so a mandatory action there is the rule chain having produced one. */
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
            m->max_content = css_px_max(m->max_content, tr_line_size(m, max_line, at));
            max_line = at;
        }
        if (forced || soft) {
            m->min_content = css_px_max(m->min_content, tr_line_size(m, min_line, at));
            min_line = at;
        }
    }
    DCHECK(max_line == m->count && min_line == m->count,
           "[UAX14] LB3 \"Always break at the end of text\" did not close the last line of one of the two "
           "answers over the whole ITEM collection, so a run's tail was collected and never measured. The last "
           "line runs to `count` rather than to a break position exactly so that a box edge after the final "
           "character is still on a line; a short answer here is that mapping and this walk disagreeing");
    /* THE CODE POINTS ARE THE ONE THING NO PARTITION READS AGAIN. [UAX14]'s answer for this run is `actions`,
       and every walk over it — the two above and CSS 2.2 §9.4.2's fill — asks the ACTION at a position and
       never re-derives it from a character, which is the whole reason there is one pass. An advance measure is
       read off the ITEM (`tr_line_size`), which carries the element the code point belongs to and which a bare
       code point could not answer for. */
    free(cps);
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
    free(m->items);
    free(m->actions);
    free(m->item_of_cp);
    m->items = NULL;
    m->actions = NULL;
    m->item_of_cp = NULL;
    m->count = 0;
    m->capacity = 0;
    m->ncps = 0;
}

/* THE ONE RELATION BETWEEN THE TWO ANSWERS, asserted where they are read rather than where they are written,
   because it is a statement about the PAIR. css-sizing-3 §2.1 defines the min-content inline size over the same
   content with MORE soft wrap opportunities taken, and every advance summed is non-negative — so each
   min-content line is a sub-run of some max-content line and cannot exceed the widest of them. A violation is
   this file's own arithmetic having gone wrong, which is exactly what a dev-only abort is for; CSS 2.2
   §10.3.5's `min(max(preferred minimum width, available width), preferred width)` and css-sizing-3 §2.1's
   `clamp(min-content, stretch-fit, max-content)` are the SAME function only while it holds, so a caller that
   took either spelling on trust would silently be running the other one's algorithm. */
static void tr_require_answers(const TextRunMeasure *m)
{
    DCHECK(m != NULL, "css-sizing-3 §2.1's intrinsic inline sizes were read from no measurement");
    DCHECK(m->finished,
           "css-sizing-3 §2.1's intrinsic inline sizes were read from a measurement that has not run [UAX14] "
           "over its characters yet. text_run_measure_finish is what produces them, and the fields before it "
           "hold the zero text_run_measure_init wrote — which is a plausible answer for a run with text in it "
           "and is why this is a crash rather than a default");
    DCHECK(!m->released,
           "a text run measurement was read AFTER `text_run_measure_release`. Every answer this component has "
           "— css-sizing-3 §2.1's two sizes and CSS 2.2 §9.4.2's line boxes — is derived from a collection "
           "that call returned to the allocator, so the numbers still sitting in the struct describe a run "
           "whose items are gone and the fill would walk freed memory. The measurement ENDS at the release");
    DCHECK(m->min_content.px <= m->max_content.px,
           "a text run's MIN-CONTENT inline size came out WIDER than its max-content inline size. css-sizing-3 "
           "§2.1 states the first as the size \"if ALL soft wrap opportunities within the box were taken\" and "
           "the second as the size \"if NONE\" were, over the same characters — so with every advance "
           "non-negative the first is a sub-run of the second. The two spellings of the shrink-to-fit formula "
           "agree only under this relation, so its failure would silently switch which algorithm runs");
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

bool text_run_measure_splits(const TextRunMeasure *m)
{
    tr_require_answers(m);
    return m->splits;
}

/* ---- CSS 2.2 §9.4.2's DISTRIBUTION ACROSS LINE BOXES ------------------------------------------------------
   "When several inline-level boxes cannot fit horizontally within a single line box, they are distributed among
   two or more vertically-stacked line boxes."
   THE ITERATION IS THE SAME ONE `text_run_measure_finish` RUNS, AND THAT IS THE POINT: the same positions, the
   same `actions`, the same `tr_opportunity_enabled` question about the same pair of inline boxes. What differs
   is only which of those positions is TAKEN — none of them for §2.1's max-content, all of them for its
   min-content, and here the ones a width forces.
   `prev` IS THE LAST POSITION THAT STILL FIT and it is what makes this greedy rather than optimal: when a
   position's line has grown past `available`, the break goes back to `prev` — the widest prefix that fitted —
   and the segment between them opens the next line. When there is no such prefix (`prev == start`, the very
   first segment of a line) §9.4.2's own sentence applies and NOTHING is emitted early: "if an inline box cannot
   be split … then the inline box OVERFLOWS the line box." That is why this loop needs no bound and cannot fail
   to terminate — every position either closes a line at a strictly greater boundary or leaves the line alone.
   EVERY LINE IS NON-EMPTY IN ITEMS AND THE LINES PARTITION `[0, count)`, which is what bounds the allocation
   at `count` exactly rather than by a guess: a line is closed at a boundary strictly greater than its start
   (`tr_item_boundary_of_cp` is strictly increasing in the code point index, and the last line closes at
   `count`), so there can be no more lines than there are items. */
size_t text_run_measure_fill(const TextRunMeasure *m, CssPx available, TextRunLine **lines)
{
    TextRunLine *out;
    size_t i, n = 0, start = 0, prev = 0;

    tr_require_answers(m);
    DCHECK(lines != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked for with nowhere to put them. The count alone is not the "
           "answer this entry has: a caller asks which line each BOX is on — §10.8's step 3 is two maxima over "
           "the boxes on ONE line — and a number cannot be asked that");
    if (m->count == 0) {
        /* §9.4.2: "line boxes are created AS NEEDED to hold inline-level content within an inline formatting
           context", and there is none. This is not the zero-height line box the section describes two
           sentences later — that one is a line box that exists and is treated as not existing; this is a
           formatting context with no line box in it at all. */
        DCHECK(m->items == NULL, "an empty run holds a collection it never appended to");
        *lines = NULL;
        return 0;
    }
    out = malloc(m->count * sizeof *out);
    CHECK(out != NULL, "out of memory distributing one inline formatting context's content across CSS 2.2 "
                       "§9.4.2's line boxes. The array is one entry per collected item and the lines partition "
                       "them, so a failure here is the physical floor");
    if (m->ncps == 0) {
        /* css-text-3 §5.5 "Line Breaking Details": "out-of-flow boxes and inline box boundaries do not
           introduce a forced line break or soft wrap opportunity in the flow." A run of box edges therefore
           has nowhere to break at any width, which is `splits` being false, and §9.4.2's overflow sentence
           puts all of it on ONE line box. */
        DCHECK(!m->splits, "a run with no CODE POINTS in it reported a break position, and css-text-3 §5.5 says "
                           "an inline box boundary is not one — so `finish` recorded a split from an item that "
                           "carries no code point for [UAX14] to have decided about. A forced break carries one "
                           "and so does an atomic inline, so a `br` or an `<img>` in this run would have been "
                           "counted and this arm not taken");
        out[0].from = 0;
        out[0].to = m->count;
        out[0].size = tr_line_size(m, 0, m->count);
        *lines = out;
        return 1;
    }
    for (i = 1; i <= m->ncps; i++) {
        size_t at = i == m->ncps ? m->count : tr_item_boundary_of_cp(m->item_of_cp, i);
        bool forced = m->actions[i] == LINE_BREAK_MANDATORY;
        bool soft = false;

        if (i < m->ncps && m->actions[i] == LINE_BREAK_OPPORTUNITY)
            soft = tr_opportunity_enabled(m, m->item_of_cp[i - 1], m->item_of_cp[i]);
        if (!forced && !soft) continue;
        DCHECK(at > prev, "CSS 2.2 §9.4.2's fill met two break positions at the SAME item boundary, so one of "
                          "them divides nothing. `tr_item_boundary_of_cp` is strictly increasing in the "
                          "code point index and the eot position maps past every item, so this is that mapping "
                          "having stopped being a function of the code point");
        /* "cannot fit horizontally within a single line box" — measured over the line as it WOULD be if this
           position were not taken, which is why the comparison is against `at` and the emission is at `prev`.
           `prev > start` is §9.4.2's cannot-be-split condition stated positively: below it there is no earlier
           position on this line to fall back to, and the section says the content overflows instead. */
        if (prev > start && tr_line_size(m, start, at).px > available.px) {
            DCHECK(n < m->count, "CSS 2.2 §9.4.2's fill produced more line boxes than the run has items, and "
                                 "the lines partition the items — so a line was emitted empty or a boundary "
                                 "went backwards");
            out[n].from = start;
            out[n].to = prev;
            out[n].size = tr_line_size(m, start, prev);
            n++;
            start = prev;
        }
        if (forced) {
            /* css-text-3 §5.5 makes this break MANDATORY "regardless of the white-space value", so the line
               ends here whether or not anything overflowed — and at `i == ncps` this is [UAX14] LB3 closing
               the run, which is what leaves no tail case below. */
            DCHECK(n < m->count, "CSS 2.2 §9.4.2's fill produced more line boxes than the run has items at a "
                                 "FORCED break, and the lines partition the items");
            out[n].from = start;
            out[n].to = at;
            out[n].size = tr_line_size(m, start, at);
            n++;
            start = at;
        }
        prev = at;
    }
    DCHECK(start == m->count,
           "[UAX14] LB3 \"Always break at the end of text\" did not close CSS 2.2 §9.4.2's last line box over "
           "the whole ITEM collection, so the run's tail is on no line at all. The last line runs to `count` "
           "rather than to a break position exactly so that a box edge after the final character is still on a "
           "line, and a caller reading these lines would place that box nowhere");
    DCHECK(n >= 1, "CSS 2.2 §9.4.2 produced NO line box for a run that has code points in it — \"line boxes are "
                   "created as needed to hold inline-level content\" and there is content");
    /* §9.4.2's OVERFLOW THEOREM, CLOSED HERE RATHER THAN TRUSTED. `text_run_measure_splits` tells a caller it
       need not derive an available width, and the only thing that makes that safe is this: with no position
       inside the run, the loop above can emit only at eot, so the answer is ONE line box and `available` was
       never compared against anything. A caller that skipped the derivation and got two lines would have been
       handed a layout computed against whatever it passed instead. */
    DCHECK(m->splits || n == 1,
           "CSS 2.2 §9.4.2's fill divided a run that `text_run_measure_splits` says has nowhere to be divided. "
           "The section's \"if an inline box cannot be split … then the inline box overflows the line box\" is "
           "what makes that predicate a theorem, and a caller trusting it has passed an available width it "
           "derived from nothing");
    *lines = out;
    return n;
}

/* CSS 2.2 §9.4.2's PER-ITEM POSITION ON ONE LINE BOX — see text_run.h for what the number is measured from and
   for why it is a distance from the LINE BOX's start edge and not a coordinate. */
CssPx text_run_measure_line_offset(const TextRunMeasure *m, TextRunLine line, size_t i)
{
    tr_require_answers(m);
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
    return tr_line_prefix(m, line.from, line.to, i);
}
