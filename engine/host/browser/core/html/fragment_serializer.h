/* THE HTML FRAGMENT SERIALIZATION ALGORITHM — HTML §13.3, and the four members that ARE it. See
 * fragment_serializer.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_FRAGMENT_SERIALIZER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_FRAGMENT_SERIALIZER_H
#include "quickjs.h"
#include "core/idl_args.h"

/* WHICH INVOCATION — the magic every declaration of the one machine carries. §13.3 takes THE NODE plus two
   options, and the three magics differ in nothing else: where the walk starts, and whether the caller was
   allowed to state the options at all. */
enum {
    /* §8.5.4's innerHTML getter, on Element and on ShadowRoot: the node's CHILDREN, and — §8.5.4's own words —
       "HTML fragment serialization algorithm with node, FALSE, and « »", so no shadow root is ever emitted. */
    FRAGMENT_SERIALIZE_CHILDREN = 0,
    /* §8.5.5's outerHTML getter: the element ITSELF, as the only child of a fictional parent. */
    FRAGMENT_SERIALIZE_SELF,
    /* §8.5.3's getHTML(options): the node's children, with the two GetHTMLOptions the caller passed. */
    FRAGMENT_SERIALIZE_GET_HTML,
};

void fragment_serializer_init(JSContext *ctx);
void fragment_serializer_free(void);

/* THE ONE SERIALIZER, as the declaration innerHTML's and outerHTML's getters are declared against. There is no
   second one: `getHTML` differs from `innerHTML` in the value of two arguments, which is a magic and not
   another walk, and a private copy beside this one is how two members start disagreeing about `<template>`. */
const IdlStepDecl *fragment_serializer_decl(void);

/* §8.5.3's `DOMString getHTML(optional GetHTMLOptions options = {})`. The IDL declares the SAME operation on
   Element and on ShadowRoot, so it is one declaration installed on both prototypes. */
void fragment_serializer_install_get_html(JSContext *ctx, JSValueConst proto);

#endif
