/* css-flexbox-1 §4 "Flex Items"' box tree, §5.1's main axis and §5.2's line count. See flex_item.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_logical.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_item.h"

static char *fi_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);
    char nbuf[160];

    DCHECKF(v != NULL,
            "%s, property `%s`: the cascade produced no computed value for a property this engine models — "
            "every one of them is in lexbor's registry with an initial value, so the last layer always answers",
            box_subject(el, nbuf, sizeof nbuf), name);
    return v;
}

static bool fi_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = fi_computed(el, name);
    bool same = strcmp(v, kw) == 0;

    free(v);
    return same;
}

bool flex_item_display_is_flex_container(const char *display)
{
    DCHECK(display != NULL, "css-flexbox-1 §3's flex-container test was asked about a NULL computed `display`");
    return strcmp(display, "flex") == 0 || strcmp(display, "inline-flex") == 0;
}

/* The container's `display` is asserted at every entry reading a property §5 declares `Applies to: flex
   containers`: off any other box `flex-direction` reads the initial `row`, a real keyword meaning nothing
   there. */
static void fi_require_container(lxb_dom_element_t *el, const char *section)
{
    char *d;
    bool container;
    char nbuf[160];

    DCHECKF(el != NULL, "css-flexbox-1 %s was asked about a flex container with no element", section);
    d = fi_computed(el, "display");
    container = flex_item_display_is_flex_container(d);
    free(d);
    DCHECKF(container,
            "%s: css-flexbox-1 %s states its property `Applies to: flex containers`, and this element's "
            "computed `display` is neither `flex` nor `inline-flex`. §3 \"Flex Containers: the flex and "
            "inline-flex display values\" is what makes those two the whole of the set, so the value read here "
            "would be the cascade's initial keyword standing for an answer this box has no question for",
            box_subject(el, nbuf, sizeof nbuf), section);
}

FlexMainAxis flex_container_main_axis(lxb_dom_element_t *el)
{
    char *v;
    FlexMainAxis axis;
    char nbuf[160];

    fi_require_container(el, "§5.1 \"Flex Flow Direction: the flex-direction property\"");
    v = fi_computed(el, "flex-direction");
    /* §5.1's `Value:` line is `row | row-reverse | column | column-reverse`: `row` takes "the same orientation as
       the inline axis of the current writing mode", `column` "the block axis of the current writing mode", and
       each `-reverse` swaps main-start and main-end, an order this answer cannot carry. */
    if (strcmp(v, "row") == 0 || strcmp(v, "row-reverse") == 0) {
        axis = FLEX_MAIN_AXIS_INLINE;
    } else if (strcmp(v, "column") == 0 || strcmp(v, "column-reverse") == 0) {
        axis = FLEX_MAIN_AXIS_BLOCK;
    } else {
        axis = FLEX_MAIN_AXIS_INLINE;
        DFAILF("%s, computed `flex-direction` `%s`: css-flexbox-1 §5.1 \"Flex Flow Direction: the "
               "flex-direction property\"' `Value:` line is `row | row-reverse | column | column-reverse` and "
               "admits nothing else, so this is a declaration that reached the cascade without its grammar — "
               "core/css/css_shorthand.c is where §5.3 \"Flex Direction and Wrap: the flex-flow shorthand\"'s "
               "expansion validates the two keyword sets, and lexbor's own property parser is where a longhand "
               "declaration is validated",
               box_subject(el, nbuf, sizeof nbuf), v);
    }
    free(v);
    return axis;
}

bool flex_container_axis_is_vertical(lxb_dom_element_t *container, FlexAxis axis)
{
    FlexMainAxis main_axis;
    CssLogicalAxis abstract;

    DCHECK(axis == FLEX_AXIS_MAIN || axis == FLEX_AXIS_CROSS,
           "css-flexbox-1 §2 \"Flex Layout Box Model\" gives a flex container TWO axes and this is neither — "
           "\"The axis perpendicular to the main axis is called the cross axis\" is the whole of the pair");
    /* §5.1's mapping, which also establishes that `container` is a flex container at all. */
    main_axis = flex_container_main_axis(container);
    /* §2: "The axis perpendicular to the main axis is called the cross axis", read over css-writing-modes-4
       §6.1 "Abstract Dimensions"' pair: the cross axis is the other abstract axis. One expression, not four
       arms; `css_logical_init` asserts §6.4 sends the two abstract axes to different physical ones. */
    abstract = (main_axis == FLEX_MAIN_AXIS_INLINE) == (axis == FLEX_AXIS_MAIN) ? CSS_LOGICAL_AXIS_INLINE
                                                                                : CSS_LOGICAL_AXIS_BLOCK;
    return css_logical_axis_is_vertical(container, abstract);
}

