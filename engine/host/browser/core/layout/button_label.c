/* HTML §15.5.12 "The input element as a button"'s contents. See button_label.h. */
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "core/html/html_form.h"
#include "core/layout/button_label.h"

static HtmlInputState bl_state(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);

    if (n->ns != LXB_NS_HTML || !lxb_html_tree_node_is(n, LXB_TAG_INPUT)) return INPUT_STATE_NONE;
    return html_form_input_state(n);
}

bool button_label_generated(lxb_dom_element_t *el)
{
    HtmlInputState st;

    DCHECK(el != NULL, "HTML §15.5.12's contents were asked about no element");
    st = bl_state(el);
    return st == INPUT_STATE_SUBMIT || st == INPUT_STATE_RESET || st == INPUT_STATE_BUTTON;
}

const lxb_char_t *button_label_text(lxb_dom_element_t *el, size_t *len)
{
    const lxb_char_t *v;
    HtmlInputState st;

    DCHECK(button_label_generated(el) && len != NULL,
           "HTML §15.5.12's label was read of an element that is not an input button, or with nowhere for its "
           "length");
    /* "the text of the element's value attribute, if any": presence, so `value=""` is an empty label. */
    if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"value", 5)) {
        *len = 0;
        v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"value", 5, len);
        DCHECK(v != NULL || *len == 0, "a `value` attribute answered a length with no bytes");
        return v;
    }
    st = bl_state(el);
    switch (st) {
    case INPUT_STATE_SUBMIT: *len = 6; return (const lxb_char_t *)"Submit";
    case INPUT_STATE_RESET:  *len = 5; return (const lxb_char_t *)"Reset";
    case INPUT_STATE_BUTTON: *len = 0; return NULL;
    default: break;
    }
    DFAIL("HTML §15.5.12's label was derived for a state that is not one of the section's three");
    *len = 0;
    return NULL;
}
