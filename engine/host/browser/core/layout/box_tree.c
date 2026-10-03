/* css-display-3 §2.5 "Box Generation: the none and contents keywords"' splice, as the child axis of §1's box
   tree. See box_tree.h for the contract, for why the splice is one component rather than a descent at each of
   the five walks that need it, and for why this reads a COMPUTED `display` where the ancestor direction reads a
   specified one. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
/* ONE ENUM AND NOT <lexbor/css/css.h>, WHICH IS THE WHOLE-PARSER HEADER: the decision below needs the
   pseudo-element NAME SPACE and nothing in the CSS parser, and this one is a pure generated enum with no
   includes of its own, so it pulls in nothing a LAYOUT translation unit has any business compiling.
   THIS COMMENT USED TO SAY css.h IS "what this directory's other lexbor-CSS consumers take", WHICH WAS A CLAIM
   ABOUT THE TREE AND WAS FALSE: `git grep -l '#include <lexbor/css' -- engine/host/browser/core/layout/`
   answers this file and nothing else. It is recorded rather than deleted because the reason to prefer the
   narrow header is the ARGUMENT above and never a convention somebody counted — a scope claim in a comment is
   the half that rots, and this one was wrong on the hour it was written. */
#include <lexbor/css/selectors/pseudo_const.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"

/* THE MEMBER TYPE THIS SEQUENCE MUST GAIN — DECIDED HERE AND PINNED, with the answers it refuses and why in
   `bt_require_marker_box_is_spellable`'s abort below and the ORDER of the landing in box_tree.h. THE DECISION:
   the member becomes a BY-VALUE PAIR of the ORIGINATING DOM NODE and an
   `lxb_css_selector_pseudo_element_id_t`, with `__UNDEF` naming every member that is a source-document node.
   IT IS THE STANDARD'S OWN NAMING AND NOT A SHAPE PICKED HERE. css-pseudo-4 §1 "Introduction": "Each
   pseudo-element is associated with an originating element and has syntax of the form ::name-of-pseudo".
   §4 "Tree-Abiding Pseudo-elements", which §4.2 "List Markers: the ::marker pseudo-element" is inside, says
   they "always fit within the box tree" and "inherit any inheritable properties from their originating
   element; non-inheritable properties take their initial values as usual". css-lists-3 §3.2 "Generating Marker
   Contents" states each of its arms over "the originating element". So the pair IS (that element, that name),
   and a box so named needs nothing allocated to exist — which is what core/layout/block_flow.h's NOTHING IS
   STORED and this file's own "two steps and not a materialised list" both require of a member.
   IT IS A PAIR AND NOT A BIT, because §3.1 puts the marker "before the ::before pseudo-element" and this
   sequence therefore owes THREE pseudo members in a defined order; a marker-only flag is a field that has to
   be widened twice.
   IT NAMES WHICH BOX A MEMBER IS AND NEVER THAT ONE EXISTS, which is what keeps a CONTENTLESS marker out: a
   list item whose §3.2 answer is that section's `otherwise` arm ("The marker box has no contents and
   ::marker does not generate a box") has a complete sequence with NO pseudo member in it, and the pair simply
   does not yield one. A member type that forced a marker member per list item would be exactly the no-content
   box §3.2 says is not generated — which is why §3.2 is the landing BEFORE this one and not after it.
   THE DISCRIMINATOR IS ROUTED TO AND NOT INVENTED, which is the whole reason the asserts below are here:
   `lxb_css_selector_pseudo_element_id_t` already carries `_MARKER`, `_BEFORE` and `_AFTER`, and it is already
   the type CSSOM §7.2's step 3.1 wants — core/css/css_style_declaration.c's getComputedStyle abort names that
   id as §7.2's `type` and points its reader back to this component for step 3.3, so ONE spelling answers the
   style side and the box side. A second enum for one of them is the two right answers that drift.
   `__UNDEF == 0` IS WHAT MAKES A ZERO-INITIALISED MEMBER A SOURCE-DOCUMENT NODE, and engine/lexbor is VENDORED
   AND SYNCED — so this is the one premise of the decision an upstream sync can falsify in silence: renumber
   `__UNDEF` off zero and every `{0}` member names a REAL pseudo-element instead of none, which is a wrong box
   rather than an absent one. Here it is a build failure at the site that recorded the decision. */
_Static_assert(LXB_CSS_SELECTOR_PSEUDO_ELEMENT__UNDEF == 0,
               "a zero-initialised box-tree child must name NO pseudo-element");
