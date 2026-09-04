/* CSS 2 §9.4.1 "Block formatting contexts" and CSS 2.1 §8.3.1 "Collapsing margins" — THE BOX TREE, which is
 * the one walk two members of this directory were each waiting on separately.
 *
 * ONE SUBPROBLEM, TWO ANSWERS, WHICH IS WHY THIS IS ONE COMPONENT AND NOT TWO. core/layout/flow_position.c's
 * non-root crash and core/layout/used_value.c's `height: auto` crash both named the SAME missing thing in the
 * same words — "the distance from its top content edge to the bottom margin edge of the last in-flow child"
 * (§10.6.3) and "boxes are laid out one after the other, vertically, beginning at the top of a containing
 * block" (§9.4.1) — because a box's y IS the running offset the height walk already computes and the height IS
 * where that offset ends up. Building them apart would be two walks over one child list that can disagree
 * about where a margin collapsed, so there is one walk and it answers both.
 *
 * §8.3.1 IS THE ALGORITHM, NOT A CORRECTION TO IT. "The vertical distance between two sibling boxes is
 * determined by the margin properties" is §9.4.1's whole placement rule, and the very next sentence is
 * "vertical margins between adjacent block-level boxes in a block formatting context collapse" — so a walk
 * that adds margins and then subtracts a collapse is a walk that had the rule wrong. What travels through the
 * walk is therefore not a number but §8.3.1's ADJOINING RUN: the set of margins that have met without a line
 * box, clearance, padding or border between them, whose collapsed width is "the maximum of the collapsing
 * margins' widths" with, "in the case of negative margins, the maximum of the absolute values of the negative
 * adjoining margins deducted from the maximum of the positive adjoining margins". That reduction is two
 * running maxima, so a run is two lengths and nothing else and it merges associatively — which is what lets the
 * SAME structure carry a run upward out of a child, downward out of a child, and through a child that
 * collapses through entirely.
 *
 * A RUN LEAVING THE BOX IS THE ONLY DIFFERENCE BETWEEN §10.6.3 AND §10.6.7, AND IT IS THE SPEC'S OWN.
 * §8.3.1's third and first adjoining pairs make a box's top margin adjoin its FIRST in-flow child's, and its
 * bottom margin adjoin its LAST in-flow child's — but only when nothing separates them, which is "no line
 * boxes, no clearance, no padding and no border", and only when the box does not establish a new block
 * formatting context (§8.3.1's own note: "margins of elements that establish new block formatting contexts …
 * do not collapse with their in-flow children"). When the run DOES escape, the child's top border edge is at
 * the box's own top content edge and the margin contributes nothing INSIDE the box — which is exactly
 * §10.6.3's "the bottom border edge of the last in-flow child whose top margin doesn't collapse with the
 * element's bottom margin" read from the other end. When it does NOT escape, the same walk measures from the
 * child's top MARGIN edge to the last child's bottom MARGIN edge, which is §10.6.7's rule verbatim. So the two
 * sections are one walk under two boolean flags, and the flags are a fact about the box (§9.4.1's list of
 * what establishes a formatting context, §8.3.1's list of what separates two margins), never a mode this
 * component picks.
 *
 * WHAT IS DELIBERATELY NOT BUILT, AND WHY EACH IS A CRASH AND NOT A ZERO. The smallest box model that answers
 * CSSOM VIEW §6 and §7 for the documents this engine parses is BLOCK FLOW: a block container whose in-flow
 * children are all block-level, over the used values core/layout/used_value.h already computes. Everything
 * else names its own section and aborts, because a zero from an unimplemented box passes a presence test and
 * is indistinguishable from a real one:
 *   - INLINE-LEVEL CONTENT is §9.4.2's inline formatting context and core/layout/line_box.h's, which this
 *     walk ROUTES TO rather than measures: §9.2.1 makes the two exclusive ("either contains only block-level
 *     boxes or establishes an inline formatting context"), so the choice is made once over the whole child
 *     list and each algorithm then sees only its own boxes. A container holding BOTH is not a third case and
 *     not an absence: §9.2.1.1 "Anonymous block boxes" says "if a block container box … has a block-level box
 *     inside it …, then we force it to have only block-level boxes inside it", so this walk iterates the BOX
 *     list that forcing produces — each maximal run of inline-level children wrapped in one anonymous block
 *     box whose height is §10.6.3's over line_box.h's line boxes and whose every other property is a constant
 *     the section fixes ("the margins will be 0"). That generation is a box-tree step and lives at the top of
 *     this walk, where the same list css-display-3 §2.5 "Box Generation: the none and contents keywords"'
 *     `contents` flattening will one day be spliced into.
 *   - A FLOAT in the formatting context is §9.5's own placement, and it is not enough to note that §10.6.3
 *     ignores floats: §9.5.2's `clear` on a LATER sibling introduces CLEARANCE, which §8.3.1 makes
 *     non-adjoining, so one float invalidates every collapse below it.
 *   - A FLEX or GRID container's height is its own spec's, not §10.6.3's walk, and a container whose children
 *     this walk placed would be a wrong number rather than an absent one.
 *   - A TABLE is CLASSIFIED and not measured, and the two halves are deliberately in different places. CSS 2.1
 *     §17.4 Tables in the visual formatting model puts a TABLE WRAPPER BOX on this stack — "the table
 *     generates a principal block box called the table wrapper box that contains the table box itself and any
 *     caption boxes" — and that box is block-level, establishes a block formatting context, and carries the
 *     table element's `margin-*`, so the CHILD CLASSIFICATION is answered here like any other block-level box
 *     (core/layout/table_wrapper.h owns §17.4's declaration split). Its HEIGHT is §10.6.3's over the caption
 *     boxes and the table box, and it is MEASURED in `bf_box` rather than walked, because with no caption that
 *     walk has a closed form: §17.4 gives the wrapper the INITIAL value of every non-inherited property and
 *     gives the table box the initial `margin-*`, so there is nothing to collapse and the wrapper's height is
 *     the table box's border-box height. A caption makes it a real walk and `bf_box` refuses it, naming the one
 *     number that walk still lacks. Read the site rather than this line — it is a header nothing re-checks, and
 *     it said the walk simply stopped here until the day §17.5.3 was built.
 *   - An OUT-OF-FLOW child is not a gap at all: §10.6.3 states outright that "absolutely positioned boxes are
 *     ignored", so skipping one is the rule running, and the box's own position is §9.3.2's over a static
 *     position that this walk is what will one day provide.
 *
 * NOTHING IS STORED, FOR used_value.h's REASON, RESTATED BECAUSE THIS COMPONENT IS WHERE IT WOULD FIRST BE
 * TEMPTING TO BREAK IT. A layout is per-flow state — two flows with different DOMs have different boxes — so a
 * cached box tree is shared state the COW delta does not swap, and a stale one is a geometry from another
 * flow's document. Every answer here is DERIVED PER READ from the running flow's own tree and its own cascade,
 * which makes it per-flow by construction with no capture to write. The cost is that a subtree is walked once
 * per question asked of it; the day that is the bottleneck the cache is per-flow state and needs
 * solver/dom_cow.h's capture at its accessor, exactly as a browser component's own C record does.
 *
 * A HEIGHT IS A `CssPx` FOR used_value.h's REASON AND THE PROPAGATION IS FREE. Every operand of the walk is
 * already a used value carrying the set of environment facts it derives from — a percentage margin resolves
 * against the containing block's WIDTH (§8.3, and "note that this is true for 'margin-top' and 'margin-bottom'
 * as well"), which bottoms out in the initial containing block; a border width carries the device pixel ratio.
 * So an auto height computed here is a joint function of whatever its children's margins and borders were
 * functions of, and css_length.h's arithmetic unions those sets without this file deciding anything. A box
 * whose whole subtree is author-pinned comes out with the empty set and is CONCRETE, which is the correct
 * answer and not a lost domain: viewport.h's test is whether the model PICKED a value out of a range the
 * environment leaves free, and a stack of declared heights is not that. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_BLOCK_FLOW_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_BLOCK_FLOW_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* CSS 2.1 §9.2.1's BLOCK CONTAINER BOX, decided from a computed `display` and from nothing else — the box
   §9.4.1's formatting context is stated over, and the box §10.1's second case looks for when it walks for a
   containing block. It is exported because BOTH of those callers ask it and the list is not "everything
   block-level": an `inline-block` is a block container and is not block-level, and a TABLE box is block-level
   and is not a block container (§17.4 makes the CELL and the CAPTION the block containers inside one). */
