/* css-display-3 §1 "Introduction"'s box tree on its child axis: §2.5's `contents` splice, css-lists-3 §3.1's
   marker member and HTML §15.5.12's label member. See box_tree.h for the contract and for why the member is a
   pair rather than a node. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_style_declaration.h"   /* the style ask a child walk makes before its reads */
#include "core/dom/flat_tree.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/button_label.h"
#include "core/layout/flow_placement.h"   /* where a stopped children style ask holds its place */
#include "core/layout/layout_job.h"       /* the resting drive and its scan allowance */
#include "core/layout/list_marker.h"

/* A zero-initialised member is a source-document node. */
_Static_assert(BOX_TREE_NODE == 0, "a zero-initialised box-tree child must be a source-document node");

/* `<li> (display `list-item`)::marker`, `<input> (…) label`, or the node's own subject for a source member. */
static const char *bt_subject(BoxTreeChild c, char *buf, size_t cap)
{
    char inner[160];

    if (!box_tree_child_is_generated(c)) return box_subject_node(c.node, buf, cap);
    snprintf(buf, cap, "%s%s", box_subject_node(c.node, inner, sizeof inner),
             box_tree_child_is_marker(c) ? "::marker" : " label");
    return buf;
}

/* Is this node one §2.5 replaces by its contents? Only an element with the computed value `contents`: the
   other keyword elides the subtree ("The element and its descendants generate no boxes or text sequences"),
   so a `none` element answers FALSE and stays in the sequence for each walk's own arm to skip. */
static bool bt_is_spliced(lxb_dom_node_t *n)
{
    CssDisplay d;
    bool spliced;

    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    d = css_computed_display(lxb_dom_interface_element(n));
    spliced = css_display_spells(d, CSS_KEYWORD_CONTENTS);
    return spliced;
}

/* Where `box`'s sequence continues after node `n`: its next sibling, or, past the last child of an element
   the sequence was spliced through, that element's next sibling, out to `box`. NULL is the end. The ascent is
   what makes the position after a spliced element's last child the position after the element. */
static lxb_dom_node_t *bt_continue_after(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(box);

    for (; n != root; n = flat_tree_parent(n)) {
        lxb_dom_node_t *next = flat_tree_next_sibling(n);

        if (next != NULL) return next;
        DCHECK(flat_tree_parent(n) != NULL,
               "css-display-3 §2.5's spliced child sequence was stepped out of the top of the tree without ever "
               "reaching the box it was asked about, so the node it was stepped from is not in that box's "
               "sequence at all — the ascent only ever passes through elements §2.5 replaced by their contents");
    }
    return NULL;
}

/* The first position at or after `n` that is a member rather than a splice into the sequence. An empty
   `contents` element is replaced by nothing and the walk continues past it; the loop ends because every arm
   moves strictly forward in document order over a finite tree. */
static lxb_dom_node_t *bt_resolve(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    while (n != NULL && bt_is_spliced(n))
        n = flat_tree_first_child(n) != NULL ? flat_tree_first_child(n) : bt_continue_after(box, n);
    return n;
}

bool box_tree_children_style_ask(lxb_dom_element_t *box)
{
    lxb_dom_node_t *n;

    DCHECK(box != NULL, "a box's children's styles were asked of no box");
    /* Off a resting drive every style ask answers at once, so there is no walk to rest. */
    if (!layout_job_rests_innermost()) return true;
    if (!flow_placement_children_style_peek(box, &n)) n = flat_tree_first_child(lxb_dom_interface_node(box));
    else if (n == NULL) return true;
    while (n != NULL) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (!layout_job_scan_take() || !cssom_style_ask(lxb_dom_interface_element(n))) {
                flow_placement_children_style_hold(box, n);
                return false;
            }
            if (bt_is_spliced(n) && flat_tree_first_child(n) != NULL) {
                n = flat_tree_first_child(n);
                continue;
            }
        }
        n = bt_continue_after(box, n);
    }
    flow_placement_children_style_hold(box, NULL);
    return true;
}

/* cssom_style_ask of each element `bt_resolve` reads from `n`, in its order: false at the first that parked. */
static bool bt_resolve_ask(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    while (n != NULL) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && !cssom_style_ask(lxb_dom_interface_element(n))) return false;
        if (!bt_is_spliced(n)) return true;
        n = flat_tree_first_child(n) != NULL ? flat_tree_first_child(n) : bt_continue_after(box, n);
    }
    return true;
}

