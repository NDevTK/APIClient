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

/* Those elements, one entry per attribute carrying the name (an element whose id and name both carry it is listed
   twice, adjacently), in tree order: their count, and the list at `*out`, borrowed until the next tree write. */
size_t document_name_index_list(lxb_dom_node_t *doc, const char *name, size_t n, lxb_dom_element_t *const **out);

/* The values some element of `doc`'s tree carries, ordered by their bytes, a function of the tree: how many, and
   the `i`th (`*len` bytes, borrowed until the next tree write). */
size_t document_name_index_values(lxb_dom_node_t *doc);
const char *document_name_index_value_at(lxb_dom_node_t *doc, size_t i, size_t *len);

/* Dev builds: recount every live document's id and name values from its tree and assert each equals its
   index, naming the first value that differs and the caller's site. The index's correctness rests on every
   tree write passing an observed primitive; this states it at rest points (a session's close, a cold resume). */
void document_name_index_verify_all_at(const char *file, int line);
#define document_name_index_verify_all() document_name_index_verify_all_at(__FILE__, __LINE__)

#endif