bool block_flow_display_is_block_container(const char *display);

/* CSS 2.2 §9.2.2.1 "Anonymous inline boxes"'s WHITE-SPACE RULE for one TEXT child of a block container:
   "White space content that would subsequently be collapsed away according to the 'white-space' property does
   not generate any anonymous inline boxes." FALSE is that sentence — a run this element's computed
   `white-space` collapses away, which is most of the character data in a pretty-printed document — and TRUE
   is a run that generates an anonymous inline box.
   IT CLASSIFIES AND DOES NOT MEASURE, which is the contract each caller then has to satisfy for itself. Both
   of them go on to ask core/layout/line_box.h about the run and they ask it DIFFERENT questions: this file's
   walk wants §10.6.3's distance down the line boxes it flows into, while CSSOM VIEW §2's scrolling area wants
   where the boxes ON those line boxes REACH. Folding either measurement into this predicate would put one
   caller's question inside the other's classification, and the classification is right for both.
   IT IS EXPORTED BECAUSE EVERY WALK OVER A BLOCK CONTAINER'S CHILDREN MUST ASK IT, and the scrolling area
   asks for a reason the height walk cannot cover — a box with a DECLARED height never reaches the walk at all
   (`bf_height_needs_content`), so its own text would be invisible to a caller that only measured heights, and
   a text run that overflows a declared-height box is exactly what `scrollHeight` is asked about. A second
   copy of §9.2.2.1 would be one rule with two answers about whether a page's white space is content. */
