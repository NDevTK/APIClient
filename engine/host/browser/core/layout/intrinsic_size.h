/* css-sizing-3 §5.1 "Intrinsic Sizes" — A BOX'S MIN-CONTENT AND MAX-CONTENT INLINE SIZES, which are the two
 * terms CSS 2.2 §10.3.5 "Floating, non-replaced elements" calls the preferred minimum width and the preferred
 * width and sends its `auto` widths to.
 *
 * THE MODERN DEFINITION IS CIRCULAR BACK TO CSS 2 AND THAT IS THE FIRST THING TO KNOW ABOUT IT, because a
 * reader who takes css-sizing-3 for the algorithm finds none. §5.1 states both sizes in terms of the very
 * layout that consumes them — "the min-content size of a box in each axis is the size it would have IF IT WAS A
 * FLOAT given an auto preferred size in that axis … and if its containing block was ZERO-sized in that axis",
 * the max-content size the same sentence with "INFINITELY-sized" — and then says outright: "this specification
 * does not define how to determine the sizes of floats. Please refer to [CSS2]." So §5.1 is a DEFINITION and
 * CSS 2.2 §10.3.5 is the operation, in its own words: "calculate the preferred width by FORMATTING THE CONTENT
 * WITHOUT BREAKING LINES other than where explicit line breaks occur, and also calculate the preferred minimum
 * width, e.g., by TRYING ALL POSSIBLE LINE BREAKS."
 * css-sizing-3 §2.1 "Auto Box Sizes" is what pins the two vocabularies to each other, by name — "this is called
 * the 'preferred width' in CSS2.1§10.3.5" of the max-content inline size, "this is called the 'preferred
 * minimum width' in CSS2.1§10.3.5" of the min-content inline size — and it also states the operation in the
 * form this engine implements: max-content is the size "if NONE of the soft wrap opportunities within the box
 * were taken", min-content the size "if ALL soft wrap opportunities within the box were taken". Two spellings,
 * one walk, and citing only one of them would leave the other reader unable to check the code.
 *
 * WHAT THIS COMPONENT IS AND WHAT core/layout/text_run.h IS, because they are one measurement split at the
 * place the two problems actually part. text_run.h owns the CHARACTERS — css-text-3 §4.1's white space
 * processing and §5's soft wrap opportunities over [UAX14] — and knows nothing about boxes. This owns the BOX
 * TREE: which of a box's children contribute, and how css-sizing-3 §5.2 "Intrinsic Contributions" combines
 * them. The split is not tidiness: core/layout/line_box.c needs the first without the second (a line box's
 * height is a function of where the run breaks, not of an intrinsic size) and so does CSSOM VIEW §2's scrolling
 * area, so folding the characters in here would make those two consumers depend on a sizing algorithm neither
 * of them asks about.
 *
 * THE ANSWER IS THE CONTENT BOX'S INLINE SIZE, which is the box CSS 2.2 knows and the one §10.3.5's formula is
 * arithmetic in: every term of `min(max(preferred minimum width, available width), preferred width)` is a
 * content width, and the result is the used `width`. css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing
 * property"'s conversion to the border box is applied to the RESULT by the caller, exactly as core/layout/
 * used_value.c applies it to §10.3.3's constraint equation — so this component never asks about `box-sizing`
 * and a caller that forgot to is wrong in one place rather than in two.
 *
 * WHAT REACHES THIS WALK IS AN INLINE FORMATTING CONTEXT AND EVERYTHING ELSE CRASHES BY NAME. CSS 2.2 §9.4.2
 * says which box establishes one — "a block container box that contains no block-level boxes" — and a box that
 * contains one is §9.4.1's, whose intrinsic size is css-sizing-3 §5.2's MAXIMUM over its in-flow children's
 * contributions rather than a sum along a line. Both are real algorithms and they share no step, so this
 * component implements the one whose inputs exist and crashes for the other rather than running a walk that
 * would be right for half the documents it is handed.
 *
 * NOTHING IS STORED, for core/layout/used_value.h's reason: a layout is per-flow state, so a cached intrinsic
 * size is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_SIZE_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* css-sizing-3 §5.1's PAIR. They are returned together and never separately because §2.1 defines them over the
   same content with only the soft wrap opportunities differing, so one walk produces both — and because the
   one relation between them (`min_content <= max_content`) is a statement about the pair that a caller holding
   one of them could not check. */
typedef struct {
    CssPx min_content;   /* §2.1's min-content inline size — CSS 2.2 §10.3.5's "preferred minimum width" */
    CssPx max_content;   /* §2.1's max-content inline size — CSS 2.2 §10.3.5's "preferred width" */
} IntrinsicInlineSizes;

/* THE INTRINSIC INLINE SIZES OF `el`'s BOX, as CONTENT-box widths in CSS pixels.
   THE CALLER HAS NOT ESTABLISHED ANYTHING and does not need to: every box type this walk cannot size crashes
   here naming its own section, which is what lets a consumer ask the question without first re-deriving the
   classification core/layout/block_flow.c owns. */
IntrinsicInlineSizes intrinsic_inline_sizes(lxb_dom_element_t *el);

#endif