bool box_tree_first_child_ask(lxb_dom_element_t *box)
{
    DCHECK(box != NULL, "a box's first member was asked of no box");
    if (!cssom_style_ask(box)) return false;
    if (button_label_generated(box)) return true;
    return bt_resolve_ask(box, flat_tree_first_child(lxb_dom_interface_node(box)));
}

bool box_tree_next_sibling_ask(lxb_dom_element_t *box, BoxTreeChild child)
{
    DCHECK(box != NULL && child.node != NULL, "a box's next member was asked with no box, or from no node");
    if (box_tree_child_is_label(child)) return true;
    if (box_tree_child_is_marker(child))
        return button_label_generated(box) || bt_resolve_ask(box, flat_tree_first_child(child.node));
    return bt_resolve_ask(box, bt_continue_after(box, child.node));
}

/* `bt_continue_after` read backward: the position before a spliced element's first child is the position
   before the element. */
static lxb_dom_node_t *bt_continue_before(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(box);

    for (; n != root; n = flat_tree_parent(n)) {
        lxb_dom_node_t *prev = flat_tree_prev_sibling(n);

        if (prev != NULL) return prev;
        DCHECK(flat_tree_parent(n) != NULL,
               "css-display-3 §2.5's spliced child sequence was stepped BACKWARD out of the top of the tree "
               "without ever reaching the box it was asked about, so the node it was stepped from is not in "
               "that box's sequence at all — the ascent only ever passes through elements §2.5 replaced by "
               "their contents");
    }
    return NULL;
}

/* `bt_resolve` read backward: it descends to a spliced element's LAST child, since the contents occupy the
   element's place in order. */
static lxb_dom_node_t *bt_resolve_back(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    while (n != NULL && bt_is_spliced(n))
        n = flat_tree_last_child(n) != NULL ? flat_tree_last_child(n) : bt_continue_before(box, n);
    return n;
}

/* The member a list item's sequence opens with when it has a marker box (css-lists-3 §3.1). */
static BoxTreeChild bt_marker_of(lxb_dom_element_t *box)
{
    BoxTreeChild c = { lxb_dom_interface_node(box), BOX_TREE_MARKER };

    return c;
}

static BoxTreeChild bt_label_of(lxb_dom_element_t *box)
{
    BoxTreeChild c = { lxb_dom_interface_node(box), BOX_TREE_LABEL };

    return c;
}

/* What follows the marker, or opens a sequence with none: an input button's label, which is its whole content
   (HTML §15.5.12: "its contents are expected to be the text of the element's value attribute"), or the first
   source-document member. */
static BoxTreeChild bt_first_content_member(lxb_dom_element_t *box)
{
    if (button_label_generated(box)) return bt_label_of(box);
    return box_tree_child_of_node(bt_resolve(box, flat_tree_first_child(lxb_dom_interface_node(box))));
}

/* Is `n` a member of `box`'s sequence? `box_tree_parent` is the box a member's box is a child of, so
   membership and box-parenthood are one equality, shared by the input and the answer refusals below so the
   two cannot disagree about a tree. */
static bool bt_in_sequence(lxb_dom_element_t *box, BoxTreeChild n)
{
    return box_tree_parent(n) == box;
}

/* REFUSES A STEP FROM A NON-MEMBER — a node with a box-generating element between it and `box`, or a generated
   member this sequence did not yield. Asked at the entry rather than on the ascent's path, which returns at
   the first node that has a sibling and would pass for exactly the half-converted walk this exists to catch.
   The guard sits in the body so the call is declared at both settings: a release DCHECK type-checks its
   condition without evaluating it, and `-Werror=implicit-function-declaration` refuses a dev-only predicate. */