bool flex_container_is_multi_line(lxb_dom_element_t *el)
{
    char *v;
    bool multi;
    char nbuf[160];

    fi_require_container(el, "§5.2 \"Flex Line Wrapping: the flex-wrap property\"");
    v = fi_computed(el, "flex-wrap");
    /* §6 "Flex Lines" defines both words over §5.2's keywords: single-line is "one with flex-wrap: nowrap",
       multi-line "one with flex-wrap: wrap or flex-wrap: wrap-reverse". */
    if (strcmp(v, "nowrap") == 0) {
        multi = false;
    } else if (strcmp(v, "wrap") == 0 || strcmp(v, "wrap-reverse") == 0) {
        multi = true;
    } else {
        multi = false;
        DFAILF("%s, computed `flex-wrap` `%s`: css-flexbox-1 §5.2 \"Flex Line Wrapping: the flex-wrap "
               "property\"' `Value:` line is `nowrap | wrap | wrap-reverse` and admits nothing else, so this "
               "is a declaration that reached the cascade without its grammar",
               box_subject(el, nbuf, sizeof nbuf), v);
    }
    free(v);
    return multi;
}

/* Nodes invisible to contiguity: css-display-3 §1 "Introduction"'s "for the purposes of CSS, all of these
   additional types of nodes are ignored, as if they didn't exist", and §2.5 "Box Generation: the none and
   contents keywords"' `none` ("anonymous box generation rules will ignore the elided elements entirely, as if
   they did not exist in the box tree"). Anything else that generates a box ends the sequence. A `contents`
   element is never asked: core/layout/box_tree.h's sequence replaces it by its children, each judged on
   itself. */
static bool fi_invisible_to_a_text_sequence(lxb_dom_node_t *n)
{
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        return true;
    case LXB_DOM_NODE_TYPE_ELEMENT:
        return fi_computed_is(lxb_dom_interface_element(n), "display", "none");
    default:
        return false;
    }
}

/* The maximal child text sequence containing `n`, as its two end text nodes. Walked both ways so every member
   classifies alike ("the entire text sequence"). The list is `container`'s box-tree child sequence:
   css-display-3 §1 defines text sequences "in the box tree", and §2.5 puts a `contents` element's children in
   its box parent's sequence, `->next`-unreachable across that element. `container` is a parameter because the
   caller holds it; deriving it would repeat §2.5's ascent. */
static void fi_text_sequence(lxb_dom_element_t *container, lxb_dom_node_t *n,
                             lxb_dom_node_t **first, lxb_dom_node_t **last)
{
    BoxTreeChild m;

    /* A flex container is never a list item (css-display-3 §2.3 "Generating Marker Boxes: the list-item
       keyword" pairs `list-item` with `flow` or `flow-root`), so every member is a node. */
    *first = *last = n;
    for (m = box_tree_prev_sibling(container, box_tree_child_of_node(n)); box_tree_child_exists(m);
         m = box_tree_prev_sibling(container, m)) {
        lxb_dom_node_t *c = box_tree_child_node(m);

        if (c->type == LXB_DOM_NODE_TYPE_TEXT) { *first = c; continue; }
        if (!fi_invisible_to_a_text_sequence(c)) break;
    }
    for (m = box_tree_next_sibling(container, box_tree_child_of_node(n)); box_tree_child_exists(m);
         m = box_tree_next_sibling(container, m)) {
        lxb_dom_node_t *c = box_tree_child_node(m);

        if (c->type == LXB_DOM_NODE_TYPE_TEXT) { *last = c; continue; }
        if (!fi_invisible_to_a_text_sequence(c)) break;
    }
}

/* §4's white-space rule over a whole sequence: "if the entire text sequences contains only document white
   space characters (i.e. characters that can be affected by the white-space property) it is instead not
   rendered (just as if its text nodes were display:none)". The character set is core/layout/block_flow.h's
   css-text-3 §4 "White Space Processing Rules" derivation; the condition reads no declaration, unlike CSS 2.2
   §9.2.2.1 "Anonymous inline boxes". */
