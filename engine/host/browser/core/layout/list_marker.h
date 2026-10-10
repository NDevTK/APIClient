/* css-lists-3 §3 "Markers" — WHETHER a list item's ::marker generates a box, which is css-lists-3 §3.2
 * "Generating Marker Contents" read for the one answer its last arm states and NOT for its contents, and WHERE
 * that box is positioned, which is css-lists-3 §3.5 "Positioning Markers: The list-style-position property".
 *
 * IT IS A COMPONENT BECAUSE ITS ANSWERS HAVE SEVERAL CONSUMERS THAT MUST AGREE: core/layout/box_tree.h yields
 * a marker member at the head of exactly the list items this entry answers TRUE for, every walk over that
 * sequence classifies the member through core/layout/block_flow.h by the position below, and the marker's own
 * CONTENTS are §3.2's other three arms over the same two properties.
 *
 * IT IS ASKED OF ANY ELEMENT AND NOT ONLY OF A LIST ITEM, because css-lists-3 §3.1 "The ::marker
 * Pseudo-Element"' last sentence IS the answer for every
 * other one: "Marker boxes only exist for list items: on any other element, the ::marker pseudo-element’s
 * content property must compute to none, which suppresses its creation." So the css-display-3 §2.3
 * "Generating Marker Boxes: the list-item keyword" test lives here once rather than as a precondition each
 * caller re-derives — and it is a THIRD question rather than a third copy of the `list-item` comparison in
 * core/layout/block_flow.c and core/layout/used_value.c, which ask whether the PRINCIPAL box is block-level
 * where this asks whether a SECOND box is generated beside it.
 *
 * IT ANSWERS EXISTENCE AND NOT CONTENTS, AND THE SPLIT IS §3.2's OWN ORDER RATHER THAN A NARROWING MADE HERE.
 * Its first three arms each say what the box CONTAINS and its last says there is no box at all, so a caller
 * that needs only "is there one" needs the arms' TRUTH and none of their values — and the values are two
 * components later: css-counter-styles-3 §6.3 "Symbolic: disc, circle, square, disclosure-open,
 * disclosure-closed" for the symbol a `disc` is filled with, over the `@counter-style` registry
 * core/css/css_counter_style.h assigns to a different and later component, and a FETCH for the image.
 * THE TYPE ARM THEREFORE NEEDS NEITHER, which is the one step that makes this landing possible and is
 * css-counter-styles-3 §5 "Extending list-style-type, counter(), and counters()"' own sentence: "If a
 * <counter-style-name> is used that does not refer to any existing counter style, it must act identically to
 * the decimal counter style". A name that denotes nothing still denotes a marker string, so "defines a marker
 * string" is TRUE for every value of §3.4's line but its own `none` keyword — and asking which string it is
 * would have made the symbol a prerequisite of the existence answer, which is how §3.2 came to be recorded as
 * blocked on css-counter-styles-3 §6.3 in three files at once.
 *
 * ONE ARM REFUSES AND IT IS THE IMAGE ONE, for a reason that is a FETCH and not a cascade step.
 * css-images-3 §2 "Image Values: the <image> type" states both halves: "In some cases an image is invalid, such as a <url>
 * pointing to a resource that is not a valid image format or that has failed to load", and then the
 * error-handling clause this property owns — "an invalid image in list-style-image it is treated as none,
 * allowing the list-style-type to render in its place". An image this engine has not fetched has not FAILED to
 * load, so it is neither valid nor invalid here and §3.2's second arm has no answer: that is a capability to
 * build and not a value to guess, so the entry CRASHES there rather than deciding which arm a page is on.
 *
 * RETIREMENT: this file goes when css-lists-3 §3.2's three content arms are answered here, because an entry
 * that states WHAT a marker box contains has stated whether there is one and this boolean is then its caller's
 * reading of that answer rather than a question of its own. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LIST_MARKER_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LIST_MARKER_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* Does `el`'s ::marker generate a box — css-lists-3 §3.2's condition, read for the arm it lands on? FALSE for
   every element that is not a list item and for a list item §3.2's last arm answers ("The marker box has no
   contents and ::marker does not generate a box"), TRUE where one of its first three arms is. */
bool list_marker_box_generated(lxb_dom_element_t *el);

/* css-lists-3 §3.5's two values. `inside`: "The ::marker is an inline element at the start of the list item’s
   contents." `outside`, for a list item that is a block container: "the marker box is a block container and
   is placed outside the principal block box". */
typedef enum {
    LIST_MARKER_POSITION_INSIDE,
    LIST_MARKER_POSITION_OUTSIDE
} ListMarkerPosition;

/* Where `list_item`'s marker box is placed — §3.5 read off the list item's computed `list-style-position`,
   after the property's own arm "If the list item is an inline box: this value is equivalent to inside".
   `list_marker_box_generated(list_item)` must be TRUE; asserted. */
ListMarkerPosition list_marker_position(lxb_dom_element_t *list_item);

#endif
