/* The Element interface — Blink core/dom. One JS object per Lexbor element, per document. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#include <stddef.h>
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void    element_init(JSContext *ctx);
void    element_free(JSContext *ctx);

/* The wrapper for `el`, or JS_NULL. The SAME Lexbor element always yields the SAME JS object: a page compares
   nodes by identity constantly, and a fresh wrapper per lookup makes every such comparison silently false. */
JSValue element_wrap(JSContext *ctx, lxb_dom_element_t *el);

/* Element.prototype, borrowed — what HTMLElement is built on top of. */
/* §4.9's prototype for ONE realm — declared into core/realm.h's list. */
void element_install_proto(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue element_proto(JSContext *ctx);

/* A [Reflect]ed content attribute: the pair of names that IS the reflection, plus which kind of value it holds.
   A STRING reflection is the attribute's value ("" when absent); a BOOLEAN one is its PRESENCE, because
   `<input disabled="false">` is disabled and a string reflection would report the word. */
enum { REFLECT_STRING = 0, REFLECT_BOOL };
typedef struct { const char *idl; const char *attr; int kind; } ElReflect;

/* Install an interface's OWN reflections on its prototype. Each is assigned a magic out of one shared registry,
   so the two bodies that implement every reflection still take exactly one index. */
/* DECLARE a table of reflections once per AGENT; returns the BASE registry index the install names them by. */
int  element_declare_reflections(JSContext *ctx, const ElReflect *r, int n);
/* INSTALL the `n` reflections declared at `base` onto THIS realm's prototype. */
void element_install_reflections(JSContext *ctx, JSValueConst proto, int base, int n);

/* AN ELEMENT'S CONTENT ATTRIBUTE, through the same chokepoint the reflections use — so a component that reads
   and writes one (§4.6.3's hyperlink members re-serialise a URL back into `href`) stays captured in the
   running flow's DOM delta. element_attr_get returns an OWNED string, or NULL when the attribute is absent. */
char *element_attr_get(JSContext *ctx, JSValueConst el, const char *name);
void  element_attr_set(JSContext *ctx, JSValueConst el, const char *name, const char *value);

/* The element behind a wrapper, or NULL when the value is not one. */
lxb_dom_element_t *element_of_value(JSValueConst v);
/* THE ELEMENT AS §3.8 NAMES IT — its namespace URL and its local name, which together decide which interface it
   implements and therefore which row of the Trusted Types table it can match. Borrowed from Lexbor's interned
   strings into the caller's buffers; `*ns` is NULL for an element in no namespace. */
void element_ns_and_local(lxb_dom_element_t *el, const char **ns, const char **local,
                          char *nsbuf, size_t nscap, char *lobuf, size_t locap);

#endif
