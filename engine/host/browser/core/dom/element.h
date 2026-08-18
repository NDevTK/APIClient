/* The Element interface — Blink core/dom. One JS object per Lexbor element, per document. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#include <stddef.h>
#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "core/idl_args.h"

void    element_init(JSContext *ctx);
void    element_free(JSRuntime *rt);

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
    /* §8.5.2's `setHTML` — the SAFE member — on the same two interfaces. It is the same machine and not a
       sixth algorithm: §8.6.4's `set and filter HTML` is what all four of these are, and `safe` is the one
       argument that differs. What `safe` decides is stated where §8.6.4 states it — the sanitizer the options
       resolve to, and §8.6.4's step 3 removal of what is unsafe from it. */
    ELEMENT_SET_HTML,             /* §8.5.2's setHTML, on Element */
    SHADOW_ROOT_SET_HTML,         /* §8.5.2's setHTML, on ShadowRoot */
};
const IdlStepDecl *element_set_html_decl(void);
/* §8.5.2's `setHTMLUnsafe` DECLARED for one of the two interfaces that have it — one argument list and one
   dictionary, because the IDL states the same line on Element and on ShadowRoot. Returns the step id. */
int element_declare_set_html_unsafe(JSContext *ctx, int magic);
/* §8.5.2's `setHTML` for one of the same two interfaces. Its own declaration and not a magic on the one above,
   because the IDL is a DIFFERENT line: `setHTML(DOMString html, optional SetHTMLOptions options = {})` takes no
   TrustedHTML union (there is nothing to trust — the sanitizer is what makes it safe) and its dictionary has
   ONE member where SetHTMLUnsafeOptions has two. Returns the step id. */
int element_declare_set_html(JSContext *ctx, int magic);

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
   `<input disabled="false">` is disabled and a string reflection would report the word.
   A NULLABLE STRING is HTML §2.6.1's `DOMString?` processing model and it is a THIRD kind rather than the
   first one read leniently: its getter answers NULL for an absent attribute where `DOMString` answers "", and
   its setter DELETES the attribute for null where `DOMString` would write the four characters "null". Every
   one of ARIAMixin's 44 string members is this kind, and `el.ariaLabel === null` is how a page tests one.
   A URL is HTML §2.6.1's "type USVString, treated as a URL" — §2.6.2's `[ReflectURL]`, which that section says
   may appear on nothing but a `USVString`. Its getter RESOLVES: encoding-parse-and-serialize the attribute
   value against the ELEMENT'S NODE DOCUMENT, so `<img src="/x">` reads back `http://host/x` and not `/x`. A
   REFLECT_STRING row for one of these is not a lenient reading, it is a different value — the mirror answers
   every relative URL wrong, silently, and `img.src`, `iframe.src`, `link.href`, `video.poster`, `source.src`,
   `object.data` and eleven more are declared that way today. */
enum { REFLECT_STRING = 0, REFLECT_BOOL, REFLECT_STRING_NULLABLE, REFLECT_URL };
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
/* WEB IDL §3.2.15's "V implements Element" — the narrowing an `Element` argument brands with, since every node
   wrapper is ONE class and a class id therefore cannot say which interface the node implements. */
bool element_is(JSValueConst v);
/* THE ELEMENT AS §3.8 NAMES IT — its namespace URL and its local name, which together decide which interface it
   implements and therefore which row of the Trusted Types table it can match. Borrowed from Lexbor's interned
   strings into the caller's buffers; `*ns` is NULL for an element in no namespace. */
void element_ns_and_local(lxb_dom_element_t *el, const char **ns, const char **local,
                          char *nsbuf, size_t nscap, char *lobuf, size_t locap);
/* DOM §4.9 `prefix` — the reader above's nullable third name, BORROWED from the document's prefix hash and NUL
   terminated by nothing. NULL with `*len` 0 is §1.4's null prefix; `*len` is written on every path, which is
   the whole reason this exists rather than lxb_dom_element_prefix (see element.c). */
const char *element_prefix(lxb_dom_element_t *el, size_t *len);
/* DOM §4.5 "create an element internal"'s storage step — "Set element's namespace to namespace, namespace
   prefix to prefix, local name to localName". The WRITER whose reader is the function above, which is why the
   two are declared together: what the standard calls "as given" is a property of how those three strings are
   interned, and lexbor's own element creation interns all three CASE-FOLDED. See element.c for the
   measurement. `ns` and `prefix` are NULL when there is none (§1.4's null, not the empty string); all three
   slices are borrowed and none is NUL-terminated. Never returns NULL — an allocation failure is fatal. */
lxb_dom_element_t *element_create_ns(lxb_dom_document_t *doc, const char *ns, size_t ns_len,
                                     const char *local, size_t local_len,
                                     const char *prefix, size_t prefix_len);

#endif
