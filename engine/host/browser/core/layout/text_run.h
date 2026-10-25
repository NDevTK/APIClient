/* The inline-axis measurement of a text run: css-text-3 §4.1 "The White Space Processing Rules" for which
 * characters remain, css-text-3 §5 "Line Breaking and Word Boundaries" (over core/layout/line_break.h's
 * [UAX14]) for where it may break, and css-values-4 §6.1.1's advance measure for each character's width.
 *
 * It answers css-sizing-3 §2.1 "Auto Box Sizes"' two sizes from one walk: max-content "if none of the soft wrap
 * opportunities within the box were taken" ("the 'preferred width' in CSS2.1§10.3.5") and min-content "if all
 * soft wrap opportunities within the box were taken" ("the 'preferred minimum width'").
 *
 * It is an accumulator over a formatting context, not one text node: §4.1.1 "Phase I: Collapsing and
 * Transformation" collapses spaces "even one outside the boundary of the inline containing that space, provided
 * both spaces are within the same inline formatting context", and `foo`/`bar` in adjacent nodes are one word.
 * It collects, then measures from `text_run_measure_finish_begin`, because [UAX14] reads forward past the boundary it
 * decides (see line_break.h); Blink likewise collects an inline formatting context's text. The collection and
 * the one [UAX14] pass live until `text_run_measure_release`; nothing is cached across that (per-flow state
 * solver/dom_cow.h does not swap). */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TEXT_RUN_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TEXT_RUN_H

/* Three partitions over one [UAX14] pass: §2.1's two sizes (no opportunities taken, all taken) and CSS 2.2
   §9.4.2 "Inline formatting contexts"' greedy fill against an available width ("when several inline-level boxes
   cannot fit horizontally within a single line box, they are distributed among two or more vertically-stacked
   line boxes", `text_run_fill_step`). The fill is not a generalisation of the other two, which need no width
   comparisons; all three read one `actions` array, so they cannot disagree about where the run may break.
   §9.4.2's overflow sentence ("If an inline box cannot be split … then the inline box overflows the line box")
   means a segment always goes on the current line, so the fill never invents a break, and a run with no break
   inside it is one line box at every width. `text_run_measure_splits` exports that, so a caller can skip
   deriving an available width; the fill asserts it. */

/* What Phase I leaves under `normal` or `nowrap` (every space, tab and segment break collapsible): one U+0020
   per maximal white-space run. §4.1.1 step 1 removes "any sequence of collapsible spaces and tabs immediately
   preceding or following a segment break"; §4.1.3 "Segment Break Transformation Rules" removes "any collapsible
   segment break immediately following another collapsible segment break" and turns the rest "into a space
   (U+0020) or removed depending on the context before and after the break"; step 3 makes tabs spaces; step 4
   gives "any collapsible space immediately following another collapsible space" zero advance.
   Phase II is per line: §4.1.2 "Phase II: Trimming and Positioning" removes collapsible spaces "at the beginning
   of a line" and "at the end of a line", so a line's inline size is its advances less its leading and trailing
   collapsible spaces. Max-content lines end at forced breaks only (CSS 2.2 §10.3.5: "without breaking lines
   other than where explicit line breaks occur"); min-content lines end at every break.
   Forced breaks are measured: "preserved segment breaks, and—regardless of the white-space value—any Unicode
   character with the BK and NL line breaking class, must be treated as forced line breaks" (§5.5); U+000C,
   U+000B (BK) and U+0085 (NL) are not collapsible under `normal`. Under `pre` every space and tab is measured
   and every segment break is a forced break; `pre-line` collapses as `normal` does and keeps its segment breaks;
   `pre-wrap` and `break-spaces` are refused (text_run.c's `tr_white_space`). */