bool block_flow_text_child_generates_box(lxb_dom_element_t *parent, const lxb_dom_node_t *text);

/* THE CHARACTER HALF OF THE RULE ABOVE, ON ITS OWN: is every character of this text node one css-text-3 §4
   "White Space Processing Rules" gives to the `white-space` property — a space, a tab, a line feed or a
   carriage return — and therefore NOT U+000C FORM FEED, which that section makes a visible glyph and [UAX14]
   makes a forced break.
   IT IS SPLIT OUT BECAUSE TWO SECTIONS ASK IT AND ONLY ONE OF THEM READS A DECLARATION. CSS 2.2 §9.2.2.1
   "Anonymous inline boxes" removes such a run only where the `white-space` property "would subsequently"
   collapse it away, which is the predicate above; css-flexbox-1 §4 "Flex Items" removes a text sequence made
   only of these characters UNCONDITIONALLY — "if the entire text sequences contains only document white space
   characters (i.e. characters that can be affected by the white-space property) it is instead not rendered" —
   and its parenthesis is a definition of the CHARACTER SET rather than a condition on the property. So the two
   consumers share the set and disagree about the declaration, which is exactly the shape that must be one
   function and two callers: a second copy of the set is one document with two ideas of what its white space
   is, and the FORM FEED line above is the derivation that would drift. */
bool block_flow_text_is_all_document_white_space(const lxb_dom_node_t *text);

/* ---- ONE CHILD NODE'S BOX, CLASSIFIED --------------------------------------------------------------------
   CSS 2 §9.2 "Controlling box generation" decides whether a child of a block container generates a box at all,
   §9.3.1 "Choosing a positioning scheme: 'position' property" and §9.5 "Floats" decide whether that box is in
   the container's NORMAL FLOW, and §9.2.1 "Block-level elements and block boxes" / §9.2.2 "Inline-level
   elements and inline boxes" decide its LEVEL. This is those three questions' ONE answer for one child, and
   every walk over a block container's children needs it before it can do anything else.
   IT ANSWERS AND IT NEVER REFUSES, which is the whole of why it can be shared. A value here is a FACT about
   the child — the four are exhaustive over §9.2's box types — and what each caller DOES with a fact is the
   caller's own section: §10.6.3's height walk, css-sizing-3 §5.2 "Intrinsic Contributions"' maximum and CSSOM
   VIEW §2's scrolling area each meet a FLOAT with a different missing capability and a different sentence to
   say about it. A classification that crashed for one of them would report ITS line and ITS remedy for every
   caller, which is a crash whose reader has nowhere to apply the instruction: a `DCHECK` stamps the line it is
   WRITTEN at, so a refusal inside a shared helper names the helper for callers it has never heard of.
   THE CRASHES THAT REMAIN INSIDE IT ARE ABOUT BOX GENERATION AND ARE THEREFORE EVERY CALLER'S, and that is the
   line: a `display: contents` splice, a misparented table-internal box and an unmodelled `display` are each a
   BOX TREE this list is not yet, so the thing to build is this enumeration and the site is this entry.
   NO_BOX FOLDS TWO FACTS AND THE FOLD IS THE ANSWER'S OWN QUESTION. A comment generates no box at all; an
   absolutely positioned element generates one that is not in this container's flow — §9.3.1: "Absolutely
   positioned boxes are taken out of the normal flow. This means they have no impact on the layout of later
   siblings." Both are "not a member of this box's in-flow child box list AND contributing nothing to it", which
   is the question every caller asks. A FLOAT is not folded in with them because it fails only the first half:
   §9.5 "Floats" takes it out of the flow — "since a float is not in the flow, non-positioned block boxes
   created before and after the float box flow vertically as if the float did not exist" — and then the SAME
   sentence goes on to say it still changes the result, "however, the current and subsequent line boxes created
   next to the float are shortened as necessary to make room for the margin box of the float". A caller that
   treated it as a NO_BOX would silently drop a box every one of its sections still counts. */