static void bt_require_in_sequence(lxb_dom_element_t *box, BoxTreeChild child)
{
#if APICLIENT_DEV
    char nbuf[200], bbuf[160];

    if (box_tree_child_is_generated(child)) {
        DCHECKF(child.node == lxb_dom_interface_node(box) &&
                    (box_tree_child_is_marker(child) ? list_marker_box_generated(box)
                                                     : button_label_generated(box)),
                "%s, stepped as a child of %s: a generated member this sequence does not yield. It yields "
                "css-lists-3 §3.1's ::marker at the head of a list item's sequence where "
                "`list_marker_box_generated` answers TRUE, and HTML §15.5.12's label as an input button's "
                "content where `button_label_generated` does",
                bt_subject(child, nbuf, sizeof nbuf), box_subject(box, bbuf, sizeof bbuf));
        return;
    }
    if (bt_in_sequence(box, child)) return;
    DFAILF("%s, stepped as a child of %s: css-display-3 §2.5 \"Box Generation: the none and contents "
           "keywords\"' spliced child sequence was stepped from a node that is not IN it. Between that node and "
           "this box stands an element that GENERATES A BOX, so the node is a child of THAT box's sequence and "
           "not of this one — §2.5 replaces only a `contents` element by its contents. A walk converted halfway "
           "hands this a node it still reached through `->next`",
           bt_subject(child, nbuf, sizeof nbuf), box_subject(box, bbuf, sizeof bbuf));
#else
    (void) box;
    (void) child;
#endif
}

/* REFUSES A FIRST-CHILD ANSWER THAT IS NOT A MEMBER. A walk that steps re-checks every later member through
   the refusal above; a caller that takes the first child without stepping hands it to nothing, and
   core/layout/block_flow.c has such callers. */
static void bt_require_answer_is_in_sequence(lxb_dom_element_t *box, BoxTreeChild answer)
{
#if APICLIENT_DEV
    char nbuf[200], bbuf[160];

    if (!box_tree_child_exists(answer) || bt_in_sequence(box, answer)) return;
    DFAILF("%s, answered as the first child of %s: css-display-3 §2.5 \"Box Generation: the none and contents "
           "keywords\"' spliced child sequence ANSWERED a member that is not in it — every member it yields "
           "stands under `box` through nothing but elements §2.5 replaced by their contents, so "
           "`box_tree_parent` of it is `box`",
           bt_subject(answer, nbuf, sizeof nbuf), box_subject(box, bbuf, sizeof bbuf));
#else
    (void) box;
    (void) answer;
#endif
}

lxb_dom_node_t *box_tree_child_node_at(BoxTreeChild c, const char *file, int line)
{
    char nbuf[200];

    DCHECKF(!box_tree_child_is_generated(c),
            "%s:%d met %s: a generated member of core/layout/box_tree.h's sequence (css-lists-3 §3.1's ::marker "
            "or HTML §15.5.12's label) reached a walk that reads it as a source-document node. BUILD that walk's "
            "arm for it (core/layout/block_flow.h's `block_flow_child_kind` classifies both)",
            file, line, bt_subject(c, nbuf, sizeof nbuf));
    (void) file;
    (void) line;
    (void) nbuf;
    return c.node;
}

lxb_dom_element_t *box_tree_parent(BoxTreeChild c)
{
    lxb_dom_node_t *p;

    DCHECK(c.node != NULL, "css-display-3 §2.5's box parent was asked of no node");
    /* A generated member's box is a child of its originating element's box: css-lists-3 §3.1 generates the marker
       "as the list item’s first child", and HTML §15.5.12's label is the element's contents. */
    if (box_tree_child_is_generated(c)) {
        DCHECK(c.node->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "a generated member's originating node is not an element — css-pseudo-4 §1 "
               "\"Introduction\" associates each pseudo-element with an originating ELEMENT");
        return lxb_dom_interface_element(c.node);
    }
    /* §2.5's replacement read upward: the box parent is the first ancestor past every `contents` element. A
       spliced `n` is answered rather than refused (see box_tree.h). The loop ends at a non-element parent,
       which is the root's own case. */
    for (p = flat_tree_parent(c.node); p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT;
         p = flat_tree_parent(p))
        if (!bt_is_spliced(p)) return lxb_dom_interface_element(p);
    return NULL;
}