static bool fi_sequence_is_all_white_space(lxb_dom_element_t *container, lxb_dom_node_t *first,
                                           lxb_dom_node_t *last)
{
    lxb_dom_node_t *c = first;

    for (;;) {
        if (c->type == LXB_DOM_NODE_TYPE_TEXT && !block_flow_text_is_all_document_white_space(c)) return false;
        if (c == last) return true;
        /* Stepped through the same box-tree sequence the ends came from; `->next` would stop at the end of a
           `contents` element's children. */
        c = box_tree_child_node(box_tree_next_sibling(container, box_tree_child_of_node(c)));
        DCHECK(c != NULL,
               "css-flexbox-1 §4's text sequence ran off the end of the box-tree child sequence before "
               "reaching the last node the same walk had just found, so the two ends came from different "
               "sequences");
    }
}

FlexItemChildKind flex_item_child_kind(lxb_dom_element_t *container, lxb_dom_node_t *child)
{
    lxb_dom_node_t *first, *last;
    char nbuf[160];

    DCHECK(container != NULL && child != NULL,
           "css-flexbox-1 §4's flex-item classification was asked about a child with no node, or with no flex "
           "container for it to be a child OF — the container is not decoration here, since §4's rule is "
           "stated over ITS child list and §4.1 reads the child's own out-of-flow status against it");
    DCHECK(box_tree_parent(box_tree_child_of_node(child)) == container,
           "css-flexbox-1 §4's flex-item classification was asked about a node that is not a CHILD of the flex "
           "container it was asked with. §4's sentence is \"Each in-flow child of a flex container becomes a "
           "flex item\" and its text-sequence rule is over sibling nodes of that same list, so a node from "
           "elsewhere in the tree would be classified against a formatting context it is not in. "
           "THIS EQUALITY USED TO READ `child->parent == container` AND IS REWRITTEN RATHER THAN DELETED, "
           "which is what css-display-3 §2.5 \"Box Generation: the none and contents keywords\" does to it "
           "rather than retiring it: \"the element must be treated as if it had been replaced in the element "
           "tree by its contents\" makes a box-tree child of a flex container need not be a DOM child of it, "
           "so the DOM equality was the right precondition read through the wrong relation. A CALLER THAT "
           "COMPOSED `n->parent` FOR THE ARGUMENT IS EXACTLY WHAT THIS NOW CATCHES — a `contents` element "
           "generates no box and cannot be the container any flex item is classified against");
    switch (child->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT: {
        lxb_dom_element_t *el = lxb_dom_interface_element(child);
        char *d = fi_computed(el, "display");

        if (strcmp(d, "none") == 0) { free(d); return FLEX_ITEM_CHILD_NONE; }
        if (strcmp(d, "contents") == 0) {
            free(d);
            DFAILF("%s: css-display-3 §2.5 \"Box Generation: the none and contents keywords\" gives this child "
                   "`display: contents`: \"The element itself does not generate any boxes, but its children "
                   "and pseudo-elements still generate boxes and text sequences as normal.\" So this element "
                   "is not a flex item and its CHILDREN are — css-flexbox-1 §4's \"Each in-flow child of a "
                   "flex container becomes a flex item\" is stated over the child list §2.5's splice produces, "
                   "\"the element must be treated as if it had been replaced in the element tree by its "
                   "contents\", and core/layout/box_tree.h is that splice as a child sequence. "
                   "THIS FILE'S OWN WALKS ARE ROUTED TO IT, WHICH IS WHY REACHING THIS LINE NOW MEANS A "
                   "CALLER OUTSIDE THEM: this classification's own box-parent precondition, §4's text "
                   "sequence in BOTH directions, that sequence's white-space test and its end are all taken "
                   "over that sequence, which never yields a `contents` element — and so are the four walks "
                   "that step this classification (core/layout/flex_line.c, "
                   "core/layout/flex_intrinsic_size.c's two, core/layout/flex_cross_size.c). A caller that "
                   "still composes a child list with `->first_child` and `->next`, or a run bound with "
                   "`->prev`, is what gets here. "
                   "THE COUPLING THAT MADE THE ROUTING ONE LANDING RATHER THAN SEVERAL IS RECORDED BECAUSE A "
                   "READER CONVERTING A REMAINING WALK MEETS IT AGAIN: §4's text sequence is a MAXIMAL run "
                   "and the classification is asked about ONE member, so a walk converted without the "
                   "sequence's own two steps would delimit a run with `->prev`/`->next` and answer §4's "
                   "\"if the entire text sequences contains only document white space characters\" over a "
                   "DIFFERENT run from the one it then measures — two anonymous flex items where §4 makes "
                   "one, which is a plausible item list and not a crash",
                   box_subject(el, nbuf, sizeof nbuf));
            return FLEX_ITEM_CHILD_NONE;
        }
        free(d);
        /* §4.1 "Absolutely-Positioned Flex Children": "As it is out-of-flow, an absolutely-positioned child of
           a flex container does not participate in flex layout." A float is not asked about: §4's example
           marks a `float: left` child a flex item ("floating is ignored"). */
        if (fi_computed_is(el, "position", "absolute") || fi_computed_is(el, "position", "fixed"))
            return FLEX_ITEM_CHILD_NONE;
        return FLEX_ITEM_CHILD_ELEMENT;
    }
    case LXB_DOM_NODE_TYPE_TEXT:
        fi_text_sequence(container, child, &first, &last);
        return fi_sequence_is_all_white_space(container, first, last) ? FLEX_ITEM_CHILD_NONE
                                                                     : FLEX_ITEM_CHILD_TEXT;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* css-display-3 §1: "for the purposes of CSS, all of these additional types of nodes are ignored, as
           if they didn't exist". */
        return FLEX_ITEM_CHILD_NONE;
    default:
        DFAIL("a node type css-display-3 §1's box tree does not describe is a CHILD of a flex container being "
              "laid out — the tree this walk iterates holds elements, text, comments, processing instructions "
              "and a doctype, and a CDATA section, a document or a fragment is not a child any parser this "
              "engine runs produces there. Find the writer that inserted it");
    }
    return FLEX_ITEM_CHILD_NONE;
}

