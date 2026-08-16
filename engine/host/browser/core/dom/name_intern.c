/* See name_intern.h. */
#include <stdint.h>
#include <string.h>

#include <lexbor/core/hash.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/attr_const.h>

#include "check.h"
#include "core/dom/name_intern.h"

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

/* ─── ONE NODE'S WHOLE NAME SET — see name_intern.h ────────────────────────────────────────────────────────
 * The four hashes are per document (lxb_dom_document_init creates them, lxb_dom_document_destroy destroys
 * them with their entries), so the ids below are the only node state whose LIFETIME is the document's rather
 * than the node's. */

/* Is `id` an entry of THIS hash? A static id is a small index into a shared array and needs no hash at all;
   a non-static one is the entry's address, and the hash holds ONE entry per byte string, so the search for
   the id's own spelling either returns the id or proves the id belongs elsewhere. `lexbor_hash_search_raw`
   is right for every one of the four even where lexbor's own folding appends made the entry: the QUERY is the
   entry's STORED bytes, which are already folded, and `lexbor_hash_make_id_lower`/`_upper` differ from
   `lexbor_hash_make_id` only by the fold they apply — so on bytes that are already folded the three agree, and
   so do their comparators. */
static bool name_id_owned(lexbor_hash_t *hash, uintptr_t id, uintptr_t last,
                          const lxb_char_t *s, size_t len)
{
    if (id < last) return true;
    if (hash == NULL || s == NULL || len == 0) return false;
    return (uintptr_t)lexbor_hash_search(hash, lexbor_hash_search_raw, s, len) == id;
}

static bool ns_owned(const lxb_dom_document_t *doc, lxb_ns_id_t id)
{
    size_t len = 0;
    const lxb_char_t *s = (id < LXB_NS__LAST_ENTRY) ? NULL : lxb_ns_by_id(doc->ns, id, &len);
    return name_id_owned(doc->ns, id, LXB_NS__LAST_ENTRY, s, len);
}

static bool prefix_owned(const lxb_dom_document_t *doc, lxb_ns_prefix_id_t id)
{
    const lxb_ns_prefix_data_t *d = (id < LXB_NS__LAST_ENTRY) ? NULL
                                                             : lxb_ns_prefix_data_by_id(doc->prefix, id);
    return name_id_owned(doc->prefix, id, LXB_NS__LAST_ENTRY,
                         d ? lexbor_hash_entry_str(&d->entry) : NULL, d ? d->entry.length : 0);
}

static bool tag_owned(const lxb_dom_document_t *doc, lxb_tag_id_t id)
{
    const lxb_tag_data_t *d = (id < LXB_TAG__LAST_ENTRY) ? NULL : lxb_tag_data_by_id(id);
    return name_id_owned(doc->tags, id, LXB_TAG__LAST_ENTRY,
                         d ? lexbor_hash_entry_str(&d->entry) : NULL, d ? d->entry.length : 0);
}

static bool attr_owned(const lxb_dom_document_t *doc, lxb_dom_attr_id_t id)
{
    const lxb_dom_attr_data_t *d = (id < LXB_DOM_ATTR__LAST_ENTRY) ? NULL
                                                                   : lxb_dom_attr_data_by_id(doc->attrs, id);
    return name_id_owned(doc->attrs, id, LXB_DOM_ATTR__LAST_ENTRY,
                         d ? lexbor_hash_entry_str(&d->entry) : NULL, d ? d->entry.length : 0);
}

/* A node kind that carries NO per-document name at all still has the three node-struct fields, and every one
   of them must be a STATIC id or the kind belongs in a case of its own. lexbor callocs each of these
   interfaces and writes only `type` and `owner_document` (text, comment, CDATA section, processing
   instruction, document fragment, shadow root); a Document is given LXB_TAG__DOCUMENT. So the assertion is
   the statement that this engine has not since given one of them a name — it is what fires the day it does,
   instead of the name silently staying in the old document. */
static bool node_struct_names_static(const lxb_dom_node_t *n)
{
    return n->local_name < LXB_TAG__LAST_ENTRY && n->ns < LXB_NS__LAST_ENTRY &&
           n->prefix < LXB_NS__LAST_ENTRY;
}

bool dom_names_owned_by(const lxb_dom_document_t *doc, const lxb_dom_node_t *n)
{
    DCHECK(doc != NULL && n != NULL, "the node-name ownership invariant was asked of nothing");
    if (!ns_owned(doc, n->ns) || !prefix_owned(doc, n->prefix)) return false;
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT: {
        const lxb_dom_element_t *el = (const lxb_dom_element_t *)n;
        /* `upper_name` is not asked about: it is lexbor's lazily-rebuilt cache of the uppercase qualified
           name and the import below clears it, so the only value it can hold here is one THIS document's
           `lxb_dom_element_upper_update` inserted. */
        return tag_owned(doc, n->local_name) && tag_owned(doc, el->qualified_name);
    }
    case LXB_DOM_NODE_TYPE_ATTRIBUTE: {
        const lxb_dom_attr_t *a = (const lxb_dom_attr_t *)n;
        return attr_owned(doc, n->local_name) && attr_owned(doc, a->qualified_name);
    }
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: {
        const lxb_dom_document_type_t *dt = (const lxb_dom_document_type_t *)n;
        return node_struct_names_static(n) && attr_owned(doc, dt->name);
    }
    default:
        return node_struct_names_static(n);
    }
}

