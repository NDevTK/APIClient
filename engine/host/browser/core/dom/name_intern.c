/* See name_intern.h. */
#include <string.h>

#include <lexbor/core/hash.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/attr_const.h>

#include "check.h"
#include "core/dom/name_intern.h"
#include "core/dom/node_interface.h"   /* a namespace id born here is one lexbor's destroy table has no column
                                          for — see that header for the whole of it */

/* Lexbor's, exported and declared in no lexbor header — its own dom/interfaces/node.c and attr.c declare them
   at the call site in exactly this form, which is why the declarations are copied rather than invented. The two
   used here are the RAW ones: lxb_tag_append probes the static table with lexbor_shs_entry_get_static and
   inserts with lexbor_hash_insert_raw, and lxb_dom_attr_qualified_name_append inserts raw with no probe at all,
   so both already store the bytes they were given and there is nothing for this file to replace. Their folding
   siblings — lxb_tag_append_lower and lxb_dom_attr_local_name_append — are deliberately NOT declared here: the
   whole point of the file is that they are not reachable from it. */
const lxb_tag_data_t *
lxb_tag_append(lexbor_hash_t *hash, lxb_tag_id_t tag_id, const lxb_char_t *name, size_t length);
lxb_dom_attr_data_t *
lxb_dom_attr_qualified_name_append(lexbor_hash_t *hash, const lxb_char_t *name, size_t length);

lxb_ns_id_t dom_intern_namespace(lxb_dom_document_t *doc, const char *ns, size_t len)
{
    lxb_ns_id_t id;
    lxb_ns_data_t *data;

    DCHECK(doc != NULL, "a namespace name was interned into no document");
    if (ns == NULL || len == 0) return LXB_NS__UNDEF;   /* §1.4 step 1 already made "" the null namespace */
    /* From __ANY + 1: the two ids below it are SENTINELS ("no namespace" and "any namespace"), not names a
       document can hold, and asking the table about them would let a page's own string claim one of them. */
    for (id = LXB_NS__ANY + 1; id < LXB_NS__LAST_ENTRY; id++) {
        size_t klen = 0;
        const lxb_char_t *k = lxb_ns_by_id(doc->ns, id, &klen);
        if (k != NULL && klen == len && memcmp(k, ns, len) == 0) return id;
    }
    data = lexbor_hash_insert(doc->ns, lexbor_hash_insert_raw, (const lxb_char_t *)ns, len);
    CHECK(data != NULL, "a namespace name could not be interned — the document's namespace hash is out of "
                        "memory, and a node with no namespace is a different node");
    /* An id at or below the last static entry is a small integer, and every id above one IS the entry's own
       address — which is what makes lxb_ns_data_by_id a cast. lexbor states the same invariant as a NULL
       return; here it is the assertion it always was. */
    DCHECK((lxb_ns_id_t)data > LXB_NS__LAST_ENTRY,
           "an interned namespace landed at an address a static namespace id already names");
    data->ns_id = (lxb_ns_id_t)data;
    /* AND THE DOCUMENT NOW DESTROYS ITS OWN NODES. This line is the only thing in the engine that produces a
       namespace id outside the domain lexbor's `res_destructor[tag][ns]` indexes, so it is the only thing that
       can create the obligation, and it takes it here rather than at each of the members that ask — an
       element, an attribute and §4.4's import all reach this one mint. See node_interface.h. */
    dom_document_own_node_interfaces(doc);
    return data->ns_id;
}