/* §4.1.3's UA-defined choice takes the space branch ("in languages that use word separators, such as English
   and Korean, 'unbreaking' a line requires joining the two lines with a space"); the no-separator branch needs
   content-language tailoring through §5's controls (`word-break`, `line-break`, `overflow-wrap`, `hyphens`),
   none of which the cascade computes, which text_run.c asserts.
   Not measured, and asserted in text_run.c: css-text-3 §7.1 "Word Spacing: the word-spacing property", §7.2
   "Tracking: the letter-spacing property" and §2.1 "Case Transforms: the text-transform property", none of
   which the cascade computes; the assert fires when one gains a computed value. A character's advance gains
   its 'GPOS' pair kerning (css_font_kerning_px, which reads css-fonts-4 §6.3's `font-kerning`). Ligatures are
   absent because no installed face (core/fonts/installed_font.h) ships 'GSUB'. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/fonts/font_matching.h"
#include "core/layout/line_break.h"

/* The run is a sequence of items, four kinds of which only one is text.
 *
 * An edge is an inline box's horizontal margin, border and padding at one boundary (css-sizing-3 §2.2
 * "Intrinsic Size Contributions": "based on the outer size of the box"), a width at a position, since
 * "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the flow" (css-text-3
 * §5.5 "Line Breaking Details"). It has no code point, so [UAX14] never sees it, and it is not a run total,
 * since a box spanning three lines puts its edges on the first and third. The per-line sum walks items, the
 * break pass code points; text_run.c's `tr_item_boundary_of_cp` reconciles the two index spaces once.
 *
 * A forced break is HTML §15.3.4 "Phrasing content"'s `br { display-outside: newline; }`
 * (core/layout/phrasing_break.h): a break and no size, not an edge or a character (CSS 2.2 §9.4.2's zero-height
 * rule asks "line boxes that contain no text" separately from "and do not end with a preserved newline"). Its
 * code point is U+000A, since "in the case of HTML, newlines are normalized to line feed characters (U+000A)"
 * (css-text-3 §4 "White Space Processing & Control Characters") and §5.5 makes a preserved segment break forced;
 * [UAX14] LB6 `× ( BK | CR | LF | NL )` and LB5 `LF !` then decide its boundaries in the one pass. */
/* An atomic inline has a width and a code point: "for Web-compatibility there is a soft wrap opportunity before
   and after each replaced element or other atomic inline, even when adjacent to a character that would normally
   suppress them, including U+00A0 NO-BREAK SPACE" (§5.5), and CSS 2.2 §9.4.2 puts its margin box on the line.
   One item is the whole box ("a single opaque box", CSS 2.2 §9.2.2), for a replaced element or an
   `inline-block` alike; the caller derives the width (CSS 2.2 §10.3.2 or §10.3.9).
   Its code point is U+FFFC (class CB): LB20 `÷ CB`, `CB ÷` gives the two opportunities, and earlier rules (LB8a
   `ZWJ ×`, LB11 `× WJ`, `WJ ×`, LB12 `GL ×`, LB12a `[^SP BA HY] × GL`) already forbid what §5.5 forbids ("with
   the exception of U+00A0 NO-BREAK SPACE, there must be no soft wrap opportunity between atomic inlines and
   adjacent characters belonging to the Unicode GL, WJ, or ZWJ line breaking classes"). U+00A0 is GL, so that
   exception is a stated tailoring of the one pass (text_run.c's `tr_atomic_nbsp_tailoring`). */
typedef enum {
    TEXT_RUN_ITEM_CHAR = 0,   /* one code point surviving css-text-3 §4.1.1's Phase I */
    TEXT_RUN_ITEM_EDGE,       /* an inline box boundary: an inline size, and no code point at all */
    TEXT_RUN_ITEM_FORCED_BREAK, /* HTML §15.3.4's `display-outside: newline`: a code point, and no size */
    TEXT_RUN_ITEM_ATOMIC,     /* css-text-3 §5.5's replaced element or other atomic inline: a size and a code
                                 point — CSS 2.2 §8.1 "Box dimensions"' margin box, put on the line by CSS 2.2
                                 §9.4.2 "Inline formatting contexts", and the U+FFFC whose [UAX14] class CB
                                 puts an opportunity on each side of it */
    TEXT_RUN_ITEM_FLOAT       /* CSS 2.2 §9.5's float at its place in the content: out of flow, so no size and
                                 no code point (css-text-3 §5.5: "out-of-flow boxes ... do not introduce a
                                 forced line break or soft wrap opportunity in the flow") */
} TextRunItemKind;

