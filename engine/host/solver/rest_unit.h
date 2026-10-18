/* rest_unit.h — how many of a substrate's own items one rest point covers, decided in one place for every
 * pull-shaped component and owned by the scheduler rather than by the component that rests.
 *
 * A rest unit is a granularity, not a bound: it decides how often a flow offers to rest, never whether work
 * happens. Choosing it trades the cost of asking the scheduler (a step-machine yield, a frontier re-rank,
 * possibly a COW delta swap) against the delay before a due suspension is taken. Both belong to the
 * scheduler, so the answer lives here beside ENGINE_QUANTUM_WORK and components ask for it. The finest unit a
 * substrate offers is also a choice, and the expensive one: one scheduler round trip per item.
 *
 * HTML §7.5.2 "Loading HTML documents" and §7.5.4 "Loading text documents" name the loaders' unit in the
 * same sentence:
 *
 *     "Each task that the networking task source places on the task queue while fetching runs must then fill
 *      the parser's input byte stream with the fetched bytes and cause the HTML parser to perform the
 *      appropriate processing of the input stream."
 *
 * The unit is one task's bytes, and the rest point is between two tasks. §7.5.3 "Loading XML documents"
 * names none, so its unit is the XML grammar's own: one construct of XML §2.1 "Well-Formed XML Documents"'
 * [1] `document`, which is core/xml/xml_parse.h's step.
 *
 * Every unit satisfies two bounds.
 * (1) It does not grow with the response. A fraction of the response ("rest sixteen times per document") is
 *     drive-to-completion under another name. `rest_unit_items` takes only a kind — no length, document or
 *     handle — so such a unit is not expressible.
 * (2) One unit's work is small against ENGINE_QUANTUM_WORK. A coarser unit never misses a due suspension; it
 *     delays the offer by at most one unit, which cannot change which flow runs next while the unit is small
 *     against the slice. `rest_unit_begin`/`rest_unit_end` assert this on the engine work the unit retired in a
 *     dev build, and only for kinds whose answer is above 1: at 1 there is no smaller number, so an overrun is
 *     a fact about the document and the fix is to make that item a pull one layer down. A kind whose items
 *     charge no work is measured by nothing (core/timing/thread_work.h names which).
 *
 * Every answer is at least 1 (asserted), so no value of this policy turns a pull back into a call. */
#ifndef ENGINE_HOST_SOLVER_REST_UNIT_H
#define ENGINE_HOST_SOLVER_REST_UNIT_H

#include <stddef.h>

/* The item a rest unit is counted in. Each name says what one item is, because the cost differs per kind:
   four are items whose size the document chooses, where a multiplier multiplies a quantity the page picks.
   A new kind needs its number and its reason in rest_unit.c, whose switch warns (-Wswitch) and whose check
   fails at run time without it. */