typedef enum {
    BLOCK_FLOW_CHILD_NO_BOX = 0,  /* §9.2 generates none, or §9.3.1 takes it out of flow: contributes nothing */
    BLOCK_FLOW_CHILD_BLOCK,       /* §9.2.1's in-flow BLOCK-LEVEL box */
    BLOCK_FLOW_CHILD_INLINE,      /* §9.2.2's in-flow INLINE-LEVEL content */
    BLOCK_FLOW_CHILD_FLOAT        /* §9.5's float: out of flow, and still counted by every caller's section */
} BlockFlowChildKind;

BlockFlowChildKind block_flow_child_kind(lxb_dom_element_t *parent, lxb_dom_node_t *child);

/* ---- CSS 2.2 §9.2.1.1 "Anonymous block boxes"' BOX LIST, IN CONTENT ORDER --------------------------------
   A block container's box list is not a partition of its child nodes and the section says so outright: an
   inline box holding an in-flow block-level box "is broken around the block-level box …, splitting the inline
   box into two boxes (even if either side is empty)", and "the block-level box becomes a sibling of those
   anonymous boxes" — so ONE child node can yield THREE boxes on §9.4.1's stack. Its own worked example spells
   them: "a block box representing the BODY, containing an anonymous block box around C1, the SPAN block box,
   and another anonymous block box around C2".
   SO THE LIST IS ENUMERATED OVER THE CONTAINER'S CONTENT: its descendants in document order, with an in-flow
   INLINE BOX transparent — §9.2.2 "Inline-level elements and inline boxes"' own distinction, since "a
   non-replaced element with a 'display' value of 'inline' generates an inline box" while an ATOMIC
   inline-level box "participate[s] in [its] inline formatting context as a single opaque box" and keeps its
   block-level children to itself. The two entries below are the whole enumeration: the BLOCK-LEVEL boxes in
   order, and the step between content positions. Everything strictly between two consecutive block-level
   boxes is one ANONYMOUS BLOCK BOX, and the range before the first and after the last are the same shape.
   THE STEP DESCENDS ONLY INTO AN INLINE BOX THAT BREAKS, which is an equivalence and not an optimisation: the
   walk exists to find the positions at which the box LIST changes, and an inline box with no in-flow
   block-level box inside it changes it nowhere. A consumer that must reach the CONTENT of such a box descends
   on its own — what it must not do is delimit on its own, which is the one document with two box trees this
   header keeps warning about. That step is therefore NOT exported: a caller holding it would be holding half
   a delimitation, and the one entry below is the whole of what a box list is.
   THE NEXT BLOCK-LEVEL BOX STRICTLY AFTER `after`, or NULL when there is none; `after == NULL` starts at the
   beginning of `el`'s content. Seeding each call with the box the last one returned enumerates §9.2.1.1's
   block-level boxes in order, and everything strictly between two consecutive answers — plus the range before
   the first and the one after the last — is one ANONYMOUS BLOCK BOX. `after` is EXCLUSIVE so that the loop
   cannot stand still, and NULL means the start of the content rather than "no constraint": the two would
   differ for a container whose very first content position is a block-level box. Each call scans only
   forward, so the whole enumeration costs one pass over the content however many boxes it yields. */
