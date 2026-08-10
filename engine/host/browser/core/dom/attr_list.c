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
#include <lexbor/dom/interfaces/attr_const.h>   /* LXB_DOM_ATTR_ID / _CLASS — the interned ids lexbor's own
                                                   readers compare against, so the cache below agrees with them */
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

/* §4.9 "APPEND AN ATTRIBUTE" STEPS 1 AND 2 — and the ELEMENT'S ID/CLASS CACHE, which is why this is written
 * here instead of delegated to `lxb_dom_element_attr_append`.
 *
 * Lexbor's append keys those two caches on the LOCAL NAME ALONE (`attr->node.local_name == LXB_DOM_ATTR_ID`),
 * and on a hit it REMOVES AND DESTROYS whatever the cache already named. So appending an XLink `id` would have
 * destroyed the element's real, null-namespace `id` attribute — the page's own `id` silently gone, its
 * `getElementById` entry with it, at a call the page made about a completely different attribute.
 *
 * DOM decides it, not the tree: "an `A` attribute" is §4.9's three-part definition — local name `A`, namespace
 * null AND namespace prefix null — and the ID change steps fire only "if localName is id, namespace is null".
 * So the cache slot names the (null namespace, `id`) attribute and a namespaced one never touches it. That IS
 * keying on the full identity: a cache over a one-member key space whose member is (null, "id").
 *
 * The destroy arm is gone rather than reproduced: the only caller is dom_attr_write, which appends only when
 * `dom_attr_get_ns` found no attribute with that identity, so a second (null, "id") is unreachable — and
 * (namespace, local name) is §4.9's UNIQUENESS key on the list, which is what makes that a DCHECK. */
void dom_attr_attach(lxb_dom_element_t *el, lxb_dom_attr_t *a, lxb_dom_attr_t *before)
{
    lxb_dom_document_t *doc = lxb_dom_interface_node(el)->owner_document;
    bool null_ns = a->node.ns == LXB_NS__UNDEF;

    DCHECK(a->owner == NULL, "an attribute already on an element was appended to one — §4.9's \"set an "
                             "attribute\" throws InUseAttributeError for exactly that, so it never reaches here");
    DCHECK(a->node.owner_document == lxb_dom_interface_node(el)->owner_document,
           "an attribute from ANOTHER document was appended — §4.9's \"append an attribute\" step 3 sets the "
           "attribute's node document to the element's, and lexbor allocates a node out of its document's own "
           "arena, so the pointer cannot simply be reassigned: build §4.5's adopt for an Attr");
    DCHECK(!before || before->owner == el, "an attribute was inserted before one that is not on this element");
    DCHECK(!null_ns || a->node.prefix == LXB_NS__UNDEF,
           "an attribute carries a prefix and the null namespace — \"validate and extract\" step 8 throws "
           "NamespaceError for that pair, so it is unconstructible through any API");
    if (null_ns && a->node.local_name == LXB_DOM_ATTR_ID) {
        DCHECK(el->attr_id == NULL, "a second null-namespace `id` reached the attribute list — (namespace, "
                                    "local name) is §4.9's uniqueness key, so the append had to have found it");
        el->attr_id = a;
    } else if (null_ns && a->node.local_name == LXB_DOM_ATTR_CLASS) {
        DCHECK(el->attr_class == NULL, "a second null-namespace `class` reached the attribute list — see `id`");
        el->attr_class = a;
    }
    a->prev = before ? before->prev : el->last_attr;
    a->next = before;
    if (a->prev) a->prev->next = a; else el->first_attr = a;
    if (a->next) a->next->prev = a; else el->last_attr = a;
    a->owner = el;                                    /* step 2: the attribute's element */
    if (doc->node_cb->insert != NULL) doc->node_cb->insert(lxb_dom_interface_node(a));
}

/* §4.9 "REMOVE AN ATTRIBUTE" STEPS 2 AND 3 — unlink it from its element's list, and set its element to null.
 *
 * THE NODE SURVIVES, and that is the whole algorithm: §9.4.7 clears `element` and NOTHING ELSE (the removed
 * attribute keeps its namespace, prefix, local name, value and even its node document). `removeAttributeNode`
 * and `removeNamedItem` RETURN that attribute, so destroying it here is not an optimisation, it is a different
 * algorithm — one that hands the page back a neutered wrapper whose `name` and `value` answer undefined.
 * WHO FREES IT is then the delta's question, not this one's: an attribute a flow created is a CREATION in the
 * per-flow COW sense and is destroyed when that delta is discarded, exactly like a node the flow made. */
