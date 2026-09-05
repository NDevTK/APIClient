/* DOM §4.4 "Interface Node" — LOCATE A NAMESPACE and LOCATE A NAMESPACE PREFIX. See core/dom/node_ns.h. */
#include <string.h>

#include "check.h"
#include "core/dom/attr_list.h"
#include "core/dom/element.h"
#include "core/dom/node_ns.h"
#include "core/xml/xml_ns.h"

/* Is `a` (length `alen`, never NUL-terminated) the same string as the NUL-terminated `b`? The standard compares
   a namespace name and a prefix by CODE POINTS, and Namespaces in XML 1.0 (Third Edition) §2.3 "Comparing URI
   References" spells out that "the comparison is case-sensitive, and no %-escaping is done or undone", giving
   `http://www.example.org/wine`, `http://www.Example.org/wine` and `http://www.example.org/Wine` as three URI
   references that "are all different for the purposes of identifying namespaces, since they differ in case".
   Which is why this is a memcmp and never one of lexbor's folding compares — see core/xml/xml_ns.h for what
   those do to these very strings. */
static bool ns_eq(const char *a, size_t alen, const char *b)
{
    size_t blen;

    if (a == NULL || b == NULL) return false;
    blen = strlen(b);
    return alen == blen && memcmp(a, b, alen) == 0;
}

static bool ns_eq_n(const char *a, size_t alen, const char *b, size_t blen)
{
    if (a == NULL || b == NULL) return alen == 0 && blen == 0 && a == b;
    return alen == blen && memcmp(a, b, alen) == 0;
}

/* An element's own NAMESPACE — the standard's "its namespace", NULL for the standard's null rather than the
   empty string. THEY ARE DIFFERENT VALUES HERE and DOM §4.4 "Interface Node" is explicit about it in the very
   step below that reads a declaration: "return its value if it is not the empty string, and null otherwise". */
static const char *element_ns(lxb_dom_element_t *el, size_t *len)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    const lxb_char_t *ns;
    size_t nl = 0;

    ns = lxb_ns_by_id(n->owner_document->ns, n->ns, &nl);
    *len = nl;
    /* lexbor answers the null namespace with a zero-length string rather than a NULL, and the standard's null
       is what every step below branches on — so the two are separated HERE, once, rather than at each of the
       five sites that ask. A zero-length namespace reaching a comparison as if it were a real one is how
       `lookupNamespaceURI("p")` starts answering "" for an element in no namespace. */
    return (ns != NULL && nl != 0) ? (const char *)ns : NULL;
}

/* DOM §4.4's "parent element" — the parent IF IT IS AN ELEMENT, and null otherwise. It is NOT "the nearest
   ancestor that is an element", and the difference is observable: a Text node inside a DocumentFragment has a
   parent that is not an element, so §4.4's Otherwise arm returns null there rather than climbing past the
   fragment into whatever holds it. This engine's lookup used to climb, which made a detached subtree answer
   with its document's bindings. */
static lxb_dom_element_t *parent_element(lxb_dom_node_t *n)
{
    lxb_dom_node_t *p = n ? n->parent : NULL;

    return (p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT) ? lxb_dom_interface_element(p) : NULL;
}

/* STEP 4 OF THE ELEMENT ARM, which is one sentence with two shapes and one answer, so it is one function.
   "If it has an attribute whose namespace is the XMLNS namespace, namespace prefix is `xmlns`, and local name
   is prefix, or if prefix is null and it has an attribute whose namespace is the XMLNS namespace, namespace
   prefix is null, and local name is `xmlns`" — the first shape is `xmlns:p="…"` and the second is the default
   declaration `xmlns="…"`. `*found` says whether the sentence MATCHED, which is not the same question as what
   it RETURNED: "return its value if it is not the empty string, and null otherwise" makes `xmlns=""` a match
   whose answer is null, and that null STOPS THE WALK rather than falling through to the parent. Collapsing the
   two into "returned NULL" is how an undeclaration silently inherits the binding it exists to remove. */
