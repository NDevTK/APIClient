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
 * run, which is every call site that exists at the moment this sequence is first consumed.
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

#endif