void dom_attr_detach(lxb_dom_attr_t *a)
{
    lxb_dom_element_t *el;

    DCHECK(a != NULL, "an attribute detach was asked for no attribute");
    el = a->owner;
    if (!el) return;   /* §9.4.7 over an attribute already detached — its element is already null */
    if (el->attr_id == a) el->attr_id = NULL;
    else if (el->attr_class == a) el->attr_class = NULL;
    if (a->prev) a->prev->next = a->next; else el->first_attr = a->next;
    if (a->next) a->next->prev = a->prev; else el->last_attr = a->prev;
    a->prev = a->next = NULL;
    a->owner = NULL;                                  /* step 3 */
    DCHECK(el->attr_id != a && el->attr_class != a,
           "an attribute was unlinked and the element's id/class cache still names it");
}

/* §4.9 "REPLACE AN ATTRIBUTE" STEPS 2 TO 5 — in place, so the POSITION in the list is preserved and with it
   `NamedNodeMap` index order. That is the whole difference between this and a detach followed by an append,
   and it is observable: `el.attributes[0]` after `el.setAttributeNode(replacementForTheFirstOne)` is still the
   first attribute. Step 6's ONE change notification belongs to the chokepoint, not here. */
void dom_attr_replace(lxb_dom_attr_t *old_a, lxb_dom_attr_t *new_a)
{
    lxb_dom_element_t *el;

    DCHECK(old_a && new_a, "an attribute replace was asked for no attribute");
    DCHECK(old_a->owner != NULL, "an attribute not on any element was replaced — §4.9's \"replace an "
                                 "attribute\" step 1 reads the OLD attribute's element and steps 3/4 write it "
                                 "onto the new one, so there is nothing to replace it in");
    DCHECK(new_a->owner == NULL, "an attribute already on an element was used as a replacement — §4.9's \"set "
                                 "an attribute\" step 2 throws InUseAttributeError for exactly that");
    DCHECK(new_a->node.owner_document == old_a->node.owner_document,
           "an attribute from ANOTHER document replaced one — §4.9's \"replace an attribute\" step 4 sets the "
           "new attribute's node document to the element's, and lexbor allocates a node out of its document's "
           "own arena: build §4.5's adopt for an Attr");
    DCHECK(old_a->node.ns == new_a->node.ns && old_a->node.local_name == new_a->node.local_name,
           "an attribute was replaced by one with a different (namespace, local name) — \"set an attribute\" "
           "step 3 finds the old one BY that identity, so the two match by construction");
    el = old_a->owner;
    if (el->attr_id == old_a) el->attr_id = new_a;
    else if (el->attr_class == old_a) el->attr_class = new_a;
    new_a->prev = old_a->prev;
    new_a->next = old_a->next;
    if (new_a->prev) new_a->prev->next = new_a; else el->first_attr = new_a;
    if (new_a->next) new_a->next->prev = new_a; else el->last_attr = new_a;
    new_a->owner = el;                                /* step 3 */
    old_a->prev = old_a->next = NULL;
    old_a->owner = NULL;                              /* step 5 */
}

void dom_attr_destroy(JSContext *ctx, lxb_dom_attr_t *a)
{
    DCHECK(a != NULL, "an attribute destroy was asked for no attribute");
    DCHECK(a->owner == NULL, "an attribute still ON an element was destroyed — it is reachable from the "
                             "document, so the free would leave the element's list naming freed memory");
    /* AN Attr IS A WRAPPED NODE, so a free that does not tell the identity map leaves it naming freed memory —
       the same use-after-free `dom_forget_wrappers` exists to prevent for elements, and what a pool allocator
       turns into the next attribute inheriting this one's wrapper. */
    node_wrap_forget(ctx, lxb_dom_interface_node(a));
    lxb_dom_attr_interface_destroy(a);
}

