/* A Document's name index: the id and name values of the elements in its document tree — see
   document_name_index.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_NAME_INDEX_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_NAME_INDEX_H
#include <stdbool.h>
#include <stddef.h>
#include <lexbor/dom/dom.h>

/* Install the index as lexbor's tree observer, before any document holds a node; idempotent. */
void document_name_index_install(void);

/* Whether an element in `doc`'s document tree has a null-namespace `id` or `name` attribute whose value is the
   `n` bytes of `name`. HTML §3.1.7's supported property names of a Document and HTML §7.2.2.3's of a Window are
   among these values, so false answers "not a supported property name" with no walk. */
bool document_name_index_holds(lxb_dom_node_t *doc, const char *name, size_t n);

#endif
