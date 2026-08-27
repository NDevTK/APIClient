/* css-text-3 §4.1's white space processing and §5's line breaking, over the advance measures core/css/
   font_metrics.h reads off the first available font. See text_run.h for the two answers this produces, for why
   it is an accumulator, for the alphabet it admits and for what UAX14's ordered rules decide over it. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/encoding/decode.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
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
   wrap opportunity". Under `nowrap` it is not enabled, so the run has no break in it and its min-content size
   IS its max-content size; under `normal` it is. The value is read here rather than derived from the collapsing
   arm because §3 makes them independent — `pre-wrap` collapses nothing and wraps, `nowrap` collapses and does
   not — so one boolean standing for both would be right for two values and wrong for the other four.
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
              "answer to both of this component's questions, and none of them is the collapsed one. THREE "
              "THINGS ARE NEEDED and the first is the largest: §4.1.3 \"Segment Break Transformation Rules\" "
              "makes a preserved segment break a FORCED LINE BREAK (css-text-3 §5.5 \"Line Breaking Details\": "
              "\"preserved segment breaks … must be treated as forced line breaks\"), so the max-content size "
              "stops being one line and becomes the MAXIMUM over the pieces the forced breaks cut it into — "
              "CSS 2.2 §10.3.5's own \"formatting the content without breaking lines OTHER THAN WHERE EXPLICIT "
              "LINE BREAKS OCCUR\". The second is §4.1.2 \"Phase II: Trimming and Positioning\"'s preserved TAB, "
              "which is not an advance at all but \"a horizontal shift that lines up the start edge of the next "
              "glyph with the next TAB STOP\" at multiples of `tab-size` from the nearest block container "
              "ancestor's starting content edge — a position, so it cannot be summed and needs the run's origin. "
              "The third is `tab-size` itself, which core/css/css_computed_value.c derives no computed value "
              "for. BUILD the forced-break split first: it is the one that changes what MAX-CONTENT MEANS, and "
              "the other two are terms inside a piece of it");
    return normal;
}

/* THE THREE PROPERTIES THAT WOULD CHANGE THIS MEASUREMENT AND THAT THIS ENGINE HAS NO COMPUTED VALUE FOR.
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
           "letter-spacing, to the segment as well as to the whole, since it survives at a break the same way "
           "the trailing space does not) rather than deleting this assert");
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
   U+000C FORM FEED IS DELIBERATELY NOT HERE. §4's control-character rule covers it — "control characters
   (Unicode category Cc) OTHER THAN tabs (U+0009), line feeds (U+000A), carriage returns (U+000D) and sequences
   that form a segment break must be rendered as a VISIBLE GLYPH which the UA must synthesize" — so it is
   content and not white space, and [UAX14] additionally gives it line breaking class BK, which css-text-3 §5.5
   makes a FORCED LINE BREAK. It therefore crashes below as a character this component's alphabet does not
   admit, which is the right answer twice over. */
static bool tr_is_collapsible_white_space(uint32_t cp)
{
    return cp == 0x0020 || cp == 0x0009 || cp == 0x000A || cp == 0x000D;
}

/* ---- [UAX14]'s Line_Break PROPERTY, FOR THE ALPHABET THIS COMPONENT ADMITS ---------------------------------
   The values are LineBreak.txt's own, for the ASCII range, and nothing here derives them: AL is the letters
   plus the eleven ASCII symbols that file assigns AL, and NU is the digits. Every character outside the union
   crashes at the one place that asks, so a wrong break position cannot be produced by a character this file has
   never considered. */
static bool tr_is_uax14_al(uint32_t cp)
{
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           cp == '#' || cp == '&' || cp == '*' || cp == '<' || cp == '=' || cp == '>' || cp == '@' ||
           cp == '^' || cp == '_' || cp == '`' || cp == '~';
}

static bool tr_is_uax14_nu(uint32_t cp)
{
    return cp >= '0' && cp <= '9';
}