static const char *element_declaration(lxb_dom_element_t *el, const char *prefix, size_t prefix_len,
                                       size_t *out_len, bool *found)
{
    lxb_dom_attr_t *a;

    *found = false;
    for (a = lxb_dom_element_first_attribute(el); a != NULL; a = lxb_dom_element_next_attribute(a)) {
        const lxb_char_t *ans, *apx, *al, *val;
        size_t anslen = 0, apxlen = 0, allen = 0, vlen = 0;

        ans = dom_attr_ns(a, &anslen);
        if (!ns_eq((const char *)ans, anslen, XML_NS_XMLNS_NAMESPACE)) continue;
        apx = dom_attr_prefix(a, &apxlen);
        al  = lxb_dom_attr_local_name(a, &allen);

        if (prefix != NULL) {
            /* `xmlns:<prefix>` — the DECLARED prefix is the attribute's LOCAL name, which is the same fact
               core/xml/xml_ns.h states about `xml_ns_declare` taking a whole QName. */
            if (!ns_eq((const char *)apx, apxlen, "xmlns")) continue;
            if (!ns_eq_n((const char *)al, allen, prefix, prefix_len)) continue;
        } else {
            /* The DEFAULT declaration, which has NO prefix and the local name `xmlns`. An `xmlns:xmlns` would
               satisfy the local-name half and is excluded by the prefix half — Namespaces in XML §3 makes that
               attribute unconstructible anyway, and this arm does not depend on that. */
            if (apx != NULL && apxlen != 0) continue;
            if (!ns_eq((const char *)al, allen, "xmlns")) continue;
        }
        *found = true;
        val = lxb_dom_attr_value(a, &vlen);
        *out_len = vlen;
        return (val != NULL && vlen != 0) ? (const char *)val : NULL;
    }
    *out_len = 0;
    return NULL;
}

/* WHICH ELEMENT A NODE ASKS — and it is ONE function because it is ONE switch. DOM §4.4 writes the interface
   dispatch twice, once inside "locate a namespace" and once inside the `lookupPrefix(namespace)` member steps,
   and the two are the same five arms with the same five answers: an Element asks itself, a Document asks its
   document element, a DocumentType or DocumentFragment asks nothing, an Attr asks its element, and anything
   else asks its parent element. Transcribing that twice is two spellings of one rule, of which the last to be
   updated is the one that is wrong. Returns NULL where the standard returns null. */
lxb_dom_element_t *node_ns_start_element(lxb_dom_node_t *node)
{
    if (node == NULL) return NULL;

    switch (node->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT:
        return lxb_dom_interface_element(node);

    /* Document — "If its document element is null, then return null." */
    case LXB_DOM_NODE_TYPE_DOCUMENT:
        return lxb_dom_document_element(lxb_dom_interface_document(node));

    /* DocumentType and DocumentFragment — "Return null." A ShadowRoot IS a DocumentFragment (DOM §4.8 makes
       `ShadowRoot : DocumentFragment`), and lexbor gives it a node type of its own, so it is named here rather
       than left to the Otherwise arm — where it would have climbed to its HOST and answered with the light
       tree's bindings, which is the shadow boundary leaking through a namespace lookup. */
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
    case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT:
    case LXB_DOM_NODE_TYPE_SHADOW_ROOT:
        return NULL;

    /* Attr — "If its element is null, then return null." A detached Attr answers null; it does NOT climb,
       because it has nothing to climb through. */
    case LXB_DOM_NODE_TYPE_ATTRIBUTE:
        return lxb_dom_interface_attr(node)->owner;

    /* Otherwise — "If this's parent element is null, then return null." ONE STEP to the parent, and only if
       that parent is an element; see parent_element. */
    default:
        return parent_element(node);
    }
}