lxb_ns_prefix_id_t dom_intern_prefix(lxb_dom_document_t *doc, const char *prefix, size_t len)
{
    lxb_ns_prefix_id_t id;
    lxb_ns_prefix_data_t *data;

    DCHECK(doc != NULL, "a namespace prefix was interned into no document");
    if (prefix == NULL || len == 0) return LXB_NS__UNDEF;
    /* Same sentinel skip as above, and it matters MORE here: "#undef" and "#any" are spellings DOM §1.4 lets a
       page write as a prefix, and answering one of them would say "this node has no prefix". */
    for (id = LXB_NS__ANY + 1; id < LXB_NS__LAST_ENTRY; id++) {
        const lxb_ns_prefix_data_t *d = lxb_ns_prefix_data_by_id(doc->prefix, id);
        if (d != NULL && d->entry.length == len &&
            memcmp(lexbor_hash_entry_str(&d->entry), prefix, len) == 0) return id;
    }
    data = lexbor_hash_insert(doc->prefix, lexbor_hash_insert_raw, (const lxb_char_t *)prefix, len);
    CHECK(data != NULL, "a namespace prefix could not be interned — the document's prefix hash is out of "
                        "memory, and a node with the wrong prefix serializes as a different node");
    DCHECK((lxb_ns_prefix_id_t)data > LXB_NS__LAST_ENTRY,
           "an interned namespace prefix landed at an address a static prefix id already names");
    data->prefix_id = (lxb_ns_prefix_id_t)data;
    return data->prefix_id;
}

lxb_tag_id_t dom_intern_element_local_name(lxb_dom_document_t *doc, const char *local, size_t len)
{
    const lxb_tag_data_t *tag;

    DCHECK(doc != NULL, "an element local name was interned into no document");
    DCHECK(local != NULL && len != 0, "an element local name of zero length was interned — DOM §1.4 step 1 "
                                      "rejects it, so validate-and-extract cannot have produced one");
    /* LXB_TAG__UNDEF as the id argument is what makes lxb_tag_append give a NEW entry its own address as its
       tag id, rather than aliasing the id of some other name. */
    tag = lxb_tag_append(doc->tags, LXB_TAG__UNDEF, (const lxb_char_t *)local, len);
    CHECK(tag != NULL, "an element's local name could not be interned — the document's tag hash is out of "
                       "memory, and an element with the wrong local name matches nothing the page queries");
    return tag->tag_id;
}

lxb_dom_attr_id_t dom_intern_attribute_local_name(lxb_dom_document_t *doc, const char *local, size_t len)
{
    lxb_dom_attr_id_t id;
    lxb_dom_attr_data_t *data;

    DCHECK(doc != NULL, "an attribute local name was interned into no document");
    DCHECK(local != NULL && len != 0, "an attribute local name of zero length was interned — §1.4's "
                                      "valid-attribute-local-name predicate rejects it");
    /* The EXACT probe of lexbor's static attribute table, which its own append reaches through
       lexbor_shs_entry_get_lower_static. It is asked about ITSELF — each id's canonical spelling comes back out
       of lxb_dom_attr_data_by_id — so there is no second copy of its 36 strings here. From 1, because
       LXB_DOM_ATTR__UNDEF is the "no attribute" sentinel and not a name.
       What the exact probe CHANGES is what it should: `setAttributeNS(null, "ID", v)` no longer answers
       LXB_DOM_ATTR_ID, so it is no longer the element's id — which is DOM §4.9's own rule, whose ID change
       steps fire only "if localName is id, namespace is null". */
    for (id = 1; id < LXB_DOM_ATTR__LAST_ENTRY; id++) {
        const lxb_dom_attr_data_t *d = lxb_dom_attr_data_by_id(doc->attrs, id);
        if (d != NULL && d->entry.length == len &&
            memcmp(lexbor_hash_entry_str(&d->entry), local, len) == 0) {
            DCHECK(d->attr_id == id, "a static attribute entry answers an id that is not its own index — the "
                                     "table is what lxb_dom_attr_data_by_id indexes, so the two cannot differ");
            return d->attr_id;
        }
    }
    data = lexbor_hash_insert(doc->attrs, lexbor_hash_insert_raw, (const lxb_char_t *)local, len);
    CHECK(data != NULL, "an attribute's local name could not be interned — the document's attribute hash is "
                        "out of memory, and §4.9's key is (namespace, local name)");
    DCHECK((lxb_dom_attr_id_t)data > LXB_DOM_ATTR__LAST_ENTRY,
           "an interned attribute local name landed at an address a static attribute id already names");
    data->attr_id = (lxb_dom_attr_id_t)data;
    return data->attr_id;
}

