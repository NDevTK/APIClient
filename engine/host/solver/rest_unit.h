/* rest_unit.h — HOW MUCH OF A SUBSTRATE'S OWN ITEMS ONE REST POINT COVERS, decided in ONE place for every
 * pull-shaped component in the engine, and owned by the scheduler rather than by the component that rests.
 *
 * WHAT A REST UNIT IS, AND WHY IT IS NOT A BOUND. CLAUDE.md §NO BOUNDS: "a bound decides work will not happen;
 * a granularity decides only how often a flow OFFERS to rest, and an offer declined costs one predicate." A
 * component converted from one completing call into a pull has to answer a question the call never asked —
 * how much work is one item? — and the answer is a POLICY, because it trades the cost of asking the scheduler
 * against the delay before a due suspension is taken. Neither side of that trade is a fact the component
 * knows: the cost of asking belongs to the scheduler (a step-machine yield, a frontier re-rank, possibly a COW
 * delta swap) and so does the delay that may be tolerated (the slice a flow is already allowed to hold,
 * ENGINE_QUANTUM_MS). So the answer lives here, beside the slice it is reasoned against, and the components
 * ASK.
 *
 * TAKING THE FINEST UNIT THE SUBSTRATE OFFERS IS STILL A CHOICE, AND IT IS THE EXPENSIVE ONE. HTML §7.5.2
 * "Loading HTML documents" and §7.5.4 "Loading text documents" were converted to a pull that rested after ONE
 * BYTE of the response, on the stated grounds that a byte is the finest unit lexbor offers and so needs no
 * chosen quantum to invent and defend. That reading treats "no chosen constant" as a virtue in itself; it is
 * not, it is a constant of 1 that nobody had to argue for. What it buys is one whole scheduler round trip per
 * byte of every document this engine loads — the frontier re-ranked, the step machine re-entered and the
 * flow's arm re-dispatched, per byte — for a suspension opportunity nothing can use at that resolution.
 *
 * AND THE STANDARD NAMES THE UNIT, so this is not an engine-shaped constant standing where a spec-shaped one
 * belongs. HTML §7.5.2 "Loading HTML documents" and §7.5.4 "Loading text documents" carry the SAME sentence,
 * word for word:
 *
 *     "Each task that the networking task source places on the task queue while fetching runs must then fill
 *      the parser's input byte stream with the fetched bytes and cause the HTML parser to perform the
 *      appropriate processing of the input stream."
 *
 * A TASK'S WORTH OF BYTES — a chunk — and the rest point is BETWEEN two of those tasks, because a task is what
 * the event loop can interleave other work between. A per-byte fill is not a stricter reading of that
 * sentence; it is a different algorithm, one in which the networking task source delivers one byte per task.
 *
 * §7.5.3 "Loading XML documents" HANDS OVER NO SUCH UNIT and that absence is a positive statement rather than
 * a gap to fill by analogy: it says only that "user agents must follow the requirements defined in XML and
 * Namespaces in XML, XML Media Types, DOM, and other relevant specifications", so the unit that section
 * implies is the XML grammar's own — one construct of XML §2.1 "Well-Formed XML Documents"' [1] `document`,
 * which is exactly what core/xml/xml_parse.h's step already performs. See REST_UNIT_XML_CONSTRUCTS for why
 * that one stays at one construct rather than being multiplied.
 *
 * ── THE TWO BOUNDS A UNIT MUST SATISFY ─────────────────────────────────────────────────────────────────────
 *
 * (1) IT MUST NOT GROW WITH THE RESPONSE, AND THAT IS ENFORCED BY THE SIGNATURE RATHER THAN ASSERTED.
 * CLAUDE.md §C-stack: "running no user code is not what makes a C span safe to leave un-parkable; being O(1)
 * is." The quantity that must not appear in a step's cost is the one the ATTACKER chooses — the response's
 * length. A per-byte step and a per-16-KiB step are both O(1) by that criterion and differ only in the
 * constant; the whole-document drive that both replaced is O(response) and is the thing the criterion excludes.
 * What a granularity may never be is a FRACTION of the response ("rest sixteen times per document"), because
 * that is O(response) wearing a granularity's name and would re-open the drive-to-completion on the one input
 * that matters. `rest_unit_items` is told a KIND and nothing else — no length, no document, no handle — so a
 * unit that grows with the response is not expressible through this interface at all.
 *
 * (2) ONE UNIT'S WORK MUST BE SMALL AGAINST THE SLICE, AND WHERE THE UNIT IS A CHOICE THAT IS ASSERTED.
 * A coarser unit cannot MISS a suspension that is due: the rest point is still offered at every unit boundary
 * and the resume is still byte-identical, so the only thing a coarser unit changes is WHEN the offer arrives.
 * The delay is at most ONE unit's work. That is acceptable exactly while one unit's work is small compared
 * with ENGINE_QUANTUM_MS — the slice the scheduler already permits a flow to hold without offering the thread
 * back at all — because a delay smaller than the interval the scheduler itself operates at cannot change which
 * flow runs next. That is the whole argument for the constant, and an argument about a runtime cost is worth
 * what the measurement behind it is worth, so it is MEASURED: `rest_unit_begin`/`rest_unit_end` bracket one
 * unit's work and assert the ceiling in a dev build, on consumed CPU (§Testing: "MEASURE THE THING THE
 * INVARIANT IS ABOUT — CPU actually consumed … never elapsed time"), and only where solver/quantum.h says the
 * reading IS CPU.
 *
 * THE CHECK IS GATED ON THE UNIT BEING A CHOICE, NOT ON A HAND-PICKED LIST OF KINDS. Where the answer is 1 the
 * substrate's own item is the unit, and a unit that overruns the slice is then a fact about the DOCUMENT
 * rather than about policy — there is no smaller number, so an assert naming "lower this unit" would name a
 * fix that does not exist, and the real response is to make that construct itself a pull one layer down. So
 * the bracket checks when `rest_unit_items(kind) > 1` and is silent otherwise, which follows the policy
 * automatically instead of following a list somebody has to remember to edit.
 *
 * WHAT IS NOT TUNABLE HERE IS WHETHER A SPAN CAN REST AT ALL. §NO BOUNDS again: that "is BFS versus
 * drive-to-completion, which is a different JavaScript and DOM engine rather than a setting". Every answer
 * below is at least 1, asserted, so there is no value of this policy that turns a pull back into a call.
 */