lxb_dom_node_t *block_flow_next_block_box(lxb_dom_element_t *el, lxb_dom_node_t *after);


/* ONE OF §9.2.1.1's ANONYMOUS BLOCK BOXES, AS A RANGE OF ITS CONTAINER'S CONTENT — the two consecutive answers
   of the enumeration above that bracket it, either of which may be NULL for the start or the end of the
   content. It is a TYPE and not two arguments for one reason: every entry that measures, fills or places such
   a box takes exactly this pair, so a caller that hands one of them a SIBLING range — which is what every one
   of them used to take — must fail to COMPILE rather than start one node late and answer a plausible number.
   `after` IS EXCLUSIVE AND IS NOT "the run's first node", and the difference is §9.2.1.1's own "even if either
   side is empty". A run that follows a break inside an inline box may contain NOTHING — `<div><a>t<div>x</div>
   </a>tail</div>` gives the `<a>` a second fragment with no content — and that fragment still carries the
   box's closing edge, under "the border would be drawn around C1 (open at the end of the line) and C2 (open at
   the start of the line)". There is no node at that position, so only the break can name it. Naming the break
   also names the OPEN ANCESTORS the run inherits, whose opening edges belong to an earlier box.
   A SIBLING RANGE IS THE DEGENERATE CASE AND NEEDS NO SECOND SHAPE: `{ first->prev, end }` is exactly the
   half-open sibling range `[first, end)` whenever `first` is a child of the container, because this pair is
   read as `after`'s next sibling inside `after`'s parent. css-flexbox-1 §4 "Flex Items"' child text
   sequence is delimited that way and is expressed like that at its own call site. */
typedef struct {
    lxb_dom_node_t *after;   /* the in-flow block-level box this run FOLLOWS; NULL is the start of the content */
    lxb_dom_node_t *end;     /* the one it ENDS BEFORE; NULL is the end of the content */
} BlockFlowRun;

/* §9.2.1.1's PRECONDITION about ONE CHILD: is it an INLINE BOX that the section BREAKS — "when an inline box
   contains an in-flow block-level box, the inline box (and its inline ancestors within the same line box) is
   broken around the block-level box"? A child that is not an element, or is not §9.2.2's inline box (a
   non-replaced `display: inline` element), or holds no in-flow block-level box at any inline depth, is FALSE.
   IT IS EXPORTED FOR THE ONE QUESTION THE ENUMERATION ABOVE CANNOT ANSWER: which BOX a caller is asking about
   when a child is in SEVERAL of them. The runs partition the container's content, not its child list, so a
   broken child sits in two or more of them and a lookup keyed by the child is ambiguous exactly here — a
   consumer that keys by one crashes on a TRUE rather than answering with whichever run it saw last. */
bool block_flow_child_breaks_inline_box(lxb_dom_element_t *parent, lxb_dom_node_t *child);

/* DOES §9.2.1.1 GENERATE AN ANONYMOUS BLOCK BOX FOR THIS RUN? The section generates one only to wrap
   inline-level content — "we assume that there is an anonymous block box around 'Some text'" — so a run
   holding none is not a box at all, and `<div><p></p></div>` has ONE box on its stack rather than three.
   A RUN THAT OPENS OR CLOSES INSIDE AN INLINE BOX IS ALWAYS ONE, even with no node in it, because the split
   itself makes a box: "splitting the inline box into two boxes (even if either side is empty)". So
   `<div><a>text<div>x</div></a></div>` has three, the last of them holding nothing but the `<a>`'s closing
   edge.
   BOTH CONSUMERS ASK IT, which is why it is exported rather than each deciding for itself: §9.4.1's placement
   must not put an empty box on its stack, and css-sizing-3 §5.2's maximum would not notice one — so a walk
   that skipped the question there would be a second box list that happens to agree, and is free to stop. */
bool block_flow_run_generates_box(lxb_dom_element_t *el, BlockFlowRun run);


