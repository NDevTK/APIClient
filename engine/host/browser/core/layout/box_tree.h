/* css-display-3 §1 "Introduction"'s BOX TREE, ON ITS CHILD AXIS — the nodes whose boxes are the CHILDREN of
 * one box, which is not that box's element's DOM child list wherever css-display-3 §2.5 "Box Generation: the
 * none and contents keywords"' splice has run: "For the purposes of box generation and layout, the element
 * must be treated as if it had been replaced in the element tree by its contents (including both its
 * source-document children and its pseudo-elements, such as ::before and ::after pseudo-elements, which are
 * generated before/after the element's children as normal)."
 *
 * WHY IT IS A COMPONENT AND NOT A DESCENT AT EACH WALK, which five separate crashes in this tree each said in
 * their own words before any of them could be converted: core/layout/block_flow.c's child classification,
 * core/layout/flex_item.c's flex-item classification, core/layout/line_box.c's line walk,
 * core/layout/table_box.c's anonymous-table walk and core/layout/table_column_box.c's column walk all meet a
 * `contents` child at their own child walk, and a copy per walk is ONE box-tree rule with five answers about
 * which children a box has. Those walks are REQUIRED to agree and say so: core/layout/intrinsic_size.c
 * asserts one such agreement by name, and core/paint/box_paint.c asserts another.
 *
 * WHICH OF THEM HAVE BEEN ROUTED IS NOT RECORDED HERE, AND THE OMISSION IS DELIBERATE RATHER THAN AN
 * OVERSIGHT: it is a count of what is missing, it shrinks as the work is done, and its only reader is the
 * person about to invalidate it. The DERIVATION is one command — `git grep -n box_tree_first_child` names
 * every converted walk, and `bf_element_child`'s `contents` crash is what an unconverted one reaches — and
 * that answers about the tree in hand rather than about the tree somebody was looking at.
 *
 * IT PIERCES `contents` AND NOTHING ELSE, because §2.5's other keyword is the opposite sentence — `none` is
 * "The element and its descendants generate no boxes or text sequences", so a `none` element IS yielded here
 * and each walk goes on skipping it under its own CSS 2 §9.2 arm. Piercing it too would delete that arm from
 * five files at once and put §2.5's two keywords behind one predicate, which is the shape that let one walk
 * skip a `contents` child as though it were a `none` one in the first place.
 *
 * IT READS THE COMPUTED `display` WHILE THE ANCESTOR DIRECTION READS THE SPECIFIED ONE, and §1 is why that is
 * two readings of one relation rather than one of them being sloppy: "To create the box tree, CSS first uses
 * cascading and inheritance, to assign a computed value for each CSS property to each element and text node in
 * the source tree." — "Then, for each element, CSS generates zero or more boxes as specified by that element's
 * display property." This walk is in the SECOND sentence, so §2.5's last sentence (Appendix B's suppression to
 * `none`) and §2.8's root rule are already applied and the test is one comparison.
 * core/css/css_computed_value.h's `css_box_parent_display` is in the FIRST, as part of computing `display`
 * itself, so it reads a SPECIFIED value and re-derives §2.8's root rule by hand — it cannot ask for a computed
 * one without re-entering the computation it is inside.
 *
 * IT MOVES NO STYLE, WHICH IS §2.5's OWN NOTE AND IS THE PART A CONVERTING CALLER GETS WRONG: "As only the box
 * tree is affected, any semantics based on the document tree, such as selector-matching, event handling, and
 * property inheritance, are not affected." So a node this sequence yields keeps its DOM parent for every
 * inherited property, and a walk that reads a property OF THE CHILD'S PARENT — a text node's inherited
 * `white-space`, the element whose computed properties a run of characters has — must read that node's own
 * parent element and never the box it is now a child of. The two are the same element only where no splice has
 * run. THIS CLAUSE USED TO SAY "which is every call site that exists at the moment this sequence is first
 * consumed", and it is rewritten rather than deleted because a reader who counts the converted walks will
 * re-derive it: the SECOND consumer is core/layout/table_box.c's §17.2.1 walk, whose text arm reads exactly
 * such a property, so the sentence stopped being true of every call site at the commit that routed it. What
 * closed it there is not a careful caller but a SIGNATURE: core/layout/block_flow.h's §9.2.2.1 white-space
 * predicate no longer TAKES the container, it derives the text node's own parent, so the wrong element is
 * unspellable at this sequence's remaining consumers instead of being a sentence each of them must recall.
 *
 * NOT COVERED: §2.5's splice carries an element's PSEUDO-ELEMENTS as well as its source-document children, and
 * this sequence holds only the second — no `::before`/`::after` box generation exists in this engine for it to
 * splice, so the splice's pseudo-element half has nothing to splice yet. WHAT THE NEXT DIFF BUILDS:
 * pseudo-element box generation, whose boxes then enter THIS sequence before and after an element's children
 * rather than at each caller. HOW ITS ABSENCE WOULD SHOW: a document whose `contents` element carries
 * generated content renders that content in a different box parent from the one §2.5 names. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_BOX_TREE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_BOX_TREE_H

#include <lexbor/dom/dom.h>

/* THE FIRST NODE of `box`'s box-tree child sequence, or NULL when it is empty. `box` must GENERATE A BOX:
   §2.5 puts a `contents` element's children in that element's own box parent's sequence, so asking this of one
   is asking about a box that is not there.
   SEED A LOOP WITH IT AND STEP WITH THE ENTRY BELOW — `for (c = box_tree_first_child(el); c != NULL;
   c = box_tree_next_sibling(el, c))` is the shape every walk this replaces already had, which is why the
   sequence is two steps and not a materialised list: a list would be an allocation per container per walk, and
   there are five walks over one container. */