lxb_dom_attr_id_t dom_intern_attribute_qualified_name(lxb_dom_document_t *doc, const char *qname, size_t len)
{
    lxb_dom_attr_data_t *data;

    DCHECK(doc != NULL, "an attribute qualified name was interned into no document");
    DCHECK(qname != NULL && len != 0, "an attribute qualified name of zero length was interned");
    data = lxb_dom_attr_qualified_name_append(doc->attrs, (const lxb_char_t *)qname, len);
    CHECK(data != NULL, "an attribute's qualified name could not be interned — the document's attribute hash "
                        "is out of memory, and §4.9's setAttribute matches on exactly this name");
    return data->attr_id;
}

/* ─── §4.4's IMPORTS — see name_intern.h ───────────────────────────────────────────────────────────────────
 * Each is the same two questions: is this id already an answer in the destination (same document, or a static
 * id every document shares), and if not, what BYTES does it name in the source. */
lxb_ns_id_t dom_import_namespace(lxb_dom_document_t *to, lxb_dom_document_t *from, lxb_ns_id_t id)
{
    size_t len = 0;
    const lxb_char_t *s;

    DCHECK(to != NULL && from != NULL, "a namespace was imported between documents one of which is not there");
    if (to == from || id < LXB_NS__LAST_ENTRY) return id;
    s = lxb_ns_by_id(from->ns, id, &len);
    CHECK(s != NULL, "a node names a namespace its own document's hash cannot resolve — the id is the entry's "
                     "own address, so an unresolvable one means the node outlived the document that named it");
    return dom_intern_namespace(to, (const char *)s, len);
}

lxb_ns_prefix_id_t dom_import_prefix(lxb_dom_document_t *to, lxb_dom_document_t *from, lxb_ns_prefix_id_t id)
{
    const lxb_ns_prefix_data_t *d;

    DCHECK(to != NULL && from != NULL, "a prefix was imported between documents one of which is not there");
    if (to == from || id < LXB_NS__LAST_ENTRY) return id;
    d = lxb_ns_prefix_data_by_id(from->prefix, id);
    CHECK(d != NULL, "a node names a namespace prefix its own document's hash cannot resolve");
    return dom_intern_prefix(to, (const char *)lexbor_hash_entry_str(&d->entry), d->entry.length);
}

lxb_tag_id_t dom_import_tag(lxb_dom_document_t *to, lxb_dom_document_t *from, lxb_tag_id_t id)
{
    const lxb_tag_data_t *d;

    DCHECK(to != NULL && from != NULL, "a tag name was imported between documents one of which is not there");
    if (to == from || id < LXB_TAG__LAST_ENTRY) return id;
    /* No hash argument: a non-static tag id IS the entry's address, which is why lexbor's own reader takes
       only the id. */
    d = lxb_tag_data_by_id(id);
    CHECK(d != NULL, "a node names a tag its own document's hash cannot resolve");
    return dom_intern_element_local_name(to, (const char *)lexbor_hash_entry_str(&d->entry), d->entry.length);
}

lxb_dom_attr_id_t dom_import_attribute_local_name(lxb_dom_document_t *to, lxb_dom_document_t *from,
                                                  lxb_dom_attr_id_t id)
{
    const lxb_dom_attr_data_t *d;

    DCHECK(to != NULL && from != NULL, "an attribute name was imported between documents one of which is not "
                                       "there");
    if (to == from || id < LXB_DOM_ATTR__LAST_ENTRY) return id;
    d = lxb_dom_attr_data_by_id(from->attrs, id);
    CHECK(d != NULL, "an attribute names a local name its own document's hash cannot resolve");
    return dom_intern_attribute_local_name(to, (const char *)lexbor_hash_entry_str(&d->entry), d->entry.length);
}

lxb_dom_attr_id_t dom_import_attribute_qualified_name(lxb_dom_document_t *to, lxb_dom_document_t *from,
                                                      lxb_dom_attr_id_t id)
{
    const lxb_dom_attr_data_t *d;

    DCHECK(to != NULL && from != NULL, "an attribute qualified name was imported between documents one of "
                                       "which is not there");
    if (to == from || id < LXB_DOM_ATTR__LAST_ENTRY) return id;
    d = lxb_dom_attr_data_by_id(from->attrs, id);
    CHECK(d != NULL, "an attribute names a qualified name its own document's hash cannot resolve");
    return dom_intern_attribute_qualified_name(to, (const char *)lexbor_hash_entry_str(&d->entry),
                                               d->entry.length);
}