/* Which of css-sizing-3 §2.1 "Auto Box Sizes"' two constraints a per-line sum is taken under. Only an atomic
   inline differs: its own content wraps, so css-sizing-3 §5.2 "Intrinsic Contributions" gives it two
   contributions. An edge's lengths resolve identically for both ("For the min size properties, as well as for
   margins and paddings (and gutters), a cyclic percentage is resolved against zero for determining intrinsic
   size contributions", §5.2.1), so `text_run_measure_add_box_edge` takes one number and writes both.
   CSS 2.2 §9.4.2's fill is under neither: at a determined width every box has one used width, stated twice by
   its producer, and `text_run_fill_step` asserts the two agree rather than picking one. */
typedef enum {
    TEXT_RUN_SIZE_MIN_CONTENT = 0,
    TEXT_RUN_SIZE_MAX_CONTENT = 1,
} TextRunSizeBasis;

/* One item of the collected run. Its element travels with it because the run crosses elements and both
   properties read are per-element: css-values-4 §6.1.1's advance measure is over "the element on which it is
   used", and §5.5 says whose `white-space` governs a break ("properties on the box directly containing that
   character control the line breaking at that opportunity" for a disappearing character, otherwise "the
   white-space property on the nearest common ancestor of the two characters").
   Fields of another kind are never read: text_run.c reaches `cp`, `wraps` and `collapsible_space` only through
   accessors that DCHECK the kind. */
typedef struct {
    TextRunItemKind kind;
    lxb_dom_element_t *style;
    /* The code point [UAX14] decides this item's boundaries from: the character itself, a forced break's
       U+000A, or an atomic inline's U+FFFC (class CB). An edge has none and its slot is never read. */
    uint32_t cp;
    /* The installed face css-fonts-4 §5.2 renders a character with, chosen once at collection so the advance
       summed and the glyph painted are one face's. Only a character has one. */
    InstalledFont face;
    /* A character's advance measure in that face (css-values-4 §6.1.1), at its element's `font-size`, measured
       once at collection; a tab's is unused, since §4.2's tab stop is positioned where it lands. */
    CssPx advance;
    /* css-text-3 §3's `nowrap` "suppresses line breaks (text wrapping) within the source" while leaving white
       space collapsible, so whether an opportunity exists is a second per-element fact (§5.5). */
    bool wraps;
    /* Whether this character is §4.1.1's surviving collapsible space, which §4.1.2 trims at a line's ends; a
       U+0020 that is content under another `white-space` value is not, so the flag is carried. */
    bool collapsible_space;
    /* The inline size this item occupies on its line, one per `TextRunSizeBasis`. An edge holds §2.2's outer
       contribution at one boundary of an inline box; an atomic inline holds its whole margin box (CSS 2 §8.1).
       They share a slot because the per-line sum adds either at the item's position; the kind tells them apart.
       A character's advance is `advance`, not this; a forced break has none. Indexed so `tr_line_prefix` adds
       `size[basis]` for every sized kind without an arm per kind; which members differ is recorded on the
       measurement (`atomic_sizes_differ`). */
    CssPx size[2];
} TextRunItem;

/* One reading of a line sum: the sized items and closed shaping runs, and the run still open. A shaping run is
   one element's consecutive characters; its advances, each with css-fonts-4 §6.3's pair kerning, are snapped
   once where it ends. The kernings of `[kern_lo, end)` depend on what follows them, so they are read when the
   sum is. Held as sums (core/css/css_length.h), so a partial sum's form is interned only where it is read. */