const char *node_locate_namespace(lxb_dom_node_t *node, const char *prefix, size_t prefix_len, size_t *out_len)
{
    lxb_dom_element_t *el;

    DCHECK(out_len != NULL,
           "DOM §4.4's locate a namespace was asked for a namespace name with nowhere to write its LENGTH — "
           "the answer is a borrowed slice of the tree and is not NUL-terminated, so a caller without the "
           "length has no way to use it that is not a read past the end");
    DCHECK(prefix != NULL || prefix_len == 0,
           "DOM §4.4's locate a namespace was given the standard's NULL prefix with a non-zero length — null "
           "and the empty string are DIFFERENT arguments here (§4.4's lookupNamespaceURI step 1 maps one onto "
           "the other precisely because they are), and a length beside a null pointer says neither");
    *out_len = 0;

    /* THE ELEMENT ARM, in the standard's own order. It is a LOOP and not the recursion the standard writes,
       because step 6's recursion is a TAIL call over a chain whose length is the page's own nesting depth —
       CLAUDE.md §C-stack: a page's depth must never reach the C stack. */
    for (el = node_ns_start_element(node); el != NULL; el = parent_element(lxb_dom_interface_node(el))) {
        const char *ns, *decl;
        size_t nslen = 0;
        bool found = false;

        /* Steps 1 and 2 — the two prefixes Namespaces in XML §3 binds BY DEFINITION, which is why they are
           answered before the element is looked at at all and why they are read from core/xml/xml_ns.h rather
           than written again here. They are re-asked at every level, which costs nothing and is what the
           standard says: a recursion that reached step 6 would ask them again on the parent. */
        if (ns_eq(prefix, prefix_len, "xml")) {
            *out_len = strlen(XML_NS_XML_NAMESPACE);
            return XML_NS_XML_NAMESPACE;
        }
        if (ns_eq(prefix, prefix_len, "xmlns")) {
            *out_len = strlen(XML_NS_XMLNS_NAMESPACE);
            return XML_NS_XMLNS_NAMESPACE;
        }
        /* Step 3 — "If its namespace is non-null and its namespace prefix is prefix, then return namespace."
           A null `prefix` matches an element that HAS no prefix, which is what makes an unprefixed element in
           a namespace the holder of the default namespace at its own depth. */
        ns = element_ns(el, &nslen);
        if (ns != NULL) {
            size_t pxlen = 0;
            const char *px = element_prefix(el, &pxlen);

            if (px == NULL) pxlen = 0;
            if ((prefix == NULL && pxlen == 0) ||
                (prefix != NULL && pxlen != 0 && ns_eq_n(px, pxlen, prefix, prefix_len))) {
                *out_len = nslen;
                return ns;
            }
        }
        /* Step 4 — and its match ENDS the walk whichever way it answered. */
        decl = element_declaration(el, prefix, prefix_len, out_len, &found);
        if (found) return decl;
        /* Steps 5 and 6 — the loop's own condition. */
    }
    *out_len = 0;
    return NULL;
}

const char *node_locate_namespace_prefix(lxb_dom_element_t *element, const char *ns, size_t ns_len,
                                         size_t *out_len)
{
    lxb_dom_element_t *el;

    DCHECK(out_len != NULL,
           "DOM §4.4's locate a namespace prefix was asked for a prefix with nowhere to write its LENGTH — the "
           "answer is a borrowed slice of the tree and is not NUL-terminated");
    DCHECK(ns != NULL || ns_len == 0,
           "DOM §4.4's locate a namespace prefix was given a null namespace with a non-zero length");
    *out_len = 0;

    /* The same tail-call-to-loop as above, and for the same reason: step 3 recurses on the parent element. */
    for (el = element; el != NULL; el = parent_element(lxb_dom_interface_node(el))) {
        lxb_dom_attr_t *a;
        const char *ens;
        size_t enslen = 0;

        /* Step 1 — "If element's namespace is namespace and its namespace prefix is non-null, then return its
           namespace prefix." The non-null requirement is the whole point: an element in the namespace with NO
           prefix does not name a prefix for it, and answering "" there would hand the caller a prefix that
           spells nothing. */
        ens = element_ns(el, &enslen);
        if (ens != NULL && ns_eq_n(ens, enslen, ns, ns_len)) {
            size_t pxlen = 0;
            const char *px = element_prefix(el, &pxlen);

            if (px != NULL && pxlen != 0) {
                *out_len = pxlen;
                return px;
            }
        }
        /* Step 2 — "If element has an attribute whose namespace prefix is `xmlns` and value is namespace, then
           return element's FIRST such attribute's local name." First in attribute order, which is why this
           returns out of the loop rather than remembering a candidate.
           NOTE WHAT IT DOES NOT ASK: the attribute's NAMESPACE. Step 2 tests only the namespace PREFIX and the
           VALUE, unlike the locate-a-namespace step 4 above, which tests the namespace as well. That asymmetry
           is the standard's and is transcribed rather than smoothed — a reader who "fixes" it here has changed
           which attribute a page's `lookupPrefix` finds. */
        for (a = lxb_dom_element_first_attribute(el); a != NULL; a = lxb_dom_element_next_attribute(a)) {
            const lxb_char_t *apx, *val, *al;
            size_t apxlen = 0, vlen = 0, allen = 0;

            apx = dom_attr_prefix(a, &apxlen);
            if (!ns_eq((const char *)apx, apxlen, "xmlns")) continue;
            val = lxb_dom_attr_value(a, &vlen);
            if (!ns_eq_n((const char *)val, vlen, ns, ns_len)) continue;
            al = lxb_dom_attr_local_name(a, &allen);
            if (al == NULL || allen == 0) continue;
            *out_len = allen;
            return (const char *)al;
        }
        /* Steps 3 and 4 — the loop's own condition, and its end. */
    }
    *out_len = 0;
    return NULL;
}

