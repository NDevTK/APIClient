/* The style change event's walk: the elements whose CSS animations core/animation/css_animations.h updates when
 * their style is resolved, as a resting step that a step machine owns.
 *
 * The walk holds a layout job of its own and updates elements under a resting drive, charged as a DOM walk; it
 * rests where the drive's allowance is spent or an element's author match parks, and forks the owning machine
 * where a selector cannot be decided. It is a re-derivable cache: a fork sheds it, and the copy walks again,
 * which updates nothing a second time because the update is a function of the computed values. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_STYLE_CHANGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_STYLE_CHANGE_EVENT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"

typedef struct StyleChangeWalk StyleChangeWalk;

/* A walk over `root` itself when `self` (an element), and its descendant elements when `subtree`. OWNED. */
StyleChangeWalk *style_change_walk_new(lxb_dom_node_t *root, bool self, bool subtree);
/* The JSStepVisit cache destructor a holder declares the walk through. */
void style_change_walk_destroy(JSContext *ctx, void *walk);
/* One activation for the machine `hdr`: 0 when every element is updated, otherwise the step code the machine
   returns (a rest, or a fork's). */
int style_change_walk_run(JSContext *ctx, JSStepHdr *hdr, StyleChangeWalk *walk);

/* The process's census of the walks: the elements they updated, and the rests they took. */
unsigned long long style_change_walk_elements_life(void);
unsigned long long style_change_walk_rests_life(void);

#endif