typedef struct {
    CssPxSum closed;
    CssPxSum run;
    lxb_dom_element_t *run_style;   /* the open run's element; NULL when none is open */
    size_t kern_lo, end;            /* `end` is one past the last item this reading holds */
} TextRunLineEnd;

/* css-text-3 §4.1.2's per-line sum over `[lo, at)`, taken forward one item at a time so a walk that is still
   deciding where a line ends pays for each item once. It is the one summation of a line's items: the line
   breaking, the intrinsic sizes and every position on a line (`tr_line_prefix`) read it. Written and read only
   by text_run.c. */
typedef struct {
    size_t lo, at;
    bool   content;        /* an item §4.1.2 stops trimming at is in `[lo, at)` */
    TextRunLineEnd sum;    /* the sized items, and the characters from the first content item on */
    TextRunLineEnd size;   /* the line `[lo, at)`'s inline size: `sum` less the characters after the last content */
} TextRunLineSum;

/* The running state of one inline formatting context's measurement, written and read only through the entries
   below. */
typedef struct {
    /* §4.1.1's Phase I output interleaved with box edges, forced breaks and atomics, in document order.
       Owned; released by `text_run_measure_release`. */
    TextRunItem *items;
    size_t count, capacity;
    /* Whether the last character added was in a collapsible white-space run, so the next does not open a second
       surviving space (§4.1.1 step 4). It survives between nodes, so a run split across nodes collapses once. */
    bool in_white_space_run;
    /* [UAX14]'s action at every position of the run's code points (characters, forced breaks, atomics):
       `actions[k]` is the boundary before code point `k`, and `item_of_cp[k]` its item. Written by `finish`,
       which also applies §5.5's U+00A0 tailoring beside an atomic inline (`tr_atomic_nbsp_tailoring`), and
       retained until `release` so all three partitions read this one pass. Owned; NULL exactly while `ncps` is
       zero. */
    LineBreakAction *actions;
    size_t *item_of_cp;
    size_t ncps;
    /* css-sizing-3 §2.1's two answers, written by `finish` and undefined before it. A CssPx's double holds every
       multiple of 1/64 px exactly, so Blink's LayoutUnit answer is representable. */
    CssPx max_content;
    CssPx min_content;
    /* CSS 2.2 §9.4.2's theorem, written by `finish`: is there a forced break or enabled soft wrap opportunity
       strictly inside the run. LB3's end-of-text break closes the run and does not count, so this is false for a
       run that "overflows the line box". */
    bool splits;
    /* Whether any atomic inline carries two different sizes, set by `text_run_measure_add_atomic`. Such a run is
       an intrinsic measurement with no used widths, which `text_run_fill_step` and
       `text_run_measure_line_offset` refuse. Recorded at the append because the offset entry is called per box,
       and a scan there would be quadratic. */
    bool atomic_sizes_differ;
    /* The fallback list of the last character's inline box, so a run of characters in one box resolves its
       `font-family` once. Scoped to this measurement, during which no computed value moves. */
    lxb_dom_element_t *faces_of;
    FontFallbackList faces;
    /* The same box's advance operands (core/css/css_computed_value.h), held for the same reason. */
    lxb_dom_element_t *advance_of;
    CssFontAdvanceBasis advance_basis;
    /* The collection is closed (`text_run_measure_finish_begin`). */
    bool finished;
    /* Where the steps after the collection stand: counting the code points, copying them out, [UAX14]'s pass,
       §5.5's tailoring, then §2.1's walk. `cursor` is the phase's next item or position, `copied` the code
       points copied so far; text_run.c's `TrPrep` names the phases. */
    int prep;
    size_t cursor, copied;
    LineBreakPass breaks;
    /* §2.1's walk over the code point positions: the next one (from 1), the code points it classifies, and the
       open line of each of the two partitions. `cps` is owned and freed when the walk completes. */
    size_t walk;
    uint32_t *cps;
    TextRunLineSum max_line, min_line;
    /* The walk is complete, so the two answers and `splits` are written. */
    bool answered;
    /* `release` has run: the answers derive from freed memory, so reads assert this is false. */
    bool released;
} TextRunMeasure;

