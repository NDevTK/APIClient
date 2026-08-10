/* DOM §4.9's ATTRIBUTE LIST — the raw operations, and the only place they exist.
 *
 * This is the layer BELOW the per-flow chokepoint, and it is its own file for the reason dom_cow.c is: it is
 * the one place allowed to call Lexbor's attribute mutators, so `engine/check_dom_chokepoint.mjs` can ban them
 * everywhere else. Put in attr.c beside the Attr interface, the ban would have had to exempt a component file,
 * and the next `lxb_dom_element_set_attribute` written there would have been legal.
 *
 * AN ATTRIBUTE'S IDENTITY IS (NAMESPACE, LOCAL NAME) — see attr_list.h. */
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/attr_list.h"
#include "core/dom/node.h"   /* node_wrap_forget — a destroyed Attr hands back its wrapper */

/* lexbor's own, exported but absent from its headers: it appends the namespace, splits `prefix:local`, and sets
   local name, qualified name and prefix together — which is exactly the shape §4.9 stores after "validate and
   extract" has run. Declared here rather than reimplemented, so there is one place that builds an attribute's
   name and it is the one lexbor's parser uses. */
lxb_status_t lxb_dom_attr_set_name_ns(lxb_dom_attr_t *attr, const lxb_char_t *link, size_t link_length,
                                      const lxb_char_t *name, size_t name_length, bool to_lowercase);

const lxb_char_t *dom_attr_ns(const lxb_dom_attr_t *a, size_t *len)
{
    *len = 0;
    if (!a || a->node.ns == LXB_NS__UNDEF) return NULL;
    return lxb_ns_by_id(a->node.owner_document->ns, a->node.ns, len);
}

const lxb_char_t *dom_attr_prefix(const lxb_dom_attr_t *a, size_t *len)
{
    const lxb_ns_prefix_data_t *d;

    *len = 0;
    if (!a || a->node.prefix == LXB_NS__UNDEF) return NULL;
    d = lxb_ns_prefix_data_by_id(a->node.owner_document->ns, a->node.prefix);
    if (!d) return NULL;
    *len = d->entry.length;
    return lexbor_hash_entry_str(&d->entry);
}

/* Do these two namespaces match? Both NULL is the null namespace matching itself, which §4.9 relies on far more
   than any other case: every attribute an HTML page sets is in it. */
static bool ns_eq(const char *want, const lxb_char_t *have, size_t have_len)
{
    if (!want) return have == NULL;
    if (!have) return false;
    return strlen(want) == have_len && memcmp(want, have, have_len) == 0;
}

lxb_dom_attr_t *dom_attr_get_ns(lxb_dom_element_t *el, const char *ns, const char *local)
{
    lxb_dom_attr_t *a;
    size_t want = local ? strlen(local) : 0, llen = 0, nlen = 0;

    if (!el || !local) return NULL;
    for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        const lxb_char_t *l = lxb_dom_attr_local_name(a, &llen);
        if (!l || llen != want || memcmp(l, local, want) != 0) continue;
        if (ns_eq(ns, dom_attr_ns(a, &nlen), nlen)) return a;
    }
    return NULL;
}

lxb_dom_attr_t *dom_attr_get_qname(lxb_dom_element_t *el, const char *qname)
{
    lxb_dom_attr_t *a;
    size_t want = qname ? strlen(qname) : 0, qlen = 0;

    if (!el || !qname) return NULL;
    for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        const lxb_char_t *q = lxb_dom_attr_qualified_name(a, &qlen);
        if (q && qlen == want && memcmp(q, qname, want) == 0) return a;
    }
    return NULL;
}

