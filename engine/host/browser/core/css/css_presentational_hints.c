/* The author presentational hint origin — see css_presentational_hints.h for why it is an origin. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_presentational_hints.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
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

/* HTML §15.3.2 "The page"'s CONTAINER FRAME ELEMENT, in the section's own two conjuncts: "if the body
   element's node document's node navigable is a CHILD NAVIGABLE, and the container of that navigable is a
   FRAME OR IFRAME element, then the container frame element of the body element is that frame or iframe
   element. Otherwise, there is no container frame element."
   THE SECOND CONJUNCT IS NOT DECORATION. §7.3.1.3's navigable containers are `iframe`, `object`, `embed` and
   `fencedframe`, and §15.3.2 names two of them — so a `<body marginheight=…>`-less document inside an
   `<object>` has NO container frame element and takes the 8px default, while the same document inside an
   `<iframe marginheight=20>` takes 20. A test that only asked whether the navigable has a container would
   read an `object`'s attribute that HTML never routes here.
   NULL IS TWO DIFFERENT ANSWERS AND ONLY ONE OF THEM IS A GAP. There is genuinely no container frame element
   for a top-level traversable and for a container HTML does not name, and §15.3.2 states the default for
   exactly those. A container that EXISTS and cannot be reached is the third thing, and it crashes: §7.3.1.3
   gives every child navigable a container, so a child navigable whose container reads back null is one whose
   container element lives in another agent — the cross-origin case §15.3.2's own note says is intentional
   ("a page can change the margins of another page, including one from another origin"). */
static lxb_dom_element_t *hint_container_frame_element(const lxb_dom_node_t *doc)
{
    JSContext *ctx;
    JSValue container;
    lxb_dom_node_t *n;
    const lxb_char_t *tag;
    size_t taglen = 0;

    if (!document_is_child_navigable(doc)) return NULL;
    ctx = document_active_realm_of(doc);
    DCHECK(ctx != NULL && JS_IsObject(document_window_proxy(ctx)),
           "§15.3.2's container-frame walk found a document that IS a child navigable and has no realm or no "
           "WindowProxy — the predicate above establishes both before answering true, so the two tests have "
           "come apart");
    container = window_proxy_container(ctx, document_window_proxy(ctx));
    if (JS_IsNull(container)) {
        JS_FreeValue(ctx, container);
        DFAIL("HTML §15.3.2 \"The page\"'s THIRD SOURCE for a body margin is the body element's CONTAINER "
              "FRAME ELEMENT's `marginheight`/`marginwidth` attribute, this document's navigable IS a child "
              "navigable — so §7.3.1.3 \"Child navigables\" gives it a container — and that container reads "
              "back NULL. THE REVERSE EDGE IS NOT WHAT IS MISSING: `window_proxy_container` answers it, "
              "recorded by create-a-new-child-navigable and confirmed against the element's own content "
              "navigable so it is per-flow. What is missing is a container in ANOTHER AGENT: a cross-origin "
              "parent holds the `iframe` element in a different WASM instance, so there is no wrapper in this "
              "heap to read an attribute off. HTML §15.3.2's own note says that read is intentional rather "
              "than an oversight — \"a page can change the margins of another page (including one from another "
              "origin)\" — and answering 8px here is a REAL NUMBER for a page whose embedder wrote "
              "`<iframe marginheight=20>`, with nothing to say the attribute was never read. BUILD the "
              "SUSPEND-AT-THE-BOUNDARY read (CLAUDE.md §Security: a synchronous cross-instance read is a "
              "suspend point) and ask the peer for the container's attribute");
        return NULL;
    }
    n = node_of(container);
    JS_FreeValue(ctx, container);
    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§7.3.1.3's container of a navigable is a NAVIGABLE CONTAINER ELEMENT, and the wrapper this "
           "navigable holds is not an element — the create that recorded it is handed the element itself, so "
           "anything else is a slot written by something that is not that algorithm");
    /* The node is owned by the parent document's tree, not by the wrapper, so it outlives the reference freed
       above — an element that is a navigable's container is by definition still in that tree. */
    tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &taglen);
    DCHECK(tag != NULL, "a navigable container element has no local name");
    if (taglen == 6 && memcmp(tag, "iframe", 6) == 0) return lxb_dom_interface_element(n);
    if (taglen == 5 && memcmp(tag, "frame", 5) == 0) return lxb_dom_interface_element(n);
    return NULL;
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
        /* §15.3.2's THREE SOURCES PER PROPERTY, in the table's own order: the body element's own attribute,
           its legacy spelling, and the body element's CONTAINER FRAME ELEMENT's attribute — which carries the
           first spelling again, on a different element. One loop, because "the FIRST attribute that exists
           maps to the pixel length property" is one rule over all three and a third source tested outside the
           loop would be that rule written twice.
           THE THIRD SOURCE IS RESOLVED ONLY WHEN IT IS REACHED, which is what keeps its cross-origin crash
           where the spec puts the read: a page inside a cross-origin frame that writes `<body marginheight=0>`
           has its answer from the first source and must never pay for a container it does not consult. */
        for (k = 0; k < 3; k++) {
            const char *attr = k == 1 ? PAGE_MARGIN[i].legacy : PAGE_MARGIN[i].own;
            lxb_dom_element_t *src = el;
            const lxb_char_t *v, *digits;
            size_t vlen = 0, dlen = 0;

            if (k == 2) {
                src = hint_container_frame_element(doc);
                /* "Otherwise, there is no container frame element" — so there is no third attribute to be the
                   first that exists, and §15.3.2's default is the answer. */
                if (src == NULL) break;
            }
            v = lxb_dom_element_get_attribute(src, (const lxb_char_t *)attr, strlen(attr), &vlen);
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
        /* "If none of the attributes for a property are found, or if the value of the attribute that was
           found cannot be parsed successfully, then a default value of 8px is expected to be used." */
        return hint_strdup(PAGE_MARGIN_DEFAULT);
    }
    /* Every other hint HTML defines is a row this file does not have yet, and a property no row names is not
       declared by this origin — which is a different statement from being declared with a default. */
    return NULL;
}
