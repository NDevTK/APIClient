/* HTML §15.3.4 "Phrasing content"'s two line-breaking `display-outside` declarations. See phrasing_break.h for
   why the answer is HTML's rather than the cascade's, and for why the namespace is part of the test. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/layout/phrasing_break.h"

PhrasingBreak phrasing_break_of(lxb_dom_element_t *el)
{
    const lxb_dom_node_t *n;
    const lxb_char_t *tag;
    size_t len = 0;

    DCHECK(el != NULL, "HTML §15.3.4's line-breaking classification was asked for with no element");
    n = lxb_dom_interface_node(el);
    /* §15.3.4's sheet opens `@namespace "http://www.w3.org/1999/xhtml";`, so the two rules select HTML elements
       and nothing else. An SVG `br` is an ordinary inline box. */
    if (n->ns != LXB_NS_HTML) return PHRASING_BREAK_NONE;
    tag = lxb_dom_element_local_name(el, &len);
    DCHECK(tag != NULL,
           "an element has no local name — every element this engine's parsers and every `createElement` path "
           "mint is created with one, so a NULL here is a node built by neither");
    if (tag == NULL) return PHRASING_BREAK_NONE;
    if (len == 2 && memcmp(tag, "br", 2) == 0) return PHRASING_BREAK_FORCED;
    if (len == 3 && memcmp(tag, "wbr", 3) == 0) return PHRASING_BREAK_OPPORTUNITY;
    return PHRASING_BREAK_NONE;
}