/* CSS 2.2 §9.4.2's OWN CONDITION over `el`'s WHOLE CONTENT: "an inline formatting context is established by
   a block container box that contains no block-level boxes."
   IT IS THE QUESTION "IS THIS ELEMENT WHERE core/layout/line_box.h's RUN IS THE WHOLE CHILD LIST", which is the
   one shape of §9.4.2's context that has an ELEMENT to name it. §9.2.1.1's anonymous block box is the other,
   and it is deliberately NOT reachable through this entry: a MIXED container answers FALSE here even though
   its inline-level children do sit in inline formatting contexts, because those contexts belong to boxes the
   element tree does not contain and a caller that wants them wants a RUN and not an element.
   IT IS EXPORTED BECAUSE §9.2.1's ALTERNATIVE IS ASKED FROM OUTSIDE THIS WALK AS WELL AS INSIDE IT. CSSOM VIEW
   §2's scrolling area needs the boxes on this context's line boxes PLACED, and reaching them means knowing
   which element establishes the context they are on — the same classification this file's own walk makes to
   choose between §9.4.1 and §9.4.2, and a second copy of it would be one document with two box trees, free to
   disagree about whether a run of white space is content (§9.2.2.1, the predicate above).
   A CONTAINER WHOSE INLINE BOX HOLDS AN IN-FLOW BLOCK-LEVEL BOX ANSWERS FALSE, and §9.2.1.1's second
   paragraph is why: that block-level box "becomes a sibling of those anonymous boxes", so it is one of THIS
   container's boxes and §9.4.2's "contains no block-level boxes" is not satisfied — whatever the child LIST
   looks like. The question is therefore asked over the container's CONTENT and not over its children, which
   is the one thing a caller re-deriving this predicate from `display` and a child walk would get wrong. */
bool block_flow_establishes_inline_context(lxb_dom_element_t *el);

/* CSS 2.2 §10.8.1 "Leading and half-leading"'s `inline-block` BASELINE, as the DISTANCE §9.4.1's OWN STACK
   REACHES IT AT. The section states the whole rule in one sentence with an exception in it: "The baseline of
   an 'inline-block' is the baseline of its last line box in the normal flow, unless it has either no in-flow
   line boxes or if its 'overflow' property has a computed value other than 'visible', in which case the
   baseline is the bottom margin edge." This entry answers the MAIN ARM and the FIRST DISJUNCT of that
   exception — it returns whether §9.4.1's stack met an in-flow line box at all, and stores at `*baseline` the
   offset from `el`'s TOP CONTENT EDGE to that last line box's baseline. The `overflow` disjunct is not asked
   here and must not be: it is a fact about the box's own declarations that its CALLER decides before it has
   any reason to look inside, and asking it twice would be one sentence with two readers.
   `*baseline` IS A DISTANCE ONLY WHEN THE ANSWER IS TRUE. Its zero otherwise is not a coordinate — there is no
   line box for a baseline to be inside — exactly as core/layout/line_box.h's height then measures no last line
   box. It is written on every path, so a caller that reads it after a false is reading a measurement of
   nothing rather than whatever it initialised.
   IT IS THE SAME WALK AS §10.6.3's HEIGHT AND A THIRD READING OF ITS RUNNING POSITION, WHICH IS WHY IT LIVES
   HERE AND NOT AT THE CALLER. §9.2.1 makes a block container one that "either contains only block-level boxes
   or establishes an inline formatting context", so this box's last line box is in one of exactly two places:
   in the ONE inline formatting context this element's own box establishes, or inside whichever box on §9.4.1's
   stack was placed last and has one — at THAT box's own offset down the stack, which is the running position
   §10.6.3's height walk already computes and nothing else can re-derive. §9.2.1.1 "Anonymous block boxes" puts
   a third kind of box on that stack ("if a block container box … has a block-level box inside it …, then we
   force it to have only block-level boxes inside it"), and it is a candidate like any other. Composing this at
   a call site out of a height and a descendant's line boxes would be a second walk over one child list, free
   to disagree with the first about where a margin collapsed.
   THE STACK IS WALKED WHATEVER `height` SAYS, for `block_flow_anonymous_boxes`' reason: `bf_height_needs_content`
   decides whether a box's own content decides ITS SIZE, and where its line boxes ARE is a different question —
   a box with a declared `height` is exactly the one whose last line box can sit below its own content edge, and
   §10.8.1 asks for that baseline and not for a clamped one.
   `el` MUST BE A BLOCK CONTAINER (§9.2.1), because §10.8.1's sentence and §9.2.1's alternative are both stated
   over that box; a caller has classified it first and a box that is not one crashes here. Every box on the
   stack whose own baseline belongs to another module — a `flex` or a `grid` container — crashes naming that
   module, because a bottom margin edge substituted for it would be a wrong number on a real line rather than
   an absent one. */
