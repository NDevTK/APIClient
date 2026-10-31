/* HTML §3.1.7 "DOM tree accessors" — the Document's named properties, and Web IDL §3.9's legacy platform object
 * internal methods the Document's [LegacyOverrideBuiltIns] gives them. Blink's DocumentNameCollection and its
 * named-item lookup. See document_named.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_NAMED_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_NAMED_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* §3.1.7's "named element with the name name", over `el`'s current content attributes. `name` is `n` bytes. */
bool document_named_element_is(lxb_dom_element_t *el, const char *name, size_t n);

/* The exotic methods the Document classes carry: a Document supports named properties, so it is a legacy
   platform object (Web IDL §3.9). */
const JSClassExoticMethods *document_named_exotic(void);
/* Declares the named-access step member; the agent's, from document_init. */
void document_named_init(JSContext *ctx);
void document_named_free(void);

#endif