/* Begin a formatting context's measurement. Every field is written. */
void text_run_measure_init(TextRunMeasure *m);

/* Add one text node's characters, in tree order, one code point per `step`, held between them so a caller
   that rests can stop inside a node. Nothing else may be added to `m` until `done`. `style` is the text node's
   parent, the inline box they are in. Nodes of one formatting context must be added in document order (§4.1.1
   and [UAX14] are stated over adjacent characters). The caller has established that the run generates a box
   (CSS 2.2 §9.2.2.1 "Anonymous inline boxes", answered by core/layout/block_flow.h). */
typedef struct {
    lxb_dom_element_t *style;
    const lxb_char_t *p, *end;              /* the node's bytes still to decode */
    bool collapse, preserve_breaks, wraps;  /* css-text-3 §3's `white-space`, read once per node */
} TextRunText;

void text_run_measure_text_begin(TextRunMeasure *m, lxb_dom_element_t *style, const lxb_dom_node_t *text,
                                 TextRunText *t);
bool text_run_measure_text_done(const TextRunText *t);
void text_run_measure_text_step(TextRunMeasure *m, TextRunText *t);

/* Add one inline box boundary at the current position: css-sizing-3 §2.2's outer-size contribution for one
   side of `style`'s box, in CSS pixels. Called only at the box's two boundaries (§5.5). There is no side
   argument: the caller emits the opening edge before the content and the closing edge after, and the run's
   break mapping puts each on its line. `size` may be zero; a zero edge still has a position, which decides the
   line an otherwise-empty inline box is on. It may be negative (a declared negative margin, CSS 2.2 §8.3
   "Margin properties"); the measurement floors the two answers, so no sum is asked to be positive. */
void text_run_measure_add_box_edge(TextRunMeasure *m, lxb_dom_element_t *style, CssPx size);

/* Add one forced line break at the current position: HTML §15.3.4 "Phrasing content"'s `br { display-outside:
   newline; }` (core/layout/phrasing_break.h). `style` is the `br`, an inline-level box on the line whose
   `line-height` and first available font are CSS 2.2 §10.8 step 1 operands (`<br style="line-height:100px">`
   makes a 100px line). No code point argument: U+000A is fixed by the declaration. No advance: the break draws
   nothing, and measuring U+000A would add the font's .notdef width. */
void text_run_measure_add_forced_break(TextRunMeasure *m, lxb_dom_element_t *style);

/* Add one atomic inline at the current position: css-text-3 §5.5's "each replaced element or other atomic
   inline", sized by its margin box (CSS 2 §8.1 "Box dimensions": "the four margin edges define the box's margin
   box"), which CSS 2.2 §9.4.2 puts on the line. One item, not two edges: such boxes "participate in their inline
   formatting context as a single opaque box" (CSS 2.2 §9.2.2), and edges would place boundaries that introduce
   no break around a box that introduces two.
   The caller derives the sizes (CSS 2.2 §10.3.2 for a replaced element, §10.3.9 "'Inline-block', non-replaced
   elements in normal flow", or the box's module). Two numbers because its content wraps under §2.1's two
   constraints; a replaced box's are equal (css-sizing-3 §5.1), but that is a property of replaced elements. A
   caller with a used width (core/layout/line_box.c) states it twice, which `text_run_fill_step` asserts.
   No code point argument, as for a forced break. Either size may be zero (HTML §15.4.2's empty `img`) or
   negative (negative margins, CSS 2.2 §8.3 "Margin properties"); an inverted pair is refused, since
   css-sizing-3 §2.2 repairs it one level up ("the effective max-content contribution is floored by the
   min-content contribution"). */
void text_run_measure_add_atomic(TextRunMeasure *m, lxb_dom_element_t *style, CssPx min_content,
                                 CssPx max_content);