_Static_assert(LXB_CSS_SELECTOR_PSEUDO_ELEMENT_MARKER != LXB_CSS_SELECTOR_PSEUDO_ELEMENT__UNDEF,
               "css-lists-3 §3.1's ::marker must be a nameable value of the discriminator this sequence takes");

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

/* WHERE `box`'s SEQUENCE CONTINUES BEFORE `n` — `n`'s own previous sibling, or, when `n` is the FIRST child of
   an element the sequence was spliced THROUGH, that element's previous sibling, out to `box` itself. NULL is
   the start. It is `bt_continue_after` read the other way and is a MIRROR rather than a second design: §2.5
   replaces the element BY its contents, so the position before its first child is the position before the
   element exactly as the position after its last child is the position after it. */
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

/* THE FIRST POSITION AT OR BEFORE `n` THAT IS A MEMBER of the sequence rather than a splice into it —
   `bt_resolve` read the other way, and it descends to a spliced element's LAST child where that one descends
   to its first, which is the same sentence read from the other end: the contents occupy the element's place in
   order, so the last of them stands where the element ended. AN EMPTY `contents` ELEMENT IS REPLACED BY NOTHING
   here too and the walk continues before it, which is why this is a loop and not one test. It terminates
   because every arm moves strictly BACKWARD in document order over a finite tree. */
static lxb_dom_node_t *bt_resolve_back(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    while (n != NULL && bt_is_spliced(n))
        n = n->last_child != NULL ? n->last_child : bt_continue_before(box, n);
    return n;
}

/* IS `n` A MEMBER OF `box`'s SEQUENCE? `box_tree_parent` is the nearest ancestor that GENERATES a box, so
   "n is in box's sequence" and "box is n's box parent" are ONE equality — and this is a function rather than
   that comparison written at each of the two refusals below because an input-refusal and an answer-refusal
   spelling it separately are two tests only one of which gets taught about a tree. */