/* DOM §4.4's TWO WALKS READ AS A LIST OF QUESTIONS RATHER THAN AS AN ANSWER — see node_ns.h for who needs
 * that and why the walk itself cannot serve them.
 *
 * IT SITS HERE, AT THE WALKS, BECAUSE THE ONE WAY IT CAN BE WRONG IS BY DISAGREEING WITH THEM. Every entry
 * below is the argument-side operand of an `ns_eq`/`ns_eq_n` call above, and each names the call it lists;
 * a site added to a walk and not to this list is a world its member never asks about, which is silent —
 * the member answers null and nothing anywhere says a question was skipped. Reading the two together is the
 * whole of the defence, so a diff that adds a comparison to either walk adds a site here in the same hunk.
 *
 * DUPLICATES ARE LISTED AND ARE NOT A DEFECT. One prefix declared on two ancestors is two sites, so it is
 * yielded twice — and because each question is filed under the string's own NAME rather than its rank, the
 * second ask is the SAME question and the flow answers it from its own record instead of minting a second
 * world. Collapsing them would need a set, and a set here would be a second fact about the tree to keep
 * true; the mechanism already makes the repetition free. */
enum { NS_CAND_XML = 0, NS_CAND_XMLNS, NS_CAND_ELEMENT, NS_CAND_ATTR, NS_CAND_END };

void node_ns_candidates_begin(NodeNsCandidates *it, lxb_dom_node_t *node, bool prefixes)
{
    DCHECK(it != NULL,
           "DOM §4.4's candidate listing was begun with nowhere to keep its cursor — the cursor is a position "
           "in the page's own tree and is the caller's C local, so an absent one is a caller that has nothing "
           "to enumerate into");
    it->el       = node_ns_start_element(node);
    it->attr     = NULL;
    it->prefixes = prefixes;
    /* Locate a namespace's steps 1 and 2 are asked ONCE and not per level. The walk re-asks them at every
       element because the standard's recursion does, and they are level-INDEPENDENT — an argument that is
       "xml" returns at the first element and never reaches the second — so a second listing of them would be
       one question asked twice with the first answer already recorded. Locate a namespace prefix has no such
       steps, which is why it starts at the element. */
    it->site     = prefixes ? NS_CAND_XML : NS_CAND_ELEMENT;
}

