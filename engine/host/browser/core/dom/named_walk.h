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

/* A walk's resume point, kept in a step machine's state: the element last visited and the first two matches.
   Zeroed means not started. Nodes only, so a step state holding one needs no visit. */
typedef struct {
    lxb_dom_node_t *root, *cursor, *first;
    int count;
} NamedWalk;

/* The element after `n` in tree order within `root`'s subtree, or NULL past its end. */
lxb_dom_node_t *named_walk_successor(lxb_dom_node_t *n, lxb_dom_node_t *root);

/* Visits the next element of `root`'s subtree under `is`, charging one element. True while the walk has more to
   visit, so the caller rests; false once the subtree is exhausted or a second match is found. A walk keeps the
   root it began with, asserted on every call. */
bool named_walk_next(JSContext *ctx, NamedWalk *w, lxb_dom_node_t *root, NamedWalkIs is, const char *name,
                     size_t n, const void *ud);

#endif