void dom_attr_write(lxb_dom_element_t *el, const char *ns, const char *prefix, const char *local,
                    const char *val, size_t val_len)
{
    lxb_dom_attr_t *a;
    lxb_status_t st;

    DCHECK(el && local, "an attribute write with no element or no local name");
    DCHECK(!prefix || ns, "an attribute was given a PREFIX and the null namespace — §4.9's \"validate and "
                          "extract\" throws NamespaceError for that pair, so it must never reach the tree");
    a = dom_attr_get_ns(el, ns, local);
    if (!a) {
        a = lxb_dom_attr_interface_create(lxb_dom_interface_node(el)->owner_document);
        CHECK(a != NULL, "dom-attr-oom: an attribute could not be created");
        if (ns) {
            /* The qualified name is what lexbor's name builder takes, and it is the only thing the prefix is
               carried in — so it is assembled here and nowhere else. */
            size_t pl = prefix ? strlen(prefix) : 0, ll = strlen(local);
            char *q = malloc(pl + (pl ? 1 : 0) + ll + 1);
            CHECK(q != NULL, "dom-attr-oom: an attribute's qualified name could not be assembled");
            if (pl) { memcpy(q, prefix, pl); q[pl] = ':'; }
            memcpy(q + pl + (pl ? 1 : 0), local, ll);
            q[pl + (pl ? 1 : 0) + ll] = 0;
            st = lxb_dom_attr_set_name_ns(a, (const lxb_char_t *)ns, strlen(ns),
                                          (const lxb_char_t *)q, pl + (pl ? 1 : 0) + ll, false);
            free(q);
        } else {
            st = lxb_dom_attr_set_name(a, (const lxb_char_t *)local, strlen(local), false);
        }
        CHECK(st == LXB_STATUS_OK, "dom-attr-oom: an attribute's name could not be interned");
        /* lexbor's append keys its id/class CACHES on the local name alone, so appending a NAMESPACED `id`
           would destroy the element's real, null-namespace `id` attribute. Nothing can reach this yet —
           setAttributeNS is not built — and this is what makes building it come with the cache fix rather than
           a silent tree corruption discovered three layers away. */
        DCHECK(!ns || (strcmp(local, "id") != 0 && strcmp(local, "class") != 0),
               "a NAMESPACED id/class attribute reached the attribute list — lexbor's id/class caches are keyed "
               "on the local name alone and would destroy the element's null-namespace one: key those caches on "
               "the full (namespace, local name) identity first");
        lxb_dom_element_attr_append(el, a);
    }
    st = lxb_dom_attr_set_value(a, (const lxb_char_t *)val, val_len);
    CHECK(st == LXB_STATUS_OK, "dom-attr-oom: an attribute's value could not be stored");
}

void dom_attr_erase(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    lxb_dom_attr_t *a = dom_attr_get_ns(el, ns, local);

    if (!a) return;
    /* AN Attr IS A WRAPPED NODE, so a removal that frees it leaves the identity map naming freed memory — the
       same use-after-free `dom_forget_wrappers` exists to prevent for elements, and the reason removal cannot
       be lexbor's `remove_attribute` (which destroys without telling anyone). A page that holds the Attr from
       `el.attributes.href` across `el.removeAttribute("href")` is all it takes. */
    node_wrap_forget(ctx, lxb_dom_interface_node(a));
    lxb_dom_element_attr_remove(el, a);   /* clears the element's id/class cache when it named this one */
    DCHECK(el->attr_id != a && el->attr_class != a,
           "an attribute was unlinked and the element's id/class cache still names it");
    lxb_dom_attr_interface_destroy(a);
}

/* THE PARSER'S ATTRIBUTES ARE IN THE NULL NAMESPACE, AND LEXBOR'S ARE NOT.
 *
 * HTML tree construction puts every attribute it creates in the null namespace, and moves exactly three
 * families out of it — `xml:*`, `xmlns*` and `xlink:*` on foreign content, which is what "adjust foreign
 * attributes" is for. Lexbor instead stamps every parsed attribute with the ELEMENT's namespace and then lets
 * its adjust hook overwrite the three families, so on a plain HTML page every attribute came out in the XHTML
 * namespace: `attr.namespaceURI` answered a URI where the spec says null, and `getAttributeNS(null, "id")`
 * matched nothing on a document full of ids.
 *
 * IT CANNOT BE FIXED AT THE READ, and that is the whole reason this is a pass over the tree. After the fact,
 * a parsed attribute lexbor stamped with the element's namespace and a scripted `setAttributeNS(XHTML, …)` are
 * the same two fields with the same two values — the difference is WHERE THEY CAME FROM, which only exists at
 * the boundary. So the correction happens once, on the tree the parser just built, and everything the engine
 * creates afterwards is already right by construction because attr_write is the only thing that creates one.
 *
 * `ns == the element's ns` IS EXACTLY "the adjust hook did not move it": the hook is the only writer that sets
 * an attribute's namespace to anything else, and it always sets one of the three that differ from the element's.
 * No C recursion — depth here is the page's data. */
void dom_attr_normalize_parsed(lxb_dom_node_t *root)
{
    lxb_dom_node_t *n = root;

    if (!root) return;
    for (;;) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            lxb_dom_attr_t *a;
            for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a))
                if (a->node.ns == n->ns) a->node.ns = LXB_NS__UNDEF;
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n != root && !n->next) n = n->parent;
        if (n == root) return;
        n = n->next;
    }
}