static void tr_require_known_break_class(uint32_t cp)
{
    if (tr_is_uax14_al(cp) || tr_is_uax14_nu(cp)) return;
    DFAIL("a character whose [UAX14] Line_Break property this engine does not know reached a line-breaking "
          "measurement, and css-text-3 §5.5 \"Line Breaking Details\" is what makes that a crash rather than a "
          "sum: it hands the rules to [UAX14] outright, and the Line_Break property is a table over EVERY "
          "Unicode scalar value that this engine ships no copy of. THE FAILURE MODE OF GUESSING IS INVISIBLE, "
          "which is why nothing here falls back: a break this component does not know about makes the "
          "min-content size TOO LARGE, makes CSS 2.2 §10.3.5's shrink-to-fit width too wide, and produces a "
          "used value nothing in the engine disagrees with. What this file admits is [UAX14] classes AL and NU "
          "over ASCII, plus SP, because over that alphabet UAX14's ordered rules decide EVERY adjacent pair "
          "(LB7, LB18, LB23, LB25's `NU ( SY | IS )* × NU`, LB28) and its LB31 \"break everywhere else\" is "
          "unreachable. Nearly every other character reaches it: `,` `.` `:` `;` are class IS, `-` is HY, `/` "
          "is SY, `!` `?` are EX, `(` `[` `{` are OP, `)` `]` are CP, `}` is CL, `\"` `'` are QU, `$` `+` `\\` "
          "are PR, `%` is PO, `|` is BA, and every non-ASCII letter is AL, HL, ID, CJ, SA or one of the Brahmic "
          "classes. BUILD the Line_Break property table — a generated file from "
          "https://www.unicode.org/Public/UCD/latest/ucd/LineBreak.txt, in the shape core/fonts/"
          "default_font_data.c has for the other generated table in this engine — and then UAX14 §6.1's pair "
          "table over it. THE TWO ARE ONE UNIT and the table alone answers nothing: the classes are only "
          "meaningful through the rules, and a table with a rule set that stops at AL and NU would be this same "
          "alphabet with more code in front of it");
}

/* ---- the measurement ---------------------------------------------------------------------------------------- */

void text_run_measure_init(TextRunMeasure *m)
{
    DCHECK(m != NULL, "a text run measurement was begun with nowhere to keep it");
    m->max_content = css_px(0.0);
    m->min_content = css_px(0.0);
    m->segment = css_px(0.0);
    m->pending_space = css_px(0.0);
    m->has_pending_space = false;
    m->pending_space_wraps = false;
    m->any_glyph = false;
}

/* §4.1.1's ONE SURVIVING SPACE of a run, opened at the run's FIRST collapsible character and not re-opened by
   the ones after it — which is step 4's own sentence ("any collapsible space immediately following another
   collapsible space … is collapsed to have zero advance width"), taken as a property of the run rather than as
   a per-character subtraction. The advance recorded is U+0020's in THIS inline, which is what every path
   through §4.1.1 leaves: step 3 turns a tab into one, §4.1.3 turns the surviving segment break into one, and a
   CR is one "in all respects" (§4). */
static void tr_open_space(TextRunMeasure *m, lxb_dom_element_t *style, bool wraps)
{
    if (m->has_pending_space) return;
    m->pending_space = css_font_advance_measure_px(style, 0x0020);
    DCHECK(m->pending_space.px >= 0.0,
           "the first available font reported a NEGATIVE advance for U+0020 SPACE. OpenType 'hmtx' — Horizontal "
           "Metrics Table's advanceWidth is a UFWORD and this element's computed `font-size` is non-negative by "
           "css-fonts-4 §2.5's grammar, so a negative product is one of the two operands having been derived "
           "rather than read");
    m->pending_space_wraps = wraps;
    m->has_pending_space = true;
}

/* ONE TYPOGRAPHIC CHARACTER UNIT placed on the line — the point at which §4.1.2's trimming is DECIDED for the
   white-space run before it, because "at the beginning of a line" and "at the end of a line" are both facts
   about whether anything follows. */
