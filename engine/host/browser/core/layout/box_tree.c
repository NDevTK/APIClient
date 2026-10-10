/* css-display-3 §1 "Introduction"'s box tree on its child axis: §2.5's `contents` splice and css-lists-3 §3.1's
   marker member. See box_tree.h for the contract and for why the member is a pair rather than a node. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/css/selectors/pseudo_const.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/list_marker.h"

/* `__UNDEF == 0` makes a zero-initialised member a source-document node. engine/lexbor is vendored and synced,
   so an upstream renumbering would silently turn every `{0}` member into a real pseudo-element. */
_Static_assert(LXB_CSS_SELECTOR_PSEUDO_ELEMENT__UNDEF == 0,
               "a zero-initialised box-tree child must name NO pseudo-element");
_Static_assert(LXB_CSS_SELECTOR_PSEUDO_ELEMENT_MARKER != LXB_CSS_SELECTOR_PSEUDO_ELEMENT__UNDEF,
               "css-lists-3 §3.1's ::marker must be a nameable value of the discriminator this sequence takes");

/* `<li> (display `list-item`)::marker`, or the node's own subject for a source-document member. */
static const char *bt_subject(BoxTreeChild c, char *buf, size_t cap)
{
    char inner[160];

    if (!box_tree_child_is_pseudo(c)) return box_subject_node(c.node, buf, cap);
    snprintf(buf, cap, "%s::%s", box_subject_node(c.node, inner, sizeof inner),
             c.pseudo == LXB_CSS_SELECTOR_PSEUDO_ELEMENT_MARKER ? "marker" : "(pseudo-element)");
    return buf;
}

/* Is this node one §2.5 replaces by its contents? Only an element with the computed value `contents`: the
   other keyword elides the subtree ("The element and its descendants generate no boxes or text sequences"),
   so a `none` element answers FALSE and stays in the sequence for each walk's own arm to skip. */
static bool bt_is_spliced(lxb_dom_node_t *n)
{
    char *d;
    bool spliced;

    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    d = css_computed_value(lxb_dom_interface_element(n), "display");
    DCHECK(d != NULL, "the cascade produced no computed `display` — the UA layer answers `inline` for every "
                      "element it does not name, so this cannot be unset");
    spliced = strcmp(d, "contents") == 0;
    free(d);
    return spliced;
}

/* Where `box`'s sequence continues after node `n`: its next sibling, or, past the last child of an element
   the sequence was spliced through, that element's next sibling, out to `box`. NULL is the end. The ascent is
   what makes the position after a spliced element's last child the position after the element. */
static lxb_dom_node_t *bt_continue_after(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(box);

    for (; n != root; n = n->parent) {
        if (n->next != NULL) return n->next;
        DCHECK(n->parent != NULL,
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
        n = n->first_child != NULL ? n->first_child : bt_continue_after(box, n);
    return n;
}

/* `bt_continue_after` read backward: the position before a spliced element's first child is the position
   before the element. */
static lxb_dom_node_t *bt_continue_before(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(box);

    for (; n != root; n = n->parent) {
        if (n->prev != NULL) return n->prev;
        DCHECK(n->parent != NULL,
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
        n = n->last_child != NULL ? n->last_child : bt_continue_before(box, n);
    return n;
}

/* The member a list item's sequence opens with when it has a marker box (css-lists-3 §3.1). */
static BoxTreeChild bt_marker_of(lxb_dom_element_t *box)
{
    BoxTreeChild c = { lxb_dom_interface_node(box), LXB_CSS_SELECTOR_PSEUDO_ELEMENT_MARKER };

    return c;
}

/* The first source-document member of `box`'s sequence, which follows the marker where there is one. */
static BoxTreeChild bt_first_source_member(lxb_dom_element_t *box)
{
    return box_tree_child_of_node(bt_resolve(box, lxb_dom_interface_node(box)->first_child));
}

/* Is `n` a member of `box`'s sequence? `box_tree_parent` is the box a member's box is a child of, so
   membership and box-parenthood are one equality, shared by the input and the answer refusals below so the
   two cannot disagree about a tree. */
static bool bt_in_sequence(lxb_dom_element_t *box, BoxTreeChild n)
{
    return box_tree_parent(n) == box;
}

/* REFUSES A STEP FROM A NON-MEMBER — a node with a box-generating element between it and `box`, or a pseudo
   member this sequence did not yield. Asked at the entry rather than on the ascent's path, which returns at
   the first node that has a sibling and would pass for exactly the half-converted walk this exists to catch.
   The guard sits in the body so the call is declared at both settings: a release DCHECK type-checks its
   condition without evaluating it, and `-Werror=implicit-function-declaration` refuses a dev-only predicate. */
static void bt_require_in_sequence(lxb_dom_element_t *box, BoxTreeChild child)
{
#if APICLIENT_DEV
    char nbuf[200], bbuf[160];

    if (box_tree_child_is_pseudo(child)) {
        DCHECKF(child.pseudo == LXB_CSS_SELECTOR_PSEUDO_ELEMENT_MARKER && child.node == lxb_dom_interface_node(box)
                    && list_marker_box_generated(box),
                "%s, stepped as a child of %s: a pseudo-element member this sequence does not yield. It yields "
                "one pseudo member, css-lists-3 §3.1's ::marker, as the head of the sequence of the list item "
                "it originates from and only where `list_marker_box_generated` answers TRUE",
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

    DCHECKF(!box_tree_child_is_pseudo(c),
            "%s:%d met %s: a css-pseudo-4 §4 \"Tree-Abiding Pseudo-elements\" member of core/layout/box_tree.h's "
            "sequence reached a walk that reads it as a source-document node. The walk at that site has no arm "
            "for a pseudo-element box — BUILD that arm (core/layout/block_flow.h's `block_flow_child_kind` "
            "classifies css-lists-3 §3.1's ::marker)",
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
    /* A pseudo-element's box is a child of its originating element's box: css-lists-3 §3.1 generates the marker
       "as the list item’s first child". */
    if (box_tree_child_is_pseudo(c)) {
        DCHECK(c.node->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "a pseudo-element member's originating node is not an element — css-pseudo-4 §1 "
               "\"Introduction\" associates each pseudo-element with an originating ELEMENT");
        return lxb_dom_interface_element(c.node);
    }
    /* §2.5's replacement read upward: the box parent is the first ancestor past every `contents` element. A
       spliced `n` is answered rather than refused (see box_tree.h). The loop ends at a non-element parent,
       which is the root's own case. */
    for (p = c.node->parent; p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT; p = p->parent)
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
    first = bt_first_source_member(box);
    bt_require_answer_is_in_sequence(box, first);
    return first;
}

BoxTreeChild box_tree_next_sibling(lxb_dom_element_t *box, BoxTreeChild child)
{
    DCHECK(box != NULL && child.node != NULL,
           "css-display-3 §2.5's spliced child sequence was stepped with no box, or from no node");
    bt_require_in_sequence(box, child);
    /* The marker heads the sequence, so the member after it is the first source-document member. */
    if (box_tree_child_is_pseudo(child)) return bt_first_source_member(box);
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
    if (box_tree_child_is_pseudo(child)) return box_tree_child_of_node(NULL);
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