typedef enum {
    /* One byte into an HTML parser's input byte stream, §7.5.2/§7.5.4's fill quoted above. Cost: linear in
       the unit, with HTML §13.2.5 "Tokenization", §13.2.6 "Tree construction" and solver/dom_cow.h's
       announcement of each node as the per-byte constant. A fill runs no page code because a Document's
       scripts are seeded after §13.2.7 "The end" (core/frame/navigable.c's navigable_seed_scripts); if script
       execution moves into the parse, the bracket's work assert is what reports the unbounded unit. */
    REST_UNIT_INPUT_BYTES = 0,
    /* One construct of XML §2.1 "Well-Formed XML Documents"' [1] `document`, core/xml/xml_parse.h's step,
       reached from HTML §7.5.3 "Loading XML documents". Cost: one production instance, whose size the document
       chooses ([14] `CharData` is as long as the response makes it). */
    REST_UNIT_XML_CONSTRUCTS,
    /* One top-level child removed from the partial tree a failed XML parse left, which §7.5.3 permits a user
       agent to replace with an inline report. Cost: one subtree, whose size the document chooses. */
    REST_UNIT_SUBTREE_REMOVALS,
    /* One layout question body open on the C stack above its job's drive (core/layout/layout_job.h). An ask
       made while the drive already hosts this many bodies is deferred and computed on a fresh C stack. Cost:
       one body, whose walk the document sizes. This number decides C stack per activation and how often a
       body stages its walk; the rest point a resting drive offers is counted by REST_UNIT_LAYOUT_BOXES. */
    REST_UNIT_LAYOUT_QUESTIONS,
    /* One box folded into a scrolling area, CSSOM View §2 "Terminology"'s extreme over "all of the element's
       descendants' boxes", walked by core/layout/scrolling_area.h's cursor: a descendant's margin edge, one
       inline formatting context, or one anonymous block box. core/dom/element_scrolling.c's scroll-into-view
       yields to the scheduler after each unit. Cost: one box's derivation, which lays out what it asks, so the
       document sizes it; bound (2) is asserted around each unit. */
    REST_UNIT_LAYOUT_DESCENDANTS,
    /* One box laid out under a resting drive in one activation (core/layout/layout_job.h): a question body
       opened, or an anonymous block box a body's stack walk lays out. Past this many an ask is deferred or the
       walk stages and suspends, and the step machine that owns the drive rests. Cost: one box's own work
       between its asks; bound (2) is asserted by the REST_UNIT_LAYOUT_DESCENDANTS bracket the drive runs
       inside. */
    REST_UNIT_LAYOUT_BOXES,
    /* One element examined under a resting drive by CSS 2.2 §9.5's search of a block formatting context for
       its first float (core/layout/block_flow.h), which is a walk of the whole context where it finds none.
       Past this many the search holds its place and the drive rests. Cost: one element's `display`,
       `position` and `float`, the first read of which builds its cascaded style; bound (2) is asserted by the
       REST_UNIT_LAYOUT_DESCENDANTS bracket. */
    REST_UNIT_LAYOUT_FLOAT_SCAN,
    /* One item of an inline formatting context's collection (core/layout/text_run.h: a character, a box edge,
       a forced break or an atomic inline) taken by a step of its fill under a resting drive
       (core/layout/line_box.h): css-sizing-3 §2.1's walk, CSS 2.2 §9.4.2's distribution or §10.8's line box
       extents. Past this many the fill is held and the drive rests. Cost: a few advance measures and one
       `white-space` read; bound (2) is asserted by the REST_UNIT_LAYOUT_DESCENDANTS bracket. */
    REST_UNIT_LAYOUT_LINE_ITEMS,
    /* One inline box's CSS 2.2 §10.8.1 metrics derived by a resting drive's line box walks (core/layout/
       line_box.h): a strut's `A'` and `D'` from the box's first available font and used `line-height`, or an
       atomic inline's split margin box. Counted apart from REST_UNIT_LAYOUT_LINE_ITEMS because one costs what a
       hundred of those do, and how many a line derives is the document's choice (a box per character).
       Cost: three font metric derivations or one memoized layout answer; bound (2) as above. */
    REST_UNIT_LAYOUT_LINE_METRICS
} RestUnitKind;

/* How many items of `kind` one rest point covers; at least 1 (asserted). It takes only the kind, which is
   how bound (1) is enforced: a caller cannot pass a length, so the answer cannot be a fraction of one. */
size_t rest_unit_items(RestUnitKind kind);

/* One unit's work, bracketed: bound (2), measured. `begin` latches the kind and the work counter's reading;
   `end` asserts the unit stayed under the scheduler's slice. Brackets do not nest, and an `end` needs an open
   `begin` (both asserted). They observe only: nothing branches on them, and both compile to nothing in
   release. A driver whose unit is fixed at 1 may leave its units unbracketed. */
void rest_unit_begin(RestUnitKind kind);
void rest_unit_end(void);

#endif
