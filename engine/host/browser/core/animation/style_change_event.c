/* The style change event's walk. See style_change_event.h. */
#include <stdbool.h>
#include <stdlib.h>

#include "check.h"
#include "core/animation/css_animations.h"
#include "core/animation/style_change_event.h"
#include "core/css/css_style_declaration.h"
#include "core/dom/named_walk.h"
#include "core/layout/layout_job.h"
#include "core/timing/thread_work.h"
#include "solver/rest_unit.h"

struct StyleChangeWalk {
    LayoutJob      *job;      /* owned */
    lxb_dom_node_t *root;     /* the running flow's nodes, shared by address */
    lxb_dom_node_t *cursor;   /* the element updated next; NULL once the walk is done */
    bool            subtree;
};

/* Lifetime counts over the process, read by the result document's census and by nothing that decides. */
static unsigned long long g_elements, g_rests;

unsigned long long style_change_walk_elements_life(void) { return g_elements; }
unsigned long long style_change_walk_rests_life(void) { return g_rests; }

StyleChangeWalk *style_change_walk_new(lxb_dom_node_t *root, bool self, bool subtree)
{
    StyleChangeWalk *w = calloc(1, sizeof *w);

    CHECK(w != NULL, "a style change event's walk could not be allocated");
    DCHECK(root != NULL && (!self || root->type == LXB_DOM_NODE_TYPE_ELEMENT),
           "a style change event was asked over no root, or over itself where it is not an element");
    w->job = layout_job_new();
    w->root = root;
    w->subtree = subtree;
    w->cursor = self ? root : subtree ? named_walk_successor(root, root) : NULL;
    return w;
}

void style_change_walk_destroy(JSContext *ctx, void *walk)
{
    StyleChangeWalk *w = walk;

    (void)ctx;
    layout_job_free(w->job);
    free(w);
}

/* Is `el`'s style readable now: its author match and its ancestors', which its `display: none` test reads? */
static bool walk_style_ask(lxb_dom_node_t *el)
{
    lxb_dom_node_t *e;

    for (e = el; e != NULL && e->type == LXB_DOM_NODE_TYPE_ELEMENT; e = e->parent)
        if (!cssom_style_ask(lxb_dom_interface_element(e))) return false;
    return true;
}

int style_change_walk_run(JSContext *ctx, JSStepHdr *hdr, StyleChangeWalk *w)
{
    LayoutJobDrive d;
    int rc;

    rc = layout_job_fork(ctx, hdr, w->job);
    if (rc) return rc;
    if (w->cursor == NULL) return 0;
    rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
    layout_job_enter(w->job);
    layout_job_drive_begin_resting(&d);
    while (w->cursor != NULL) {
        if (!walk_style_ask(w->cursor)) {
            DCHECK(layout_job_drive_spent(&d), "a style change event parked an author match with the drive's "
                                               "allowance unspent, so its next activation would ask the same "
                                               "thing over the same state for ever");
            break;
        }
        thread_work_charge(THREAD_WORK_DOM_WALK, 1);
        css_animations_update(lxb_dom_interface_element(w->cursor));
        g_elements++;
        w->cursor = w->subtree ? named_walk_successor(w->cursor, w->root) : NULL;
        if (layout_job_drive_spent(&d)) break;
    }
    layout_job_drive_end(&d);
    layout_job_leave(w->job);
    rest_unit_end();
    if (layout_job_declined(w->job)) {
        rc = layout_job_fork(ctx, hdr, w->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (w->cursor == NULL) return 0;
    layout_job_rested(w->job);
    g_rests++;
    return JS_STEP_YIELD;
}
