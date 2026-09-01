/* THE SUBJECT OF A LAYOUT CRASH. See box_subject.h for why it is a component, for why nothing here may assert,
   for the ownership contract, and for the one residual this file's totality rests on.
   THERE IS NO `#include "check.h"` IN THIS FILE AND THAT IS THE MECHANISM, not an omission: a helper a
   `DFAILF` calls must not be able to abort, and a file that cannot name the macro cannot grow one later. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_computed_value.h"
#include "core/layout/box_subject.h"

const char *box_subject_computed(lxb_dom_element_t *el, const char *name, char *buf, size_t cap)
{
    char *v;
    int n;

    if (buf == NULL || cap == 0) return "(nowhere to compose a computed value)";
    if (el == NULL) return "(no element)";
    v = css_computed_value(el, name);
    if (v == NULL) return "(no computed value)";
    n = snprintf(buf, cap, "%s", v);
    free(v);
    return n < 0 ? "(computed value unavailable)" : buf;
}

const char *box_subject(lxb_dom_element_t *el, char *buf, size_t cap)
{
    char dbuf[64];
    size_t len = 0;
    const lxb_char_t *tag;
    int n;

    if (buf == NULL || cap == 0) return "(nowhere to compose a box name)";
    if (el == NULL) return "(no element)";
    tag = lxb_dom_element_local_name(el, &len);
    if (len > 128) len = 128;
    n = tag == NULL
        ? snprintf(buf, cap, "(no local name) (display `%s`)",
                   box_subject_computed(el, "display", dbuf, sizeof dbuf))
        : snprintf(buf, cap, "<%.*s> (display `%s`)", (int) len, (const char *) tag,
                   box_subject_computed(el, "display", dbuf, sizeof dbuf));
    return n < 0 ? "(box name unavailable)" : buf;
}

const char *box_subject_node(lxb_dom_node_t *n, char *buf, size_t cap)
{
    if (n == NULL) return "(no node)";
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) return box_subject(lxb_dom_interface_element(n), buf, cap);
    if (n->type == LXB_DOM_NODE_TYPE_TEXT) return "(text node)";
    if (n->type == LXB_DOM_NODE_TYPE_COMMENT) return "(comment node)";
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) return "(the document)";
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) return "(a document fragment)";
    return "(non-element node)";
}
