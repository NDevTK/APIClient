/* css-flexbox-1 §4 "Flex Items"' box tree, §5.1's main axis and §5.2's line count. See flex_item.h for why
   this is a different enumeration from core/layout/block_flow.h's rather than a second copy of it. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
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

/* THE CONTAINER'S OWN `display` IS ASSERTED AND NOT ASSUMED, at every entry that reads a property §5 declares
   `Applies to: flex containers`. A `flex-direction` read off a box that is not one answers the cascade's
   initial `row` — a real keyword, from a real property, that means nothing about that box — which is the
   defaulted-field shape with no default in sight: nothing is missing, and the answer is about a question the
   element was never asked. */
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
    /* §5.1's `Value:` line is `row | row-reverse | column | column-reverse` and the mapping is stated per
       value: `row` takes "the same orientation as the inline axis of the current writing mode", `column` "the
       block axis of the current writing mode", and each `-reverse` is "Same as" its unreversed value except
       that main-start and main-end "are swapped" — an ORDER, which this answer deliberately cannot carry. */
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

bool flex_container_is_multi_line(lxb_dom_element_t *el)
{
    char *v;
    bool multi;
    char nbuf[160];

    fi_require_container(el, "§5.2 \"Flex Line Wrapping: the flex-wrap property\"");
    v = fi_computed(el, "flex-wrap");
    /* §6 "Flex Lines" is where the two words this answer is in are defined, over §5.2's three keywords: a
       single-line container is "one with flex-wrap: nowrap", a multi-line one "one with flex-wrap: wrap or
       flex-wrap: wrap-reverse". */
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

/* css-display-3 §1 "Introduction"' NODES THAT ARE NOT THERE — "for the purposes of CSS, all of these
   additional types of nodes are ignored, as if they didn't exist" — plus §2.5 "Box Generation: the none and
   contents keywords"' elided element, whose note states the same fact in the words a text sequence needs:
   "anonymous box generation rules will ignore the elided elements entirely, as if they did not exist in the
   box tree". Both are INVISIBLE TO CONTIGUITY, so a comment or a `display: none` element between two text
   nodes leaves them one sequence. Anything else that generates a box ENDS the sequence. */
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

/* THE MAXIMAL CHILD TEXT SEQUENCE CONTAINING `n`, as the pair of TEXT nodes at its two ends. It is walked in
   BOTH directions because §4's rule is about "the entire text sequence" and this component is asked about ONE
   node: a caller iterating a child list meets the sequence at its first text node, but the classification has
   to answer the same way for every member or two walks over one list would disagree about which anonymous
   item a node is inside. */
static void fi_text_sequence(lxb_dom_node_t *n, lxb_dom_node_t **first, lxb_dom_node_t **last)
{
    lxb_dom_node_t *c;

    *first = *last = n;
    for (c = n->prev; c != NULL; c = c->prev) {
        if (c->type == LXB_DOM_NODE_TYPE_TEXT) { *first = c; continue; }
        if (!fi_invisible_to_a_text_sequence(c)) break;
    }
    for (c = n->next; c != NULL; c = c->next) {
        if (c->type == LXB_DOM_NODE_TYPE_TEXT) { *last = c; continue; }
        if (!fi_invisible_to_a_text_sequence(c)) break;
    }
}

/* §4's WHITE-SPACE RULE over a whole sequence: "if the entire text sequences contains only document white
   space characters (i.e. characters that can be affected by the white-space property) it is instead not
   rendered (just as if its text nodes were display:none)". The CHARACTER SET is
   core/layout/block_flow.h's one derivation from css-text-3 §4 "White Space Processing Rules" and is asked
   rather than restated; the CONDITION is this section's and reads no declaration at all, which is the whole of
   the difference from CSS 2.2 §9.2.2.1 "Anonymous inline boxes"' rule over the same characters. */
static bool fi_sequence_is_all_white_space(lxb_dom_node_t *first, lxb_dom_node_t *last)
{
    lxb_dom_node_t *c = first;

    for (;;) {
        if (c->type == LXB_DOM_NODE_TYPE_TEXT && !block_flow_text_is_all_document_white_space(c)) return false;
        if (c == last) return true;
        c = c->next;
        DCHECK(c != NULL,
               "css-flexbox-1 §4's text sequence ran off the end of the child list before reaching the last "
               "node the same walk had just found, so the two ends came from different lists");
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
    DCHECK(child->parent == lxb_dom_interface_node(container),
           "css-flexbox-1 §4's flex-item classification was asked about a node that is not a CHILD of the flex "
           "container it was asked with. §4's sentence is \"Each in-flow child of a flex container becomes a "
           "flex item\" and its text-sequence rule is over sibling nodes of that same list, so a node from "
           "elsewhere in the tree would be classified against a formatting context it is not in");
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
                   "flex container becomes a flex item\" is stated over a child list this one is not yet, "
                   "because §2.5's splice has not run: \"the element must be treated as if it had been "
                   "replaced in the element tree by its contents\". BUILD THAT SPLICE AS THE THING EVERY WALK "
                   "ITERATES rather than an arm here — core/layout/block_flow.c's own classification names the "
                   "identical absence over the identical sentence for a block container's list, so one splice "
                   "answers both and two arms would be one rule with two implementations",
                   box_subject(el, nbuf, sizeof nbuf));
            return FLEX_ITEM_CHILD_NONE;
        }
        free(d);
        /* §4.1 "Absolutely-Positioned Flex Children": "As it is out-of-flow, an absolutely-positioned child of
           a flex container does not participate in flex layout." §4's own sentence is over the IN-FLOW
           children, so this child is not a flex item and contributes to nothing §9 sums.
           A FLOAT IS NOT ASKED ABOUT HERE AND THAT IS THE SECTION'S DOING, not an omission: §4's example marks
           a `float: left` child as a flex item and says "floating is ignored", so CSS 2.2 §9.5 "Floats"' own
           out-of-flow rule does not reach a flex container's child list. */
        if (fi_computed_is(el, "position", "absolute") || fi_computed_is(el, "position", "fixed"))
            return FLEX_ITEM_CHILD_NONE;
        return FLEX_ITEM_CHILD_ELEMENT;
    }
    case LXB_DOM_NODE_TYPE_TEXT:
        fi_text_sequence(child, &first, &last);
        return fi_sequence_is_all_white_space(first, last) ? FLEX_ITEM_CHILD_NONE : FLEX_ITEM_CHILD_TEXT;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* css-display-3 §1: "for the purposes of CSS, all of these additional types of nodes are ignored, as
           if they didn't exist" — so none of the three is a child §4's sentence is about. */
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
    fi_text_sequence(first, &a, &b);
    /* ONE PAST THE LAST TEXT NODE, which is not the same as one past the last node this sequence swallowed:
       a comment or an elided element BEFORE that text node is inside the jump and is never revisited, while
       one AFTER it is left for the caller's own loop to classify as FLEX_ITEM_CHILD_NONE. Ending the run at
       the last TEXT node rather than at the first non-text node is what keeps those two cases one rule. */
    return b->next;
}