static void tr_place(TextRunMeasure *m, lxb_dom_element_t *style, uint32_t cp)
{
    CssPx advance = css_font_advance_measure_px(style, cp);

    DCHECK(advance.px >= 0.0,
           "the first available font reported a NEGATIVE advance measure for a character on a line. OpenType "
           "'hmtx' — Horizontal Metrics Table's advanceWidth is a UFWORD, .notdef's included, and a computed "
           "`font-size` is non-negative — so this is a derivation that lost a sign and not a face");
    if (m->has_pending_space) {
        /* §4.1.2 removes a collapsible run at the BEGINNING of a line, and this run is at one exactly when
           nothing has been placed yet — the same sentence therefore drops a leading run from both answers and
           keeps an interior one in the max-content answer. */
        if (m->any_glyph) {
            m->max_content = css_px_add(m->max_content, m->pending_space);
            /* [UAX14] LB18 "break after spaces": the opportunity is AFTER the run, so the segment that was open
               ends here and the space belongs to it — and §4.1.2's "a sequence of collapsible spaces at the END
               of a line is removed" is what keeps it out of the closed segment's width. LB7 "do not break
               before spaces" is why there is no opportunity on the other side of it and no second segment
               boundary to consider. */
            if (m->pending_space_wraps) {
                m->min_content = css_px_max(m->min_content, m->segment);
                m->segment = css_px(0.0);
            } else {
                /* css-text-3 §5: wrapping is not enabled in this inline, so there is no opportunity here and
                   the segment continues THROUGH the space, which is then part of it. */
                m->segment = css_px_add(m->segment, m->pending_space);
            }
        }
        m->has_pending_space = false;
    }
    m->max_content = css_px_add(m->max_content, advance);
    m->segment = css_px_add(m->segment, advance);
    /* The open segment is folded in at every character rather than at a close, so the two answers are readable
       after any number of additions — including a run with no soft wrap opportunity in it at all, where the
       segment is never closed and a fold-at-close would report zero. */
    m->min_content = css_px_max(m->min_content, m->segment);
    m->any_glyph = true;
}

void text_run_measure_add_text(TextRunMeasure *m, lxb_dom_element_t *style, const lxb_dom_node_t *text)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)text;
    const lxb_char_t *p, *end;
    bool wraps;

    DCHECK(m != NULL && style != NULL && text != NULL,
           "a text run was measured with no accumulator, no style element or no node");
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
    /* THE `white-space` GROUP IS ASKED ONCE PER NODE AND NOT ONCE PER WHITE-SPACE CHARACTER, which is the
       difference between a contract and an accident. Asking it only where a space appears would answer
       correctly for a `white-space: pre` run that happens to contain none — the four Phase I steps and every
       [UAX14] rule agree on such a run — and that correctness is a property of the STRING rather than of this
       component, so it would come and go with the document. The value is a fact about the inline, so it is
       validated where the inline is. */
    wraps = tr_wraps(style);
    /* An empty text node — DOM §4.10's `data` may be the empty string — has no character to process, and
       §4.1.1's steps are all stated over characters. It is not a case to skip past: it genuinely contributes
       nothing and leaves the pending state exactly as it was, which is what keeps a collapsible run split
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
        if (tr_is_collapsible_white_space(cp)) { tr_open_space(m, style, wraps); continue; }
        tr_require_known_break_class(cp);
        tr_place(m, style, cp);
    }
}

CssPx text_run_measure_max_content(const TextRunMeasure *m)
{
    DCHECK(m != NULL, "css-sizing-3 §2.1's max-content inline size was read from no measurement");
    /* THE ONE RELATION BETWEEN THE TWO ANSWERS, asserted where they are read rather than where they are
       written, because it is a statement about the PAIR. css-sizing-3 §2.1 defines the min-content inline size
       over the same content with MORE soft wrap opportunities taken, and every advance summed below is
       non-negative — so the widest segment is a sub-run of the whole line and cannot exceed it. A violation is
       this file's own arithmetic having gone wrong, which is exactly what a dev-only abort is for; CSS 2.2
       §10.3.5's `min(max(preferred minimum width, available width), preferred width)` and css-sizing-3 §2.1's
       `clamp(min-content, stretch-fit, max-content)` are the SAME function only while it holds, so a caller
       that took either spelling on trust would silently be running the other one's algorithm. */
    DCHECK(m->min_content.px <= m->max_content.px,
           "a text run's MIN-CONTENT inline size came out WIDER than its max-content inline size. css-sizing-3 "
           "§2.1 states the first as the size \"if ALL soft wrap opportunities within the box were taken\" and "
           "the second as the size \"if NONE\" were, over the same characters — so with every advance "
           "non-negative the first is a sub-run of the second. The two spellings of the shrink-to-fit formula "
           "agree only under this relation, so its failure would silently switch which algorithm runs");
    return m->max_content;
}

CssPx text_run_measure_min_content(const TextRunMeasure *m)
{
    DCHECK(m != NULL, "css-sizing-3 §2.1's min-content inline size was read from no measurement");
    DCHECK(m->min_content.px <= m->max_content.px,
           "a text run's MIN-CONTENT inline size came out WIDER than its max-content inline size — see "
           "text_run_measure_max_content, which asserts the same relation for the same reason");
    return m->min_content;
}