bool block_flow_last_line_box_baseline(lxb_dom_element_t *el, CssPx *baseline);

/* CSS 2.1 §17.5.3 "Table height algorithms"' CELL BASELINE, as far as §9.4.1's stack answers it: the offset
   from `el`'s TOP CONTENT EDGE to the baseline of the FIRST in-flow line box under it, and whether there is
   one at all. Same frame, same two answers and the same walk as the entry above — only the END of the stack it
   reads differs, which is why it is a second entry and not a second field: the first line box and the last are
   in DIFFERENT boxes on that stack, so one pass cannot report both without walking every box for a distance
   one caller throws away.
   §17.5.3's SENTENCE HAS THREE ARMS AND THIS ENTRY ANSWERS TWO. "The baseline of a cell is the baseline of the
   first in-flow line box in the cell, or the first in-flow table-row in the cell, whichever comes first. If
   there is no such line box or table-row, the baseline is the bottom of content edge of the cell box." The
   FIRST arm is the distance stored here; the THIRD is what a `false` return sends the caller to, and it is the
   caller's own arithmetic because a bottom content edge is a fact about the box's own height rather than about
   its lines. The SECOND — a nested table reached before any line box — CRASHES INSIDE the walk, naming itself,
   because a table's line boxes are in its cells' own formatting contexts and skipping past it would answer
   with a line box that is not the first in-flow thing in this cell at all.
   css-inline-3 §4.2.1 "Alignment Baseline Source: the baseline-source longhand"'s `first` KEYWORD WANTS THE
   SAME NUMBER and is the second caller this exists for; that longhand selects between this entry and the one
   above, and nothing here reads it — the selection belongs where the box is measured.
   `el` MUST BE A BLOCK CONTAINER (§9.2.1), which a non-replaced table cell is by that section's own sentence. */
bool block_flow_first_line_box_baseline(lxb_dom_element_t *el, CssPx *baseline);

/* CSS 2.2 §9.2.1.1 "Anonymous block boxes"' OTHER SHAPE OF §9.4.2's CONTEXT — the one with no element to name
   it. §9.2.1.1: "if a block container box (such as that generated for the DIV above) has a block-level box
   inside it (such as the P above), then we force it to have only block-level boxes inside it", and the boxes
   that forcing generates are one per MAXIMAL RUN of inline-level children, each holding an inline formatting
   context of its own.
   ONE BOX, AND EVERY FIELD OF IT IS THE SECTION'S OWN CONSTANT OR THE STACK'S OWN NUMBER. "The properties of
   anonymous boxes are inherited from the enclosing non-anonymous box …. Non-inherited properties have their
   initial value. For example, the font of the anonymous box is inherited from the DIV, but the margins will be
   0." So there is no margin, no border and no padding on it: its content box, its border box and its MARGIN
   box are ONE rectangle, which is why a single origin and a single extent describe all three.
   ITS INLINE-AXIS EDGES ARE NOT REPORTED, and that is a derivation rather than an omission. `width` has the
   initial value `auto` and both margins are zero, so CSS 2.1 §10.3.3's constraint equation leaves the whole of
   the containing block's content width to `width` — the anonymous box's two inline margin edges are exactly its
   container's two CONTENT edges, and CSS 2 §8.1 "Box dimensions" nests those inside the padding edge every
   caller of this entry has already folded. A number for them would be one an extreme cannot see. */
typedef struct {
    BlockFlowRun run;        /* the content this box wraps — core/layout/line_box.h takes exactly this */
    CssPx content_x;         /* its content box origin as an OFFSET from the container's own content box
                                origin — zero on the inline axis, by the derivation above */
    CssPx content_y;         /* … and on the block axis, which is where §9.4.1's stack put it */
    CssPx height;            /* its border-box height — §10.6.3's first bullet over its own line boxes */
} BlockFlowAnonBox;