BoxTreeChild box_tree_first_child(lxb_dom_element_t *box)
{
    BoxTreeChild first;

    DCHECK(box != NULL, "css-display-3 §2.5's spliced child sequence was asked for of no box");
    DCHECK(!bt_is_spliced(lxb_dom_interface_node(box)),
           "css-display-3 §2.5's spliced child sequence was asked for of an element whose own computed "
           "`display` is `contents` — it generates no box for these nodes to be the children OF, and §2.5 puts "
           "them in that element's own box parent's sequence instead. Ask the box parent, which "
           "core/css/css_computed_value.h's `css_box_parent_display` is the other direction of");
    /* css-lists-3 §3.1 "The ::marker Pseudo-Element": the marker box is "the list item’s first child", where
       css-lists-3 §3.2 "Generating Marker Contents" generates one at all. */
    if (list_marker_box_generated(box)) return bt_marker_of(box);
    first = bt_first_content_member(box);
    bt_require_answer_is_in_sequence(box, first);
    return first;
}

BoxTreeChild box_tree_next_sibling(lxb_dom_element_t *box, BoxTreeChild child)
{
    DCHECK(box != NULL && child.node != NULL,
           "css-display-3 §2.5's spliced child sequence was stepped with no box, or from no node");
    bt_require_in_sequence(box, child);
    /* The marker heads the sequence, so the member after it is the first content member; the label is an input
       button's whole content, so nothing follows it. */
    if (box_tree_child_is_marker(child)) return bt_first_content_member(box);
    if (box_tree_child_is_label(child)) return box_tree_child_of_node(NULL);
    DCHECK(!button_label_generated(box),
           "an input button's sequence was stepped from a source-document member, which HTML §15.5.12 does not "
           "make its contents — its one content member is its label");
    DCHECK(!bt_is_spliced(child.node),
           "css-display-3 §2.5's spliced child sequence was stepped FROM an element whose own computed "
           "`display` is `contents`. This sequence never yields one — §2.5 replaces it by its contents — so a "
           "caller holding one did not get it here, and is stepping a DOM child list with this entry");
    return box_tree_child_of_node(bt_resolve(box, bt_continue_after(box, child.node)));
}

BoxTreeChild box_tree_prev_sibling(lxb_dom_element_t *box, BoxTreeChild child)
{
    BoxTreeChild prev;

    DCHECK(box != NULL && child.node != NULL,
           "css-display-3 §2.5's spliced child sequence was stepped backward with no box, or from no node");
    bt_require_in_sequence(box, child);
    if (box_tree_child_is_marker(child)) return box_tree_child_of_node(NULL);
    if (box_tree_child_is_label(child))
        return list_marker_box_generated(box) ? bt_marker_of(box) : box_tree_child_of_node(NULL);
    DCHECK(!bt_is_spliced(child.node),
           "css-display-3 §2.5's spliced child sequence was stepped BACKWARD from an element whose own "
           "computed `display` is `contents`. This sequence never yields one — §2.5 replaces it by its "
           "contents — so a caller holding one did not get it here, and is stepping a DOM child list with "
           "this entry");
    prev = box_tree_child_of_node(bt_resolve_back(box, bt_continue_before(box, child.node)));
    /* Before the first source-document member stands the marker, where the list item has one. */
    if (!box_tree_child_exists(prev) && list_marker_box_generated(box)) prev = bt_marker_of(box);
    /* THE ROUND TRIP. The two directions descend into a spliced element at opposite ends, so they can be
       taught about a tree separately; this equality makes a disagreement a crash. */
    DCHECK(!box_tree_child_exists(prev) || box_tree_child_same(box_tree_next_sibling(box, prev), child),
           "css-display-3 §2.5's spliced child sequence disagreed with itself: the member BEFORE this one is "
           "not a member this one FOLLOWS. The backward step descends into an element §2.5 replaced by its "
           "contents at its LAST child and the forward step at its FIRST, so the two answers are one sequence "
           "only while both read the same splice");
    /* THE HEAD, which the round trip is blind to because its exemption is the answer "none". The two sides are
       computed by different code — an ascent out of a spliced element's first child against a descent into
       it, and the marker arm of each entry — so they can disagree. */
    DCHECK(box_tree_child_exists(prev) || box_tree_child_same(box_tree_first_child(box), child),
           "css-display-3 §2.5 \"Box Generation: the none and contents keywords\"' spliced child sequence "
           "answered NO PREDECESSOR for a member that is not its FIRST: the backward step says this box's "
           "content begins at this member and the forward step begins it at another");
    return prev;
}
