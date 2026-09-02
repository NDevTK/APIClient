/* HTMLElement and the PER-TAG interfaces — HTML §3.2.2 and §4's element-interface table. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_ELEMENT_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"

/* Build HTMLElement.prototype on Element.prototype, then every per-tag interface on top of it. Called by
   element_init, because the HTML layer is built ON the DOM layer and there is no order in which it is not. */
void html_element_init(JSContext *ctx);
/* §3.2.2 and §4's prototypes for ONE realm — declared into core/realm.h's list. */
void html_element_install_protos(JSContext *ctx);
void html_element_free(JSRuntime *rt);
/* §3.2.2's HTMLElement.prototype FOR THIS REALM. OWNED: the caller frees. HTML §3.2.3 "HTML element
   constructors" step 11 needs it — a custom element constructor whose NewTarget carries a non-object
   `prototype` gets the interface prototype object of that constructor's realm, so the answer is a realm's
   and never a static. */
JSValue html_element_proto(JSContext *ctx);
/* IS THIS VALUE AN HTMLElement — an ELEMENT node in the HTML namespace. Every node wrapper shares one class,
   so a class-id brand cannot tell a `Node` from an `HTMLElement`, and the platform's IDL says both: it is
   `optional HTMLElement anchor` that a foreign-namespace element must not cross. The question is HTML's (the
   namespace is what decides it) so the answer lives here, and idl_iface_narrow is what a member declares it
   with. */
bool html_element_is(JSValueConst v);
/* §4's HTMLUnknownElement.prototype FOR THIS REALM. OWNED. DOM §4.9 step 5.1.4's failure arm names the
   interface by name — "create an element internal given document, HTMLUnknownElement, localName, …" — so a
   custom element whose constructor threw is an HTMLUnknownElement with that local name, which is what makes
   `el instanceof HTMLUnknownElement` the page's way to see that the upgrade failed. */
JSValue html_unknown_element_proto(JSContext *ctx);
/* HTML §3.2.2 Elements in the DOM's "element interface for an element with name `name` in the HTML namespace",
   AS THIS REALM'S INTERFACE PROTOTYPE OBJECT. OWNED. The caller has already decided the namespace is HTML —
   this asks nothing about a node, so it is the form §4.13.4 step 7.3 (`options.extends`) and §3.2.3 step 8.2
   (is this definition's local name one of the names the active function object's interface serves) need, both
   of which run for names no element carries. Compare the ANSWER against another interface prototype object;
   Web IDL §3.7.3 Interface prototype object gives an interface exactly one per realm, so identity of the
   object IS identity of the interface, and no interface NAME crosses the seam to be shadowed. */
JSValue html_element_interface_proto(JSContext *ctx, const char *name, size_t n);
/* The interface OBJECTS as globals — `HTMLElement`, `HTMLAnchorElement`, … Separate from the prototypes because
   they need a global to hang off, which the document install has and this does not. */
void html_element_install(JSContext *ctx, JSValueConst global);

#endif