const char *node_ns_candidates_next(NodeNsCandidates *it, size_t *out_len)
{
    DCHECK(it != NULL && out_len != NULL,
           "DOM §4.4's candidate listing was advanced with no cursor or nowhere to write a LENGTH — every "
           "string it yields is a borrowed slice of the tree and is not NUL-terminated, exactly like the "
           "walks' own answers");
    *out_len = 0;
    for (;;) {
        switch (it->site) {
        /* Locate a namespace step 1 — `ns_eq(prefix, prefix_len, "xml")`. */
        case NS_CAND_XML:
            it->site = NS_CAND_XMLNS;
            *out_len = strlen("xml");
            return "xml";

        /* Locate a namespace step 2 — `ns_eq(prefix, prefix_len, "xmlns")`. */
        case NS_CAND_XMLNS:
            it->site = NS_CAND_ELEMENT;
            *out_len = strlen("xmlns");
            return "xmlns";

        /* THE ELEMENT'S OWN NAME — locate a namespace step 3's `ns_eq_n(px, pxlen, prefix, prefix_len)`, and
           locate a namespace prefix step 1's `ns_eq_n(ens, enslen, ns, ns_len)`. Both are GUARDED on the
           element's namespace being non-null and the prefix walk's is additionally guarded on the element
           having a prefix at all, and those guards are transcribed rather than dropped: a site the walk does
           not reach is a world that cannot exist, and listing it would mint a sibling standing on a
           comparison the algorithm never makes. */
        case NS_CAND_ELEMENT: {
            lxb_dom_element_t *el = it->el;
            const char *ns;
            size_t nslen = 0;

            if (el == NULL) { it->site = NS_CAND_END; return NULL; }
            it->site = NS_CAND_ATTR;
            it->attr = lxb_dom_element_first_attribute(el);
            ns = element_ns(el, &nslen);
            if (ns == NULL) break;
            if (it->prefixes) {
                size_t pxlen = 0;
                const char *px = element_prefix(el, &pxlen);

                if (px == NULL || pxlen == 0) break;
                *out_len = pxlen;
                return px;
            }
            *out_len = nslen;
            return ns;
        }

        /* THE DECLARATIONS ON THAT ELEMENT — locate a namespace step 4's first shape, whose comparison is
           element_declaration's `ns_eq_n(al, allen, prefix, prefix_len)`, and locate a namespace prefix step
           2's `ns_eq_n(val, vlen, ns, ns_len)`. THE TWO DO NOT ASK THE SAME THING OF THE ATTRIBUTE and the
           asymmetry is the standard's: step 4 tests the attribute's NAMESPACE as well as its namespace
           prefix, and step 2 tests only the prefix. node_locate_namespace_prefix's own note says a reader who
           smooths that has changed which attribute a page's `lookupPrefix` finds, and it is the same here.
           A ZERO-LENGTH LOCAL NAME OR VALUE IS SKIPPED because the walk's comparison cannot match one: the
           argument reaching either walk from a member is non-empty (the empty string was mapped to null at
           the member's step 1), and `ns_eq_n` answers false whenever the lengths differ. */
        case NS_CAND_ATTR: {
            lxb_dom_attr_t *a = it->attr;
            const lxb_char_t *apx;
            size_t apxlen = 0;

            if (a == NULL) {
                it->el   = parent_element(lxb_dom_interface_node(it->el));
                it->site = NS_CAND_ELEMENT;
                break;
            }
            it->attr = lxb_dom_element_next_attribute(a);
            apx = dom_attr_prefix(a, &apxlen);
            if (!ns_eq((const char *)apx, apxlen, "xmlns")) break;
            if (it->prefixes) {
                const lxb_char_t *ans, *al;
                size_t anslen = 0, allen = 0;

                ans = dom_attr_ns(a, &anslen);
                if (!ns_eq((const char *)ans, anslen, XML_NS_XMLNS_NAMESPACE)) break;
                al = lxb_dom_attr_local_name(a, &allen);
                if (al == NULL || allen == 0) break;
                *out_len = allen;
                return (const char *)al;
            } else {
                const lxb_char_t *val;
                size_t vlen = 0;

                val = lxb_dom_attr_value(a, &vlen);
                if (val == NULL || vlen == 0) break;
                *out_len = vlen;
                return (const char *)val;
            }
        }

        /* Both walks' own "Return null", which is the answer for an argument that matched no site above. */
        default:
            DCHECK(it->site == NS_CAND_END,
                   "DOM §4.4's candidate listing stood at a comparison site it does not name — the sites are "
                   "the walks' own and this cursor is written nowhere but here");
            return NULL;
        }
    }
}
