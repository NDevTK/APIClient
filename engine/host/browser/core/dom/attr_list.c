/* DOM §4.9's ATTRIBUTE LIST — the raw operations, and the only place they exist.
 *
 * This is the layer BELOW the per-flow chokepoint, and it is its own file for the reason dom_cow.c is: it is
 * the one place that calls Lexbor's attribute mutators. Put in attr.c beside the Attr interface, a component
 * file would have had to be exempt, and the next `lxb_dom_element_set_attribute` written there would have
 * looked like it belonged.
 * `engine/check_dom_chokepoint.mjs` used to make that a BAN rather than a boundary; it has been deleted, and
 * this file's isolation is now the whole of the enforcement. What that costs is stated rather than left to be
 * discovered: a mutator called from a component compiles, and the write it performs is invisible to the
 * running flow's DOM delta — a sibling flow sees it and an unapply leaks it, with nothing to say so. The
 * structural replacement is not another scanner but the mutators having no declaration a component can reach;
 * until that is built, this paragraph is the enforcement.
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
#include "core/dom/name_intern.h"   /* §4.9.2's storage step stores the three names AS GIVEN */
#include "core/dom/node.h"   /* node_wrap_forget — a destroyed Attr hands back its wrapper */

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
    /* The document's PREFIX hash, which is where dom_attr_create stores one. Lexbor's own attribute name
       builder put an attribute's prefix in the NAMESPACE hash instead, and the only reason that read back at
       all is that lxb_ns_prefix_data_by_id ignores the hash it is handed and resolves a non-static id as its
       own address — so both spellings work today and exactly one of them says what it means. */
    d = lxb_ns_prefix_data_by_id(a->node.owner_document->prefix, a->node.prefix);
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

/* DOM §4.9.2 "CREATE AN ATTRIBUTE", THE STORAGE STEP — "set attribute's namespace to namespace, namespace
 * prefix to prefix, local name to localName", the strings §1.4's validate-and-extract handed back.
 *
 * NOT lxb_dom_attr_set_name_ns, WHICH IS THE SAME DEFECT 0962568a MEASURED ON THE ELEMENT SIDE. That entry
 * interned the namespace with lxb_ns_append, the local name with lxb_dom_attr_local_name_append and the prefix
 * with lxb_ns_prefix_append, and all three of those probe the static tables with
 * lexbor_shs_entry_get_lower_static and insert with lexbor_hash_insert_lower. core/dom/name_intern.h states
 * which standard says each of the three is a different name; what it cost HERE was:
 *   - `setAttributeNS("http://www.Example.org/wine", "a", v)` stored a lower-cased namespace, so
 *     `getAttributeNS("http://www.Example.org/wine", "a")` — whose key is §4.9's (namespace, local name),
 *     compared byte for byte by ns_eq above — could not find the attribute that call had just created;
 *   - `svg.setAttributeNS(null, "viewBox", v)` stored `viewbox`. §4.9 lowercases a qualified name in exactly
 *     ONE place, setAttribute's step 2 "if this is in the HTML namespace and its node document is an HTML
 *     document", which core/dom/element.c performs itself and setAttributeNS does not perform at all — so the
 *     local name here is the one the standard says to store, and folding it made every non-HTML attribute name
 *     a name the page cannot read back;
 *   - a prefix spelled `XML` became the reserved LXB_NS_XML, and went into the document's NAMESPACE hash.
 * lexbor's hashes are still the storage and its readers still read them; only the insert is this engine's.
 *
 * THE THREE WRITES ARE THE WHOLE OF IT, so they are made here rather than through a lexbor entry that also
 * splits the qualified name: this file already HAS the prefix and the local name apart — that is what §1.4
 * handed its caller — and re-joining them so lexbor can re-split them is what made the qualified name the only
 * thing the prefix was carried in. */
lxb_dom_attr_t *dom_attr_create(lxb_dom_document_t *doc, const char *ns, const char *prefix, const char *local)
{
    lxb_dom_attr_t *a;
    size_t pl = prefix ? strlen(prefix) : 0, ll = strlen(local ? local : "");

    DCHECK(doc && local, "an attribute was created with no document or no local name");
    DCHECK(!prefix || ns, "an attribute was given a PREFIX and the null namespace — §4.9's \"validate and "
                          "extract\" throws NamespaceError for that pair, so it must never reach the tree");
    a = lxb_dom_attr_interface_create(doc);
    CHECK(a != NULL, "dom-attr-oom: an attribute could not be created");
    a->node.local_name = dom_intern_attribute_local_name(doc, local, ll);
    a->node.ns         = dom_intern_namespace(doc, ns, ns ? strlen(ns) : 0);
    a->node.prefix     = dom_intern_prefix(doc, prefix, pl);
    /* THE QUALIFIED NAME IS STORED AND NOT DERIVED, because §4.9's setAttribute matches "the first attribute
       whose qualified name is qualifiedName" and the step-2 loop above reads exactly this field. It is
       `prefix:local` when there is a prefix and the local name otherwise, which is the one place the two
       halves are put back together. */
    if (pl) {
        char *q = malloc(pl + 1 + ll + 1);
        CHECK(q != NULL, "dom-attr-oom: an attribute's qualified name could not be assembled");
        memcpy(q, prefix, pl);
        q[pl] = ':';
        memcpy(q + pl + 1, local, ll);
        q[pl + 1 + ll] = 0;
        a->qualified_name = dom_intern_attribute_qualified_name(doc, q, pl + 1 + ll);
        free(q);
    } else {
        a->qualified_name = dom_intern_attribute_qualified_name(doc, local, ll);
    }
    /* §4.9.2 "create an attribute" step 2 sets the VALUE, and its default is THE EMPTY STRING. §9.1 lists an
       attribute's value as "a string", so a value-less attribute is a state the model does not have — lexbor
       leaves the field NULL until something writes it, and every reader would then have to carry that case. */
    dom_attr_set_value(a, "", 0);
    return a;
}

