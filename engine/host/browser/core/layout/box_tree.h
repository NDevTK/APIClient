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
 * NOT COVERED, AND THE REFUSAL IS AT box_tree.c RATHER THAN IN THIS PARAGRAPH — WHICH IS WHAT CHANGED SINCE IT
 * WAS A RESIDUAL. THIS SEQUENCE'S MEMBER TYPE IS A DOM NODE, and css-display-3 §1 "Introduction" names a
 * box-tree child that is not one — "a principal block box and a child marker box". So a `list-item` box's
 * sequence cannot be stated here at all, and the two entries that would state it CRASH instead of answering:
 * `box_tree_first_child` for such a box, and `box_tree_prev_sibling` where its answer would be the START of
 * the sequence. Read `bt_require_marker_box_is_spellable` for the spec sentences and for what narrows it.
 * THIS CLAUSE USED TO READ `no ::before/::after box generation exists in this engine for it to splice`, AND IT
 * IS KEPT BECAUSE A READER WHO GREPS THIS ENGINE FOR A PSEUDO-ELEMENT BOX RE-DERIVES IT. It ENUMERATED where
 * the gap is a PROPERTY, and §2.5's own sentence says "such as ::before and ::after pseudo-elements" rather
 * than naming a closed pair — so the two it listed made the population look empty when the commonest member of
 * that population is neither of them. ::before AND ::after ARE STILL EMPTY, BY CONSTRUCTION RATHER THAN BY
 * OMISSION, and no diff at this component can change it: css-pseudo-4 §4.1 "Generated Content Pseudo-elements:
 * ::before and ::after" gates them on a COMPUTED VALUE — "When their computed content value is not none, these
 * pseudo-elements generate boxes as if they were immediate children of their originating element" — and
 * css-content-3 §1 "Inserting and Replacing Content: the content property" makes the initial `normal` compute
 * to `none` for exactly those two: "For ::before and ::after, this computes to none. For ::marker,
 * ::placeholder, and ::file-selector-button, this computes to itself (normal)." Nothing can move it off
 * `none`, because the property does not PARSE: the derivation is a grep of lexbor's
 * `source/lexbor/css/property/const.h` for `LXB_CSS_PROPERTY_CONTENT`, with `LXB_CSS_PROPERTY_COLOR` as the
 * armed control that shows the question reaches an answer at all.
 * AND THE SIX-POSITION NEXT-DIFF CLAUSE THAT REPLACED IT WAS ITSELF REFUTED BY THE DIFF THAT ACTED ON IT,
 * RECORDED HERE RATHER THAN DELETED BECAUSE A READER WHO RE-DERIVES IT FROM §2.5's SPLICE WILL WRITE IT AGAIN.
 * It named `bt_resolve`, `bt_continue_before`, `bt_resolve_back` and `bt_continue_after` — the four SPLICE
 * positions — on the ground that "a `contents` list item has no principal box for the marker to be a child OF
 * … and §2.5 then splices the marker into the ancestor's sequence exactly as it splices the source-document
 * children". THERE IS NO SUCH ELEMENT AND THE MARKER NEVER MEETS THE SPLICE. An element computes ONE
 * `display`, and `bt_is_spliced` is one comparison against `contents`, so an element this walk pierces is not
 * `list-item` and generates no marker at all — css-display-3 §2.3 "Generating Marker Boxes: the list-item
 * keyword" generates it from that keyword and from nothing else ("causes the element to generate a ::marker
 * pseudo-element"). A `list-item` element GENERATES a box, so it terminates the splice rather than being
 * pierced by it, and its marker is a child of that box. What §2.5's sentence really carries through the splice
 * is the pseudo-elements it names — "its children and pseudo-elements still generate boxes and text sequences
 * as normal" — which is the empty pair above. TWO positions are touched and not six, and both now refuse.
 * HOW ITS ABSENCE WOULD SHOW, NOW THAT IT IS A CRASH: a document containing a list item reaches
 * `box_tree_first_child` of that item through any walk over its contents and ABORTS naming css-lists-3 §3.1,
 * where before the same walk got the item's first SOURCE-DOCUMENT child and a list rendered with no marker.
 * In release, where the refusal compiles out, that earlier answer is what is still returned.
 * WHAT THE NEXT DIFF BUILDS IS css-lists-3 §3.2 "Generating Marker Contents", NOT THE MEMBER TYPE, and the
 * order is forced rather than chosen: §3.2 is what makes the refusal PRECISE instead of blanket, because its
 * last arm is "otherwise: The marker box has no contents and ::marker does not generate a box" — a list item
 * with no marker string has a COMPLETE sequence here and must not be refused. It needs `list-style-type`
 * (css-lists-3 §3.4 "Text-based Markers: the list-style-type property", whose `Initial:` line is `disc` and
 * whose `Inherited:` line is `yes`) to be a computed value at all, which it is not: the property is in neither
 * lexbor's registry nor core/css/css_style_declaration.c's unregistered-initial table, so css-cascade-5 §7.1
 * "Initial Values" has no initial value to fall to and it answers NULL for every element. §3.2's arms then
 * reach css-counter-styles-3 §6.3 "Symbolic: disc, circle, square, disclosure-open, disclosure-closed" for the
 * symbol a `disc` marker is filled with, and any NUMERIC style additionally needs css-lists-3 §4.6 "The
 * Implicit list-item Counter" — which is why that arm is a crash of its own and not part of the same landing.
 * THE MEMBER TYPE IS THE LANDING AFTER THAT AND IS NO LONGER AN OPEN QUESTION: it becomes a BY-VALUE PAIR of
 * the ORIGINATING NODE and an `lxb_css_selector_pseudo_element_id_t`, decided and PINNED at box_tree.c beside
 * the four answers it refuses and the reason each is refused. Read it there rather than re-deriving it, because
 * three of those four leave every signature in this file exactly as it is and would land with nothing noticing.
 * THE CLAUSE THAT STOOD HERE NAMED `bt_require_in_sequence` AS WHERE THAT DECISION LANDS, and the refutation is
 * recorded at that crash rather than here: a pseudo member's node half IS its originating element, so the
 * equality HOLDS for it — by a `box_tree_parent` arm that does not exist yet — and what the decision constrains
 * is therefore the ASCENT and not the refusal. `box_tree_prev_sibling`'s round trip is still the guard that
 * makes a sequence taught in only ONE of its two directions a crash rather than one that reads one way forward
 * and another way back. THE STYLE AND THE LAYOUT ARE TWO MORE AND ARE NOT THIS COMPONENT'S:
 * css-pseudo-4 §4 "Tree-Abiding Pseudo-elements" gives the box its properties ("They inherit any inheritable
 * properties from their originating element; non-inheritable properties take their initial values as usual"),
 * over a box core/css/css_computed_value.h can only key on an ELEMENT; and css-lists-3 §3.5 "Positioning
 * Markers: The list-style-position property" has an `Initial:` of `outside`, whose block-container arm is "the
 * marker box is a block container and is placed outside the principal block box" — a positioning scheme this
 * engine has no component for, and one §3.5's own text calls "handwavey nonsense from CSS2".
 * RETIREMENT: this paragraph goes when this sequence can yield a box-tree child that is NOT a DOM node,
 * because the enumeration that made its gap look empty is unspellable once the member type can name one. */
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