static bool bt_in_sequence(lxb_dom_element_t *box, lxb_dom_node_t *n)
{
    return box_tree_parent(n) == box;
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
    char nbuf[160], bbuf[160];

    /* THE MEMBERSHIP TEST IS THE ASCENT'S OWN ANSWER AND IS NOT A SECOND WALK OF IT — `bt_in_sequence` above,
       which the answer-refusal below shares, because writing the loop twice is how the step and the refusal
       would come to disagree about a tree only one of them had been taught about. */
    if (bt_in_sequence(box, child)) return;
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

/* REFUSES AN ANSWER THAT IS NOT A MEMBER OF THE SEQUENCE IT CAME OUT OF — the refusal above read the other
   way, over the same one spelling, and it exists because `box_tree_first_child`'s answer is the ONE answer this
   component makes that no later step re-checks. A walk that seeds a loop and steps it hands every later node
   back to `bt_require_in_sequence`; a caller that takes the first child and uses it WITHOUT stepping hands it
   to nothing, and core/layout/block_flow.c has such callers.
   WHAT IT MAKES IMPOSSIBLE IS ONE OF THE ANSWERS THE DECISION AT THE TOP OF THIS FILE REFUSES, and it is the
   attractive one: MINTING a synthetic `lxb_dom_node_t` for css-lists-3 §3.1's marker box keeps every signature
   and every call site exactly as they are, so nothing else in this tree would notice. This does. A minted node
   is not in any box's sequence — its ascent reaches no box at all, or reaches one through a `->parent` the
   minting assigned — so the equality the whole sequence is stated over is the thing it cannot satisfy without
   being linked into the document, at which point it is the OTHER refused answer and the page's own
   `childNodes` can see it. */
static void bt_require_answer_is_in_sequence(lxb_dom_element_t *box, lxb_dom_node_t *answer)
{
#if APICLIENT_DEV
    char nbuf[160], bbuf[160];

    if (answer == NULL || bt_in_sequence(box, answer)) return;
    DFAILF("%s, answered as the first child of %s: css-display-3 §2.5 \"Box Generation: the none and contents "
           "keywords\"' spliced child sequence ANSWERED a node that is not in it. Every node this sequence can "
           "yield stands under `box` through nothing but elements §2.5 replaced by their contents, so "
           "`box_tree_parent` of it IS `box` — the one equality the sequence is stated over, and the one a "
           "MINTED node cannot satisfy. A marker box is not minted as a node: this sequence's member type is "
           "(ORIGINATING NODE, `lxb_css_selector_pseudo_element_id_t`) and the decision is at the top of this "
           "file",
           box_subject_node(answer, nbuf, sizeof nbuf), box_subject(box, bbuf, sizeof bbuf));
#else
    (void) box;
    (void) answer;
#endif
}

/* REFUSES TO STATE THE CHILD SEQUENCE OF A BOX WHOSE FIRST MEMBER THIS SEQUENCE CANNOT NAME. css-lists-3 §3.1
   "The ::marker Pseudo-Element": "The marker box is generated by the ::marker pseudo-element of a list item as
   the list item's first child, before the ::before pseudo-element (if it exists on the element)." That box has
   no source-document node — css-display-3 §1 "Introduction" names it as one of the boxes an element generates
   beside its principal box, "a principal block box and a child marker box" — and this sequence's member type
   is `lxb_dom_node_t *`, so the member EXISTS and is UNSPELLABLE here. WHAT IT MUST BECOME IS DECIDED AT THE
   TOP OF THIS FILE.
   THIS CRASH USED TO NAME `bt_require_in_sequence` AS WHERE THAT DECISION LANDS, "because its membership test
   IS `box_tree_parent(child) == box` and a member with no DOM parent cannot satisfy it", AND THE CLAUSE IS
   RECORDED RATHER THAN DELETED BECAUSE A READER WHO RE-DERIVES IT FROM THAT EQUALITY WILL WRITE IT AGAIN. It
   is right that the equality is the test a half-landing fails and WRONG about which way that cuts: a marker
   member's node half IS its originating element, so under the decided type `box_tree_parent` of it is that
   element — the equality HOLDS, by a second arm the entry does not have yet, and the thing the decision
   constrains is therefore `box_tree_parent` rather than the refusal. The refusal that DOES do work is the one
   over an ANSWER (`bt_require_answer_is_in_sequence`), which refuses the minted node the old clause read as
   already impossible.
   IT IS A CRASH AND NOT A RESIDUAL BECAUSE THE ANSWER IS WRONG NOW RATHER THAN NARROWER. Without it
   `box_tree_first_child` of a list item returns that element's first SOURCE-DOCUMENT child, which is a real
   node standing one position too early: the sequence is one member short AT ITS HEAD for every list item in
   every document, and a walk reading it gets a plausible box tree instead of a refusal.
   core/css/css_style_declaration.c's UA rule table carries `li { display: list-item }`, so the population is
   every `li` and is reached with no page CSS anywhere.
   THE RELEASE ARM IS TODAY'S ANSWER AND IS COHERENT, which is what a `DFAIL`'s shipped arm owes the components
   downstream of it: the entry goes on returning the first source-document child, which is the marker-less list
   every consumer of this sequence already handles. No component is left holding a state it has no step for.
   IT REFUSES EVERY LIST ITEM, AND css-lists-3 §3.2 "Generating Marker Contents" IS WHAT WOULD NARROW IT:
   that section's `otherwise` arm reads "The marker box has no contents and ::marker does not generate a
   box", so a list item §3.2 answers that arm for has a COMPLETE sequence here and must not be refused.
   §3.2's CONDITION IS THREE ARMS AND NOT ONE, AND THIS CLAUSE USED TO STATE ONE OF THEM — "a list item
   with no marker string" — WHICH IS KEPT BECAUSE A READER WHO REACHES FOR `list-style-type` ALONE WILL
   RE-DERIVE IT. §3.2 is ORDERED: "The contents of a marker box are determined by the first of these
   conditions that is true", fetched from drafts.csswg.org/css-lists-3/ because engine/specindex has no
   css-lists-3 row and nothing in this tree checks a citation to it. So the `otherwise` arm is reached only
   once all three are false: `content` on the ::marker is `normal`, which this engine cannot move it off at
   all (box_tree.h states that derivation and its armed control); no `list-style-image` on the originating
   element defines a marker image; and no `list-style-type` defines a marker string. A narrowing built from
   the third arm alone would answer §3.2 for an arm whose predecessors it never read.
   THE TWO PROPERTIES §3.2 READS OFF THE ORIGINATING ELEMENT NOW HAVE AN INITIAL VALUE AND ARE STILL NOT A
   COMPUTED VALUE, which is the other half of what this clause used to say: it read that `list-style-type`
   "is in neither lexbor's property registry nor core/css/css_style_declaration.c's unregistered-initial
   table", and the second half is RETIRED — that table carries css-lists-3 §3.4 "Text-based Markers: the
   list-style-type property"'s `disc` and css-lists-3 §3.3 "Image Markers: the list-style-image property"'s
   `none`, so css-cascade-5 §7.1 "Initial Values" has an initial value to fall to and §7.2 "Inheritance"
   has a base case. WHAT STILL BLOCKS §3.2 IS ONE ROW IN A THIRD FILE: `css_computed_value` is the entry a C
   algorithm asks and it asserts `css_shorthand_complete_for` FIRST, which must answer FALSE while
   css-lists-3 §3.6 "Styling Markers: the list-style shorthand property" has no row in
   core/css/css_shorthand.c — that shorthand sets both longhands, nothing takes it apart, so a
   `ul { list-style: none }` would read as `disc` and this refusal would fire on exactly the list items
   §3.2 says have no marker box. §3.6's row is therefore the landing before THE NARROWING and not
   before this refusal, which already stands; the named residual at that table states its shape.
   §3.1's LAST SENTENCE IS WHY THE `list-item` TEST IS THE WHOLE POPULATION AND NOT A FIRST APPROXIMATION OF IT:
   "Marker boxes only exist for list items: on any other element, the ::marker pseudo-element's content property
   must compute to none, which suppresses its creation."
   THE `list-item` TEST IS A THIRD QUESTION AND NOT A THIRD COPY of the same spelling in core/layout/
   block_flow.c and core/layout/used_value.c: those two ask whether the PRINCIPAL box is block-level, this asks
   whether a SECOND box is generated beside it. What it inherits from them is one narrowing it does not widen —
   css-display-3 §2.3's two-value form `block flow list-item` is not a computed value this engine produces, and
   core/css/css_computed_value.c's `blockified` crashes on it by name before this entry is reached.
   RETIREMENT: this function and its two call sites go when this sequence can yield the marker itself. */
static void bt_require_marker_box_is_spellable(lxb_dom_element_t *box)
{
#if APICLIENT_DEV
    char bbuf[160], *d;
    bool list_item;

    d = css_computed_value(box, "display");
    DCHECK(d != NULL, "the cascade produced no computed `display` — the UA layer answers `inline` for every "
                      "element it does not name, so this cannot be unset");
    list_item = strcmp(d, "list-item") == 0;
    free(d);
    if (!list_item) return;
    DFAILF("%s: css-lists-3 §3.1 \"The ::marker Pseudo-Element\" puts a MARKER BOX first in this box's child "
           "sequence — \"the marker box is generated by the ::marker pseudo-element of a list item as the list "
           "item's first child\" — and css-display-3 §2.5's spliced child sequence is a sequence of DOM NODES, "
           "which that box is not. So this sequence is one member short at its head and cannot say so by "
           "answering. BUILD the marker as a member it can yield: css-lists-3 §3.2 \"Generating Marker "
           "Contents\" decides whether one exists at all, css-pseudo-4 §4 \"Tree-Abiding Pseudo-elements\" "
           "gives it its style (\"They inherit any inheritable properties from their originating element; "
           "non-inheritable properties take their initial values as usual\"), and THE MEMBER TYPE IS DECIDED "
           "AND IS AT THE TOP OF THIS FILE: a BY-VALUE PAIR of the ORIGINATING NODE and an "
           "`lxb_css_selector_pseudo_element_id_t`, whose `__UNDEF` names every member that is a "
           "source-document node and whose `_MARKER` names this box. SO `box_tree_parent` OF A PSEUDO MEMBER IS "
           "ITS OWN NODE HALF rather than an ascent from it, which is the one arm the entry does not have yet. "
           "FOUR ANSWERS ARE REFUSED AND EACH FOR ITS OWN REASON, because three of them keep every signature in "
           "this directory exactly as it is and would therefore land without anything noticing: a REAL DOM node "
           "inserted as the list item's first child is a node the page's own `childNodes`, `firstChild` and "
           "selectors would then see, which no browser answers and which is what a PSEUDO-element is not — "
           "css-pseudo-4 §1 \"Introduction\" says so informatively (\"Since they are not restricted to fitting "
           "into the document tree\") and css-display-3 §2.5's own Note states the invariant it would break "
           "(\"any semantics based on the document tree, such as selector-matching, event handling, and property "
           "inheritance, are not affected\"); a SYNTHETIC "
           "off-tree node is refused by this entry's own signature, which returns a pointer NO caller frees, so "
           "ONE IS LEAKED PER CALL, at every one of this directory's walks over that container, and it is "
           "malloc'd C rather than a GC object so "
           "`JS_FreeRuntime`'s `gc_obj_list` walk cannot report; a TAGGED pointer encodes a second fact in a "
           "value the node type admits as valid; and a SECOND ACCESSOR beside this sequence leaves the sequence "
           "itself one member short at its head, which is the state this refusal stands at. "
           "ANSWERING INSTEAD WOULD RETURN A REAL NODE AT THE WRONG POSITION",
           box_subject(box, bbuf, sizeof bbuf));
#else
    (void) box;
#endif
}

lxb_dom_element_t *box_tree_parent(lxb_dom_node_t *n)
{
    lxb_dom_node_t *p;

    DCHECK(n != NULL, "css-display-3 §2.5's box parent was asked of no node");
    /* A SPLICED `n` IS ANSWERED RATHER THAN REFUSED — see box_tree.h for why this entry differs from its two
       siblings there, and for the crash that refusing it would have masked.
       §2.5's replacement, read upward: an ancestor the section replaced by its contents is not a box, so the
       box this node's boxes are children of is the first ancestor past every such element. The loop ends at
       the root at the latest — css-display-3 §2.8 "The Root Element's Principal Box" computes a root
       `contents` to `block` — and at a non-element parent, which is the root's own case. */
    for (p = n->parent; p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT; p = p->parent)
        if (!bt_is_spliced(p)) return lxb_dom_interface_element(p);
    return NULL;
}

lxb_dom_node_t *box_tree_first_child(lxb_dom_element_t *box)
{
    DCHECK(box != NULL, "css-display-3 §2.5's spliced child sequence was asked for of no box");
    DCHECK(!bt_is_spliced(lxb_dom_interface_node(box)),
           "css-display-3 §2.5's spliced child sequence was asked for of an element whose own computed "
           "`display` is `contents` — it generates no box for these nodes to be the children OF, and §2.5 puts "
           "them in that element's own box parent's sequence instead. Ask the box parent, which "
           "core/css/css_computed_value.h's `css_box_parent_display` is the other direction of");
    bt_require_marker_box_is_spellable(box);
    {
        lxb_dom_node_t *first = bt_resolve(box, lxb_dom_interface_node(box)->first_child);

        bt_require_answer_is_in_sequence(box, first);
        return first;
    }
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

lxb_dom_node_t *box_tree_prev_sibling(lxb_dom_element_t *box, lxb_dom_node_t *child)
{
    lxb_dom_node_t *prev;

    DCHECK(box != NULL && child != NULL,
           "css-display-3 §2.5's spliced child sequence was stepped backward with no box, or from no node");
    DCHECK(!bt_is_spliced(child),
           "css-display-3 §2.5's spliced child sequence was stepped BACKWARD from an element whose own "
           "computed `display` is `contents`. This sequence never yields one — §2.5 replaces it by its "
           "contents — so a caller holding one did not get it here, and is stepping a DOM child list with "
           "this entry");
    bt_require_in_sequence(box, child);
    prev = bt_resolve_back(box, bt_continue_before(box, child));
    /* THE START OF THE SEQUENCE IS WHERE THE MARKER STANDS, so a NULL is the one answer THIS direction can get
       wrong for the reason the entry above is refused outright: css-lists-3 §3.1 makes the marker box the list
       item's FIRST child, and NULL says there is nothing before `child` at all. The FORWARD step needs no such
       guard and is left without one — a marker precedes every source-document child, so no forward answer is
       ever the marker — which is why the two directions are refused at DIFFERENT conditions rather than by one
       shared test placed where both happen to pass. */
    if (prev == NULL) bt_require_marker_box_is_spellable(box);
    /* THE ROUND TRIP, ASSERTED AND NOT ARGUED — see box_tree.h. The two directions descend into a spliced
       element at OPPOSITE ends, so they are the pair that can be taught about a tree separately, and this
       equality is what makes a disagreement a crash rather than a sequence that reads one way forward and
       another way back. A NULL answer is exempt because it names the start of the sequence, which the forward
       step has no node to be asked about. */
    DCHECK(prev == NULL || box_tree_next_sibling(box, prev) == child,
           "css-display-3 §2.5's spliced child sequence disagreed with itself: the node BEFORE this one is not "
           "a node this one FOLLOWS. The backward step descends into an element §2.5 replaced by its contents "
           "at its LAST child and the forward step at its FIRST, so the two answers are one sequence only "
           "while both read the same splice — and a walk delimited with one direction and stepped with the "
           "other would then run over a range that is a range in no box's content");
    return prev;
}
