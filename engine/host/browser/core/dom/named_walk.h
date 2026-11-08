/* The resting walk behind the named getters that count a tree's named elements for a name: HTML §3.1.7's
 * Document named properties and §7.2.2.3's Window named access. One element per call, charged
 * THREAD_WORK_DOM_WALK, so a step machine rests between elements and a lookup's cost is the scheduler's work.
 * HTML §4.10.3's form element named properties walk it too, asking each element's form owner. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NAMED_WALK_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NAMED_WALK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Whether `el` is a named element with the name `name` (`n` bytes) under the asking algorithm's rule; `ud` is the
   asker's, borrowed (the form whose named properties are asked, or NULL). */
typedef bool (*NamedWalkIs)(JSContext *ctx, lxb_dom_element_t *el, const char *name, size_t n, const void *ud);

/* A walk's resume point, kept in a step machine's state: the element last visited (or, for an indexed walk, the
   position in the name's element list) and the first two matches. Zeroed means not started. Nodes and a count
   only, so a step state holding one needs no visit. */
typedef struct {
    lxb_dom_node_t *root, *cursor, *first;
    size_t pos;
    int count;
} NamedWalk;

/* The element after `n` in tree order within `root`'s subtree, or NULL past its end. */
lxb_dom_node_t *named_walk_successor(lxb_dom_node_t *n, lxb_dom_node_t *root);

/* Visits the next element of `root`'s subtree under `is`, charging one element. True while the walk has more to
   visit, so the caller rests; false once the subtree is exhausted or a second match is found. A walk keeps the
   root it began with, asserted on every call. */
bool named_walk_next(JSContext *ctx, NamedWalk *w, lxb_dom_node_t *root, NamedWalkIs is, const char *name,
                     size_t n, const void *ud);

/* The same walk over only the elements `doc`'s name index lists for `name`, in tree order, within `root`'s
   subtree (`root` is `doc` or a connected node of its tree): one listed element per call, charged as one visited
   element. For a rule `is` that matches an element only by an `id` or `name` value equal to `name`, the answer is
   the tree walk's, at the cost of the elements carrying the name. The list is the tree's function, so a resume
   in the same world continues at the same element. */
bool named_walk_next_indexed(JSContext *ctx, NamedWalk *w, lxb_dom_node_t *doc, lxb_dom_node_t *root,
                             NamedWalkIs is, const char *name, size_t n, const void *ud);

/* Whether any of the `k` names a named-element rule gives `el` (`v[i]`, `len[i]` bytes) lies in the domain of the
   unknown String key `key` (concolic_key_domain_holds). Every name is asserted to be one the rule's own lookup
   `is` (with `ud`) answers for `el`, so the domain answer and the named lookup stay one fact; `file`/`line` are
   the caller's. */
bool named_walk_names_in_domain_at(JSContext *ctx, lxb_dom_element_t *el, const lxb_char_t *const v[],
                                   const size_t len[], int k, NamedWalkIs is, const void *ud, JSValueConst key,
                                   const char *file, int line);
#define named_walk_names_in_domain(ctx, el, v, len, k, is, ud, key) \
    named_walk_names_in_domain_at((ctx), (el), (v), (len), (k), (is), (ud), (key), __FILE__, __LINE__)

#endif
