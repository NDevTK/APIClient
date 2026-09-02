/* CSS 2.1 §17.4 Tables in the visual formatting model — THE TABLE WRAPPER BOX, which is the box §9.4.1's
 * stack actually places and which nothing in this engine used to name.
 *
 * WHY THIS IS A COMPONENT AND NOT A LINE IN THE BLOCK WALK. A `display: table` element generates TWO boxes,
 * not one: "the table generates a principal block box called the table wrapper box that contains the table box
 * itself and any caption boxes (in document order)". Every consumer that has ever asked about a table has
 * silently meant one or the other of them, and the two take DIFFERENT declarations off the same element —
 * §17.4 states the split by name. A walk that read the table element's `display` and then read its `margin-top`
 * and its `border-top-width` off the same box has already merged them, and the merge is invisible: both are
 * real values on a real element, and only the spec says which box each lands on. So the split is asked HERE,
 * once, and no caller decides it for itself.
 *
 * WHICH BOX IS BLOCK-LEVEL IS THE HALF THAT WAS COSTING THE MOST. §17.4: "The table wrapper box is a 'block'
 * box if the table is block-level, and an 'inline-block' box if the table is inline-level." So a `display:
 * table` child of a block container is an ordinary BLOCK-LEVEL box on §9.4.1's stack — a fact §9.2.1 states
 * from the other direction ("Except for table boxes, which are described in a later chapter, and replaced
 * elements, a block-level box is also a block container box"), which is why a table is block-LEVEL without
 * being a block CONTAINER and why a walk testing only the second answered `false` and stopped. The wrapper
 * also "establishes a block formatting context", so §8.3.1's runs do not reach through it.
 *
 * WHAT IS DELIBERATELY NOT HERE, AND WHY EACH IS NAMED RATHER THAN GUESSED.
 *   - The wrapper's HEIGHT is §10.6.3's over its own in-flow children, and those children are the caption
 *     boxes and the table box — so it is not this component's until the table box HAS a height, which is
 *     §17.5.3 Table height algorithms' over §17.5.2's used column widths over §17.5's grid, which
 *     core/layout/table_grid.h answers. That chain is what
 *     core/layout/block_flow.c's table arm names, in that order.
 *   - The wrapper's WIDTH is stated by §17.4 itself and is equally not available yet: "The width of the table
 *     wrapper box is the border-edge width of the table box inside it, as described by section 17.5.2."
 *   - The wrapper's CHILD ORDER — which captions sit above the table box and which below — is `caption-side`'s
 *     (§17.4.1 Caption position and alignment), and that property is NOT in core/css/css_computed_value.c's
 *     modelled set, so it cannot be read at all today. §17.4.1 gives it `Computed value: as specified`, which
 *     makes it one row of that file's as-specified arm; core/css/css_defaulting.c already lists it among the
 *     INHERITED properties, which §17.4.1's own `Inherited: yes` requires, so only the computed-value half is
 *     missing. The order is not built here because its only consumer is the height above, and building it
 *     first would be an ordering nothing can yet ask for.
 *   - Which boxes the wrapper contains BESIDES the table box is already answered one component over:
 *     core/layout/table_box.h's `table_box_captions`. This component does not re-derive it. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_WRAPPER_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_WRAPPER_H

#include <stdbool.h>

/* Does an element with this computed `display` generate a TABLE WRAPPER BOX? True for exactly the two values
   §17.2 The CSS table model gives a table box — `table` and `inline-table` — because §17.4's sentence is
   written over "a table" and both spellings are one. `display` must not be NULL. */
bool table_wrapper_generates(const char *display);

/* Is that wrapper BLOCK-LEVEL? §17.4: "a 'block' box if the table is block-level, and an 'inline-block' box if
   the table is inline-level" — so `table` answers true and `inline-table` answers false, and the caller must
   have established that a wrapper exists at all (the predicate above) before asking, because "not block-level"
   and "no box" are different answers and a shared false would merge them. */
bool table_wrapper_is_block_level(const char *display);

/* §17.4's DECLARATION SPLIT, as the one question every consumer of a table's geometry has to ask: is the
   computed value of `name` on the table element used on the WRAPPER box (true) or on the TABLE BOX (false)?
   §17.4 states it as a closed list and this predicate is that list: "The computed values of properties
   'position', 'float', 'margin-*', 'top', 'right', 'bottom', and 'left' on the table element are used on the
   table wrapper box and not the table box; all other values of non-inheritable properties are used on the
   table box and not the table wrapper box."
   IT IS ASKED ABOUT NON-INHERITED PROPERTIES ONLY, which is the sentence's own scope and not a narrowing
   added here: an INHERITED property is inherited by both boxes from the same parent and there is nothing to
   divide, so asking about one is a question the section does not answer and the caller has confused two
   things. A caller that cannot tell them apart has core/css/css_defaulting.c's inherited list to ask.
   THE OTHER BOX GETS THE INITIAL VALUE, NOT NOTHING — §17.4's parenthesis: "(Where the table element's values
   are not used on the table and table wrapper boxes, the initial values are used instead.)" So a wrapper has
   NO border and NO padding whatever the table element declares, and a table box has NO margin; supplying that
   initial value is the consumer's step, because it is a per-property lookup this predicate would have to
   duplicate a cascade to answer. */
bool table_wrapper_owns_property(const char *name);

#endif
