/* HTML §4.2.5.1 "Standard metadata names"' `referrer`, steps 1 to 5 and the test of step 6. */
#include <string.h>
#include <strings.h>

#include "check.h"
#include "core/dom/node.h"
#include "core/fetch/fetch.h"
#include "core/html/html_meta_referrer.h"

/* Step 5's table of legacy values, and the policies themselves (Referrer Policy §3), as the literals this
   component answers with. "default" maps to "the default referrer policy", which Referrer Policy §3 makes
   "strict-origin-when-cross-origin". */
static const char *const LEGACY[][2] = {
    { "never", "no-referrer" },
    { "default", "strict-origin-when-cross-origin" },
    { "always", "unsafe-url" },
    { "origin-when-crossorigin", "origin-when-cross-origin" },
};
static const char *const POLICIES[] = {
    "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin",
    "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url",
};

const char *html_meta_referrer_policy(const lxb_dom_node_t *node)
{
    lxb_dom_element_t *meta;
    const lxb_char_t *name, *content;
    size_t name_len = 0, content_len = 0, i;
    char value[64];

    DCHECK(node != NULL, "§4.2.5.1's referrer steps were asked of no node");
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->local_name != LXB_TAG_META || node->ns != LXB_NS_HTML)
        return NULL;
    /* Step 1: "If element is not in a document tree, then return." A document tree's root is a document; a
       shadow tree's is a shadow root. */
    if (node_root((lxb_dom_node_t *)node)->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return NULL;
    meta = lxb_dom_interface_element((lxb_dom_node_t *)node);
    /* Step 2: a name attribute that is an ASCII case-insensitive match for "referrer". */
    name = lxb_dom_element_get_attribute(meta, (const lxb_char_t *)"name", 4, &name_len);
    if (!name || name_len != 8 || strncasecmp((const char *)name, "referrer", 8) != 0)
        return NULL;
    /* Step 3: "If element does not have a content attribute, or that attribute's value is the empty string,
       then return." */
    content = lxb_dom_element_get_attribute(meta, (const lxb_char_t *)"content", 7, &content_len);
    if (!content || content_len == 0)
        return NULL;
    /* Step 4: "Let value be the value of element's content attribute, converted to ASCII lowercase." A value
       longer than every policy and legacy word names none, so step 6 returns for it unread. */
    if (content_len >= sizeof value)
        return NULL;
    for (i = 0; i < content_len; i++) {
        char c = (char)content[i];
        value[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    value[content_len] = '\0';
    /* Step 5: the legacy table. */
    for (i = 0; i < sizeof LEGACY / sizeof *LEGACY; i++)
        if (!strcmp(value, LEGACY[i][0])) return LEGACY[i][1];
    /* Step 6's test: "If value is a referrer policy". The empty string is one, and step 3 has excluded it. */
    for (i = 0; i < sizeof POLICIES / sizeof *POLICIES; i++)
        if (!strcmp(value, POLICIES[i])) {
            DCHECK(fetch_is_referrer_policy(POLICIES[i]), "§4.2.5.1's policy table and Referrer Policy §3 differ");
            return POLICIES[i];
        }
    return NULL;
}