/* `el`'s ANONYMOUS BLOCK BOXES, in tree order. Answers the count and stores a newly allocated array of that
   many at `*out`, WHICH THE CALLER OWNS AND MUST FREE; a count of zero stores NULL.
   ZERO IS A POSITIVE ANSWER AT EVERY ELEMENT THAT GETS IT, never a shrug. §9.2.1.1 generates a box only where
   a BLOCK CONTAINER holds a block-level box AND inline-level content, so: a container with no block-level box
   establishes ONE inline formatting context with its OWN element to name it (the predicate above, and the run
   is then the whole child list); a container with no inline-level content has nothing to wrap; and an element
   that is not a block container at all is outside the section's sentence — an inline box's inline content is
   on its ANCESTOR's lines, and a flex or grid container's is css-flexbox §4's anonymous flex ITEM, which is a
   different box this engine does not build. A caller that needs the difference asks the predicate above too.
   IT IS A SECOND ENTRY BESIDE THAT PREDICATE BECAUSE §9.2.1's TWO SHAPES OF ONE CONTEXT ARE REACHED
   DIFFERENTLY, and the header states why the predicate deliberately does not answer for this one: a MIXED
   container answers FALSE there, because the contexts inside it belong to boxes the ELEMENT TREE DOES NOT
   CONTAIN. So a caller that wants to reach every inline formatting context under an element — CSSOM VIEW §2
   "Terminology"'s scrolling area is the one that must, since §9.2.2.1's anonymous inline boxes around a text
   run are "descendants' boxes" wherever they sit — asks the predicate for the shape that has an element and
   this entry for the shape that does not.
   THE POSITION COMES OUT OF §9.4.1's OWN STACK AND IS NOT RE-DERIVED, which is the whole reason this lives
   here: the offset reported is the same running position the walk reads out for an ELEMENT that asks
   `block_flow_child_top`, taken at the same point, so an anonymous box and its block-level siblings cannot
   come to disagree about where a margin collapsed. */
size_t block_flow_anonymous_boxes(lxb_dom_element_t *el, BlockFlowAnonBox **out);

/* CSS 2.1 §10.6.3's (and, for a box that establishes a block formatting context, §10.6.7's) CONTENT-BASED
   HEIGHT of `el`'s box, in CSS pixels — the used value of a `height` that BEHAVES AS AUTO (css-sizing-3
   §3.2.1, `used_value_height_behaves_as_auto`), which is a computed `auto` and also a percentage whose
   containing block's height is indefinite.
   THE CALLER HAS ALREADY ESTABLISHED that the box is one §10.6.3 or §10.6.6 covers; core/layout/used_value.c's
   `uv_size` is one caller and it has classified the box type first. Every case this component does not lay out
   crashes naming its own section — see the header.
   THAT IT BEHAVES AS AUTO IS THE CALLER'S CLAIM AND IS NOT ASKED HERE, WHICH ONE CALLER RELIES ON. This entry
   runs §10.6.3's walk over `el`'s children and returns its distance whatever `height` says — the property is
   read only for §8.3.1's collapsing conjuncts — and CSS 2.1 §17.5.3 "Table height algorithms" needs exactly
   that: "In CSS 2.1, the height of a cell box is the minimum height required by the content", and its next
   sentence says the declaration does not change it ("The table cell's 'height' property can influence the
   height of the row (see above), but it does not increase the height of the cell box"). So
   core/layout/table_height.c asks for a CELL's content height with a declared `height` on the box, and that is
   the section running rather than a contract being bent. An earlier form of this paragraph said the caller had
   established the property behaves as auto, which was true of the one caller there was. */
CssPx block_flow_auto_height(lxb_dom_element_t *el);

/* CSS 2 §9.4.1's VERTICAL PLACEMENT of `el`: the distance from the TOP CONTENT EDGE of `el`'s containing block
   to `el`'s TOP BORDER EDGE, in CSS pixels. It is stated against the containing block's CONTENT edge because
   that is the rectangle §10.1 gives the box and the one §10.6.3 measures its height from, so the caller adds
   exactly the containing block's own origin plus its top border and padding and nothing else.
   `el` MUST BE AN IN-FLOW BLOCK-LEVEL BOX whose containing block is §10.1's second case — core/layout/
   flow_position.c is the caller and it has taken every other positioning scheme out through its own section
   first. An element the walk over its containing block's children never places crashes rather than answering
   a coordinate no box has. */
CssPx block_flow_child_top(lxb_dom_element_t *el);

#endif
