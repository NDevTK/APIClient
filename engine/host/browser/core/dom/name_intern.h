/* STORING A NAME IN THE DOCUMENT'S HASHES AS THE STANDARD GAVE IT.
 *
 * WHAT THIS IS. Every name the DOM keeps about a node — an element's namespace, prefix and local name, an
 * attribute's namespace, prefix, local name and qualified name — is stored by this engine as an ID into one of
 * the document's hashes, and read back by turning that id into bytes. So "the standard says to store the string
 * it gave you" is a property of THE INTERNING and of nothing else, and lexbor's own append functions CASE-FOLD:
 * lxb_ns_append, lxb_ns_prefix_append, lxb_tag_append_lower and lxb_dom_attr_local_name_append each probe the
 * static tables with lexbor_shs_entry_get_lower_static and insert with lexbor_hash_insert_lower, whose copy is
 * `to[i] = lexbor_str_res_map_lowercase[key[i]]`.
 *
 * THAT IS RIGHT FOR HTML AND WRONG FOR THE DOM, and the difference is not a nicety:
 *   - Namespaces in XML 1.0 (Third Edition) §2.3 states the namespace-name comparison with its own example —
 *     "http://www.example.org/wine", "http://www.Example.org/wine" and "http://www.example.org/Wine" are "all
 *     different for the purposes of identifying namespaces, since they differ in case".
 *   - DOM §4.5 lowercases a local name in exactly ONE place, createElement's step 2 "if this is an HTML
 *     document, then set localName to localName in ASCII lowercase", and §4.9 setAttribute's step 2 does the
 *     same for an element in the HTML namespace. The NS-suffixed members do NEITHER, and getElementsByTagNameNS
 *     and §4.9's (namespace, local name) key both match a local name EXACTLY.
 *   - Namespaces in XML §3 RESERVES prefixes matching "xml" case-insensitively and asks that a processor "MUST
 *     NOT treat them as fatal errors", while binding the "xml" prefix to anything else is illegal;
 *     lxb_ns_prefix_append(hash, "XML", 3) answers LXB_NS_XML and collapses the two rules into one.
 * Where the DOM DOES lowercase, the caller performs it — element.c's setAttribute step 2 loop is the whole of
 * it — so the storage layer never has to know which document type it is in.
 *
 * ITS OWN FILE, because both elements and attributes ask it and the measurement above must not be written
 * twice. It was born inside core/dom/element.c, where 0962568a needed it for createElementNS's three names, and
 * it moved the moment core/dom/attr_list.c needed exactly the same four sentences about exactly the same
 * hashes: a second copy would have been a second thing to keep in step with lexbor.
 *
 * LEXBOR IS STILL THE STORAGE. The document's four hashes (`ns`, `prefix`, `tags`, `attrs`) are lexbor's, the
 * static tables are lexbor's and every reader is lexbor's; the only thing this file replaces is the INSERT
 * function, which is the smallest bypass that makes the stored bytes right. The static tables are probed by
 * asking lexbor for each id's OWN canonical spelling, so there is no second copy of its strings here to drift.
 *
 * EVERY ENTRY TAKES A LENGTH and none of its arguments is NUL-terminated — a name may contain U+0000, which is
 * a code point DOM §1.4 lets a page write into an element local name, and `strlen` would answer about the
 * prefix of the string in front of it. */
#ifndef APICLIENT_DOM_NAME_INTERN_H
#define APICLIENT_DOM_NAME_INTERN_H

#include <stddef.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>
#include <lexbor/tag/tag.h>

/* The NAMESPACE NAME (§2.3's "all different ... since they differ in case"). NULL or a zero length is §1.4's
   null namespace and answers LXB_NS__UNDEF — which is not the same thing as a namespace named "". */
lxb_ns_id_t dom_intern_namespace(lxb_dom_document_t *doc, const char *ns, size_t len);
/* The NAMESPACE PREFIX (§3's reserved-but-legal "XML"). NULL or a zero length is the null prefix. */
lxb_ns_prefix_id_t dom_intern_prefix(lxb_dom_document_t *doc, const char *prefix, size_t len);
/* An ELEMENT's local name, into the document's tag hash. */
lxb_tag_id_t dom_intern_element_local_name(lxb_dom_document_t *doc, const char *local, size_t len);
/* An ATTRIBUTE's local name, into the document's attribute hash — a different hash and a different static
   table from the element one, which is why it is a different entry and not a flag. */
lxb_dom_attr_id_t dom_intern_attribute_local_name(lxb_dom_document_t *doc, const char *local, size_t len);
/* An ATTRIBUTE's QUALIFIED name — `prefix:local`, or the local name when there is no prefix. §4.9's
   "get an attribute by name" matches on this one, so it is stored beside the local name and not derived. */
lxb_dom_attr_id_t dom_intern_attribute_qualified_name(lxb_dom_document_t *doc, const char *qname, size_t len);

#endif