lxb_dom_node_t *box_tree_first_child(lxb_dom_element_t *box);

/* THE NEXT NODE after `child` in that sequence, or NULL at its end. `child` must be a MEMBER of it — one of
   `box`'s own DOM children, or a child of a chain of `contents` elements inside it — and that is ASSERTED rather
   than assumed, because a walk half-converted from `->next` hands this a node out of a DIFFERENT box's sequence and
   would otherwise be answered plausibly. */
lxb_dom_node_t *box_tree_next_sibling(lxb_dom_element_t *box, lxb_dom_node_t *child);

/* THE PREVIOUS NODE before `child` in that sequence, or NULL at its start — the entry above read the other way,
   with the same membership requirement on `child` and the same refusal of a spliced one.
   A SEQUENCE IS NOT A SEQUENCE IN ONE DIRECTION ONLY, AND THE CALLER THAT NEEDS THE OTHER ONE IS THE ONE THE
   FORWARD STEP CANNOT SERVE: css-flexbox-1 §4 "Flex Items"' child text sequence is a MAXIMAL run, and §4's
   rule is stated over the whole of it — "if the entire text sequences contains only document white space
   characters (i.e. characters that can be affected by the white-space property) it is instead not rendered" —
   while the classification that answers it is asked about ONE member. A walk that could only step forward
   would answer that sentence differently depending on which member it was handed, which is two anonymous flex
   items where §4 makes one. core/layout/block_flow.h's `BlockFlowRun` wants the same direction for a different
   question: its `after` bound is the member BEFORE a run's first node, which every caller used to compose as
   `first->prev` — a node in a DIFFERENT box's sequence wherever §2.5 has spliced.
   IT IS THE FORWARD STEP'S INVERSE AND SAYS SO RATHER THAN MEANING TO BE:
   `box_tree_next_sibling(box, box_tree_prev_sibling(box, c))` is `c` for every member that has a predecessor,
   and that round trip is ASSERTED at the site. Two directions of one sequence are exactly the pair that can be
   taught about a tree separately and come apart on it — one descending into a spliced element's FIRST child
   and the other out of its LAST — and an equality is what makes that impossible rather than merely unintended. */
lxb_dom_node_t *box_tree_prev_sibling(lxb_dom_element_t *box, lxb_dom_node_t *child);

/* THE BOX `n`'s BOXES ARE CHILDREN OF — the nearest ancestor ELEMENT that generates a box, which is `n`'s DOM
   parent wherever no splice has run and is the first ancestor past a chain of `contents` elements where one
   has. NULL when the ascent leaves the element tree (a Document or a plain DocumentFragment parent, or no
   parent at all), which is the same answer core/css/css_computed_value.h's `css_parent_element` gives at a
   root.
   IT IS THE ASCENT OF THE SEQUENCE ABOVE AND NOT A SECOND WALK, which is the whole reason it is here rather
   than at the walks that need it: `box_tree_next_sibling(box_tree_parent(n), n)` is the step that continues
   `n`'s own sequence, and the membership refusal that entry makes is spelled AS this equality, so the two
   cannot come apart. A walk that derived a box parent for itself would be §2.5's predicate written a sixth
   time, and the copies would disagree about exactly the tree they were written for.
   IT READS THE COMPUTED `display`, for the reason stated at the top of this file:
   core/css/css_computed_value.h's `css_box_parent_display` answers the same RELATION over a SPECIFIED value
   because it is INSIDE the computation and cannot ask for a computed one, and it returns that ancestor's
   `display` rather than the ancestor — so it is not this entry under another name and neither is derivable
   from the other. css-display-3 §2.8 "The Root Element's Principal Box" is what makes this walk terminate at
   the root rather than by a bound: "a display of contents computes to block on the root element", so no
   ascent can pass a root whose computed value this entry reads.
   IT ANSWERS FOR A SPLICED `n` AND ITS TWO SIBLINGS REFUSE ONE, AND THE ASYMMETRY IS THE QUESTION AND NOT A
   RELAXATION. A POSITION in a `contents` element's sequence is meaningless — the element has no box for those
   nodes to be the children of, which is what `box_tree_first_child` and `box_tree_next_sibling` refuse. Its
   BOX PARENT is §2.5's own sentence read directly: the element "must be treated as if it had been replaced in
   the element tree by its contents", so the box its contents are spliced INTO is exactly what this walk
   returns, and it is the same box every one of those contents answers. REFUSING IT WOULD MASK THE CRASH THAT
   NAMES THE WORK: core/layout/block_flow.c's child classification states its precondition as this equality,
   and a half-converted walk handing it a `contents` element would abort HERE — "a box parent was asked of an
   element that has none" — instead of at `bf_element_child`'s `display: contents` arm, which is the one that
   tells its reader which walk to route and to what. */
lxb_dom_element_t *box_tree_parent(lxb_dom_node_t *n);

#endif