/* §4.9 "GET AN ATTRIBUTE BY NAME", BOTH STEPS — and step 1 is the one that was missing.
 *
 * "If element is in the HTML namespace and its node document is an HTML document, set qualifiedName to
 * qualifiedName in ASCII lowercase." Without it `el.setAttribute("x", 1); el.removeAttribute("X")` left the
 * attribute in place: setAttribute lowercases in its OWN step 2 and stores `x`, and the removal resolved `X`
 * here by an exact match and found nothing to remove. Every by-name entry point that reaches the tree through
 * the chokepoint — removeAttribute, toggleAttribute, removeNamedItem, the taint read — asks this one function,
 * so the step belongs here rather than in each of them.
 *
 * TWO TERMS, NOT ONE, and the same two everywhere §4.9 states them: an `<svg>`'s `<circle>` child in an HTML
 * document is in the SVG namespace and keeps its capitals, and a `<div>` in an XML document keeps its too.
 * ASCII lowercase is Infra's — U+0041 to U+005A and nothing else, never a locale `tolower`. No fixed buffer: a
 * name's length is the page's data, and the allocation only happens when there is an upper alpha to fold. */
lxb_dom_attr_t *dom_attr_get_qname(lxb_dom_element_t *el, const char *qname)
{
    lxb_dom_attr_t *a;
    char *lowered = NULL;
    size_t want = qname ? strlen(qname) : 0, qlen = 0, i;

    if (!el || !qname) return NULL;
    if (lxb_dom_interface_node(el)->ns == LXB_NS_HTML &&
        lxb_dom_interface_node(el)->owner_document->type == LXB_DOM_DOCUMENT_DTYPE_HTML) {   /* step 1 */
        for (i = 0; i < want; i++) {
            if (qname[i] < 'A' || qname[i] > 'Z') continue;
            lowered = malloc(want + 1);
            CHECK(lowered != NULL, "dom-attr-oom: a qualified name could not be ASCII-lowercased");
            memcpy(lowered, qname, want + 1);
            for (; i < want; i++)
                if (lowered[i] >= 'A' && lowered[i] <= 'Z') lowered[i] = (char)(lowered[i] - 'A' + 'a');
            qname = lowered;
            break;
        }
    }
    for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        const lxb_char_t *q = lxb_dom_attr_qualified_name(a, &qlen);
        /* step 2: THE FIRST attribute wearing this qualified name. An element's list may legitimately hold two
           of them — a parser-produced XLink `href` and the null-namespace `xlink:href` setAttribute creates —
           and "the first" is the standard's own tiebreak for exactly that. */
        if (q && qlen == want && memcmp(q, qname, want) == 0) { free(lowered); return a; }
    }
    free(lowered);
    return NULL;
}

lxb_dom_attr_t *dom_attr_create(lxb_dom_document_t *doc, const char *ns, const char *prefix, const char *local)
{
    lxb_dom_attr_t *a;
    lxb_status_t st;

    DCHECK(doc && local, "an attribute was created with no document or no local name");
    DCHECK(!prefix || ns, "an attribute was given a PREFIX and the null namespace — §4.9's \"validate and "
                          "extract\" throws NamespaceError for that pair, so it must never reach the tree");
    a = lxb_dom_attr_interface_create(doc);
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
    /* §4.9.2 "create an attribute" step 2 sets the VALUE, and its default is THE EMPTY STRING. §9.1 lists an
       attribute's value as "a string", so a value-less attribute is a state the model does not have — lexbor
       leaves the field NULL until something writes it, and every reader would then have to carry that case. */
    dom_attr_set_value(a, "", 0);
    return a;
}

void dom_attr_set_value(lxb_dom_attr_t *a, const char *val, size_t val_len)
{
    lxb_status_t st;

    DCHECK(a != NULL, "an attribute value was written to no attribute");
    st = lxb_dom_attr_set_value(a, (const lxb_char_t *)val, val_len);
    CHECK(st == LXB_STATUS_OK, "dom-attr-oom: an attribute's value could not be stored");
}

lxb_dom_attr_t *dom_attr_write(lxb_dom_element_t *el, const char *ns, const char *prefix, const char *local,
                               const char *val, size_t val_len, bool *created)
{
    lxb_dom_attr_t *a;

    DCHECK(el && local, "an attribute write with no element or no local name");
    a = dom_attr_get_ns(el, ns, local);
    if (created) *created = (a == NULL);
    if (!a) {
        a = dom_attr_create(lxb_dom_interface_node(el)->owner_document, ns, prefix, local);
        dom_attr_attach(el, a, NULL);
    }
    dom_attr_set_value(a, val, val_len);
    return a;
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