/* DOM §4.4 "clone a node" step 2's attribute half — "for each attribute in node's attribute list: let
 * copyAttribute be a CLONE OF attribute with document; append copyAttribute to copy".
 *
 * IT IS NOT dom_attr_create OVER THE SOURCE'S BYTES, and that is the whole reason it is a separate entry: the
 * three names are already interned in the SOURCE's hashes, so a clone into the same document is an ID COPY —
 * which is what `el.cloneNode(true)` almost always is — and a clone into another document is a re-intern of
 * exactly those bytes, both of which core/dom/name_intern.h's import entries answer. Going out through C
 * strings would have to NUL-terminate and re-validate names §1.4 accepted once already.
 *
 * WHAT LEXBOR'S lxb_dom_attr_interface_clone DID INSTEAD, measured: for a copy into ANOTHER document it re-
 * appended the namespace through lxb_ns_append and the local name through lxb_dom_attr_local_name_append, both
 * of which lower-case — so `document.cloneNode(true)` came back with `xlink:href` in a namespace the page had
 * never written and a `viewBox` spelled `viewbox`. The value is copied here rather than there for the same
 * reason the names are: this file owns what an attribute's fields mean. */
lxb_dom_attr_t *dom_attr_clone(lxb_dom_document_t *doc, const lxb_dom_attr_t *src)
{
    lxb_dom_document_t *from;
    lxb_dom_attr_t *a;

    DCHECK(doc && src, "an attribute clone was asked for with no document or no attribute");
    from = src->node.owner_document;
    a = lxb_dom_attr_interface_create(doc);
    CHECK(a != NULL, "dom-attr-oom: an attribute clone could not be created");
    /* THE FOUR IDS ARE COPIED AND THEN IMPORTED AS A SET, through name_intern.h's one list of what names an
       attribute has — rather than four import calls spelled out here. A second copy of that list is a second
       thing to keep in step with the struct: §4.5's adopt needs exactly the same four, and the day an Attr
       grows a fifth name the two spellings would disagree silently. */
    a->node.local_name = src->node.local_name;
    a->node.ns         = src->node.ns;
    a->node.prefix     = src->node.prefix;
    a->qualified_name  = src->qualified_name;
    dom_import_node_names(doc, from, &a->node);
    /* §4.9.2's value default again — a value-less attribute is a state the model does not have, and lexbor
       leaves the field NULL on an attribute nothing has written. */
    dom_attr_set_value(a, src->value ? (const char *)src->value->data : "",
                       src->value ? src->value->length : 0);
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
 * No C recursion — depth here is the page's data.
 *
 * AND THE SECOND CORRECTION, WHICH IS THE SAME BOUNDARY: HTML §13.2.6.1's "adjust SVG attributes" says to
 * "change the attribute's NAME to the name given in the corresponding cell of the second column" — one name,
 * because DOM §4.9.1 defines an attribute's qualified name AS its local name when its namespace prefix is
 * null. lexbor's lxb_html_tree_adjust_svg_attributes writes only `attr->qualified_name` and leaves the
 * tokenizer's lower-cased `local_name` in place, so a parsed `<svg viewBox="…">` held the two spellings at
 * once: `attr.name` answered `viewBox` and `attr.localName` answered `viewbox`, and §4.9's key is the LOCAL
 * one — so `getAttributeNS(null, "viewBox")` found nothing on markup that plainly has it.
 * That was invisible while dom_attr_create folded a scripted name the same way, and stopped being invisible
 * the moment it stored what the page passed: `svg.setAttributeNS(null, "viewBox", v)` then created a SECOND
 * attribute beside the parsed one. Correcting it here is the same shape as the namespace above — the parser's
 * intent only exists at the boundary — and the invariant it restores is DOM's own sentence, which every
 * reader afterwards may rely on. */
void dom_attr_normalize_parsed(lxb_dom_node_t *root)
{
    lxb_dom_node_t *n = root;

    if (!root) return;
    for (;;) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            lxb_dom_attr_t *a;
            for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
                if (a->node.ns == n->ns) a->node.ns = LXB_NS__UNDEF;
                /* §4.9.1: with a null prefix the two names ARE one. A stored qualified name that differs is
                   the SVG adjust having replaced half of it, and the qualified half is the one HTML's table
                   named — so the local name is re-interned from those bytes. A zero qualified name is the
                   ordinary HTML attribute, whose readers already fall back to the local name. */
                if (a->node.prefix == LXB_NS__UNDEF && a->qualified_name != 0 &&
                    a->qualified_name != a->node.local_name) {
                    size_t qlen = 0;
                    const lxb_char_t *q = lxb_dom_attr_qualified_name(a, &qlen);
                    DCHECK(el->attr_id != a && el->attr_class != a,
                           "the SVG adjust renamed the attribute an element's id/class cache names — those two "
                           "slots are keyed on a local name this correction is about to change, so the cache "
                           "would go on naming an attribute that no longer has that name");
                    a->node.local_name = dom_intern_attribute_local_name(lxb_dom_interface_node(el)->
                                                                         owner_document,
                                                                         (const char *)q, qlen);
                }
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n != root && !n->next) n = n->parent;
        if (n == root) return;
        n = n->next;
    }
}