/* Add one float at the current position, `style` being the floated element: the fill places it at the line it
   falls on (core/layout/line_box.c). `max_content` is what it adds to its line's max-content size, zero for the
   fill and for a float below the line's top (core/layout/intrinsic_size.c); it adds nothing to a min-content
   segment, having no break opportunity around it. */
void text_run_measure_add_float(TextRunMeasure *m, lxb_dom_element_t *style, CssPx max_content);

/* Close the collection; nothing may be added afterwards. Each `step` then takes one item or position: the code
   points copied out, [UAX14]'s pass, §5.5's tailoring, and css-sizing-3 §2.1's walk; it answers how many items
   it took, and `done` says the two answers are readable. The pass is kept for the fill; each caller owes
   `text_run_measure_release` exactly once, complete or not. */
void text_run_measure_finish_begin(TextRunMeasure *m);
bool text_run_measure_finish_done(const TextRunMeasure *m);
size_t text_run_measure_finish_step(TextRunMeasure *m);

/* End the measurement and free the collection and the [UAX14] pass. Nothing may be read afterwards; the
   accessors crash rather than answer from freed memory. */
void text_run_measure_release(TextRunMeasure *m);

/* One line box of CSS 2.2 §9.4.2's distribution: the half-open range `[from, to)` of collected items on it and
   their inline size after css-text-3 §4.1.2 "Phase II: Trimming and Positioning" trims its ends. The range is
   over items because CSS 2.2 §10.8's step 3 is over the boxes on the line, and a box is on a line exactly when
   one of its items is. */
typedef struct {
    size_t from, to;
    CssPx size;
} TextRunLine;

/* CSS 2.2 §9.4.2's distribution of an answered run across line boxes at one available width ("the width of a
   line box is determined by a containing block and the presence of floats"), as a walk over the run's code point
   positions held between steps. `available` is the open line's width, the establishing box's used content width
   unless the caller narrows it beside floats between steps. It is ignored, provably and asserted, when
   `text_run_measure_splits` is false. Greedy, as §9.4.2 is: the widest prefix that fits, or one segment when
   nothing fits. The lines partition the items in order (a trailing box edge included), so every box has a line,
   as CSSOM View §6's `getClientRects()` step 3 needs. The run must outlive the fill. */
typedef struct {
    CssPx available;
    size_t walk;                  /* the next code point position, from 1 */
    size_t n, start, prev;        /* lines emitted; the open line's start; the last position taken */
    TextRunLineSum line;          /* `[start, at)` */
    TextRunLineSum at_prev;       /* `[start, prev)`, the open line as it stood at `prev` */
    TextRunLineSum seg;           /* `[prev, at)`, the line that opens at `prev` if this one overflows */
    TextRunLine *out;             /* owned until `text_run_fill_end` hands it over */
    bool complete;
} TextRunFill;

/* `begin` starts the walk; `step` takes one position and answers how many items it summed; `done` says every
   position is taken. `end` answers the line count and hands the newly allocated array to `*lines`, which the
   caller owns and frees. A fill abandoned before its end is released with `text_run_fill_release`. */
void text_run_fill_begin(TextRunFill *f, const TextRunMeasure *m, CssPx available);
bool text_run_fill_done(const TextRunFill *f);
size_t text_run_fill_step(TextRunFill *f, const TextRunMeasure *m);
size_t text_run_fill_end(TextRunFill *f, const TextRunMeasure *m, TextRunLine **lines);
void text_run_fill_release(TextRunFill *f);
/* Between steps: the lines emitted so far (`*lines`, borrowed until the next step or `end`), and the available
   width of the line now open, which §9.4.2 lets vary line by line ("they may vary in width if available
   horizontal space is reduced due to floats"). */
size_t text_run_fill_lines(const TextRunFill *f, const TextRunLine **lines);
void text_run_fill_set_available(TextRunFill *f, CssPx available);
/* The items the fill has taken so far: every item before this index is on an emitted or the open line. */
size_t text_run_fill_items_taken(const TextRunFill *f, const TextRunMeasure *m);