#ifndef ENGINE_HOST_SOLVER_REST_UNIT_H
#define ENGINE_HOST_SOLVER_REST_UNIT_H

#include <stddef.h>

/* THE ITEM A REST UNIT IS COUNTED IN. Each name says what ONE item is, because the cost bound differs per
   kind and a shared name would hide that: two of these are counted in items whose size the DOCUMENT chooses,
   and the multiplier a policy may safely apply is a different question for those than for the one whose item
   is a fixed amount of work. Adding a kind means adding its default and its cost sentence in rest_unit.c,
   which does not compile until both exist. */
typedef enum {
    /* ONE BYTE INTO AN HTML PARSER'S INPUT BYTE STREAM — HTML §7.5.2 "Loading HTML documents" and §7.5.4
       "Loading text documents"' fill, quoted in full above. COST: linear in the unit, with HTML §13.2.5
       "Tokenization", §13.2.6 "Tree construction" and solver/dom_cow.h's announcement of each node §13.2.6
       makes as the per-byte constant. NOTHING UNBOUNDED HIDES INSIDE ONE, and that is a fact about this engine
       and not about the standard: a real browser RUNS a `<script>` from inside the parse, where the fill's cost
       would be the page's own code and no constant would bound it. Here a Document's scripts are seeded after
       §13.2.7 "The end" (core/frame/navigable.c's navigable_seed_scripts), so a fill is parser work only. The
       day script execution moves into the parse this unit is no longer bounded by its byte count, and the
       bracket below is what will say so. */
    REST_UNIT_INPUT_BYTES = 0,
    /* ONE CONSTRUCT OF XML §2.1 "Well-Formed XML Documents"' [1] `document` — core/xml/xml_parse.h's step,
       which is what HTML §7.5.3 "Loading XML documents" reaches. COST: one production instance, whose SIZE the
       document chooses ([14] `CharData` is a run as long as the response makes it), so this is the case where
       a multiplier would multiply a quantity the attacker picks. */
    REST_UNIT_XML_CONSTRUCTS,
    /* ONE TOP-LEVEL CHILD REMOVED from the partial tree a failed XML parse left standing, which §7.5.3 permits
       a user agent to replace with an inline report. COST: one subtree, whose size the document chooses —
       the same shape as the construct above and for the same reason. */
    REST_UNIT_SUBTREE_REMOVALS
} RestUnitKind;

/* HOW MANY ITEMS OF `kind` ONE REST POINT COVERS. Always >= 1 (asserted): the offer to rest survives every
   value of this policy, which is the half that is architecture rather than setting.
   IT TAKES NOTHING BUT THE KIND, and that is the enforcement of bound (1) above rather than an economy of
   arguments — a caller cannot pass a length, so this cannot answer a fraction of one. */
size_t rest_unit_items(RestUnitKind kind);

/* ONE UNIT'S WORK, BRACKETED — bound (2) above, measured instead of assumed. `begin` latches the kind and the
   consumed-CPU reading; `end` asserts the unit did not overrun the scheduler's slice. They are two-sided: a
   `begin` with a bracket already open, or an `end` with none, is a driver that lost track of its own unit.
   NOTHING BRANCHES ON THEM AND NOTHING IS TRUNCATED BY THEM. They are a diagnostic in §Testing's sense — no
   work is dropped, reordered or skipped whatever they observe — and both compile to nothing in release, so a
   release build pays neither the clock reads nor the check.
   THE ABSENCE OF A BRACKET IS NOT A CRASH, because the two loader arms whose unit is fixed at 1 have nothing
   the check could tell them (see the head comment); what the pair asserts is only that whoever DOES bracket
   does it in pairs. */
void rest_unit_begin(RestUnitKind kind);
void rest_unit_end(void);

#endif
