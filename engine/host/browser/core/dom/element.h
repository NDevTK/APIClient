/* The Element interface — Blink core/dom. One JS object per Lexbor element, per document. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#include <stddef.h>
#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "core/idl_args.h"

void    element_init(JSContext *ctx);
void    element_free(JSContext *ctx);

/* HTML §13.4's FRAGMENT PARSE, and WHICH MEMBER is driving it — the magic every declaration of the one machine
   carries. Five members over one parse: they differ in the TARGET whose children the fragment replaces, in the
   CONTEXT element the markup is parsed in, and in whether §13.4's `allowDeclarativeShadowRoots` is true (which
   is what makes `<template shadowrootmode>` inside the markup a real shadow root, and which only the two
   `Unsafe` members pass). ShadowRoot's two are declared HERE rather than in a second machine beside it,
   because §8.5.4's and §8.5.2's steps for a ShadowRoot receiver are the same algorithm over a different
   target — the difference the standard states is §13.4 step 2's "otherwise target's host". */
enum {
    ELEMENT_SET_INNER_HTML = 0,   /* §8.5.4's innerHTML setter, on Element */
    ELEMENT_SET_OUTER_HTML,       /* §8.5.5's outerHTML setter */
    ELEMENT_SET_HTML_UNSAFE,      /* §8.5.2's setHTMLUnsafe, on Element */
    SHADOW_ROOT_SET_INNER_HTML,   /* §8.5.4's innerHTML setter, on ShadowRoot */
    SHADOW_ROOT_SET_HTML_UNSAFE,  /* §8.5.2's setHTMLUnsafe, on ShadowRoot */
};
const IdlStepDecl *element_set_html_decl(void);
/* §8.5.2's `setHTMLUnsafe` DECLARED for one of the two interfaces that have it — one argument list and one
   dictionary, because the IDL states the same line on Element and on ShadowRoot. Returns the step id. */
int element_declare_set_html_unsafe(JSContext *ctx, int magic);

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
