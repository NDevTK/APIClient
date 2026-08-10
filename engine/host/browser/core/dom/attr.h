/* The Attr interface (DOM §4.9.2) and the NamedNodeMap (§4.9.1) that holds them. See attr.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ATTR_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ATTR_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "core/idl_args.h"

void attr_init(JSContext *ctx);
/* §4.9's two prototypes for ONE realm — declared into core/realm.h's list. */
void attr_install_protos(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue named_node_map_proto(JSContext *ctx);
void attr_install(JSContext *ctx, JSValueConst global);
void attr_free(JSContext *ctx);
/* A NamedNodeMap over `owner`'s attributes. The caller caches it on the element's wrapper — §4.9's
   `[SameObject] readonly attribute NamedNodeMap attributes`. */
JSValue attr_named_node_map_new(JSContext *ctx, JSValueConst owner);

/* §4.9's "GET AN ATTRIBUTE BY NAME", as one implementation. §4.9.1's `getNamedItem` and §4.9's
   `getAttributeNode` are the SAME algorithm reached through two objects, and written twice they are two rules
   about what a qualified name matches. JS_UNDEFINED when there is none; the callers turn that into their own
   interface's null. */
JSValue attr_by_name(JSContext *ctx, lxb_dom_element_t *el, const char *name);

/* THE Attr BEHIND A WRAPPER, or NULL when the value is not one (or names a node that has been destroyed).
   Exported because §4.9's node-valued MEMBERS live on Element while the interface lives here. */
lxb_dom_attr_t *attr_node_of(JSValueConst v);
/* The class an `Attr attr` IDL position brands against (idl_iface_brand). */
JSClassID attr_class_id(void);
/* The element a NamedNodeMap is over — §4.9.1's "associated element". */
lxb_dom_element_t *attr_named_node_map_owner(JSContext *ctx, JSValueConst map);

/* AN ATTRIBUTE'S OWN §4.9 IDENTITY — (namespace, LOCAL name) as NUL-terminated strings, owned. Not the
   qualified name: two attributes may wear one qualified name (§9.11 Q3), so a by-name write from an Attr can
   land on the other one. */
typedef struct { char *ns; char *local; } AttrKey;
void attr_key_of(const lxb_dom_attr_t *a, AttrKey *k);
void attr_key_free(AttrKey *k);

/* §4.9's "SET AN ATTRIBUTE" (§9.4.1), as ONE machine for the FOUR members that are that algorithm in one
   sentence each — `Element.setAttributeNode`, `setAttributeNodeNS`, `NamedNodeMap.setNamedItem` and
   `setNamedItemNS`. Its magic says only where the element comes from: 0 = `this` IS the element, 1 = `this` is
   the NamedNodeMap over it. It is a machine because its step 1 is the Trusted Types call and step 2's
   InUseAttributeError check comes AFTER it. */
const IdlStepDecl *attr_set_attribute_decl(void);

/* §4.5's `createAttribute` / `createAttributeNS`, declared here beside the interface they build and installed
   by document.c onto Document.prototype — the factories are §4.9.2's "create an attribute" with a name check in
   front, so they belong to the attribute component and not to a second copy of that algorithm. */
void attr_install_document_members(JSContext *ctx, JSValueConst doc_proto);

#endif
