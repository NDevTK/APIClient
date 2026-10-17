/* CSS 2.2 §9.2.1.1 "Anonymous block boxes"' run of a block container's content, walked in tree order as a
 * cursor held between steps: a stack of the boxes whose children are being walked. It is one walk for the
 * readers that collect a run's content (core/layout/line_box.c's fill and core/layout/intrinsic_size.c's
 * measurement), so they agree about which members the run holds and where each box's closing edge falls; each
 * reader classifies the children itself, because the two give a member different sizes.
 *
 * §9.2.1.1 breaks an inline box around an in-flow block-level box inside it, so a run ends at a node the walk
 * may meet at any depth. Every box still open there stays open on the run's last line: "the border would be
 * drawn around C1 (open at the end of the line) and C2 (open at the start of the line)", so it owes no closing
 * edge on this run, and a run that begins inside a box owes it no opening edge. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_INLINE_RUN_WALK_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_INLINE_RUN_WALK_H
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/layout/block_flow.h"
#include "core/layout/box_tree.h"

typedef struct {
    lxb_dom_element_t *el;
    BoxTreeChild c;    /* the next child to take */
    /* One of the run's open fragments, whose children run to the run's end (`block_flow_run_next`) and whose
       end steps out to its box parent, rather than an inline box the walk descended into. */
    bool fragment;
} InlineRunFrame;

typedef struct {
    lxb_dom_node_t *root;
    lxb_dom_node_t *end;      /* exclusive, and reachable at any depth; NULL runs to the end of the content */
    InlineRunFrame *frames;   /* owned until `end` or `release` */
    size_t n, cap;
    bool past_end;            /* the walk met `end` */
    bool pending;             /* `next` named a child that is not yet taken */
    bool done;
} InlineRunWalk;

typedef enum {
    INLINE_RUN_CHILD,   /* `*parent`'s member `*c` is next; the reader classifies it, then takes it */
    INLINE_RUN_CLOSE,   /* `*parent`'s box is complete on this run, so its closing edge is owed here */
    INLINE_RUN_END      /* the run is walked; `done` */
} InlineRunNext;

/* Begin at the start core/layout/block_flow.h's `block_flow_run_start` answers for `style`'s run. */
void inline_run_walk_begin(InlineRunWalk *w, lxb_dom_element_t *style, BlockFlowRun run);

/* The walk's next event. A CHILD is named again by every `next` until `take`, so a reader that cannot yet size
   it can stop there; a CLOSE and the END are taken by `next` itself. */
InlineRunNext inline_run_walk_next(InlineRunWalk *w, lxb_dom_element_t **parent, BoxTreeChild *c);

/* Take the child `next` named. `inline_box` is NULL, or that child as an inline box whose box-tree children
   are walked next, with its CLOSE after them; its opening edge is the reader's to emit before this. */
void inline_run_walk_take(InlineRunWalk *w, lxb_dom_element_t *inline_box);

/* Close a walk that reached its END, asserting it met the run's end where the run has one; or free one
   abandoned anywhere. Each walk owes exactly one of the two. */
void inline_run_walk_end(InlineRunWalk *w);
void inline_run_walk_release(InlineRunWalk *w);

#endif