/* CSS 2.2 §9.4.2's theorem: is there anywhere inside this run for the fill to break. False means one line box
   at every available width, so a caller need not derive one. */
bool text_run_measure_splits(const TextRunMeasure *m);

/* Where item `i` begins along line box `line`: the inline distance from the line box's start edge to the item's
 * start, in CSS pixels, with §4.1.2's trimming applied over the whole line as in `TextRunLine.size`. It is a
 * distance, not a coordinate: where the line box's content starts is css-text-4 §7.3 "Default Text Alignment:
 * the text-align-all property"'s, a property of the establishing box this component is not given (CSS 2.2
 * §9.4.2: "in general, the left edge of a line box touches the left edge of its containing block and the right
 * edge touches the right edge of its containing block").
 * `i == line.to` is admitted and answers the line's size, so a fragment's two edges are this entry at its two
 * bounds. It shares one walk with `TextRunLine.size`, so offsets reach exactly the line's end. */
CssPx text_run_measure_line_offset(const TextRunMeasure *m, TextRunLine line, size_t i);

/* The element whose computed properties item `i` has: a character's inline box, or an edge's box. Every kind
   carries it (css-values-4 §6.1.1's advance is over "the element on which it is used"). The raw item is not
   exposed: every other field belongs to one kind, and the kind-checking accessors are what catch a walk that
   lost track of which kind it holds. */
lxb_dom_element_t *text_run_measure_item_style(const TextRunMeasure *m, size_t i);

/* Whether item `i` is a character. CSS 2.2 §9.4.2's zero-height rule opens "line boxes that contain no text";
   a forced break is not text for it (it is the next entry's conjunct). */
bool text_run_measure_item_is_text(const TextRunMeasure *m, size_t i);

/* Whether item `i` is a forced line break: §9.4.2's "and do not end with a preserved newline", asked of a
   line's last rendering item. Separate from the text question because the section states them separately;
   folding them would make `<div><br></div>` contain no line box, which CSS 2.2 §8.3.1's collapse-through note
   would read as a box its margins collapse through. */
bool text_run_measure_item_is_forced_break(const TextRunMeasure *m, size_t i);

/* Whether item `i` is an atomic inline: §9.4.2's "and no other in-flow content", so a line holding an `<img>`
   exists. It also chooses CSS 2.2 §10.8's step 1 height: "for replaced elements, inline-block elements, and
   inline-table elements, this is the height of their margin box; for inline boxes, this is their
   'line-height'." */
bool text_run_measure_item_is_atomic(const TextRunMeasure *m, size_t i);
bool text_run_measure_item_is_float(const TextRunMeasure *m, size_t i);

/* Item `i`'s code point, for the character kind only: what css-values-4 §6.1.1 "Font-relative Lengths…"
   measured an advance for, for a painter (CSS 2.1 §E.2 "Painting order"'s step 7.2.1, "the text", at
   `text_run_measure_line_offset`'s positions) that must not redo Phase I from the DOM.
   It refuses forced breaks (U+000A, which exists "to be given to [UAX14] and to nothing else") and atomics
   (U+FFFC, css-text-3 §5.5's "soft wrap opportunity before and after each replaced element or other atomic
   inline"): neither is a character the document contains, and drawing them would render .notdef for a `br`.
   A character the face lacks is still its code point; core/css/font_metrics.h answers .notdef for it. */
uint32_t text_run_measure_item_cp(const TextRunMeasure *m, size_t i);

/* Item `i`'s face, for the character kind only: the one its advance was measured in and its glyph is drawn
   from (core/fonts/font_matching.h). */
InstalledFont text_run_measure_item_face(const TextRunMeasure *m, size_t i);

/* css-sizing-3 §2.1's max-content and min-content inline sizes, in CSS pixels, read through entries that assert
   the relation between them. */
CssPx text_run_measure_max_content(const TextRunMeasure *m);
CssPx text_run_measure_min_content(const TextRunMeasure *m);

#endif