lxb_dom_node_t *flex_item_text_sequence_end(lxb_dom_element_t *container, lxb_dom_node_t *first)
{
    lxb_dom_node_t *a, *b;

    DCHECK(flex_item_child_kind(container, first) == FLEX_ITEM_CHILD_TEXT,
           "css-flexbox-1 §4's child text sequence was delimited from a node that is not inside one. §4 wraps "
           "a sequence in an anonymous flex item only where it has content, so a run started anywhere else "
           "would be an EMPTY anonymous item — a box in every sum in §9.9 that no section generates");
    fi_text_sequence(container, first, &a, &b);
    /* One past the last text node, in the container's box-tree sequence. Nodes skipped before it are inside the
       jump; one after it is left for the caller to classify as `FLEX_ITEM_CHILD_NONE`. It is a sequence
       position, because callers advance their walk to it and pass it as core/layout/block_flow.h's
       `BlockFlowRun` `end`; `b->next` is NULL at the end of a `contents` element's children. */
    return box_tree_child_node(box_tree_next_sibling(container, box_tree_child_of_node(b)));
}

bool flex_item_is_collapsed(lxb_dom_element_t *item)
{
    char *v;
    bool collapsed;

    DCHECK(item != NULL, "css-flexbox-1 §4.4's collapsed-item question was asked with no element");
    v = css_computed_value(item, "visibility");
    DCHECK(v != NULL,
           "the cascade produced no computed `visibility` — css-display-3 §4 \"Invisibility: the visibility "
           "property\" gives it an initial value of `visible` and an `Inherited:` of `yes`, so the cascade's "
           "last layer always answers");
    collapsed = strcmp(v, "collapse") == 0;
    free(v);
    return collapsed;
}

/* css-flexbox-1 §7.2.1 "The flex-grow property"' and §7.2.2 "The flex-shrink property"' factor; see
   flex_item.h. Lexbor validated the `Value:` line, so a parse failure here is an engine fault, not input. */
double flex_item_flexibility_factor(lxb_dom_element_t *item, const char *name)
{
    char *v;
    char *end;
    double n;

    DCHECK(item != NULL, "css-flexbox-1 §7.2's flexibility factor was asked for with no element");
    v = css_computed_value(item, name);
    DCHECKF(v != NULL,
            "the cascade produced no computed `%s` — css-flexbox-1 §7.2.1 \"The flex-grow property\" and §7.2.2 "
            "\"The flex-shrink property\" give the two an `Initial:` of `0` and `1`, so the cascade's last "
            "layer always answers",
            name);
    n = strtod(v, &end);
    DCHECKF(end != v && *end == '\0',
            "`%s` computed to `%s`, which is not the `<number [0,∞]>` css-flexbox-1 §7.2.1 \"The flex-grow "
            "property\" and §7.2.2 \"The flex-shrink property\" declare. lexbor validates the `Value:` line "
            "before the cascade sees it, so a value of another shape is a declaration that should have been "
            "dropped",
            name, v);
    free(v);
    return n;
}
