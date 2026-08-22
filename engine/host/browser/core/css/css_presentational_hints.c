/* The author presentational hint origin — see css_presentational_hints.h for why it is an origin. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_presentational_hints.h"
#include "core/dom/document.h"
#include "core/frame/window_proxy.h"

static char *hint_strdup(const char *s)
{
    char *out = strdup(s);

    CHECK(out != NULL, "cssom: OOM building a presentational hint's value — a dropped one reads as the property "
                       "being undeclared by this origin, which for a body margin is the initial value 0 and not "
                       "the 8px HTML §15.3.2 gives a page with no margin attributes at all");
    return out;
}

/* HTML's RULES FOR PARSING NON-NEGATIVE INTEGERS, which §15.2's "maps to the pixel length property" is stated
   over: the rules for parsing integers, and then an error for a negative result. The integer rules skip leading
   ASCII whitespace, accept one optional sign, require an ASCII DIGIT next, and then "collect a sequence of code
   points that are ASCII digits" — so TRAILING CONTENT IS NOT AN ERROR and `marginwidth="10px"` is ten.
   WHAT COMES BACK IS THE DIGIT RUN, not a number, and that is deliberate: "interpret the resulting sequence as
   a base-ten integer" has no upper bound, so accumulating into a C integer would need a range check, and a
   range check that rejects is a CAP on what a page may write. The digits ARE the value — the property this maps
   to is a `<length>`, and the caller writes them straight into one. Leading zeros are dropped so the specified
   value serializes canonically; `*plen` is at least 1 on success. */
static bool html_parse_non_negative_integer(const lxb_char_t *s, size_t n, const lxb_char_t **pd, size_t *plen)
{
    size_t i = 0, start;

    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) i++;
    if (i == n) return false;
    if (s[i] == '-') return false;   /* "if value is less than zero, return an error" */
    if (s[i] == '+') i++;
    if (i == n || s[i] < '0' || s[i] > '9') return false;
    start = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    while (start + 1 < i && s[start] == '0') start++;
    *pd = s + start;
    *plen = i - start;
    return true;
}

/* §15.3.2's TABLE, entire. Its two rows each name a pair of properties and the attributes that source them, IN
   THE ORDER "the first attribute that exists" reads them. The table's THIRD source is the same ATTRIBUTE NAME
   as the first — `marginheight` for the margins of the block axis, `marginwidth` for the inline one — read off
   a different ELEMENT, the body's container frame element, which is why there is no third column here and why
   the third source is a question about a NAVIGABLE rather than about a name. */
static const struct {
    const char *properties[2];
    const char *own;       /* the body element's own attribute, and the container frame element's */
    const char *legacy;    /* the body element's legacy alias for it */
} PAGE_MARGIN[] = {
    { { "margin-top",  "margin-bottom" }, "marginheight", "topmargin"  },
    { { "margin-left", "margin-right"  }, "marginwidth",  "leftmargin" },
};

/* "If none of the attributes for a property are found, or if the value of the attribute that was found cannot
   be parsed successfully, then a default value of 8px is expected to be used for that property instead." It is
   the DEFAULT OF THE HINT and not a UA rule, and the difference is visible in both directions: an author
   `body { margin: 0 }` overrides it because the author origin outranks this one, and `<body marginheight=20>`
   replaces it because it is the same origin's own first source. A UA-origin row would lose the first and ignore
   the second. IT IS ALSO THE 16px that makes `document.body`'s used width 1264 in a 1280 viewport. */
static const char PAGE_MARGIN_DEFAULT[] = "8px";

/* IS THIS DOCUMENT'S NODE NAVIGABLE A CHILD NAVIGABLE — §15.3.2's own condition for there being a container
   frame element at all ("if the body element's node document's node navigable is a child navigable, and the
   container of that navigable is a frame or iframe element"). It is asked of the realm whose ACTIVE document
   this is, which is NULL for a document that has no navigable (a `createHTMLDocument`, a DOMParser parse, an
   XHR `responseXML`), and those have no container by definition. */
