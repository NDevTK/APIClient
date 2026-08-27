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
              "LB4's BK reaches it under `normal` through U+000C FORM FEED. What is missing is (1) the "
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
    m->max_content = css_px(0.0);
    m->min_content = css_px(0.0);
    m->finished = false;
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

/* THE FIELDS OF ONE KIND, REACHED ONLY THROUGH A CHECK OF THAT KIND. Every read of `cp`, `wraps`,
   `collapsible_space` or `size` in this file goes through one of these, so a walk that lost track of which
   kind it is standing on aborts at the read rather than measuring an edge as a U+0000 or summing a
   character's uninitialised `size` into a line. */
static const TextRunItem *tr_char_at(const TextRunMeasure *m, size_t i)
{
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    DCHECK(m->items[i].kind == TEXT_RUN_ITEM_CHAR,
           "a TEXT item's fields were read from an item that is an inline box EDGE. css-text-3 §5.5 \"Line "
           "Breaking Details\" makes the two different things at the same kind of position — an edge is a width "
           "and carries no code point — so this is a walk that stopped distinguishing them, and the value it "
           "would have read is a zero this component wrote rather than anything the document contains");
    return &m->items[i];
}

static const TextRunItem *tr_edge_at(const TextRunMeasure *m, size_t i)
{
    DCHECK(i < m->count, "an item of a collected run was read past its end");
    DCHECK(m->items[i].kind == TEXT_RUN_ITEM_EDGE,
           "an EDGE item's inline size was read from an item that is a CHARACTER — a character's contribution "
           "is css-values-4 §6.1.1's advance measure of its own code point, which is a different quantity read "
           "a different way");
    return &m->items[i];
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

/* ONE LINE'S INLINE SIZE — the sum of `[lo, hi)`'s advance measures, less css-text-3 §4.1.2 "Phase II: Trimming
   and Positioning"'s edges: "a sequence of collapsible spaces at the beginning of a line is removed", "a
   sequence of collapsible spaces at the end of a line is removed". The trimming is a SEQUENCE in the spec and a
   loop here even though §4.1.1 leaves at most one such space per run, because the two statements are
   independent: the day preserved white space is built, a line can begin with several. */
static CssPx tr_line_size(const TextRunMeasure *m, size_t lo, size_t hi)
{
    CssPx sum = css_px(0.0);
    size_t i, keep_lo = hi, keep_hi = lo;

    DCHECK(lo <= hi && hi <= m->count, "a line was measured over a range that is not inside the collected run");
    /* §4.1.2's TWO EDGES ARE FOUND OVER THE CHARACTERS AND AN EDGE ITEM DOES NOT STOP THE SCAN. The section
       removes "a sequence of collapsible spaces at the BEGINNING of a line" and again at the end, and a box
       boundary is not a space — an inline box opened before a line's leading space leaves that space at the
       beginning of the line, so a scan that halted on the boundary would keep a space every user agent drops.
       The two ends are found first and nothing is moved, because `lo` and `hi` still delimit the ITEMS whose
       edges are summed: trimming by advancing the range would drop an edge that sits outside the surviving
       text, which is precisely the case an empty inline box at a line's start is. */
    for (i = lo; i < hi; i++)
        if (m->items[i].kind == TEXT_RUN_ITEM_CHAR && !m->items[i].collapsible_space) { keep_lo = i; break; }
    for (i = hi; i > lo; i--)
        if (m->items[i - 1].kind == TEXT_RUN_ITEM_CHAR && !m->items[i - 1].collapsible_space) { keep_hi = i; break; }
    for (i = lo; i < hi; i++) {
        CssPx advance;

        if (m->items[i].kind == TEXT_RUN_ITEM_EDGE) {
            sum = css_px_add(sum, tr_edge_at(m, i)->size);
            continue;
        }
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

static bool tr_opportunity_enabled(const TextRunMeasure *m, size_t left_item, size_t right_item)
{
    const TextRunItem *left = tr_char_at(m, left_item), *right = tr_char_at(m, right_item);
    lxb_dom_element_t *nca;
    bool wraps;

    DCHECK(!right->collapsible_space,
           "[UAX14] LB7 \"do not break before spaces or zero width space\" was asked to allow a break before a "
           "collapsible space. core/layout/line_break.c returns PROHIBITED there unconditionally, so this is "
           "the rule chain and this file disagreeing about which side of a space the opportunity is on");
    /* §5.5's FIRST case, which is about ONE character rather than a boundary: "for soft wrap opportunities
       created by characters that disappear at the line break (e.g. U+0020 SPACE), properties on the box
       DIRECTLY CONTAINING THAT CHARACTER control the line breaking at that opportunity." */
    if (left->collapsible_space) return left->wraps;
    nca = tr_nearest_common_ancestor(left->style, right->style);
    wraps = tr_wraps(nca);
    /* TWO-SIDED, AND THE SIDE IT CHECKS IS THE COLLECTION'S. Where the two characters are in the SAME inline
       box that box IS the nearest common ancestor, so this fresh read of `white-space` must produce exactly
       the flag `tr_append` recorded when the character was collected. A disagreement is the cascade having
       answered differently for one element at two times — a per-flow computed value read outside the flow
       that collected it — which would silently move a break rather than fail. */
    DCHECK(nca != left->style || wraps == left->wraps,
           "css-text-3 §5's `white-space` read for one element answered differently at collection time and at "
           "measurement time. The characters of an inline formatting context are collected and then measured "
           "within one flow, so the cascade cannot have changed between them; this is a computed value being "
           "read against a different flow's DOM than the one the run was collected from");
    return wraps;
}

/* THE ONE PLACE THE TWO INDEX SPACES MEET — a break position expressed over CHARACTERS, turned into the ITEM
   boundary that break puts a line's end at. [UAX14] is handed code points alone (css-text-3 §5.5: an inline box
   boundary must not create or forbid a break), so `actions[k]` is the boundary before the k-th CHARACTER, while
   a line is a contiguous run of ITEMS whose edges are summed into it.
   THE MAPPING IS "IMMEDIATELY AFTER THE ITEM HOLDING CHARACTER k-1", AND THE ALTERNATIVE IS WRONG BY EXAMPLE
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
static size_t tr_item_boundary_of_char(const size_t *item_of_char, size_t k)
{
    return k == 0 ? 0 : item_of_char[k - 1] + 1;
}

void text_run_measure_finish(TextRunMeasure *m)
{
    uint32_t *cps;
    LineBreakAction *actions;
    size_t *item_of_char;
    size_t i, nchars = 0, max_line = 0, min_line = 0;

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
        if (m->items[i].kind == TEXT_RUN_ITEM_CHAR) nchars++;
    if (nchars == 0) {
        /* A run of BOX EDGES AND NOTHING ELSE — an inline formatting context whose only content is empty inline
           boxes. [UAX14] is not run at all: css-text-3 §5.5's "inline box boundaries do not introduce a forced
           line break or soft wrap opportunity in the flow" means there is nowhere to break, so there is exactly
           ONE line and both of css-sizing-3 §2.1's answers are that line's size. This is core/layout/
           line_box.h's own one-line-box theorem reached from the inline-size side, and it is a THEOREM here for
           the same reason it is one there — independent of every width in the document. */
        m->max_content = tr_line_size(m, 0, m->count);
        m->min_content = m->max_content;
        free(m->items);
        m->items = NULL;
        m->count = 0;
        m->capacity = 0;
        return;
    }
    cps = malloc(nchars * sizeof *cps);
    actions = malloc((nchars + 1) * sizeof *actions);
    item_of_char = malloc(nchars * sizeof *item_of_char);
    CHECK(cps != NULL && actions != NULL && item_of_char != NULL,
          "out of memory running [UAX14]'s line breaking over one inline formatting context. Every allocation "
          "is one entry per character of the run's own text, so a failure here is the physical floor");
    nchars = 0;
    for (i = 0; i < m->count; i++) {
        if (m->items[i].kind != TEXT_RUN_ITEM_CHAR) continue;
        cps[nchars] = m->items[i].cp;
        item_of_char[nchars] = i;
        nchars++;
    }
    line_break_actions(cps, nchars, actions);
    /* THE TWO ANSWERS ARE THE SAME WALK OVER DIFFERENT LINES, which is css-sizing-3 §2.1's own construction:
       the sizes differ by "if NONE of the soft wrap opportunities were taken" against "if ALL" were. So the
       max-content lines are cut by FORCED breaks alone — CSS 2.2 §10.3.5's "without breaking lines other than
       where explicit line breaks occur" — and the min-content lines by every break there is. `actions[count]`
       is LB3's mandatory break at eot, so the last line of each answer is closed by the loop rather than after
       it, and there is no tail case to get wrong. */
    for (i = 1; i <= nchars; i++) {
        /* §4.1.2's line ends where this break puts it, in ITEM coordinates — see tr_item_boundary_of_char for
           why the boundary is taken after character `i-1` and not before character `i`. At `i == nchars` the
           line runs to the END OF THE COLLECTION rather than to that boundary, because every trailing box edge
           after the last character is still on the last line: LB3 closes the text, not the items. */
        size_t at = i == nchars ? m->count : tr_item_boundary_of_char(item_of_char, i);
        bool forced = actions[i] == LINE_BREAK_MANDATORY;
        bool soft = false;

        /* AT eot THERE IS NO RIGHT-HAND CHARACTER TO ASK ABOUT, and there is no question either: LB3 makes
           `actions[nchars]` MANDATORY, so the opportunity arm is unreachable there. Written as a guard rather
           than left to `&&`'s short-circuit because the index it protects is `item_of_char[i]`, which is one
           past the end at that position — a reader must be able to see that it is never taken. */
        if (i < nchars && actions[i] == LINE_BREAK_OPPORTUNITY)
            soft = tr_opportunity_enabled(m, item_of_char[i - 1], item_of_char[i]);
        DCHECK(i < nchars || forced,
               "[UAX14] LB3 \"Always break at the end of text\" did not make the action at eot MANDATORY. "
               "core/layout/line_break.h states that the action at position `count` always is, and the walk "
               "below has no tail case precisely because of it");
        if (forced && i < nchars) {
            LineBreakClass cls = line_break_class_of(cps[i - 1]);

            DCHECK(cls == LB_CLASS_BK || cls == LB_CLASS_NL,
                   "a FORCED line break inside a collapsed run came from a character whose [UAX14] class is "
                   "neither BK nor NL. css-text-3 §5.5 \"Line Breaking Details\" makes exactly those two forced "
                   "\"regardless of the white-space value\"; the other two mandatory classes are CR and LF, and "
                   "§4.1.1 collapses both of those away before this measurement sees them — a CR because §4 "
                   "makes it \"treated identically to spaces (U+0020) in all respects\", an LF because it is "
                   "the segment break §4.1.3 transforms into one. So this is Phase I's collapsible set and "
                   "[UAX14]'s classes having come apart");
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
    free(cps);
    free(actions);
    free(item_of_char);
    free(m->items);
    m->items = NULL;
    m->count = 0;
    m->capacity = 0;
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
