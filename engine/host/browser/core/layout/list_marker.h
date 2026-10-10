/* css-lists-3 §3 "Markers": whether a list item's ::marker generates a box (css-lists-3 §3.2 "Generating Marker
 * Contents", read for existence only) and where that box is placed (css-lists-3 §3.5 "Positioning Markers: The
 * list-style-position property"). core/layout/box_tree.h yields a marker member for exactly the items this
 * answers true for, and walks classify it through core/layout/block_flow.h by the position below.
 *
 * It is asked of any element: css-lists-3 §3.1 "The ::marker Pseudo-Element" says "Marker boxes only exist for
 * list items: on any other element, the ::marker pseudo-element’s content property must compute to none, which
 * suppresses its creation." So the css-display-3 §2.3 "Generating Marker Boxes: the list-item keyword" test
 * lives here, distinct from the principal-box `list-item` tests in block_flow.c and used_value.c.
 *
 * Existence needs no counter-style symbol: css-counter-styles-3 §5 "Extending list-style-type, counter(), and
 * counters()" says an unknown <counter-style-name> "must act identically to the decimal counter style", so
 * every `list-style-type` but `none` defines a marker string. A `list-style-image` the engine has not fetched
 * is neither valid nor invalid (css-images-3 §2 "Image Values: the <image> type"; an invalid one "is treated as
 * none"), so that arm crashes until image fetching is built. §3.2's contents arms are not answered here. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LIST_MARKER_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LIST_MARKER_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* Whether `el`'s ::marker generates a box: false for a non-list-item and for a list item on §3.2's last arm
   ("The marker box has no contents and ::marker does not generate a box"), true on one of its first three. */
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
   `list_marker_box_generated(list_item)` must be true; asserted. */
ListMarkerPosition list_marker_position(lxb_dom_element_t *list_item);

#endif
