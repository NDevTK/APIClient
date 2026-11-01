/* HTML §15.5.12 "The input element as a button": an `input` in the Submit Button, Reset Button or Button state
 * "when it generates a CSS box, is expected to depict a button and use button layout and its contents are
 * expected to be the text of the element's value attribute, if any, or text derived from the element's type
 * attribute in an implementation-defined (and probably locale-specific) fashion, if not."
 *
 * Those contents are a text sequence of the element's own box, so core/layout/box_tree.h yields them as the
 * box's one member (never the element's DOM children, which are not its contents), and every walk measures and
 * paints them through core/layout/text_run.h's supplied-character entry with the element as their style. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_BUTTON_LABEL_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_BUTTON_LABEL_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* Whether `el` is an HTML `input` whose box §15.5.12 fills with a label. */
bool button_label_generated(lxb_dom_element_t *el);

/* The label's `*len` UTF-8 bytes: the `value` attribute's, or for an absent attribute the implementation-defined
   text Chrome derives ("Submit", "Reset", and nothing for the Button state). Borrowed for the life of the
   current layout job, which sees no DOM write. `button_label_generated(el)` must be true; asserted. */
const lxb_char_t *button_label_text(lxb_dom_element_t *el, size_t *len);

#endif
