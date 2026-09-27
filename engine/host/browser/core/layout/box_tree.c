/* css-display-3 §2.5 "Box Generation: the none and contents keywords"' splice, as the child axis of §1's box
   tree. See box_tree.h for the contract, for why the splice is one component rather than a descent at each of
   the five walks that need it, and for why this reads a COMPUTED `display` where the ancestor direction reads a
   specified one. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"

/* IS THIS NODE ONE §2.5 REPLACES BY ITS CONTENTS — "the element must be treated as if it had been replaced in
   the element tree by its contents"? Only an ELEMENT can be, and only for the one computed value: §2.5's other
   keyword elides the subtree instead ("The element and its descendants generate no boxes or text sequences"),
   so a `none` element answers FALSE here and stays in the sequence for each walk's own arm to skip.
   IT IS NOT NAMED FOR §2.5's NOTE's WORD "ELIDED", which covers BOTH keywords — "Elements with either of these
   values do not have inner or outer display types" — so a predicate called that would be one name for the two
   opposite answers this walk has to tell apart. */
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

/* WHERE `box`'s SEQUENCE CONTINUES AFTER `n` — `n`'s own next sibling, or, when `n` is the last child of an
   element the sequence was spliced THROUGH, that element's next sibling, out to `box` itself. NULL is the end.
   THE ASCENT IS WHAT MAKES THE SPLICE A SEQUENCE RATHER THAN A DESCENT: §2.5 replaces the element BY its
   contents, so the position after its last child is the position after the element, and a walk that only
   descended would stop at the end of the spliced children and drop every later child of `box`. */
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

/* THE FIRST POSITION AT OR AFTER `n` THAT IS A MEMBER OF THE SEQUENCE rather than a splice into it: §2.5's
   replacement applied until the node in hand is one it does not replace.
   AN EMPTY `contents` ELEMENT IS REPLACED BY NOTHING and the sequence continues past it, which is the same
   sentence read with an empty contents rather than a special case — and it is why this is a loop: the node the
   descent or the step lands on may itself be `contents`, to any depth. It terminates because every arm moves
   strictly forward in document order over a finite tree. */
static lxb_dom_node_t *bt_resolve(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    while (n != NULL && bt_is_spliced(n))
        n = n->first_child != NULL ? n->first_child : bt_continue_after(box, n);
    return n;
}

/* REFUSES A STEP FROM A NODE THAT IS NOT IN `box`'s SEQUENCE — one that is neither a DOM child of `box` nor a
   child of a chain of elements §2.5 replaced by their contents inside it. It is the relation the step below
   would otherwise ASSUME, and it is asked at the ENTRY rather than inside the ascent because the ascent returns
   at the first node that HAS a next sibling: a half-converted walk's node usually has one, so a test on the
   ascent's own path would pass for exactly the caller this exists to catch.
   IT IS A `require` FUNCTION WITH THE GUARD INSIDE IT, WHICH IS THIS DIRECTORY'S SPELLING FOR A CHECK THAT
   COSTS A WALK — core/layout/block_flow.c's `bf_require_float_does_not_reach_this_stack` is the same shape, and
   the reason is not style: a release `DCHECK` is `((void)sizeof(cond))`, so the condition is TYPE-CHECKED
   without being evaluated, and a predicate hidden behind `#if APICLIENT_DEV` is then an undeclared call in the
   release build — which `engine/build.mjs`'s own `-Werror=implicit-function-declaration` makes a hard failure
   of the SHIPPED program and of nothing else. Putting the guard in the BODY leaves the call declared at both
   settings and leaves release paying nothing. */
static void bt_require_in_sequence(lxb_dom_element_t *box, lxb_dom_node_t *child)
{
#if APICLIENT_DEV
    lxb_dom_node_t *root = lxb_dom_interface_node(box), *p;
    char nbuf[160], bbuf[160];

    for (p = child->parent; p != NULL; p = p->parent) {
        if (p == root) return;
        if (!bt_is_spliced(p)) break;
    }
    DFAILF("%s, stepped as a child of %s: css-display-3 §2.5 \"Box Generation: the none and contents "
           "keywords\"' spliced child sequence was stepped from a node that is not IN it. Between that node and "
           "this box stands an element that GENERATES A BOX, so the node is a child of THAT box's sequence and "
           "not of this one — §2.5 replaces only a `contents` element by its contents, and the ascent this step "
           "makes passes through nothing else. A walk converted halfway hands this a node it still reached "
           "through `->next`, and answering it would return a position out of one box's child sequence to a "
           "walk enumerating another's",
           box_subject_node(child, nbuf, sizeof nbuf), box_subject(box, bbuf, sizeof bbuf));
#else
    (void) box;
    (void) child;
#endif
}

lxb_dom_node_t *box_tree_first_child(lxb_dom_element_t *box)
{
    DCHECK(box != NULL, "css-display-3 §2.5's spliced child sequence was asked for of no box");
    DCHECK(!bt_is_spliced(lxb_dom_interface_node(box)),
           "css-display-3 §2.5's spliced child sequence was asked for of an element whose own computed "
           "`display` is `contents` — it generates no box for these nodes to be the children OF, and §2.5 puts "
           "them in that element's own box parent's sequence instead. Ask the box parent, which "
           "core/css/css_computed_value.h's `css_box_parent_display` is the other direction of");
    return bt_resolve(box, lxb_dom_interface_node(box)->first_child);
}

lxb_dom_node_t *box_tree_next_sibling(lxb_dom_element_t *box, lxb_dom_node_t *child)
{
    DCHECK(box != NULL && child != NULL,
           "css-display-3 §2.5's spliced child sequence was stepped with no box, or from no node");
    DCHECK(!bt_is_spliced(child),
           "css-display-3 §2.5's spliced child sequence was stepped FROM an element whose own computed "
           "`display` is `contents`. This sequence never yields one — §2.5 replaces it by its contents — so a "
           "caller holding one did not get it here, and is stepping a DOM child list with this entry");
    bt_require_in_sequence(box, child);
    return bt_resolve(box, bt_continue_after(box, child));
}
