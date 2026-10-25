/* CSS 2.2 §9.2.1.1's run of a block container's content as a held tree-order walk. See inline_run_walk.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_tree.h"
#include "core/layout/inline_run_walk.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

static void irw_push(InlineRunWalk *w, lxb_dom_element_t *el, BoxTreeChild c, bool fragment)
{
    if (w->n == w->cap) {
        size_t cap = w->cap != 0 ? w->cap * 2 : 8;
        InlineRunFrame *grown = lexbor_realloc(w->frames, cap * sizeof *grown);

        CHECK(grown != NULL, "out of memory holding the inline boxes one inline formatting context's walk is in");
        w->frames = grown;
        w->cap = cap;
    }
    w->frames[w->n].el = el;
    w->frames[w->n].c = c;
    w->frames[w->n].fragment = fragment;
    w->n++;
}

void inline_run_walk_begin(InlineRunWalk *w, lxb_dom_element_t *style, BlockFlowRun run)
{
    BlockFlowRunStart start;

    DCHECK(w != NULL && style != NULL, "CSS 2.2 §9.2.1.1's run was walked with no walk or no block container");
    memset(w, 0, sizeof *w);
    w->root = lxb_dom_interface_node(style);
    w->end = run.end;
    /* The run's open fragment, which `after` alone cannot name: §9.2.1.1 splits an inline box "into two boxes
       (even if either side is empty)", so a run after a break inside one begins inside that box with no
       opening edge of its own and may hold no node while still carrying the box's closing edge.
       block_flow.h answers the start, because css-display-3 §2.5 "Box Generation: the none and contents
       keywords"' splice makes "the next sibling of `after` in `after`'s parent" wrong. */
    start = block_flow_run_start(style, run);
    irw_push(w, start.open, start.at, true);
}

InlineRunNext inline_run_walk_next(InlineRunWalk *w, lxb_dom_element_t **parent, BoxTreeChild *c)
{
    InlineRunFrame *top;

    DCHECK(!w->done && w->n > 0, "a run's walk was stepped after its end");
    top = &w->frames[w->n - 1];
    if (box_tree_child_exists(top->c)) {
        /* The run's end, tested at every depth and first: §9.2.1.1's in-flow block-level box "becomes a sibling
           of those anonymous boxes", so it and everything after it are on a later box of §9.4.1's stack. */
        if (box_tree_child_is_node(top->c, w->end)) {
            w->past_end = true;
            w->done = true;
            return INLINE_RUN_END;
        }
        *parent = top->el;
        *c = top->c;
        w->pending = true;
        return INLINE_RUN_CHILD;
    }
    DCHECK(!w->pending, "a run's walk moved past a child its reader never took");
    *parent = top->el;
    if (!top->fragment) {
        /* The inline box descended into is complete on this run. */
        w->n--;
        return INLINE_RUN_CLOSE;
    }
    {
        lxb_dom_node_t *box = lxb_dom_interface_node(top->el);
        lxb_dom_element_t *up;

        if (box == w->root) {
            w->done = true;
            return INLINE_RUN_END;
        }
        /* Leaving a fragment that was already open (§9.2.1.1's "C2 (open at the start of the line)"), whose
           closing edge is owed here. The step is to the ancestor's next sibling, never the ancestor, which would
           re-walk the fragment just closed, and it is over the box tree: css-display-3 §2.5's splice makes a
           `contents` element between the fragment and its box parent no box to leave into. */
        up = box_tree_parent(box_tree_child_of_node(box));
        DCHECK(up != NULL,
               "CSS 2.2 §9.2.1.1's run was inside a box with no box parent, so the walk cannot leave it — the box "
               "ancestors of every position in a container's content are inline boxes up to the container itself, "
               "and this chain does not reach it");
        top->c = box_tree_next_sibling(up, box_tree_child_of_node(box));
        top->el = up;
        return INLINE_RUN_CLOSE;
    }
}

void inline_run_walk_take(InlineRunWalk *w, lxb_dom_element_t *inline_box)
{
    InlineRunFrame *top;
    BoxTreeChild c;

    DCHECK(w->pending && !w->done, "a run's walk was asked to take a child its `next` did not name");
    top = &w->frames[w->n - 1];
    c = top->c;
    DCHECK(inline_box == NULL || box_tree_child_is_node(c, lxb_dom_interface_node(inline_box)),
           "a run's walk was asked to descend into a box that is not the child it named");
    /* The box-tree step: css-display-3 §2.5 puts a `contents` child's children in this box's content, and
       box_tree.h never yields the `contents` element itself. */
    top->c = top->fragment ? block_flow_run_next(top->el, c) : box_tree_next_sibling(top->el, c);
    w->pending = false;
    if (inline_box != NULL) irw_push(w, inline_box, box_tree_first_child(inline_box), false);
}

void inline_run_walk_end(InlineRunWalk *w)
{
    DCHECK(w->done, "a run's walk was closed before its end");
    DCHECK(w->end == NULL || w->past_end,
           "the run handed to CSS 2.2 §9.4.2's inline formatting context ran off the end of the container's "
           "content without meeting its exclusive end, so the two came from different trees or the tree changed "
           "under the walk, and what was collected is a prefix of a formatting context no box has");
    inline_run_walk_release(w);
}

void inline_run_walk_release(InlineRunWalk *w)
{
    free(w->frames);
    w->frames = NULL;
    w->n = w->cap = 0;
}