void dom_import_node_names(lxb_dom_document_t *to, lxb_dom_document_t *from, lxb_dom_node_t *n)
{
    DCHECK(to != NULL && from != NULL && n != NULL,
           "a node's names were imported with no destination document, no source document or no node");

    switch (n->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT: {
        lxb_dom_element_t *el = lxb_dom_interface_element(n);
        /* THE LOCAL NAME AND THE NAMESPACE MOVE TOGETHER OR NOT AT ALL, because node_interface.c and lexbor's
           own table both key an element's C struct on exactly that pair: importing one without the other
           leaves an element whose struct was chosen from a name in one document and a namespace in another,
           which is the shape 41533bd7 and 5e6480c7 each found the hard way. */
        n->local_name      = dom_import_tag(to, from, n->local_name);
        n->ns              = dom_import_namespace(to, from, n->ns);
        n->prefix          = dom_import_prefix(to, from, n->prefix);
        /* Zero means "no separate qualified name" and lxb_dom_element_qualified_name falls back to the local
           name for it; dom_import_tag answers a static id unchanged, so the fallback survives the move. */
        el->qualified_name = dom_import_tag(to, from, el->qualified_name);
        /* A CACHE, CLEARED RATHER THAN IMPORTED. lxb_dom_element_upper_update inserts the UPPERCASE spelling
           into `owner_document->tags` and hands the field the entry's address, and `tagName` is the only
           thing that reads it — so the correct value in the destination is the one that document's own
           update will mint on the next read, and carrying the source's would be a fifth dangling id. */
        el->upper_name     = LXB_TAG__UNDEF;
        break;
    }
    case LXB_DOM_NODE_TYPE_ATTRIBUTE: {
        lxb_dom_attr_t *a = lxb_dom_interface_attr(n);
        /* AN ATTRIBUTE IS A NODE WITH ITS OWN NAMES, which is why §4.5's step 3.3.1 sets the node document of
           each attribute in the element's attribute list: reaching them through the element's own import
           would leave four ids per attribute in the document the element just left. Its local name is an
           `attrs` id and NOT a `tags` id — a different hash and a different static table from the element's,
           which is the whole reason this switch exists. */
        n->local_name     = dom_import_attribute_local_name(to, from, n->local_name);
        n->ns             = dom_import_namespace(to, from, n->ns);
        n->prefix         = dom_import_prefix(to, from, n->prefix);
        a->qualified_name = dom_import_attribute_qualified_name(to, from, a->qualified_name);
        /* lxb_dom_attr_t.upper_name has no writer — not in lexbor (its attr.c never assigns it) and not in
           this engine — and lxb_dom_attr_interface_create callocs the struct. So it is zero, and the day
           something starts caching an uppercase attribute name there it must decide, HERE, whether to clear
           it as the element arm does or to import it. */
        DCHECK(a->upper_name == LXB_DOM_ATTR__UNDEF,
               "an Attr carries a cached upper-case name and DOM §4.5's adopt has no rule for it — it is an "
               "id in the document the attribute is leaving, so clear it beside the element's upper_name or "
               "import it, but it may not travel");
        break;
    }
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: {
        lxb_dom_document_type_t *dt = lxb_dom_interface_document_type(n);
        /* §4.6's `name`, and it is an `attrs` id: lxb_dom_document_type_create mints it with
           lxb_dom_attr_local_name_append into `document->attrs`. A doctype is adopted by every path an
           element is — `otherDoc.appendChild(doc.doctype)` and `otherDoc.adoptNode(doc.doctype)` are both
           legal — and its publicId and systemId are strings rather than ids, so this is the whole of it. */
        DCHECK(node_struct_names_static(n),
               "a DocumentType carries a non-static name id in the node struct — lexbor callocs the interface "
               "and writes only its `name`, so something has since given a doctype a namespace, a prefix or a "
               "local name and this arm must import it");
        dt->name = dom_import_attribute_local_name(to, from, dt->name);
        break;
    }
    case LXB_DOM_NODE_TYPE_DOCUMENT:
        DFAIL("a Document's names were imported into another document — a document IS its own node document, "
              "§4.5's adoptNode throws NotSupportedError rather than reaching one, and §4.4's clone creates "
              "the copy in the copy");
        break;
    default:
        DCHECK(node_struct_names_static(n),
               "a node kind with no name arm above carries a non-static namespace, prefix or local name — it "
               "is an id in the document the node is leaving, so give this kind its own case rather than "
               "letting the name dangle");
        break;
    }

    DCHECK(dom_names_owned_by(to, n),
           "a node was moved to a document that does not own one of its name ids — a non-static id IS the "
           "hash entry's address, so the node now names memory lxb_dom_document_destroy will free with the "
           "OTHER document, and namespaceURI/localName/tagName answer out of it");
}