static bool document_is_child_navigable(const lxb_dom_node_t *doc)
{
    JSContext *ctx = document_active_realm_of(doc);
    JSValue parent;
    bool child;

    if (!ctx) return false;
    /* A realm whose navigable has no WindowProxy yet is one nothing has asked a navigable question of, and a
       navigable with no proxy has no parent to be a child of. */
    if (!JS_IsObject(document_window_proxy(ctx))) return false;
    parent = window_proxy_parent_navigable(ctx, document_window_proxy(ctx));
    child = !JS_IsUndefined(parent);
    JS_FreeValue(ctx, parent);
    return child;
}

char *css_presentational_hint(lxb_dom_element_t *el, const char *name)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    const lxb_char_t *tag;
    size_t taglen = 0;
    unsigned i, k;

    DCHECK(el != NULL && name != NULL,
           "the presentational hint origin was asked to declare with no element or no property name");
    for (i = 0; i < sizeof(PAGE_MARGIN) / sizeof(PAGE_MARGIN[0]); i++) {
        lxb_dom_node_t *doc;

        if (strcmp(PAGE_MARGIN[i].properties[0], name) != 0 &&
            strcmp(PAGE_MARGIN[i].properties[1], name) != 0)
            continue;
        /* "given a body element" — §4.3.1's body element OF ITS DOCUMENT, which is what a page means by one: a
           `<body>` left detached, a second one an author wrote, and the `<frameset>` document_body_of also
           answers for are none of them that element. */
        tag = lxb_dom_element_local_name(el, &taglen);
        if (!tag || taglen != 4 || memcmp(tag, "body", 4) != 0) return NULL;
        if (!node->owner_document) return NULL;
        doc = lxb_dom_interface_node(node->owner_document);
        if (document_body_of(doc) != node) return NULL;
        for (k = 0; k < 2; k++) {
            const char *attr = k == 0 ? PAGE_MARGIN[i].own : PAGE_MARGIN[i].legacy;
            const lxb_char_t *v, *digits;
            size_t vlen = 0, dlen = 0;

            v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)attr, strlen(attr), &vlen);
            if (!v) continue;
            /* "The FIRST attribute that exists maps to the pixel length property", so the search stops at the
               one that is PRESENT whether or not it parses, and an unparseable one falls to the default rather
               than on to the next source. */
            if (!html_parse_non_negative_integer(v, vlen, &digits, &dlen))
                return hint_strdup(PAGE_MARGIN_DEFAULT);
            {
                char *out = malloc(dlen + 3);

                CHECK(out != NULL, "cssom: OOM building a presentational hint's pixel length");
                memcpy(out, digits, dlen);
                memcpy(out + dlen, "px", 3);
                return out;
            }
        }
        DCHECK(!document_is_child_navigable(doc),
               "§15.3.2's THIRD SOURCE for a body margin is the body element's CONTAINER FRAME ELEMENT's "
               "`marginheight`/`marginwidth` attribute, and this document's navigable IS a child navigable, so "
               "it has one. This engine cannot ask it: a WindowProxy carries no container element — the same "
               "gap navigable.c names from the other side when it cannot re-snapshot §7.4.2.1's iframe "
               "sandboxing flag set. Answering 8px here is a REAL NUMBER for a page whose embedder wrote "
               "`<iframe marginheight=20>`, with nothing to say the attribute was never read. BUILD the "
               "navigable -> container element link: §4.8.5's create-a-child-navigable has the element in hand, "
               "and the CROSS-ORIGIN case is a read of another agent's DOM, which §15.3.2's own note says is "
               "intentional (`a page can change the margins of another page, including one from another "
               "origin`) and which the suspend-at-the-boundary primitive is for");
        return hint_strdup(PAGE_MARGIN_DEFAULT);
    }
    /* Every other hint HTML defines is a row this file does not have yet, and a property no row names is not
       declared by this origin — which is a different statement from being declared with a default. */
    return NULL;
}
