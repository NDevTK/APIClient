/* CSS 2.1 §10 — the used value of a box-model length. See used_value.h for the contract, for why the BOX TYPE
   is the first question, and for what the root element's `width: auto` is blocked on. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/frame/screen.h"
#include "core/frame/viewport.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_box.h"
#include "core/layout/table_column_width.h"
#include "core/layout/table_grid.h"
#include "core/layout/table_height.h"
#include "core/layout/table_width.h"
#include "core/layout/table_wrapper.h"
#include "core/layout/used_value.h"

/* CSS 2.1 §10.3's OWN LIST OF BOX TYPES, which is the list of algorithms `width` has. The names are the
   spec's, and there is one member per section because the sections DISAGREE at exactly the arms this component
   computes. §10.3.5 and §10.3.7 used to be one member here on the ground that a non-auto size and a non-auto
   margin read the same in both; that is true and it was still the wrong shape, because a computed `auto`
   MARGIN does not — §10.3.5 and §10.3.9 both say outright that its used value is 0, while §10.3.7 solves it
   from a constraint equation between `left` and `right`. One member for the two turned a rule this component
   can state into a crash it had no way to tell apart from one it cannot.
   AND THE LIST IS NOT §10's ALONE, WHICH IS WHY THE LAST ARM OF THE CLASSIFICATION CANNOT BE AN `else` THAT
   MEANS "BLOCK-LEVEL". A box type §10 never heard of that falls to §10.3.3's member does not crash — it runs
   the constraint equation and RETURNS A NUMBER, which is the one failure this file has no way to notice. The
   inline-level flex and grid containers arrived exactly that way: css-flexbox-1 §3 "Flex Containers: the flex
   and inline-flex display values" and css-grid-1 §5.1 "Establishing Grid Containers: the grid and inline-grid
   display values" each define their pair so that only the OUTER display type differs — "a flex container box
   that is block-level when placed in flow layout" against one "that is inline-level" — and matching neither
   `inline` nor `inline-block` is not the same fact as being block-level.
   THE PAIR IS SPLIT AND ONLY THE INLINE-LEVEL HALF IS A MEMBER, which is a derivation and not an oversight to
   be tidied away later. Both modules state the container's own size the same way — css-grid-1 §5.2 "Sizing
   Grid Containers" ("A grid container is sized using the rules of the formatting context in which it
   participates"), css-flexbox-1 §9.2 "Line Length Determination" ("Determine the main size of the flex
   container using the rules of the formatting context in which it participates") — and §5.2 then spells both
   halves out: a block-level one "is sized like a block box that establishes a formatting context, with an auto
   inline size CALCULATED AS FOR NON-REPLACED BLOCK BOXES", which IS §10.3.3 and is why `display: flex` and
   `display: grid` belong on `UV_BOX_BLOCK_FLOW` rather than on a member of their own; an inline-level one "is
   sized as an ATOMIC INLINE-LEVEL BOX (such as an inline-block)", which is §10.3.9's shape over intrinsic
   sizes §10 cannot measure. The block-level half's one genuine disagreement is its `auto` BLOCK size (§5.2:
   "in both inline and block formatting contexts, the grid container's auto block size is its max-content
   size"), and that arm already leaves through core/layout/block_flow.c's own crash naming the module. */
typedef enum {
    UV_BOX_INLINE = 0,       /* §10.3.1 / §10.6.1 — an inline box; `width` and `height` do not apply to it */
    UV_BOX_BLOCK_FLOW,       /* §10.3.3 / §10.6.3 — block-level, in normal flow: the constraint equation's box */
    UV_BOX_FLOAT,            /* §10.3.5 — floating: shrink-to-fit when auto, `auto` margins are 0 */
    UV_BOX_ABS,              /* §10.3.7 — absolutely positioned: the equation between `left`, `width`, `right` */
    UV_BOX_INLINE_BLOCK,     /* §10.3.9 — shrink-to-fit when auto, `auto` margins are 0 */
    UV_BOX_INLINE_FLEX_GRID, /* an INLINE-LEVEL flex or grid container: §10.3.9's shape (an atomic inline-level
                                box, so `auto` margins are 0 and an `auto` width shrink-to-fits) over the
                                intrinsic sizes its OWN module defines — css-flexbox-1 §9.9.1 "Flex Container
                                Intrinsic Main Sizes", css-grid-1 §5.2 "Sizing Grid Containers" — which are not
                                the ones §10.3.5's formula reads */
    UV_BOX_TABLE,            /* CSS 2.1 §17.5 — the table's own width and height algorithms, not §10's */
    UV_BOX_ITEM              /* a flex or grid item: css-flexbox §9.7 / css-grid sizes it, and §10 does not */
} UvBox;

static char *uv_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

static bool uv_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = uv_computed(el, name);
    bool same = strcmp(v, kw) == 0;

    free(v);
    return same;
}

/* THE SAME QUESTION OF A LENGTH-VALUED PROPERTY, which is a different entry and not a different spelling: a
   length's computed value is a `CssPx` carrying the environment fact a `50vw` or a snapped border width
   derives from (core/css/css_computed_value.h), so the keyword arm is one of three answers rather than the
   whole of it. §10's rules branch on `auto` and on `none` constantly, and this is how they ask. */
static bool uv_length_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    CssLength len = css_computed_length(el, name);

    return len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, kw) == 0;
}

/* css-display §2.4's table display types, plus the two `<display-inside>`/`<display-legacy>` spellings that
   generate a table box. CSS 2.1 §17.5 owns every one of their sizes. */
static bool uv_display_is_table(const char *d)
{
    static const char *const TABLE[] = {
        "table", "inline-table", "table-row-group", "table-header-group", "table-footer-group",
        "table-row", "table-cell", "table-column-group", "table-column", "table-caption",
    };
    unsigned i;

    for (i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++)
        if (strcmp(TABLE[i], d) == 0) return true;
    return false;
}

/* THE FOUR VALUES css-flexbox-1 §3 AND css-grid-1 §5.1 ADD TO `display`, which is the question asked of a
   PARENT to decide whether this box is an ITEM and of the box ITSELF to decide whether it is a CONTAINER. */
static bool uv_display_is_flex_or_grid(const char *d)
{
    return d != NULL && (strcmp(d, "flex") == 0 || strcmp(d, "inline-flex") == 0 ||
                         strcmp(d, "grid") == 0 || strcmp(d, "inline-grid") == 0);
}

/* AND WHICH HALF OF THE ONE PAIR IT IS. §3 and §5.1 define each pair as the SAME container box differing only
   in its outer display type — "block-level when placed in flow layout" against "inline-level" — and both
   modules then size the container "using the rules of the formatting context in which it participates"
   (css-grid-1 §5.2 "Sizing Grid Containers", css-flexbox-1 §9.2 "Line Length Determination"), so the outer
   half is the whole of what this component has to know. IT IS DERIVED FROM THE LIST ABOVE AND NOT A SECOND
   COPY OF IT: a fifth spelling added there is inline-level or block-level by the same test rather than by
   being remembered in two places, which is the failure the box-type list's own comment describes. The `inline-`
   prefix is exact over that list and nowhere near a guess about a name — §3 and §5.1 write the inline-level
   half of each pair and no other member of the list carries it. */
static bool uv_display_is_inline_flex_or_grid(const char *d)
{
    return uv_display_is_flex_or_grid(d) && strncmp(d, "inline-", 7) == 0;
}

/* css-display-3 §2.5 "Box Generation: the none and contents keywords" — THE TWO VALUES THAT ARE NOT A BOX TYPE
   AT ALL, which is a DIFFERENT question from every test below and is why it is asked separately rather than
   folded into the classification. §2.5 defines each: `contents` is "The element itself does not generate any
   boxes, but its children and pseudo-elements still generate boxes and text sequences as normal" and `none` is
   "The element and its descendants generate no boxes or text sequences" — and the section then states the
   consequence in its own sentence, "Elements with either of these values do not have inner or outer display
   types, because they don't generate any boxes at all". A BOX TYPE IS EXACTLY AN INNER AND AN OUTER DISPLAY
   TYPE, so there is nothing here for §10 to select between, and that is not the same fact as a box type this
   component has not built: the question has no SUBJECT. */
static bool uv_display_generates_no_box(const char *d)
{
    return strcmp(d, "none") == 0 || strcmp(d, "contents") == 0;
}

/* css-display-3 §2.2's `ruby` INNER display type together with §2.4's four LAYOUT-INTERNAL ruby values, which
   are ONE list because ONE module sizes all five and CSS 2.1 §10 sizes none of them. §2.2 "Inner Display
   Layout Models: the flow, flow-root, table, flex, grid, and ruby keywords" gives the container "The element
   generates a ruby container box and establishes a ruby formatting context"; §2.4 "Layout-Internal Display
   Types: the table-* and ruby-* keywords" gives the other four "The element is an internal ruby element. It
   generates the appropriate internal ruby box which participates in a ruby formatting context", under that
   section's own rule that "both the inner display type and the outer display type of elements using these
   display values are set to the given keyword".
   IT IS `uv_display_is_table`'s SHAPE ONE MODULE OVER: a table box is a table box whatever else is true of it
   and CSS 2.1 §17.5 owns its size; a ruby box is a ruby box and CSS Ruby Annotation Layout Module Level 1 owns
   its size. The only difference is that §17.5 has a member in the box-type list and CSS Ruby has a crash. */
static bool uv_display_is_ruby(const char *d)
{
    static const char *const RUBY[] = {
        "ruby", "ruby-base", "ruby-text", "ruby-base-container", "ruby-text-container",
    };
    unsigned i;

    for (i = 0; i < COUNTOF(RUBY); i++)
        if (strcmp(RUBY[i], d) == 0) return true;
    return false;
}

/* THE VALUES THAT REALLY ARE BLOCK-LEVEL IN NORMAL FLOW, WRITTEN OUT — and the whole point is that this is a
   LIST rather than an `else`, because matching none of the other tests is not the same fact as being
   block-level. Each entry is one sentence of css-display-3 and none of them is a resemblance:
     - `block` — §2.1 "Outer Display Roles for Flow Layout: the block, inline, and run-in keywords": "The
       element generates a box that is block-level when placed in flow layout".
     - `flow` and `flow-root` — a bare <display-inside> keyword, whose outer type §2.2 "Inner Display Layout
       Models: the flow, flow-root, table, flex, grid, and ruby keywords" defaults for it: "the element's outer
       display type defaults to block—except for ruby, which defaults to inline". `flow`'s own definition then
       says "Otherwise it generates a block container box", which IS §10.3.3's box. THE EXCEPTION IN THAT
       SENTENCE IS WHY `ruby` IS NOT ON THIS LIST, and it is not a near miss — a bare `display: ruby` is
       `inline ruby`, so the `else` this list replaces was wrong about the OUTER type as well as the inner one,
       which is the identical failure the inline-level flex and grid containers arrived through.
     - `list-item` — §2.3 "Generating Marker Boxes: the list-item keyword": "If no outer display type value is
       specified, the principal box's outer display type defaults to block". Its ::marker is a pseudo-element
       beside the principal box and does not change which section sizes that box.
     - `flex` and `grid` — the BLOCK-LEVEL half of each pair, derived in full at the head of this file.
     - `table-caption` — CSS 2.1 §17.4 Tables in the visual formatting model, in its own sentence: "The caption
       boxes are block-level boxes that retain their own content, padding, margin, and border areas, and are
       rendered as normal block boxes inside the table wrapper box." IT IS ONE OF §17.2 The CSS table model's
       TEN BOX TYPES AND IS STILL NOT §17.5's, and that is the whole reason it is on this list rather than in
       `uv_display_is_table`'s hands: §17.5.2 Table width algorithms: the 'table-layout' property is written
       over "the 'table' or 'inline-table' element" and reads a caption only as CAPMIN, an INTRINSIC minimum it
       feeds into the TABLE's width; §17.5.3 Table height algorithms is written over the table box and its
       rows. So no section of §17.5 states a caption's own used width or height, and the box §17.4 puts it in
       is not the table box at all — it is the table WRAPPER box, whose content edge is an ordinary §10.3.3
       containing block. A caption classified as a table box therefore reached §17.5 for a number §17.5 never
       states, and every arm it landed in refused it.
       WHAT MAKES THE PLACE ON THIS LIST SAFE IS THAT THE THREE TESTS BETWEEN §17.2's ARM AND THIS ONE CANNOT
       FIRE FOR IT, and that is css-display-3 §2.7 "Automatic Box Type Transformations" rather than an
       ordering hope: core/css/css_computed_value.c blockifies a floated box, an absolutely positioned one and
       a flex or grid item, and its map sends `table-caption` to `block` in every one of those cases — "if a
       layout-internal box is blockified, its inner display type converts to flow so that it becomes a block
       container". So a computed `table-caption` reaching `uv_box_kind` is in normal flow, unfloated,
       statically positioned and not an item, which is exactly the antecedent §10.3.3 is stated over.
   `table`, `inline-table` and the eight table-INTERNAL values are absent because `uv_display_is_table` has
   already taken them, and the inline-level spellings because the three tests before this one have. */
static bool uv_display_is_block_level_flow(const char *d)
{
    static const char *const BLOCK[] = { "block", "flow", "flow-root", "list-item", "flex", "grid",
                                         "table-caption" };
    unsigned i;

    for (i = 0; i < COUNTOF(BLOCK); i++)
        if (strcmp(BLOCK[i], d) == 0) return true;
    return false;
}

/* THE BOX TYPE, in the order the questions have to be asked. Each test is a fact about the element that makes
   the LATER tests inapplicable, which is why the order is the spec's and not a preference:
     - there is a box at all, which precedes even the table test — see below;
     - a box CSS 2.1 §17.5 sizes is one whatever else is true of it — which is NINE of §17.2 The CSS table
       model's ten box types and not all ten, because §17.4 Tables in the visual formatting model takes the
       CAPTION out of the table box: caption boxes "are rendered as normal block boxes inside the table
       wrapper box". The caption is therefore §10.3.3's and §10.6.3's like any other block-level box, and it
       is on `uv_display_is_block_level_flow`'s list with that sentence beside it. The test here is
       `table_box_kind`'s and not a second spelling of the value, so the day §17.2's list changes this arm
       changes with it;
     - a child of a flex or grid container is an ITEM, and being one is what makes `float` compute to `none`
       for it (css-flexbox §3), so the item test precedes the float test;
     - an ABSOLUTELY POSITIONED child of a flex container is NOT a flex item (css-flexbox §4.1), so the
       out-of-flow test precedes the item test;
     - and `display` is read last because css_computed_value.c has already BLOCKIFIED it for a float, for an
       absolutely positioned box and for a flex item, so by here it can no longer say `inline` for any of them.
   WHY THE NO-BOX TEST IS FIRST AND NOT MERELY EARLY. Every other test asks WHICH box, and `position: absolute`
   or a flex-container parent is a TRUE FACT about an element that generates none — so with the test anywhere
   below them a `display: none; position: absolute` element was classified `UV_BOX_ABS` without its `display`
   ever being read. The order is the spec's here too: §2.7 "Automatic Box Type Transformations" says of
   blockification and inlinification that "This has no effect on display types that generate no box at all,
   such as none or contents", so those two values SURVIVE every transformation the three tests below rely on
   having already run, and they are the one answer that cannot be reached by asking a different question first.
   THE LAST TEST IS THE OUTER DISPLAY TYPE OF A FLEX OR GRID CONTAINER, and it has to be asked BEFORE the
   block-level list because that list is where a container's block-level half belongs. css-flexbox-1 §3's and
   css-grid-1 §5.1's last paragraphs are what make the test the display string alone: each amends CSS 2.1
   §9.7's table with a row sending `inline-flex` to `flex` and `inline-grid` to `grid`, so blockification —
   which the three tests above have also already left through — has taken the inline-level spellings away from
   every box that is not genuinely inline-level in flow layout, and a surviving `inline-flex` here IS an atomic
   inline-level box.
   AND THE TAIL IS A LIST CLOSED BY CRASHES RATHER THAN AN `else`, which is the invariant this function now
   carries: it is TOTAL. Every arm either names the section that sizes the box or aborts naming the module that
   does, so a `display` value nothing here understands can no longer come back as a NUMBER computed by
   §10.3.3's constraint equation — the one failure the box-type list's own comment above says this file has no
   way to notice. */
static UvBox uv_box_kind(lxb_dom_element_t *el)
{
    char *display = uv_computed(el, "display");
    char *parent_display;
    /* THE RELEASE CONTINUATION OF EVERY `DFAIL` BELOW, named once and here rather than repeated at each arm. A
       DFAIL is compiled out in release, so each crash arm falls through to whatever this holds — and it holds
       the value the `else` those arms replace produced, deliberately: picking a DIFFERENT wrong answer for
       release would be exactly the plausible classification the crashes exist to refuse, and it would change
       geometry no dev build can exercise. What this function makes impossible is the wrong answer in DEV,
       which is where the forcing function lives. */
    UvBox kind = UV_BOX_BLOCK_FLOW;

    if (uv_display_generates_no_box(display)) {
        DFAILF("CSS 2.1 §10 was asked WHICH of its `width` algorithms sizes an element whose computed "
               "`display` is `%s`, and css-display-3 §2.5 \"Box Generation: the none and contents keywords\" "
               "says it has none to choose between: \"Elements with either of these values do not have inner "
               "or outer display types, because they don't generate any boxes at all\". SO THERE IS NOTHING TO "
               "BUILD HERE AND THIS IS NOT AN UNBUILT BOX TYPE — the question has no subject, and the defect "
               "is at the CALLER. core/layout/used_value.h states the precondition this call broke: the caller "
               "has ALREADY established CSSOM §9 \"Resolved Values\"' two conjuncts, \"If the property applies "
               "to the element or pseudo-element and the resolved value of the display property is not none or "
               "contents, then the resolved value is the used value. Otherwise the resolved value is the "
               "computed value\". TWO PREDICATES IN THIS TREE ALREADY DECIDE THAT SECOND CONJUNCT and the "
               "route that reached here carries neither — core/css/css_computed_value.c's "
               "`resolved_display_generates_a_box` guards §9's own arm, and core/dom/element_view.h's "
               "`element_view_has_box` guards CSSOM VIEW §6's and §7's. GREP BOTH, and give the entry that "
               "made this call whichever one fits it. A LAYOUT WALK OVER CHILDREN WANTS NEITHER: §2.5 makes a "
               "`contents` element's CHILDREN the boxes standing in its place, which is a box-tree splice "
               "rather than a guard, and core/layout/block_flow.c's own `contents` crash asks for exactly that "
               "flattening",
               display);
        free(display);
        return kind;
    }
    /* §17.4's ONE EXCEPTION TO THE RULE ABOVE, asked through core/layout/table_box.h's own classification
       rather than through a keyword compare: a `table-caption` generates one of §17.2's ten box types and is
       nevertheless sized by §10, because §17.4 puts it in the WRAPPER beside the table box and none of
       §17.5's algorithms is stated over it. It falls through to the block-level list below. */
    if (uv_display_is_table(display) && table_box_kind(display) != TABLE_BOX_CAPTION) {
        free(display);
        return UV_BOX_TABLE;
    }
    /* css-position §2: `absolute` and `fixed` take the box OUT OF FLOW. `relative` and `sticky` do not — a
       relatively positioned box is laid out in normal flow and then OFFSET, so §10.3.3 is still its section. */
    if (uv_computed_is(el, "position", "absolute") || uv_computed_is(el, "position", "fixed")) {
        free(display);
        return UV_BOX_ABS;
    }
    parent_display = css_box_parent_display(lxb_dom_interface_node(el));
    if (uv_display_is_flex_or_grid(parent_display)) {
        free(parent_display);
        free(display);
        return UV_BOX_ITEM;
    }
    free(parent_display);
    if (!uv_computed_is(el, "float", "none")) { free(display); return UV_BOX_FLOAT; }
    if (strcmp(display, "inline") == 0)                  kind = UV_BOX_INLINE;
    else if (strcmp(display, "inline-block") == 0)       kind = UV_BOX_INLINE_BLOCK;
    else if (uv_display_is_inline_flex_or_grid(display)) kind = UV_BOX_INLINE_FLEX_GRID;
    else if (uv_display_is_block_level_flow(display))    kind = UV_BOX_BLOCK_FLOW;
    else if (uv_display_is_ruby(display))
        DFAILF("this element's computed `display` is `%s`, which is a RUBY box — CSS Ruby Annotation Layout "
               "Module Level 1 sizes it and CSS 2.1 §10 has no algorithm for it at all. It used to fall to "
               "§10.3.3's constraint equation and come back with a NUMBER. WHAT WAS WRONG IS THE OUTER TYPE "
               "TOO, not only the inner one: css-display-3 §2.2 \"Inner Display Layout Models: the flow, "
               "flow-root, table, flex, grid, and ruby keywords\" says \"the element's outer display type "
               "defaults to block—except for ruby, which defaults to inline\", so a bare `display: ruby` is an "
               "INLINE-LEVEL box and §10.3.3 was not merely the wrong algorithm but the wrong half of §10. "
               "WHAT TO BUILD, in this order: css-ruby-1 §2.2 \"Anonymous Ruby Box Generation\" is the box "
               "tree — the internal box types are generated around whatever the author wrote, exactly as CSS "
               "2.1 §17.2.1 Anonymous table objects' are, and core/layout/table_box.h is that section built "
               "and therefore the worked example to write this one against — then §2.3 \"Annotation Pairing\" "
               "is the column "
               "structure, and §3.1.1 \"Inline-axis Interlinear Layout\" is the sizing. WHAT THE SIZE RULES "
               "ARE ONCE THAT EXISTS is css-ruby-1 §3.3 \"Styling Ruby Boxes\", and it SPLITS THE FIVE rather "
               "than giving them one arm: \"Ruby bases and ruby containers are treated as inline boxes\", "
               "while \"Neither the margin, padding, and border properties nor any of the properties that do "
               "not apply to inline boxes apply to base containers or annotation containers\". SO `ruby` and "
               "`ruby-base` land on CSS 2.1 §10.3.1 \"Inline, non-replaced elements\", whose first sentence "
               "is that `width` does not apply, and `ruby-base-container` and `ruby-text-container` answer "
               "for FEWER properties than any box type in the list above. THE UA STYLESHEET IS THE "
               "OTHER HALF OF THAT DIFF: core/css/css_style_declaration.c's UA-default table deliberately "
               "omits the `ruby` and `ruby-text` rows and records why beside them — read that record before "
               "adding either, because the rows belong with the MODULE and not with this crash",
               display);
    else if (strcmp(display, "run-in") == 0)
        DFAILF("this element's computed `display` is `run-in`, and css-display-3 §2.1 \"Outer Display Roles "
               "for Flow Layout: the block, inline, and run-in keywords\" makes it \"a type of inline-level "
               "box with special behavior that attempts to merge it into a subsequent block container\" — so "
               "the fall to §10.3.3 this arm replaces answered a BLOCK-LEVEL section for an INLINE-LEVEL box, "
               "and answered it with a number. WHAT TO BUILD IS A BOX-TREE STEP AND NOT A SECTION OF §10. "
               "css-display-3 §5 \"Run-In Layout\" states it: \"A run-in box is a box that merges into a block "
               "that comes after it\", inserted as a direct child of that block before any box its contents "
               "generate — so this element has NO BOX OF ITS OWN for §10 to size. It becomes inline-level "
               "content of the following block container and is measured there by CSS 2 §9.4.2's line boxes "
               "like any other inline. BUILD §5's re-parenting as part of the box tree the layout walk "
               "iterates — core/layout/block_flow.c's `display: contents` crash asks for the flattening step "
               "that belongs beside it, and the two are one construction with two rules rather than two cases "
               "to add to this classification. Note that a blockified `run-in` never reaches here: "
               "core/css/css_computed_value.c maps it to `block`, so what arrives is an in-flow, unfloated, "
               "statically positioned one");
    else
        DFAILF("this element's computed `display` is `%s`, which is not block-level in normal flow, not "
               "inline-level, not a table box, not a ruby box, not `run-in`, and not one of css-display-3 "
               "§2.5's two no-box values — so there is no arm of CSS 2.1 §10 to select and this classification "
               "declines rather than guessing. THE SHAPE THIS ARRIVES IN IS THE TWO-VALUE `<display-outside> "
               "<display-inside>` SYNTAX, which is a value this component only ever sees SERIALIZED: "
               "core/css/css_computed_value.c's blockification already crashes for exactly it, naming `inline "
               "flow-root` and `block flow list-item` and asking for the outer/inner PAIR to become the value "
               "that file carries. BUILD THAT PAIR AND THIS WHOLE CLASSIFICATION BECOMES A SWITCH OVER THE "
               "OUTER HALF instead of over whole keywords, which is the same fix rather than a second one — "
               "every test above is really asking the outer type and is reading a keyword to get it. MathML "
               "Core §4.1 \"The display: block math and display: inline math value\" is the other producer and "
               "is why that pair is not optional: \"The display property from CSS Display Module Level 3 is "
               "extended with a new inner display type\", spelled `math`, which exists ONLY in the two-value "
               "form. Before assuming a `math` box can reach here, grep lexbor's `LXB_CSS_DISPLAY_*` for it — "
               "a keyword the `display` grammar does not admit is dropped by the cascade and arrives at this "
               "component never",
               display);
    free(display);
    return kind;
}

/* WHETHER THIS BOX'S HORIZONTAL MARGINS ARE STATED OVER ITS USED `width` — one question, asked of the box type
 * and answered in one place, because three sites used to ask it and one of them asked a DIFFERENT one.
 * CSS 2.2 §10.3 "Calculating widths and margins" splits its ten sections into two kinds, and the split is not
 * about the width at all — it is about whether the section SOLVES AN EQUATION containing it:
 *   · §10.3.1 "Inline, non-replaced elements" states both facts in two sentences and neither is an equation —
 *     "The 'width' property does not apply. A computed value of 'auto' for 'margin-left' or 'margin-right'
 *     becomes a used value of '0'." §10.3.5 "Floating, non-replaced elements" opens with the same rule ("If
 *     'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'") and §10.3.9
 *     "'Inline-block', non-replaced elements in normal flow" closes with it. A non-`auto` margin on any of the
 *     three is its computed value, because none of them has an equation to be over-constrained BY. AN
 *     INLINE-LEVEL FLEX OR GRID CONTAINER IS ON THIS SIDE TOO, by the delegation its own module makes rather
 *     than by resemblance: css-grid-1 §5.2 "Sizing Grid Containers" says it "is sized as an ATOMIC
 *     INLINE-LEVEL BOX (such as an inline-block)" and css-flexbox-1 §9.2 "Line Length Determination" sends it
 *     to "the rules of the formatting context in which it participates", which for CSS 2.2 §9.2.2's atomic
 *     inline-level box is §10.3.9 — the section above, whose margin sentence is therefore this box's too. Note
 *     what that delegation does NOT carry: §10.3.9's shrink-to-fit reads §10.3.5's two intrinsic terms, and
 *     THOSE are the module's own (css-flexbox-1 §9.9.1, §5.2's track sums), which is why the width crashes in
 *     `uv_pass_size` while the margin answers here.
 *   · §10.3.3 "Block-level, non-replaced elements in normal flow" and §10.3.7 "Absolutely positioned,
 *     non-replaced elements" each state a CONSTRAINT among the used values with 'width' as a term, so every
 *     one of their margin rules — §10.3.3's rules 2, 4, 5 and 6 and its over-constrained case, §10.3.7's
 *     equal-margins and solve-for-that-value rules — reads it. So do the two box types §10 does not own,
 *     over a width their own algorithm produces (CSS 2.2 §17.5.2 "Table width algorithms: the 'table-layout'
 *     property"; css-flexbox-1 §9.5 "Main-Axis Alignment" over the container's free space).
 * THE REPLACED PAIRS FALL ON THE SAME SIDES AS THEIR NON-REPLACED TWINS, which is why this asks the box type
 * and not `replaced_element_of`: §10.3.2, §10.3.6 and §10.3.10 repeat the 0-outright sentence (§10.3.10
 * "Exactly as inline replaced elements"), while §10.3.4 sends a block-level replaced box to "the rules for
 * non-replaced block-level elements … to determine the margins" and §10.3.8 keeps §10.3.7's equation.
 * IT IS A `false` HERE THAT MAKES §10.3.1's FIRST SENTENCE TRUE. An inline non-replaced box HAS no used width
 * — asking for one runs §10.3.3's constraint equation on a box whose section says the property does not apply,
 * which is a question with no answer rather than an answer this engine has not built. For a float and an
 * inline-block it also declines to run §10.3.5's shrink-to-fit, an intrinsic measurement over the box's whole
 * content, to produce a number the margin rules never look at. */
static bool uv_margin_reads_width(UvBox box)
{
    return box != UV_BOX_INLINE && box != UV_BOX_FLOAT && box != UV_BOX_INLINE_BLOCK &&
           box != UV_BOX_INLINE_FLEX_GRID;
}

/* `auto` is the answer three of §10.3's rules and two of §10.6's branch on, and it is also what CSS 2.1
   §10.4/§10.7 SUBSTITUTE AWAY — so the question is asked of the length a pass is RUNNING WITH and never of the
   property, which is why it takes a `CssLength` and not a name. */
static bool uv_len_is_auto(CssLength len)
{
    return len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, "auto") == 0;
}

/* A LENGTH THAT IS A `CssPx` — CSS 2.1 §10.4's "using the computed value of 'max-width' as the computed value
   for 'width'", which is a substitution into the very parameter §10.3's rules are run with. It is the ONE
   place a used value re-enters the pipeline as a computed one, and it carries the `CssPx` whole rather than
   its number: the substituted limit is a joint function of every environment fact that decided the clamp
   BOUND, not only of the fact the winning operand happened to carry (css_length.h). */
static CssLength uv_len_px(CssPx px)
{
    CssLength len = { CSS_LENGTH_ABSOLUTE, css_px(0.0), 0.0, { '\0' } };

    len.px = px;
    return len;
}

/* THE FOUR TERMS OF css-sizing-3 §3.3's CONVERSION BETWEEN THE TWO BOXES, on one axis, and the ONE place that
   computes them. css-sizing-3 §3.3 converts a border box into a content box by subtracting "the border and
   padding in the corresponding axis", and every arm below needs that sum or half of it — the `border-box` size ADDS it, the
   padding edge subtracts it and adds the paddings back. Two copies of a four-term sum are two places for the
   terms to come to disagree, which is the only way the padding edge and the size it is derived from can stop
   describing the same box. */
typedef struct {
    CssPx padding;   /* padding-left + padding-right, or padding-top + padding-bottom */
    CssPx border;    /* border-left-width + border-right-width, or the top/bottom pair */
} UvSurround;

static UvSurround uv_surround(lxb_dom_element_t *el, bool vertical)
{
    static const char *const PADDINGS[2][2] = {
        { "padding-left", "padding-right" }, { "padding-top", "padding-bottom" },
    };
    static const char *const BORDERS[2][2] = {
        { "border-left-width", "border-right-width" }, { "border-top-width", "border-bottom-width" },
    };
    int axis = vertical ? 1 : 0, i;
    UvSurround s = { css_px(0.0), css_px(0.0) };

    for (i = 0; i < 2; i++) {
        CssLength len = css_computed_length(el, BORDERS[axis][i]);

        DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
               "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
               "§3.3's `Computed value:` line is `absolute length, snapped as a border width` and every arm of "
               "that derivation produces one — 0 for a `none`/`hidden` style, 1/3/5px for the three keywords, "
               "the absolutized length otherwise — so a percentage or a keyword here is a rule that did not "
               "run");
        s.border  = css_px_add(s.border, len.px);
        s.padding = css_px_add(s.padding, used_value_px(el, PADDINGS[axis][i]));
    }
    DCHECK(s.padding.px >= 0.0 && s.border.px >= 0.0,
           "css-sizing-3 §3.3's conversion between the content box and the border box was handed a NEGATIVE "
           "surround. CSS 2.1 §8.4 states outright that \"Unlike margin properties, values for padding values "
           "cannot be negative\" and css-backgrounds-3 §3.3's <line-width> is a non-negative <length>, so lexbor drops "
           "either declaration — a negative here is a used value this component derived rather than one an "
           "author wrote, and every box it is a term of would be smaller than the box it contains");
    return s;
}

/* §5's SUM, as one function rather than as the expression `s.padding + s.border` written twice. That is not
   tidiness: one direction of the conversion ADDS this sum and the other SUBTRACTS it, and IEEE addition is not
   associative, so two spellings of the same sum can differ by an ulp and leave a content box a few times 10^-17
   below zero — a real disagreement and a rounding artifact then look identical, and the assert that is supposed
   to catch the first would be tripped by the second. Subtracting the very number that was added cancels
   exactly, which is what makes that assert mean what it says. */
static CssPx uv_surround_total(UvSurround s)
{
    return css_px_add(s.padding, s.border);
}

/* css-sizing-3 §3.3's `box-sizing`, asked as the ONE question both directions of its conversion turn on. The
   grammar is `content-box | border-box` and lexbor validates the declaration against it, so a third value is a
   cascade layer that answered without one — asserted HERE, at the classification, rather than by whichever arm
   a not-content-box value happened to fall into. */
static bool uv_is_border_box(lxb_dom_element_t *el)
{
    char *v = uv_computed(el, "box-sizing");
    bool border = strcmp(v, "border-box") == 0;

    DCHECK(border || strcmp(v, "content-box") == 0,
           "a `box-sizing` computed to something that is neither of the two keywords css-sizing-3 §3.3's "
           "`content-box | border-box` grammar admits, and its `Computed value:` line is `specified keyword` — "
           "so nothing between the declaration and here had a rule that could produce a third one");
    free(v);
    return border;
}

/* css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property", and the one sentence in it that decides
   what this function returns: "Used values of the sizing properties, as exposed for instance through
   getComputedStyle(), also refer to the border box."
 * THE SECTION IS §3.3 AND NOT §5, AND THE STANDARD IS WRITTEN LEVELLED, because both halves of the old spelling
 * were unresolvable: css-sizing-3 §5 is "Intrinsic Size Determination", which decides nothing about box-sizing,
 * and an unlevelled `css-sizing` names no document, so every bare § in this comment fell to whatever this file
 * cites most and was read against CSS 2.1's numbering instead.
 * SO THE ANSWER IS THE BORDER BOX'S SIZE, NOT THE CONTENT BOX'S. What stood here said the opposite — that
 * "CSS 2.1 §10.2 and CSSOM §9 both mean the CONTENT width", so the used value was the declared one minus the
 * paddings and the border widths — and it crashed rather than computing that. Both halves were wrong. The
 * subtraction css-sizing-3 §3.3 describes ("the content box width and height are calculated by subtracting the
 * border and padding in the corresponding axis from the specified <length-percentage>, and flooring the result
 * at zero") produces the CONTENT box, which is
 * the number CSS 2.1 §10.3.3's equation and CSSOM VIEW's padding edge want; the number CSSOM §9 exposes is the
 * border box, which is why `getComputedStyle(el).width` is `100px` and not `80px` for a `box-sizing:
 * border-box; width: 100px; padding: 10px` box in every user agent. A crash there took out most of the modern
 * web, which sets `box-sizing: border-box` on `*`.
 * IT IS STILL NOT THE DECLARED LENGTH VERBATIM, and the exception is the one css-sizing-3 §3.3 spells out with
 * its own example: the content box floors at zero, so when the paddings and borders alone exceed the declared
 * border-box size the border box GROWS to hold them — "the border-box size ends up at 120px, even though
 * width: 100px is specified for the border box". The used border-box size is therefore the LARGER of the two,
 * and computing it needs exactly the four terms §10.3.3 was waiting on, two of which are the border widths. */
static CssPx uv_border_box_size(lxb_dom_element_t *el, CssPx declared, bool vertical)
{
    UvSurround s = uv_surround(el, vertical);

    DCHECK(uv_is_border_box(el),
           "css-sizing-3 §3.3's border-box arm was reached for a box whose `box-sizing` is `content-box` — the "
           "declared length is then the CONTENT box's and this function would report it as the border box's");
    return css_px_max(declared, uv_surround_total(s));
}

/* THE CONTENT BOX'S EXTENT on one axis, which is css-sizing-3 §3.3's conversion run in the one direction two
   callers need it in: CSSOM VIEW §6's padding edge, and §10.1's containing block (the content EDGE of the
   nearest block container ancestor). Under `content-box` the used size already IS the content box; under
   `border-box` it is the border box, and §5 converts by "subtracting the border and padding widths of the
   respective sides".
   `s` IS THE CALLER'S OWN SURROUND, passed in rather than recomputed, so the sum subtracted here is byte-
   identical to the one the caller adds back — see the note on uv_surround_total for why that is the difference
   between an assert that means what it says and one a rounding artifact can trip. */
static CssPx uv_content_size(lxb_dom_element_t *el, bool vertical, UvSurround s)
{
    CssPx used = used_value_px(el, vertical ? "height" : "width");

    if (!uv_is_border_box(el)) return used;
    return css_px_sub(used, uv_surround_total(s));
}

/* CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property, as the one thing §10 has to say about a
   TABLE box's width: nothing. §17.5.2's own second paragraph is the handover — "Note that this section
   OVERRIDES the rules that apply to calculating widths as described in section 10.3" — and its next sentence is
   the handback, "once the calculated value of 'width' for the table is found using the algorithms given below …
   then the other parts of section 10.3 do apply". So this is a ROUTE and not an algorithm: the width comes from
   core/layout/table_width.h and the two `auto` margins that centre a table are still §10.3.3's, over the slack
   this number leaves.
   THE GRID IS BUILT PER CALL AND NOT CACHED, for the reason core/layout/block_flow.h states of every layout in
   this directory: a layout is per-flow state, so a cached grid is shared state solver/dom_cow.h's delta does
   not swap and a stale one is another flow's document.
   THE RESULT IS CONVERTED BY css-sizing-3 §3.3 EXACTLY AS EVERY OTHER ARM'S IS. §17.5.2 answers CSS 2.1's own
   `width` — CSS 2.1 §17.6.1 The separated borders model's "distance from the left inner padding edge to the
   right inner padding edge", which is the CONTENT box — and §3.3 decides which box a used value is exposed in.
   §17.6.1's other sentence, that an HTML `<table>`'s declared width is its BORDER-edge distance, is applied
   where the declaration is READ (core/layout/table_width.c) and not here, because it is a rule about the
   author's input rather than about which box `getComputedStyle` reports. */
static CssPx uv_table_used_width(lxb_dom_element_t *el)
{
    TableGrid grid;
    TableUsedWidths widths;
    CssPx content;

    table_grid_build(el, &grid);
    table_widths(el, &grid, &widths);
    content = widths.content;
    table_widths_release(&widths);
    table_grid_release(&grid);
    if (uv_is_border_box(el)) return css_px_add(content, uv_surround_total(uv_surround(el, false)));
    return content;
}

/* WHICH of §17.2 The CSS table model's box types this element generates, as the ONE question every arm below
   that reaches `UV_BOX_TABLE` has to ask: `uv_display_is_table` covers all ten of them and each has its own
   owner. §17.5.2 is stated over exactly two ("the 'table' or 'inline-table' element"); a CELL's used width is
   the used width of the columns its rectangle covers, which is a DERIVATION over §17.5.2's answer rather than
   that answer; a CAPTION is not in the table box at all (§17.4 Tables in the visual formatting model puts it
   in the wrapper beside it); and the row, row-group and column boxes are §17.5's own rules. So the box type is
   read ONCE per arm, through this one entry, and the arm then ROUTES or CRASHES on the KIND rather than on a
   second read of `display`. A `uv_generates_table_box` predicate stood beside this and is deleted with it: it
   answered one of the five questions the kind answers, so an arm asking it still had to re-read `display` to
   tell a cell from a caption, and the two reads were free to see different values of one property. */
static TableBoxKind uv_table_box_kind(lxb_dom_element_t *el)
{
    char *d = uv_computed(el, "display");
    TableBoxKind kind = table_box_kind(d);

    free(d);
    return kind;
}

/* CSS 2.1 §17.5 Visual layout of table contents' USED WIDTH OF ONE CELL, which the section states as a fact
   about the GRID rather than about the cell — "Each cell is thus a rectangular box, one or more grid cells
   wide and high" — so the number is the used width of the columns that rectangle covers and §17.5.2's own
   answer is what holds them. THIS IS A ROUTE AND NOT AN ALGORITHM, exactly as `uv_table_used_width` above is:
   the table box is found (core/layout/table_box.h), §17.5.2 is run over its grid, and the cell is read out of
   the result.
   THE GRID AND THE WIDTHS ARE BUILT PER CALL for the reason core/layout/block_flow.h states of every layout in
   this directory: a layout is per-flow state, so a cached one is shared state solver/dom_cow.h's delta does
   not swap and a stale one is another flow's document.
   §17.5.2's ANSWER IS IN THE BORDER BOX AND CSS 2.1's `width` IS NOT, which is the one conversion this
   function owns. core/layout/table_column_width.h measured each cell's own padding and border INTO the column
   (that header records the choice and the reasons), so the content box is that same sum taken back OUT — and
   it is taken out through `table_cell_border_edges`, the identical spelling that put it in, never through
   `uv_surround` beside it. The two are four reads of the same four properties and would still be two
   different questions: this file's resolves a percentage padding against §10.1's containing block, and a
   CELL's containing block is a rectangle §10.1 cannot name, so the two would come apart on exactly the
   document where the difference shows.
   THE FLOOR IS css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"'s OWN, in its own words —
   "the content box width and height are calculated by subtracting the border and padding in the corresponding
   axis from the specified <length-percentage>, and flooring the result at zero (as the inner size of a box
   cannot be negative)" — and it is REACHABLE here rather than decorative: §17.5.2.1 Fixed table layout divides
   the declared table width over the columns and never consults a cell's edges, so a `table-layout: fixed`
   table declared narrower than one cell's own padding and border leaves that cell no content box at all. */
static CssPx uv_table_cell_used_width(lxb_dom_element_t *el)
{
    lxb_dom_element_t *table = table_box_table_of(el);
    TableGrid grid;
    TableUsedWidths widths;
    const TableGridCell *cell;
    CssPx border_box, content;
    char nbuf[160], tbuf[160];

    table_grid_build(table, &grid);
    table_widths(table, &grid, &widths);
    cell = table_grid_cell_of(&grid, el);
    if (cell == NULL)
        DFAILF("%s: CSS 2.1 §17.5 Visual layout of table contents placed NO cell for this box in %s's grid, "
               "which is the table box CSS 2.1 §17.2's own nesting puts it inside. The two walks have come "
               "apart: core/layout/table_box.h reached this table by climbing the internal boxes above the "
               "cell, and core/layout/table_grid.h reached the cells by descending the same table's rows, so "
               "a box that is in one and not the other is a row this cell hangs under that §17.2.1 Anonymous "
               "table objects' box generation does not report as a row of this table. There is no width to "
               "answer with — the columns in hand belong to a grid this cell is not in",
               box_subject(el, nbuf, sizeof nbuf), box_subject(table, tbuf, sizeof tbuf));
    border_box = table_cell_used_border_box(&widths, cell);
    content = css_px_max(css_px_sub(border_box, table_cell_border_edges(el)), css_px(0.0));
    table_widths_release(&widths);
    table_grid_release(&grid);
    if (uv_is_border_box(el)) return border_box;
    return content;
}

/* THE TABLE BOXES WHOSE USED WIDTH IS STILL UNANSWERED, which after the cell's route is two different
   questions and not one leftover — so the crash NAMES the box and says which section owns it. It is ONE
   function called from the declared arm and the `auto` arm because for every kind below the DECLARATION IS
   NOT THE ANSWER either way, so two copies would be two places for that to stop being true silently.
   A CAPTION IS NOT ONE OF THEM AND ITS ARM IS DELETED RATHER THAN LEFT STANDING. `uv_box_kind` no longer
   classifies a `table-caption` as `UV_BOX_TABLE` at all — §17.4 Tables in the visual formatting model renders
   it as a normal block box in the WRAPPER, so §10.3.3's constraint equation over §10.1's containing block is
   its section and `used_value_containing_block_width` answers that rectangle from §17.4's own sentence. The
   arm that stood here told its reader to build the wrapper as a box §10.1 can name; that is what the walk
   below now answers, so the instruction is retired with the code it guarded.
   THE SITE TRAVELS AS THE BOX AND NOT AS A LINE: `box_subject` prints the element and its computed `display`,
   which is the address a reader of this abort needs — the file and line would name this helper for both. */
static void uv_table_non_cell_width_fail(lxb_dom_element_t *el, TableBoxKind kind)
{
    char nbuf[160];

    DCHECK(kind != TABLE_BOX_CAPTION,
           "a CAPTION box reached CSS 2.1 §17.5's refusals. §17.4 Tables in the visual formatting model takes "
           "it out of the table box — \"rendered as normal block boxes inside the table wrapper box\" — and "
           "`uv_box_kind` classifies it as block-level in normal flow for exactly that reason, so it can no "
           "longer be a `UV_BOX_TABLE` and no arm of §17.5 can be asked for its width. A caption here is that "
           "classification and core/layout/table_box.h's ten box types having come apart");
    if (kind == TABLE_BOX_ROW || table_box_kind_is_row_group(kind))
        DFAILF("%s: a ROW box's or ROW GROUP box's used width. CSS 2.1 §17.5 Visual layout of table contents' "
               "rules 1 and 2 give each of them a whole grid row — \"Each row box occupies one row of grid "
               "cells\" and \"A row group occupies the same grid cells as the rows it contains\" — and that "
               "section's own last paragraph is what turns that into a WIDTH, in the two border models "
               "separately: \"in the separated borders model the edges coincide with the border edges of "
               "cells, and thus in this model there may be gaps between the rows, columns, row groups or "
               "column groups corresponding to the 'border-spacing' property\", while in the collapsing model "
               "\"the rows together exactly cover the table leaving no gaps\". SO IT IS NOT THE TABLE'S "
               "CONTENT WIDTH, which an earlier form of this crash claimed: §17.6.1 The separated borders "
               "model counts a spacing at each END of the row into the table's width and the row's own edges "
               "stand inside both of them. BUILD it as the sum of `TableUsedWidths.columns` plus the spacings "
               "BETWEEN them — the same reading `table_cell_used_border_box` takes for a spanning cell, over "
               "the whole grid row — and the collapsing model stays refused at §17.5.2's entry either way",
               box_subject(el, nbuf, sizeof nbuf));
    DFAILF("%s: a COLUMN box's or COLUMN GROUP box's used width, which CSS 2.1 §17.5 Visual layout of table "
           "contents' rules 3 and 4 state as a PLACEMENT and not as a size — \"A column box occupies one or "
           "more columns of grid cells\" and \"A column group box occupies the same grid cells as the columns "
           "it contains\" — and which core/layout/table_grid.h states outright that it does not place. So the "
           "used width is the sum of the grid columns the box covers once something assigns them, and the "
           "assignment is the missing piece rather than the arithmetic. §17.2 The CSS table model is why this "
           "is not urgent and is also why it must not be answered with a guess: these boxes \"are not "
           "rendered (exactly as if they had 'display: none')\", so nothing paints this rectangle and the "
           "only consumer of the number is a reader asking CSSOM for it, which is exactly the reader a "
           "fabricated width would mislead",
           box_subject(el, nbuf, sizeof nbuf));
}

/* CSS 2.1 §17.5.3 Table height algorithms, as the one thing §10.6 has to say about a TABLE box's height:
   nothing. §17.5.2's handover sentence is written about widths — "this section overrides the rules that apply
   to calculating widths as described in section 10.3" — and §17.5.3 needs no such sentence because it simply
   states the height itself: "The height of a table is given by the 'height' property for the 'table' or
   'inline-table' element. A value of 'auto' means that the height is the sum of the row heights plus any cell
   spacing or borders. Any other value is treated as a minimum height." So this is a ROUTE and not an algorithm,
   exactly as `uv_table_used_width` above is, and it answers BOTH arms — the declared value is an input to
   §17.5.3's own comparison rather than the used value, so there is nothing left here for a declared-height arm
   to do and there is none.
   THE GRID IS BUILT PER CALL AND NOT CACHED, for the reason core/layout/block_flow.h states of every layout in
   this directory: a layout is per-flow state, so a cached grid is shared state solver/dom_cow.h's delta does
   not swap and a stale one is another flow's document.
   THE RESULT IS CONVERTED BY css-sizing-3 §3.3 EXACTLY AS EVERY OTHER ARM'S IS. §17.5.3 answers CSS 2.1's own
   `height` — the table box's content extent on this axis — and §3.3 decides which box a used value is exposed
   in. §17.6.1 The separated borders model's border-edge exception does NOT apply on this axis and that is its
   own wording rather than a reading taken here: "in HTML and XHTML1, the WIDTH of the <table> element is the
   distance from the left border edge to the right border edge" says nothing about the height. */
static CssPx uv_table_used_height(lxb_dom_element_t *el)
{
    TableGrid grid;
    TableUsedHeights heights;
    CssPx content;

    table_grid_build(el, &grid);
    table_heights(el, &grid, &heights);
    content = heights.content;
    table_heights_release(&heights);
    table_grid_release(&grid);
    if (uv_is_border_box(el)) return css_px_add(content, uv_surround_total(uv_surround(el, true)));
    return content;
}

/* CSS 2.1 §17.5 Visual layout of table contents' USED HEIGHT OF ONE CELL — the block-axis twin of
   `uv_table_cell_used_width` above, and a fact about the GRID for the same reason: "Each cell is thus a
   rectangular box, one or more grid cells wide and high", and §17.5's own last paragraph puts the row edges at
   the cell border edges in the separated model, so a cell box fills the rows its rectangle covers. §17.5.3
   states the same thing from the cell's side — "Cell boxes that are smaller than the height of the row receive
   extra top or bottom padding".
   THE DECLARED `height` IS NOT THE USED VALUE HERE AND §17.5.3 SAYS SO IN ONE SENTENCE: "The table cell's
   'height' property can influence the height of the row (see above), but it does not increase the height of the
   cell box." So the declaration is an input to the ROW's maximum, inside the algorithm this function runs, and
   both of §10's arms route here — which is why `uv_pass_size` answers a cell before `len` is looked at, exactly
   as it does on the inline axis.
   §17.5.3's ANSWER IS IN THE BORDER BOX AND CSS 2.1's `height` IS NOT, which is the one conversion this
   function owns, and it is taken out through `table_cell_vertical_edges` — the identical spelling
   core/layout/table_height.c put it in with, never through `uv_surround` beside it, for the reason the inline
   twin states: this file's surround resolves a percentage padding against §10.1's containing block, and a
   CELL's containing block is a rectangle §10.1 cannot name. */
static CssPx uv_table_cell_used_height(lxb_dom_element_t *el)
{
    lxb_dom_element_t *table = table_box_table_of(el);
    TableGrid grid;
    TableUsedHeights heights;
    const TableGridCell *cell;
    CssPx border_box, content;
    char nbuf[160], tbuf[160];

    table_grid_build(table, &grid);
    table_heights(table, &grid, &heights);
    cell = table_grid_cell_of(&grid, el);
    if (cell == NULL)
        DFAILF("%s: CSS 2.1 §17.5 Visual layout of table contents placed NO cell for this box in %s's grid, "
               "which is the table box CSS 2.1 §17.2's own nesting puts it inside. The two walks have come "
               "apart: core/layout/table_box.h reached this table by climbing the internal boxes above the "
               "cell, and core/layout/table_grid.h reached the cells by descending the same table's rows, so "
               "a box that is in one and not the other is a row this cell hangs under that §17.2.1 Anonymous "
               "table objects' box generation does not report as a row of this table. There is no height to "
               "answer with — the rows in hand belong to a grid this cell is not in",
               box_subject(el, nbuf, sizeof nbuf), box_subject(table, tbuf, sizeof tbuf));
    border_box = table_cell_used_border_box_height(&heights, cell);
    content = css_px_max(css_px_sub(border_box, table_cell_vertical_edges(el)), css_px(0.0));
    table_heights_release(&heights);
    table_grid_release(&grid);
    if (uv_is_border_box(el)) return border_box;
    return content;
}

/* CSS 2.1 §17.5.3's ROW HEIGHT, read back out of the same answer — "The height of a 'table-row' element's box
   is calculated once the user agent has all the cells in the row available", which is why it is a read of the
   whole table's answer rather than a rule this function could apply to one row.
   IT IS A BORDER-BOX NUMBER AND A ROW HAS NO BORDER TO SUBTRACT: §17.6.1 The separated borders model says
   outright that "rows, columns, row groups, and column groups cannot have borders (i.e., user agents must
   ignore the border properties for those elements)", so the row's border box and its content box coincide on
   this axis and css-sizing-3 §3.3's conversion is the identity for it. That is asserted rather than assumed. */
static CssPx uv_table_row_used_height(lxb_dom_element_t *el)
{
    lxb_dom_element_t *table = table_box_table_of(el);
    TableGrid grid;
    TableUsedHeights heights;
    CssPx h = css_px(0.0);
    bool found;
    size_t row = 0;
    char nbuf[160], tbuf[160];

    table_grid_build(table, &grid);
    table_heights(table, &grid, &heights);
    /* §17.5's RULE 1 IS THE MAPPING AND IT IS READ, NOT RE-DERIVED. "Each row box occupies one row of grid
       cells", numbered in the order core/layout/table_box.h reports the rows, and core/layout/table_grid.h now
       reports that placement back per row element. THE SEARCH THAT STOOD HERE WAS NOT MERELY SLOWER: it reached
       a row only through the cells anchored in it, so `<tr></tr>` — a real row, with a real grid row and a real
       height, §17.5.3's maximum over no cell — was reported as a row this grid does not contain, and the abort
       below fired for a document nothing is wrong with. That is why the mapping had to be built where the rows
       are placed rather than inferred from where the cells are. */
    found = table_grid_row_of(&grid, el, &row);
    if (found) {
        DCHECK(row < heights.nrows,
               "CSS 2.1 §17.5's rule 1 placed a row box at a grid row CSS 2.1 §17.5.3 answered no height for. "
               "`table_heights` answers one height per grid row of the grid it was given and asserts that count "
               "against the grid's own, so the two arrays have been carried apart since it answered");
        h = heights.rows[row];
    }
    table_heights_release(&heights);
    table_grid_release(&grid);
    if (!found)
        DFAILF("%s: CSS 2.1 §17.5 Visual layout of table contents' rule 1 placed NO grid row for this ROW box "
               "in %s's grid, which is the table box CSS 2.1 §17.2's own nesting puts it inside. The two walks "
               "have come apart, exactly as they can for a cell: core/layout/table_box.h reached this table by "
               "climbing the internal boxes above the row, and core/layout/table_grid.h reached the rows by "
               "descending the same table's row groups, so a box that is in one and not the other is a row this "
               "table's own box generation does not report as a row of it. There is no height to answer with — "
               "the rows in hand belong to a grid this row is not in",
               box_subject(el, nbuf, sizeof nbuf), box_subject(table, tbuf, sizeof tbuf));
    DCHECK(!uv_is_border_box(el) || css_px_add(uv_surround_total(uv_surround(el, true)), css_px(0.0)).px == 0.0,
           "CSS 2.1 §17.6.1 The separated borders model says \"rows, columns, row groups, and column groups "
           "cannot have borders (i.e., user agents must ignore the border properties for those elements)\", so "
           "a row's border box and its content box are one rectangle on the block axis and css-sizing-3 §3.3's "
           "conversion is the identity here — and this row has a non-zero vertical padding or border. Either "
           "that sentence is not being enforced where the properties are read, or §17.5.3's row height is being "
           "measured in a different box from the one it is reported in");
    return h;
}

/* THE TABLE BOXES WHOSE USED HEIGHT IS STILL UNANSWERED, which after §17.5.3's own three routes is two
   different questions and not one leftover — so the crash NAMES the box and says which section owns it. It is
   ONE function reached from the one place the block axis is decided, for the reason its inline twin is one:
   for every kind below the DECLARATION IS NOT THE ANSWER either way.
   A CAPTION IS NOT ONE OF THEM, for the reason its inline twin above states: §17.4 Tables in the visual
   formatting model renders it as a normal block box in the WRAPPER, so its height is §10.6.3's ordinary
   content-based one and `uv_box_kind` no longer routes it here at all. The arm that stood here was right that
   the height needed no rule of its own and wrong about what was blocking it — it named the wrapper as a box
   §10.1 cannot name, and the walk below now names it.
   THE SITE TRAVELS AS THE BOX AND NOT AS A LINE: `box_subject` prints the element and its computed `display`,
   which is the address a reader of this abort needs — the file and line would name this helper for both. */
static void uv_table_non_cell_height_fail(lxb_dom_element_t *el, TableBoxKind kind)
{
    char nbuf[160];

    DCHECK(kind != TABLE_BOX_CAPTION,
           "a CAPTION box reached CSS 2.1 §17.5.3 Table height algorithms' refusals. §17.4 Tables in the "
           "visual formatting model renders it as a normal block box in the table wrapper box, so §10.6.3's "
           "content-based height is its section and `uv_box_kind` classifies it as block-level in normal flow "
           "— a caption here is that classification and core/layout/table_box.h's ten box types having come "
           "apart");
    if (kind == TABLE_BOX_ROW || table_box_kind_is_row_group(kind))
        DFAILF("%s: a ROW GROUP box's used height, which CSS 2.1 §17.5.3 Table height algorithms DECLINES TO "
               "DEFINE in its own words: \"CSS 2.1 does not define the meaning of 'height' on row groups.\" "
               "This is therefore NOT a missing algorithm and must not be built as one — it is a case with no "
               "answer in this standard, and a number returned here would be this engine deciding it. WHAT "
               "CSS 2.1 DOES SAY about the box is where it sits: §17.5 Visual layout of table contents' rule 2 "
               "gives it the grid rows of the rows it contains (\"A row group occupies the same grid cells as "
               "the rows it contains\"), so its EXTENT is the sum of those rows' heights plus the "
               "`border-spacing` between them — the same reading `table_cell_used_border_box_height` takes for "
               "a row-spanning cell, over a group's rows instead. THAT IS A PLACEMENT AND NOT A USED `height`, "
               "and the difference is the whole reason this crashes rather than answering: reporting the "
               "extent as the used value of `height` would answer a CSSOM reader with a number no cascade "
               "produced. A ROW reaches this arm only if `uv_pass_size`'s own route above stopped answering "
               "for it, since §17.5.3 states a row's height outright",
               box_subject(el, nbuf, sizeof nbuf));
    DFAILF("%s: a COLUMN box's or COLUMN GROUP box's used height, which CSS 2.1 §17.5 Visual layout of table "
           "contents' rules 3 and 4 state as a PLACEMENT and not as a size — \"A column box occupies one or "
           "more columns of grid cells\" and \"A column group box occupies the same grid cells as the columns "
           "it contains\" — and which core/layout/table_grid.h states outright that it does not place. §10.7 "
           "\"Minimum and maximum heights: 'min-height' and 'max-height'\" excludes these two boxes from its "
           "own Applies-to line by name (\"all elements but non-replaced inline elements, table columns, and "
           "column groups\"), which is the standard treating them as boxes with no block-axis size question at "
           "all. §17.2 The CSS table model is why this is not urgent and is also why it must not be answered "
           "with a guess: these boxes \"are not rendered (exactly as if they had 'display: none')\", so "
           "nothing paints this rectangle and the only consumer of the number is a reader asking CSSOM for "
           "it, which is exactly the reader a fabricated height would mislead",
           box_subject(el, nbuf, sizeof nbuf));
}

/* ---- CSS 2.1 §10.1's CONTAINING BLOCK ---------------------------------------------------------------------
   Every percentage in §8.3, §8.4 and §10.2 and every `auto` §10.3.3 solves for is stated against this one
   rectangle's WIDTH, so it is the recursion the whole of §10.3 stands on and its base case is the reason the
   recursion terminates. */


/* "The containing block in which the ROOT ELEMENT lives is a rectangle called the initial containing block" —
   the element whose parent is the Document itself. */
static bool uv_is_root(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* §10.1's FIRST CASE, and the base case of the recursion above: "For continuous media, it has the dimensions
   of the VIEWPORT and is anchored at the canvas origin." core/frame/viewport.h owns both halves — the number
   and the fact that the number is a PICKED environment choice rather than a derived one — so what this
   function does is find the realm the viewport is answered per and ask it. The realm is the ELEMENT'S own
   document's, never the running one: a layout is a fact about the document the element is in, and an iframe's
   ICB is 300 CSS pixels wide while its parent's is 1280.
   BOTH DIMENSIONS COME THROUGH THIS ONE FUNCTION. §10.1's sentence is about the RECTANGLE, so its height is
   the same case as its width and the escape below is the same escape — a document no navigable presents has
   no ICB on either axis. CSS 2.1 §10.7's percentage `min-height`/`max-height` is what asks for the height,
   and a second copy of the no-viewport crash beside it would be one fact answered from two places. */
static CssPx uv_icb(lxb_dom_element_t *el, bool vertical)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSContext *dctx;

    DCHECK(n->owner_document != NULL,
           "the initial containing block was asked for an element whose node has no owner document — every "
           "node this engine mints belongs to the document that created it");
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    if (dctx == NULL || !viewport_exists(dctx))
        DFAIL("CSS 2.1 §10.1's INITIAL CONTAINING BLOCK 'has the dimensions of the VIEWPORT', and this "
              "element's document is not being presented by any navigable — a DOMParser document, an XHR "
              "`responseXML`, a `<template>`'s contents owner, or the document of a navigable that has been "
              "destroyed. There is no viewport, so there is no ICB, so §10's used values have no rectangle to "
              "resolve against and no LAYOUT ran to produce one. CSSOM §9 does not state this escape: its two "
              "conjuncts are the property's `Applies to:` line and the element's OWN computed `display`, and "
              "both are true here, yet every user agent answers the COMPUTED value (`auto`) because the "
              "element generates no box. BUILD §9's missing conjunct over the predicate that already decides "
              "it — core/dom/element_view.h's `element_view_has_box`, which is defined over exactly this "
              "question — so the resolved value takes §9's computed-value escape before §10 is ever asked");
    return vertical ? viewport_icb_height(dctx) : viewport_icb_width(dctx);
}

/* ---- §10.1's ANSWER IS A BOX, AND A BOX IS NOT ALWAYS AN ELEMENT ------------------------------------------
   AN ELEMENT MAY GENERATE MORE THAN ONE BOX, so WHICH ELEMENT and WHICH OF THAT ELEMENT'S BOXES are two
   facts and a bare `lxb_dom_element_t *` states only the first. CSS 2.1 §17.4 Tables in the visual formatting
   model is where they come apart in this tree: "the table generates a principal block box called the table
   wrapper box that contains the table box itself and any caption boxes (in document order)", so ONE table
   element generates TWO boxes with two different content edges — the wrapper's, whose width §17.4 gives
   outright as "the border-edge width of the table box inside it", and the table box's own, which §17.5.2
   Table width algorithms: the 'table-layout' property computes and which is strictly inside the first.
   THE WRAPPER IS NOT AN ANONYMOUS BOX AND CALLING IT ONE IS WHAT KEPT IT UNNAMEABLE. Four crash sites in this
   directory said §10.1 could not name it because it was an anonymous box no element names, and CSS 2 §9.2.1.1
   Anonymous block boxes — the section that defines that term — settles the opposite twice. Its boxes are
   generated around content that no element wraps, and its own last paragraph orders every percentage past
   them, in §9.2.1.1's own words: "Anonymous block boxes are ignored when resolving percentage values that
   would refer to it: the closest non-anonymous ancestor box is used instead." So they need no name here,
   and the wrapper is not one of them — §17.4 calls it the table's PRINCIPAL block box, generated by an
   element that this walk is standing on. It was never nameless; the return type had nowhere to put the role.
   SO THE WALK ANSWERS A TAGGED BOX AND THE THREE PUBLIC ENTRIES ARE VIEWS OVER IT. That is the split
   used_value.h states one level up, taken one level further because the callers genuinely differ: §10's
   percentages want the rectangle's WIDTH, §10.3.3's over-constrained case wants its `direction`, and
   core/layout/flow_position.c, core/layout/block_flow.c and
   core/intersection_observer/intersection_observer.c want the BOX — its origin, its child list, or its place
   in the chain. One fact, three questions asked of it; a second walk for any of them would be §10.1's four
   cases implemented twice, free to disagree about which ancestor the rectangle belongs to. */
typedef enum {
    UV_CB_INITIAL = 0,      /* §10.1's FIRST case: the initial containing block. No box, no element. */
    UV_CB_PRINCIPAL,        /* the CONTENT EDGE of `element`'s own principal box — §10.1's second case. */
    UV_CB_TABLE_WRAPPER     /* §17.4's TABLE WRAPPER BOX, generated by the table element `element`. */
} UvCbBox;

typedef struct {
    UvCbBox box;
    lxb_dom_element_t *element;   /* NULL exactly when `box` is UV_CB_INITIAL */
} UvCb;

/* §10.1's four cases, in the spec's own order, answering the BOX whose CONTENT EDGE is the rectangle. Each
   case this component cannot answer crashes naming ITS case and not the neighbouring one, because they are
   missing for different reasons — and the SECOND case, which this component does answer, has crash arms of its
   own for the same reason one level down: an ancestor that is not a block container box is not one missing
   algorithm, it is four different ones and two sentences from other modules. */
static UvCb uv_cb(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n, *a;
    UvCb r;
    /* §17.2.1 Anonymous table objects' MISPARENTING RULE IS ASKED OF THE SUBJECT, ONCE, BEFORE THE WALK. A
       caption is the one box whose containing block is not its nearest block container ancestor's content
       edge, and the section states exactly which ancestor decides: "A row group box, 'table-column-group' box,
       or 'table-caption' box is misparented if its parent is neither a 'table' box nor an 'inline-table'
       box." So the FIRST ancestor that generates a box either generates a table box — and this caption is in
       that table's §17.4 wrapper — or it does not, and §17.2.1's third stage has generated an anonymous
       'table' box around the caption that no element names. Both answers are decided at that one ancestor,
       which is why the flag is read there and nowhere else in the loop. */
    bool caption = uv_table_box_kind(el) == TABLE_BOX_CAPTION;
    char nbuf[160], abuf[160];

    r.box = UV_CB_INITIAL;
    r.element = NULL;
    DCHECK(el != NULL, "§10.1's containing block was asked for with no element");
    n = lxb_dom_interface_node(el);
    if (uv_is_root(n)) return r;
    if (uv_computed_is(el, "position", "fixed"))
        DFAIL("CSS 2.1 §10.1's THIRD case: a `position: fixed` box's containing block 'is established by the "
              "VIEWPORT in the case of continuous media'. The rectangle is the one uv_icb above already "
              "answers, so the WIDTH is not what is missing — what is missing is everything else about a fixed "
              "box: §10.3.7's constraint equation between `left`, `width` and `right` is what turns that "
              "rectangle into a used width, and its `auto` cases need the STATIC POSITION, which is where the "
              "box would have been in normal flow. §9.4.1's normal flow is BUILT for IN-FLOW boxes "
              "(core/layout/flow_position.h); what is missing is a would-be position for an OUT-OF-FLOW one, "
              "which §10.6.3 keeps out of core/layout/block_flow.c's walk. EXTEND that walk, then BUILD "
              "§10.3.7 over it; this case and the fourth one below are then the same code with a different "
              "rectangle");
    if (uv_computed_is(el, "position", "absolute"))
        DFAIL("CSS 2.1 §10.1's FOURTH case: an absolutely positioned box's containing block is established by "
              "'the nearest ancestor with a position of absolute, relative or fixed', and is formed by that "
              "ancestor's PADDING EDGE — not its content edge, which is the second case's rectangle and the "
              "only one this component computes. THE BOX POSITIONS ARE NOT WHAT IS MISSING ANY MORE — "
              "core/layout/flow_position.h places every in-flow block-level box, so an ancestor's padding edge "
              "is a rectangle assembled from that origin and this component's two extents. Three pieces are, "
              "and they are different: that assembly as an ENTRY (this component answers an ELEMENT, and this "
              "case's rectangle is no element's content edge); §10.1's own exception for an INLINE ancestor, "
              "whose containing block is 'the bounding box around the padding boxes of the first and the last "
              "inline boxes' and is undefined in CSS 2.1 when that ancestor is split across lines; and the "
              "STATIC POSITION §10.3.7's and §10.6.4's `auto` cases fall back to, which is a would-be position "
              "for a box §10.6.3 tells core/layout/block_flow.c's walk to skip");
    /* §10.1's SECOND case: "For other elements, if the element's position is 'relative' or 'static', the
       containing block is formed by the CONTENT EDGE of the nearest BLOCK CONTAINER ANCESTOR BOX."
       "NEAREST" IS A SEARCH AND NOT A TEST ON THE PARENT, and §10.1's own worked example is what makes that
       checkable without a layout. For `<P id="p2">This is text <EM id="em1"> in the <STRONG id="strong1">
       second</STRONG> paragraph.</EM></P>` the section's table of containing blocks gives BOTH `em1` and
       `strong1` the block established by `p2` — so the walk steps straight OVER the inline `em1` rather than
       stopping at it, and an inline ancestor is not a case this component is missing. §10.1's inline exception
       ("the bounding box around the padding boxes of the first and the last inline boxes generated for that
       element") is written in the FOURTH case, about the nearest POSITIONED ancestor of a `position: absolute`
       box, and reading it into the second case is what used to make an ordinary `<label><input></label>`
       unanswerable here. AND THE BOX THE STEP LANDS ON IS THE RIGHT ONE EVEN WHERE AN ANONYMOUS BOX SITS
       BETWEEN, which is the one way this could have been a silent wrong answer rather than a crash. CSS 2
       §9.2.1.1 Anonymous block boxes covers both shapes: where the inline holds only inline content its run is
       wrapped in an anonymous block box whose "Non-inherited properties have their initial value ... the
       margins will be 0", and whose last paragraph says such a box "[is] ignored when resolving percentage
       values that would refer to it: the closest non-anonymous ancestor box is used instead"; and where the
       inline holds an in-flow BLOCK-LEVEL box, "the inline box ... are broken around the block-level box ...
       and the block-level box becomes a SIBLING of those anonymous boxes" — a sibling inside the same block
       container this walk returns. Either way the content edge is the one below.
       WHICH ANCESTORS ARE STEPPED OVER AND WHICH CRASH IS DECIDED PER `display`, NEVER BY AN `else`, because
       the ancestors that are not block container boxes are not one problem. Two of them ARE this case's answer
       by another module's own sentence, two need a box the ELEMENT tree does not name, and one has no subject
       at all — and an `else` here would return a rectangle for every one of them. */
    for (a = n->parent; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        lxb_dom_element_t *anc = lxb_dom_interface_element(a);
        char *d = uv_computed(anc, "display");
        bool container = block_flow_display_is_block_container(d);
        /* css-flexbox-1 §3 Flex Containers: the flex and inline-flex display values — "Flex containers form a
           containing block for their contents exactly like block containers do. [CSS2]" That is an ELEMENT's
           content edge, stated in the module's own words, so a flex container IS this case's answer even
           though CSS 2 §9.2.1 does not call it a block container box; §3 defines the pair as one container
           differing only in outer display type, so both spellings answer. css-grid-1 says the OPPOSITE of a
           grid container in its own §5.1, which is why the two are split here rather than sharing
           `uv_display_is_flex_or_grid`'s single predicate — that predicate answers "is this box a flex or grid
           CONTAINER", a question with one answer, and this is a question with two. */
        bool flex = strcmp(d, "flex") == 0 || strcmp(d, "inline-flex") == 0;
        bool grid = strcmp(d, "grid") == 0 || strcmp(d, "inline-grid") == 0;
        /* `table-cell` and `table-caption` are block containers (CSS 2 §9.2.1: "non-replaced inline blocks and
           non-replaced table cells are block containers but not block-level boxes") and are returned above, so
           what reaches the table arm is only the table box and the row/column machinery. */
        bool table = uv_display_is_table(d);
        /* §17.2.1's misparenting rule is written over a 'table' or 'inline-table' PARENT specifically, not
           over "a table box" loosely, so the test is core/layout/table_box.h's own two-value predicate. */
        bool anc_table_box = table_box_kind_generates_table_box(table_box_kind(d));
        bool none = strcmp(d, "none") == 0;
        /* THE TWO VALUES THE WALK STEPS OVER, for two different reasons that both end in "this ancestor is not
           the box §10.1 is asking about". css-display-3 §2.5 Box Generation: the none and contents keywords
           gives `contents` no box at all ("The element itself does not generate any boxes, but its children
           and pseudo-elements still generate boxes and text sequences as normal"), and §10.1 asks which BOX
           the rectangle is the content edge of; `inline` generates a box and it is simply not a block
           container one, which the section's own example above resolves. */
        bool contents = strcmp(d, "contents") == 0;
        bool step_over = contents || strcmp(d, "inline") == 0;

        free(d);
        /* §17.4's WRAPPER, WHICH IS THE ONE BOX THIS WALK ANSWERS THAT IS NOT AN ANCESTOR ELEMENT'S PRINCIPAL
           ONE. §17.2.1 Anonymous table objects decides it at the caption's own box parent and nowhere else —
           "A row group box, 'table-column-group' box, or 'table-caption' box is misparented if its parent is
           neither a 'table' box nor an 'inline-table' box" — so this test runs at the FIRST ancestor that
           generates a box and never again. `contents` is skipped because css-display-3 §2.5 Box Generation:
           the none and contents keywords gives it no box at all, so the caption's box parent is further up;
           `inline` is NOT skipped here even though the ordinary walk steps over it, because §17.2.1's own
           parenthesis makes an inline parent a MISPARENTING with its own answer ("If C's parent is an 'inline'
           box, then T must be an 'inline-table' box").
           THE TABLE ELEMENT ITSELF IS NOT GIVEN ITS OWN WRAPPER HERE, AND THAT IS §17.4's SENTENCE RATHER
           THAN AN OMISSION: "Percentages on 'width' and 'height' on the table are relative to the table
           wrapper box's containing block, not the table wrapper box itself." The wrapper's containing block is
           what this walk already returns for a table element — it starts at the table's parent — so a reader
           who "fixes" that by returning the wrapper for its own table would resolve every table percentage
           against the box the table box is inside, which is the one rectangle §17.4 rules out by name. */
        if (caption && !contents && !none) {
            if (anc_table_box) {
                r.box = UV_CB_TABLE_WRAPPER;
                r.element = anc;
                return r;
            }
            DFAILF("%s, whose box parent %s is neither a 'table' box nor an 'inline-table' box: "
                  "CSS 2 §17.2.1 Anonymous table objects calls that MISPARENTED in its own words — \"A row "
                  "group box, 'table-column-group' box, or 'table-caption' box is misparented if its parent is "
                  "neither a 'table' box nor an 'inline-table' box\" — and its third stage, Generate missing "
                  "parents, says what stands between this caption and a containing block: \"generate an "
                  "anonymous 'table' or 'inline-table' box T around C and all consecutive siblings of C that "
                  "are proper table children. (If C's parent is an 'inline' box, then T must be an "
                  "'inline-table' box; otherwise it must be a 'table' box.)\" So this caption IS in a table "
                  "wrapper box — the one around an anonymous table box that NO ELEMENT GENERATES — and there "
                  "is nothing here to name it with. RETURNING THE BOX PARENT'S CONTENT EDGE WOULD BE A REAL "
                  "RECTANGLE OF THE WRONG BOX: §17.5.2 Table width algorithms: the 'table-layout' property "
                  "sizes that anonymous table from the cells inside it and §17.4 Tables in the visual "
                  "formatting model makes the wrapper's width its border edge, which is narrower than the "
                  "block container it sits in whenever the table does not fill it. BUILD §17.2.1's THIRD "
                  "STAGE — core/layout/table_box.h owns the first two and states outright that the third is "
                  "the BLOCK walk's, because its subject is a child list that is not a table's — and this arm "
                  "then names the anonymous table it generates exactly as the arm above names an element's",
                  box_subject(el, nbuf, sizeof nbuf), box_subject(anc, abuf, sizeof abuf));
        }
        if (container || flex) {
            r.box = UV_CB_PRINCIPAL;
            r.element = anc;
            return r;
        }
        if (none)
            DFAILF("%s, whose ancestor %s generates no box: "
                  "§10.1's second case walked up to an ancestor whose computed `display` is `none`, and "
                  "css-display-3 §2.5 Box Generation: the none and contents keywords states what that means "
                  "for everything below it — \"The element and its descendants generate no boxes or text "
                  "sequences\" — so this element has no box either. That is not a missing algorithm: §10.1 has "
                  "no SUBJECT here, and a rectangle returned for it would be a containing block for a box that "
                  "does not exist. THE ANSWER IS CSSOM §9 Resolved Values' computed-value escape over "
                  "core/dom/element_view.h's `element_view_has_box`, taken at the RESOLVED-VALUE entry before "
                  "§10 is asked at all — the same escape `uv_icb` above names in full. A caller INSIDE layout "
                  "that reached here is that predicate and its own has-a-box test having come apart, since "
                  "every layout walk in this engine descends only through boxes that exist",
                  box_subject(el, nbuf, sizeof nbuf), box_subject(anc, abuf, sizeof abuf));
        if (table)
            DFAILF("%s, whose ancestor %s generates a TABLE box or a table-internal one: "
                  "CSS 2 §9.2.1 Block-level elements and block boxes excludes it from this case by name — "
                  "\"Except for table boxes, which are described in a later chapter, and replaced elements, a "
                  "block-level box is also a block container box\" — so the walk cannot stop here, and it must "
                  "not step over it either. "
                  "THIS IS NO LONGER THE TABLE WRAPPER BOX, AND A CRASH SAYING SO STOOD HERE UNTIL THE ARM "
                  "ABOVE WAS WRITTEN: the wrapper is now an answer this walk gives (`UV_CB_TABLE_WRAPPER`), "
                  "for the one box that is inside it and is not the table box — CSS 2 §17.4 Tables in the "
                  "visual formatting model's CAPTION. What reaches HERE is everything else, and it is a "
                  "DIFFERENT rectangle with a different owner. §17.4's wrapper \"contains the table box itself "
                  "and any caption boxes (in document order)\" and NOTHING ELSE, so a box under a table box is "
                  "inside the TABLE box, where §17.5 Visual layout of table contents places it on a GRID: "
                  "\"Each cell is thus a rectangular box, one or more grid cells wide and high\", and the "
                  "section's own last paragraph puts that rectangle's edges at the cells' border edges in the "
                  "separated model. A CELL'S CONTAINING BLOCK IS THEREFORE A GRID RECTANGLE AND NOT AN "
                  "ELEMENT'S CONTENT EDGE — the same shape §10.1's fourth case and css-grid-1's grid area are "
                  "blocked on — and a row's, a row group's, a column's and a column group's are §17.5's rules "
                  "1 through 4, of which core/layout/table_grid.h states outright that it places only the "
                  "cells. WHAT ARRIVES AS AN ORDINARY BOX rather than a table-internal one is §17.2.1 "
                  "Anonymous table objects' second stage generating an anonymous 'table-cell' box around it, "
                  "which no element names either. BUILD the grid rectangle as a box this walk can report, in "
                  "the same shape as the wrapper above — §17.5.2 Table width algorithms: the 'table-layout' "
                  "property and CSS 2.1 §17.5.3 Table height algorithms already answer the columns and rows it "
                  "is assembled from. "
                  "WHO IS WAITING ON IT IS NOT TWO FILES, and they do not all want the same thing: "
                  "core/layout/flow_position.c's placement and core/layout/block_flow.c's stack want a "
                  "RECTANGLE and are the same subproblem seen from the position side; "
                  "core/layout/scrolling_area.c's `sa_excluded` asks per DESCENDANT, so it reaches a cell by "
                  "walking into a table that nothing upstream classified; and "
                  "core/intersection_observer/intersection_observer.c walks the containing-block CHAIN itself, "
                  "at both of the call sites its own algorithm marks, and wants each LINK rather than any "
                  "box's edges — so a rectangle no element names breaks its loop rather than its geometry",
                  box_subject(el, nbuf, sizeof nbuf), box_subject(anc, abuf, sizeof abuf));
        if (grid)
            DFAILF("%s, whose ancestor %s is a GRID CONTAINER: "
                  "css-grid-1 §5.1 Establishing Grid Containers: the grid and inline-grid display values says "
                  "outright that \"Grid containers are not block containers\", so §10.1's second case does not "
                  "stop here — and unlike a flex container it does not answer this case by a sentence of its "
                  "own either, because the rectangle it establishes is not its content edge. css-grid-1 §3.3 "
                  "Grid Areas states where it is: \"A grid item's grid area forms the containing block into "
                  "which it is laid out.\" A grid area is a rectangle INSIDE the container that no element's "
                  "content edge is — the same shape §10.1's fourth case is blocked on — and its four edges are "
                  "grid lines that only css-grid-1's track sizing produces. THAT IS THE ONE DIFFERENCE FROM "
                  "THE FLEX ARM ABOVE, and it is the modules' own disagreement rather than a distinction drawn "
                  "here: css-flexbox-1 §3 Flex Containers: the flex and inline-flex display values says a flex "
                  "container forms the containing block \"for their contents exactly like block containers "
                  "do\", which IS an element's content edge and is answered. BUILD css-grid-1's track sizing "
                  "and the grid area as a rectangle this entry can report; until then a grid item's "
                  "percentages and its `auto` margins have no rectangle to resolve against",
                  box_subject(el, nbuf, sizeof nbuf), box_subject(anc, abuf, sizeof abuf));
        if (!step_over)
            DFAILF("%s, whose ancestor %s generates a box §10.1's second case cannot classify: "
                  "its computed `display` is not a BLOCK CONTAINER box (CSS 2 §9.2.1 Block-level elements and "
                  "block boxes), not a flex or grid container, not a table box, not `none`, and not one of the "
                  "two values this walk steps over. core/css/css_computed_value.c's css-display-3 §2.7 "
                  "Automatic Box Type Transformations blockification carries spellings this walk has never "
                  "been asked about — `run-in`, the four `ruby-*` values, a bare inner `flow`, and the "
                  "two-value `<display-outside> <display-inside>` syntax that file's own §2.7 crash names — "
                  "and each is a box type with a formatting context of its own. DECIDE THIS VALUE HERE, in the "
                  "same three-way shape the arms above have: it is a block container box (stop and return it), "
                  "a box §10.1 walks past because it is not one (continue), or a box whose OWN module owns the "
                  "rectangle (crash naming that module). An `else` that picked one of the three would answer "
                  "every future spelling the same way",
                  box_subject(el, nbuf, sizeof nbuf), box_subject(anc, abuf, sizeof abuf));
        /* css-display-3 §2.8 The Root Element's Principal Box: "The root element's display type is always
           blockified", and "Additionally, a display of contents computes to block on the root element" — so
           the root is a block container however it was declared and the walk stops AT it, never over it. */
        DCHECKF(!uv_is_root(a),
               "%s: "
               "the containing-block walk stepped OVER the ROOT ELEMENT. css-display-3 §2.8 The Root Element's "
               "Principal Box forbids it twice — the root's display type \"is always blockified\", and a "
               "`display` of `contents` \"computes to block on the root element\" — so neither value this walk "
               "steps over can survive the cascade on a root. A root reaching this line is a computed-value "
               "rule that did not run",
               box_subject(anc, abuf, sizeof abuf));
    }
    DFAIL("the containing-block walk ran out of ancestors without finding a block container box. Every walk "
          "starts below the ROOT ELEMENT — §10.1's first case answers the root itself — and the root is a "
          "block container whatever it declares (CSS Display §2.8 blockifies it), so an element in a tree with "
          "a root cannot reach here. What can is an element whose ancestors do NOT reach a root element: a "
          "node in a DocumentFragment or a detached subtree, which has no box in any user agent and whose "
          "resolved value is CSSOM §9's computed value for the reason uv_icb above states in full");
    return r;
}

/* §10.1's ANSWER AS AN ELEMENT — the view used_value.h's contract is written over, and the one that has to
   REFUSE a box no element's principal box is. */
lxb_dom_element_t *used_value_containing_block(lxb_dom_element_t *el)
{
    UvCb cb = uv_cb(el);
    char nbuf[160], abuf[160];

    if (cb.box == UV_CB_TABLE_WRAPPER)
        DFAILF("%s: §10.1's containing block for this box is CSS 2 §17.4 Tables in the visual formatting "
              "model's TABLE WRAPPER BOX around %s, and this entry answers an ELEMENT'S OWN PRINCIPAL BOX. "
              "THE RECTANGLE IS NOT WHAT IS MISSING and a caller that wanted only its width or its "
              "`direction` already has it — §17.4 gives the width outright (\"The width of the table wrapper "
              "box is the border-edge width of the table box inside it, as described by section 17.5.2\") and "
              "`used_value_containing_block_width` composes it. WHAT IS MISSING IS THE WRAPPER'S OWN CHILD "
              "BOX LIST, which is what every caller of THIS entry wants: §17.4 says it \"contains the table "
              "box itself and any caption boxes (in document order)\", and that list is not the table "
              "element's DOM children — the captions are, the table box is the SAME element wearing its other "
              "box, and the rows are not in it at all. So core/layout/block_flow.c's §9.4.1 stack cannot "
              "iterate it with `bf_element_child`, core/layout/flow_position.c cannot place inside it, and "
              "core/intersection_observer/intersection_observer.c's chain has no link to step to. BUILD THAT "
              "BOX LIST as the thing §9.4.1's walk iterates — it is the same box-tree step §9.2.1.1 Anonymous "
              "block boxes' runs and css-display-3 §2.5 Box Generation: the none and contents keywords' "
              "`contents` splice both need, and core/layout/block_flow.c's own crashes ask for all three",
              box_subject(el, nbuf, sizeof nbuf), box_subject(cb.element, abuf, sizeof abuf));
    return cb.element;
}

/* THE CONTAINING BLOCK'S WIDTH — §10.1's first case answered by the viewport, its second by the CONTENT EDGE
   of the ancestor box, and §17.4's wrapper by §17.4's own sentence. */
CssPx used_value_containing_block_width(lxb_dom_element_t *el)
{
    UvCb cb = uv_cb(el);

    if (cb.box == UV_CB_INITIAL) return uv_icb(el, false);
    /* CSS 2 §17.4 Tables in the visual formatting model: "The width of the table wrapper box is the
       border-edge width of the table box inside it, as described by section 17.5.2." It is the CONTENT edge
       this entry owes and the BORDER edge that sentence names, and the two coincide for this one box because
       the same section gives it the initial value of every property it does not list — "(Where the table
       element's values are not used on the table and table wrapper boxes, the initial values are used
       instead.)" — so the wrapper has no border and no padding whatever the table element declares. That is
       asserted through core/layout/table_wrapper.h's own reading of §17.4's list rather than re-read from the
       section here, so the day the list changes the two cannot disagree. */
    if (cb.box == UV_CB_TABLE_WRAPPER) {
        DCHECK(!table_wrapper_owns_property("border-left-width") &&
                   !table_wrapper_owns_property("padding-left") && !table_wrapper_owns_property("width"),
               "CSS 2 §17.4 Tables in the visual formatting model's declaration split has come apart from the "
               "reading this line depends on. §17.4 names `position`, `float`, the four `margin-*` and the "
               "four offsets as the WRAPPER's and sends \"all other values of non-inheritable properties\" to "
               "the table box, which is what leaves the wrapper with no border and no padding and makes its "
               "CONTENT edge the same rectangle as the border edge §17.4 states its width over. If any of "
               "those three is now the wrapper's, this composition is measuring the wrong box");
        return used_value_border_edge_px(cb.element, false);
    }
    return uv_content_size(cb.element, false, uv_surround(cb.element, false));
}

/* THE CONTAINING BLOCK'S `direction`, with §10.1's FIRST-case exception stated once. "The 'direction' property
   of the initial containing block is the same as for the root element" is that section's own sentence, so the
   root's containing block answers with the ROOT'S OWN computed value rather than with an initial value the
   rectangle would otherwise have to carry separately.
   §17.4's WRAPPER READS THE TABLE ELEMENT'S OWN COMPUTED VALUE, AND THAT IS A DERIVATION RATHER THAN A GUESS
   AT A SENTENCE §17.4 DOES NOT WRITE. §17.4's split is scoped to "non-inheritable properties" in its own
   words, and `direction` is INHERITED (css-writing-modes-4 §2.1 "Specifying Directionality: the direction
   property", `Inherited: yes`) — so there is nothing for that sentence to divide: ONE element generates both
   boxes, an inherited property has ONE computed value on that element, and both boxes carry it.
   core/layout/table_wrapper.h states the same reading of the same sentence, which is why this is a read of the
   table element rather than a second rule written here. */
bool used_value_containing_block_is_rtl(lxb_dom_element_t *el)
{
    UvCb cb = uv_cb(el);
    char *d = uv_computed(cb.box == UV_CB_INITIAL ? el : cb.element, "direction");
    bool rtl = strcmp(d, "rtl") == 0;
    bool ltr = strcmp(d, "ltr") == 0;

    free(d);
    DCHECK(rtl || ltr,
           "a computed `direction` is neither `ltr` nor `rtl`. css-writing-modes-4 §2.1 \"Specifying "
           "Directionality: the direction property\" gives the property the `Value:` line `ltr | rtl` and "
           "nothing else, and its `Computed value:` line is `specified value` — so a third spelling here is a "
           "declaration that reached the cascade without its grammar, which lexbor's own `direction` state "
           "machine refuses");
    return rtl;
}

/* css-sizing-3 §3.2.1 "“Behaving as auto”" — THE QUESTION CSS 2.1 ASKS AS "a computed value of `auto`", ASKED
 * THE WAY THE CURRENT SPEC DEFINES IT, AND THE ANSWER IS NOT A COMPUTED VALUE.
 * CSS 2.1 §10.5's `<percentage>` prose says "if the height of the containing block is not specified explicitly
 * (i.e., it depends on content height), and this element is not absolutely positioned, THE VALUE COMPUTES TO
 * 'auto'", and its `Computed value:` line carries the same reading ("the percentage or 'auto' (see prose under
 * <percentage>) or the absolute length"). css-sizing-3 SUPERSEDES BOTH: §1.1 "Module interactions" says the
 * module "extends the width, height, min-width, min-height, max-width, max-height … features defined in [CSS2]
 * chapter 10", §3.1.1 "Preferred Size Properties: the width and height properties" gives the property
 * `Computed value: as specified, with <length-percentage> values computed` — a percentage SURVIVES to the
 * computed value — and §3.2.1 names this exact case as the reason the term exists: "to have a common term for
 * both when width/height computes to auto AND WHEN IT IS DEFINED TO BEHAVE AS IF AUTO WERE SPECIFIED (as in the
 * case of block percentage heights resolving against an indefinite size, see CSS2§10.5), the property is said
 * to BEHAVE AS AUTO in both of these cases", with the note that "legacy spec prose defining layout behavior,
 * particularly in [CSS2], might explicitly refer to width/height having a computed value of auto as a
 * condition; some of these cases should be interpreted as meaning behaves as auto".
 * SO THIS IS A USED-VALUE QUESTION AND NOT A COMPUTED-VALUE RULE, which is the opposite of what the crash that
 * stood here instructed. Building it in core/css/css_computed_value.c would make `getComputedStyle(el).height`
 * answer `auto` for a `display: none` element declaring `height: 50%`, where every user agent answers `50%` —
 * a fidelity bug written under a spec sentence that a later level withdrew.
 * WHICH LAYOUT CONDITIONS IT REPLACES IS THE SPEC'S OWN CHOICE, NOT A SWEEP OF EVERY `auto` IN §10. §3.2.1's
 * test list names `margin-collapse-with-indefinite-block-size-001` through `-005`, so CSS 2.1 §8.3.1's
 * collapsing conditions ("auto computed height", "zero or auto computed height") are cases the current spec
 * reads this way, and §10.6.3's content-based height is the same question by construction — a box whose height
 * behaves as auto is a box whose height comes from its content. §10.3.2's intrinsic-ratio arms and §10.4's
 * ratio table say "computed values of 'auto'" about a REPLACED element's two axes, and those stay literal
 * here: their antecedent needs an intrinsic ratio, which core/layout/replaced_element.c mints nowhere, so
 * widening them would be a guess at a rule no fixture in this build can exercise.
 * IT READS COMPUTED VALUES AND WALKS §10.1's CHAIN, AND COMPUTES NO SIZE. That is what keeps it cheap enough to
 * be asked at every one of those conditions: the definiteness of a percentage height is a property of the chain
 * of declarations above it, and §4.1 "Percentage Sizing"'s own worked example says so — a percentage height "is
 * itself definite because it's a percentage resolved against a definite length". A size-computing answer would
 * re-run each ancestor's layout once per level. */
bool used_value_height_behaves_as_auto(lxb_dom_element_t *el)
{
    CssLength h;
    UvCb cb;

    DCHECK(el != NULL, "the behaves-as-auto question was asked with no element");
    h = css_computed_length(el, "height");
    if (h.kind == CSS_LENGTH_KEYWORD) {
        DCHECK(uv_len_is_auto(h),
               "a `height` computed to a keyword that is not `auto`. CSS 2.1 §10.5's `<length> | <percentage> "
               "| auto` admits no other, and css-sizing-3 §3.2 \"Sizing Values\"'s `min-content`/`max-content`/"
               "`fit-content()` are level-3 additions this engine records no computed-value rule for");
        return true;
    }
    if (h.kind == CSS_LENGTH_ABSOLUTE) return false;
    DCHECK(h.kind == CSS_LENGTH_PERCENTAGE || h.kind == CSS_LENGTH_CALCULATED,
           "a computed `height` is none of the four kinds css_length.h defines");
    /* §10.5's OWN EXCEPTION, "AND THIS ELEMENT IS NOT ABSOLUTELY POSITIONED", with §10.5's own reason stated
       in its note: "the height of the containing block of an absolutely positioned element is independent of
       the size of the element itself, and thus a percentage height on such an element CAN ALWAYS BE RESOLVED".
       CSS 2.1 §9.6 "Absolute positioning" defines the term over both values — "references in this
       specification to an absolutely positioned element (or its box) imply that the element's 'position'
       property has the value 'absolute' or 'fixed'" — and §9.6.1 "Fixed positioning" calls fixed positioning
       "a subcategory of absolute positioning". So such a box's height never behaves as auto for want of a
       definite containing block. Whether this engine can RESOLVE it is a DIFFERENT question and is not
       answered here: §10.1's third and fourth cases own that rectangle and crash where it is asked for. This
       arm is what keeps THIS predicate from being the thing that raises that crash, which matters because
       core/layout/block_flow.c asks it of boxes it is only classifying. */
    if (uv_computed_is(el, "position", "absolute") || uv_computed_is(el, "position", "fixed")) return false;
    /* §10.5: "A percentage height on the ROOT ELEMENT is relative to the initial containing block" — whose
       dimensions are the viewport's, so the root's percentage always resolves and never behaves as auto.
       §10.1's first case is the same element, which is why the NULL is that sentence rather than a gap. */
    cb = uv_cb(el);
    if (cb.box == UV_CB_INITIAL) return false;
    /* §17.4's WRAPPER ANSWERS TRUE OUTRIGHT AND THE RECURSION STOPS AT IT, which is the section's own
       parenthesis rather than an assumption about tables: `height` is a non-inheritable property §17.4 does
       NOT list as the wrapper's, so "the initial values are used instead" — and the initial value of `height`
       is `auto`, which css-sizing-3 §3.2.1 "“Behaving as auto”" makes this predicate's first arm. RECURSING
       INTO THE TABLE ELEMENT INSTEAD WOULD ASK THE WRONG BOX: §17.4 puts the declared `height` on the TABLE
       BOX, so `<table style="height:400px">` would answer FALSE and make a caption's `height: 50%` resolve
       against a box the caption is not in. */
    if (cb.box == UV_CB_TABLE_WRAPPER) {
        DCHECK(!table_wrapper_owns_property("height"),
               "CSS 2 §17.4 Tables in the visual formatting model's declaration split now puts `height` on "
               "the WRAPPER box, and this arm is written over the opposite — that the wrapper takes §17.4's "
               "initial value, which is `auto`, so its height always behaves as auto. If the split has "
               "changed, the wrapper's height is the table element's declaration and this answer is wrong for "
               "every box inside it");
        return true;
    }
    /* §4.1 "Percentage Sizing": the percentage is definite exactly when the containing block's height is, and
       a containing block whose OWN height is a percentage is definite "because it's a percentage resolved
       against a definite length" one level further up. The recursion terminates at the root above. */
    return used_value_height_behaves_as_auto(cb.element);
}

/* CSS 2.1 §10.7's OTHER BASIS, and the reason it is a `bool` and not a `CssPx`: "If the height of the
   containing block is NOT SPECIFIED EXPLICITLY (i.e., it depends on content height), and this element is not
   absolutely positioned, the percentage value is treated as '0' (for 'min-height') or 'none' (for
   'max-height')." So the question §10.7 asks first is whether the basis EXISTS, and false here is that
   sentence's antecedent rather than a failure to compute one — the caller turns it into the spec's two
   answers, which differ per property and are therefore not this function's to pick.
 * IT IS ALSO WHAT KEEPS THE CLAMP FROM WALKING THE TREE TWICE PER LEVEL. "Depends on content height" is
 * §10.6.3's walk, so resolving a percentage against an `auto`-height containing block would run that walk for
 * every descendant that declares a percentage limit — and §10.7 does not ask for it: the definiteness test is
 * over COMPUTED `height`s up §10.1's chain, which no layout has to run to read.
 * THE ABSOLUTELY-POSITIONED HALF OF THE SENTENCE IS NOT WRITTEN HERE AND IS NOT SKIPPED. An absolutely
 * positioned box's containing block is §10.1's FOURTH case — the PADDING EDGE of the nearest positioned
 * ancestor — and `used_value_containing_block` crashes on it naming exactly what that case needs. So an
 * abs-pos element never reaches the test below, and writing the exception here would be a branch under a
 * crash. */
static bool uv_cb_height(lxb_dom_element_t *el, CssPx *out)
{
    UvCb cb = uv_cb(el);

    /* §10.1's first case: the INITIAL containing block, whose height is the viewport's and is therefore as
       explicitly specified as a height gets — the one containing block that is definite without a layout. */
    if (cb.box == UV_CB_INITIAL) { *out = uv_icb(el, true); return true; }
    /* §17.4's WRAPPER IS NEVER EXPLICITLY SPECIFIED, so the sentence's antecedent holds for it without a
       height being computed at all: §17.4 Tables in the visual formatting model uses `height` on the TABLE
       BOX and gives the wrapper the initial value instead, which is `auto`. The predicate above states the
       same derivation and asserts §17.4's split; asking it here would recurse through this box's own arm and
       reach the same answer one call later. */
    if (cb.box == UV_CB_TABLE_WRAPPER) return false;
    /* "NOT SPECIFIED EXPLICITLY (i.e., it depends on content height)" IS THE BEHAVES-AS-AUTO QUESTION, asked
       through the one predicate above rather than re-derived from a computed value here. It answers `auto`
       and a percentage alike: a percentage `height` SURVIVES to the computed value (css-sizing-3 §3.1.1), and
       whether it resolves is a fact about §10.1's chain above the containing block — §4.1 "Percentage Sizing"'s
       own example is a `50%` height that is definite "because it's a percentage resolved against a definite
       length". The arm that used to crash here asked for §10.5 as a COMPUTED-value rule, which css-sizing-3
       §3.2.1 withdrew; the predicate is that withdrawal. */
    if (used_value_height_behaves_as_auto(cb.element)) return false;
    *out = uv_content_size(cb.element, true, uv_surround(cb.element, true));
    return true;
}

CssPx used_value_content_px(lxb_dom_element_t *el, bool vertical)
{
    DCHECK(el != NULL, "a content extent was asked for with no element");
    return uv_content_size(el, vertical, uv_surround(el, vertical));
}

/* ---- CSS 2.1 §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'" and §10.7 "Minimum and maximum
   heights: 'min-height' and 'max-height'" — THE TWO LIMITS' OWN USED VALUES ------------------------------------
   The two sections state ONE algorithm twice, and the axis is the only thing that differs in it — so the pair
   is one function taking `vertical`, exactly as every other §10 rule in this file is. What is NOT the same on
   the two axes is the PERCENTAGE BASIS, and that difference is not a detail: §10.4's basis is the containing
   block's WIDTH, which §10.1's chain always has, while §10.7's is its HEIGHT, which is frequently INDEFINITE
   and which §10.7 then answers with a rule rather than leaving undefined. */

/* THE TWO LIMITS ON ONE AXIS, resolved to used values. `min` is ALWAYS PRESENT and `max` is not, which is the
   two properties' own initial values and not a convenience: css-sizing-3 §3.1.2 "Minimum Size Properties: the
   min-width and min-height properties" gives `min-width` the initial `auto`, and §3.2 "Sizing Values: the
   <length-percentage>, auto | none, min-content, max-content, and fit-content() values" says of it "for
   min-width/min-height, specifies an automatic minimum size. Unless otherwise defined by the relevant layout
   module, however, IT RESOLVES TO A USED VALUE OF 0" — so there is always a floor and it is 0, which is also
   the CSS 2.1 initial value verbatim. §3.1.3 "Maximum Size Properties: the max-width and max-height
   properties" gives `max-width` the initial `none`, and §3.2 defines that as "no limit on the size of the
   box" — an ABSENCE, carried as one rather than as an infinity, for the reason replaced_element.h gives for
   every presence flag: a real limit and a sentinel that means there is none must not share a spelling. */
typedef struct {
    bool  has_min;
    CssPx min;
    bool  has_max;
    CssPx max;
} UvLimits;

/* ONE limit. `is_max` picks the property; `box` is read only where the answer depends on the box's type, which
   is exactly css-sizing-3 §3.2's "unless otherwise defined by the relevant layout module". */
static bool uv_limit(lxb_dom_element_t *el, UvBox box, bool vertical, bool is_max, CssPx *out)
{
    static const char *const NAMES[2][2] = {
        { "min-width",  "max-width"  },
        { "min-height", "max-height" },
    };
    const char *name = NAMES[vertical ? 1 : 0][is_max ? 1 : 0];
    CssLength len = css_computed_length(el, name);
    CssPx basis;

    /* A LENGTH, WHICH IS ALSO WHERE `min(50vw, 400px)` ARRIVES. css-values-4 §10's math functions are resolved
       by core/css/css_math.h inside the computed value (core/css/css_length.c routes any functional value
       through `css_math_eval`), so a `max-width: min(50vw, 400px)` reaches here as ONE absolute length whose
       `CssPx` already carries the ICB's fact — there is nothing for this component to evaluate and a second
       evaluation here would be the second implementation css_math.h exists to prevent.
       AND css-values-4 §9.1 "Numeric Functions"'S CLAMP IS WHY IT IS NO LONGER AN ASSERT. CSS 2.1
       §10.4 and §10.7 both state the range outright — "negative values for 'min-width' and 'max-width' are
       illegal" — and §5.1 "Range Restrictions and Range Definition Notation" makes a value outside an allowed
       range a DROPPED DECLARATION, so a negative LITERAL still cannot reach here and that half of the assert
       was right. The half that is no longer true is that nothing else can: §9.1 states the opposite rule for a
       math function — "numeric functions returning out-of-range values NEVER cause a declaration to become
       invalid. Instead, the value of a numeric function is clamped to the range allowed in the context it is
       used at computed value time if possible, and at used value time otherwise" — so `max-width: calc(100px -
       200px)` is VALID CSS whose used value is 0, and a page writing it would have crashed an assert about the
       engine's own invariants. The two are told apart by that derivation and not by a flag on the value: a
       negative that reaches here is a top-level calculation's result, because nothing else survives §5.1.
       `css_px_max` and not an `if`: the clamped-away operand's environment facts are part of the value's
       domain at every viewport, including the ones where it floors. */
    if (len.kind == CSS_LENGTH_ABSOLUTE) {
        *out = css_px_max(len.px, css_px(0.0));
        return true;
    }
    if (len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED) {
        DCHECK(len.kind != CSS_LENGTH_PERCENTAGE || len.pct >= 0.0,
               "a NEGATIVE percentage `min-`/`max-` size. §5.1's dropped declaration is what makes this "
               "unreachable for a LITERAL — the two grammars forbid it in the same sentence the length arm "
               "above quotes — and a math function's negative percentage term is CSS_LENGTH_CALCULATED, which "
               "this assert deliberately does not cover because §9.1 clamps that one after the basis resolves "
               "it rather than refusing it here");
        if (!vertical) {
            /* §10.4: "the percentage is calculated with respect to the width of the generated box's
               containing block." */
            basis = used_value_containing_block_width(el);
            DCHECK(basis.px >= 0.0,
                   "CSS 2.1 §10.4's percentage basis is NEGATIVE. The section has a rule for that — \"if the "
                   "containing block's width is negative, the used value is zero\" — and it is UNREACHABLE in "
                   "this component rather than unimplemented: every containing block width here is either the "
                   "initial containing block's (the viewport's, and core/frame/viewport.h asserts that "
                   "positive), a declared non-negative <length>, or §10.3.3's equation, which css-sizing-3 "
                   "§3.3 floors at zero. So a negative one is a derivation that lost an operand, not a page. "
                   "§10.4's OTHER undefined case is a different sentence and is unreachable for the reason "
                   "uv_padding states: \"if the containing block's width depends on this element's width, then "
                   "the resulting layout is undefined in CSS 2.1\"");
        } else if (!uv_cb_height(el, &basis)) {
            /* §10.7's OWN ANSWER for an indefinite basis, which is a rule and not a fallback: "the percentage
               value is treated as '0' (for 'min-height') or 'none' (for 'max-height')". The two properties
               get DIFFERENT answers, which is why the escape is turned into a value here rather than in
               `uv_cb_height`. Both answers are the property's own initial value, so `height: auto` on a
               containing block makes a percentage limit on its children vanish — which is why
               `min-height: 100%` inside an auto-height parent does nothing in every user agent.
               IT TAKES THE MATH FUNCTION WHOLE, and css-values-4 §10.11 "Computed Value" says so in the
               sentence that exists for exactly this: "if there are SPECIAL RULES for computing percentages in a
               value (e.g. the height property), THEY APPLY whenever a math function contains percentages." So
               `min-height: calc(100% - 2rem)` against an indefinite basis is 0 and `max-height: calc(...)` is
               `none`, rather than the percentage half vanishing and the `-2rem` surviving as a limit the author
               never wrote. */
            if (is_max) return false;
            *out = css_px(0.0);
            return true;
        }
        /* §10.11's used-value-time simplification, then §9.1's clamp in that order — the range check needs the
           number the basis produces, which is §9.1's "at used value time otherwise". For a bare percentage both
           operands are already non-negative and the floor is the identity; for a math function it is the whole
           of what makes `max-width: calc(50% - 100px)` a valid declaration on a narrow viewport instead of a
           negative limit that would clamp every box to nothing. */
        *out = css_px_max(css_length_resolve_pct(len, basis), css_px(0.0));
        return true;
    }
    DCHECK(len.kind == CSS_LENGTH_KEYWORD,
           "a `min-`/`max-` size computed to a kind that is none of the four css_length.h defines — "
           "css-sizing-3 §3.1.2 and §3.1.3's grammars admit a length, a percentage, a math function carrying "
           "both, and a keyword, and the arms above have taken the first three");
    if (is_max && strcmp(len.keyword, "none") == 0) return false;
    if (!is_max && uv_len_is_auto(len)) {
        if (box == UV_BOX_ITEM)
            DFAIL("`min-width`/`min-height` is `auto` on a FLEX or GRID ITEM, which is the one box css-sizing-3 "
                  "§3.2 hands to another module: \"specifies an automatic minimum size. UNLESS OTHERWISE "
                  "DEFINED BY THE RELEVANT LAYOUT MODULE, however, it resolves to a used value of 0.\" "
                  "css-flexbox-1 §4.5 \"Automatic Minimum Size of Flex Items\" is that definition and it is a "
                  "CONTENT-BASED minimum — the item's min-content size, clamped by its specified size "
                  "suggestion and by its natural aspect ratio. THE MIN-CONTENT SIZE IS BUILT "
                  "(core/layout/intrinsic_size.h), so what is missing here is §4.5's clamp and the flex layout "
                  "it is part of, not the measurement. This element never reaches here through "
                  "`used_value_px`: every arm of §10 crashes for a flex or grid item first, naming the "
                  "container's algorithm. BUILD the flex layout");
        /* §3.2, for every other box: "it resolves to a used value of 0". Its very next sentence pins that for
           the boxes this component classifies — "for backwards-compatibility, the resolved value of this
           keyword is zero for boxes of all [CSS2] display types: block and inline boxes, inline blocks, and
           all the table layout boxes" — so this is the spec's number and not CSS 2.1's initial value borrowed
           for a keyword CSS 2.1 does not have. */
        *out = css_px(0.0);
        return true;
    }
    /* css-sizing-3 §3.2 "Sizing Values: …"'s `min-content` AND `max-content` ON A `min-`/`max-` SIZE — "use the
       min-content size in the relevant axis; for a box's BLOCK SIZE, unless otherwise specified, this is
       equivalent to its AUTOMATIC SIZE", and the identical sentence for `max-content`.
       THE AXIS IS THE WHOLE OF THE DIFFERENCE AND §3.2 IS WHERE IT IS STATED, which is a correction worth
       carrying: read off css-sizing-3 §2.1 "Auto Box Sizes" alone the block axis looks like a second intrinsic
       measurement (its max-content block size is "the block size of the content after layout"), but the sizing
       VALUE does not reach for it — §3.2 sends both keywords to the automatic size in that axis, which is what
       `auto` would have produced and which §3.2's own `auto` entry names ("for width/height, specifies an
       automatic size"). So the inline axis reads §5.1's two sizes and the block axis reads CSS 2.2 §10.6.3's
       content-based height, and neither is the other turned sideways.
       §3.3's `box-sizing` conversion is applied to both for the reason every other arm applies it: each of the
       three is a CONTENT size and the used value css-sizing-3 §3.3 exposes is the border box's. */
    if (strcmp(len.keyword, "min-content") == 0 || strcmp(len.keyword, "max-content") == 0) {
        CssPx px;

        if (vertical) {
            px = block_flow_auto_height(el);
        } else {
            IntrinsicInlineSizes in = intrinsic_inline_sizes(el);

            px = strcmp(len.keyword, "min-content") == 0 ? in.min_content : in.max_content;
        }
        *out = uv_is_border_box(el) ? css_px_add(px, uv_surround_total(uv_surround(el, vertical))) : px;
        return true;
    }
    DFAIL("a `min-`/`max-` size is css-sizing-3 §3.2's `fit-content()` or one of the level-3 sizing values "
          "beside it, whose used value the section states as a clamp: \"use the fit-content formula with the "
          "available space replaced by the specified argument, i.e. `min(max-content, max(min-content, "
          "<length-percentage>))`, where the <length-percentage> argument is resolved exactly as for "
          "<length-percentage> values standing alone\". THE TWO INTRINSIC SIZES ARE BUILT "
          "(core/layout/intrinsic_size.h) and that clamp is the same three-term one `uv_shrink_to_fit_width` "
          "already computes; what this arm lacks is the ARGUMENT, because `fit-content(20px)` is a FUNCTION and "
          "`css_computed_length` carries a bare keyword with nothing under it. BUILD the argument through the "
          "computed-value model — css-values-4 §10.11 \"Computed Value\" leaves its percentage unresolved "
          "exactly as a math function's is, so it is the shape `CSS_LENGTH_CALCULATED` already has — and this "
          "arm is then the clamp over it. §3.2's `calc-size()` is a separate grammar and a separate build");
    return false;
}

static UvLimits uv_limits(lxb_dom_element_t *el, UvBox box, bool vertical)
{
    UvLimits lim;

    /* CSS 2.1 §10.4: "In CSS 2.1, the effect of 'min-width' and 'max-width' on tables, inline tables, table
       cells, table columns, and column groups is undefined" — and §10.7 says the same of rows and row groups.
       Every one of those is a `UV_BOX_TABLE`, and `uv_sized` returns before this function for every one of
       them, so this asserts the two classifications agree rather than choosing an answer the spec declines to
       give. */
    DCHECK(box != UV_BOX_TABLE,
           "CSS 2.1 §10.4 and §10.7 leave the effect of the four limits on a TABLE BOX undefined, and §17.5's "
           "own algorithms are what decide it — a declared width is a MINIMUM there and CSS 2.1 §17.5.2 Table "
           "width algorithms: the 'table-layout' property may exceed it. `uv_sized` returns a table box's "
           "tentative value before reaching this function, so a table box here means that return and "
           "uv_box_kind's list have come apart");
    lim.min = css_px(0.0);
    lim.max = css_px(0.0);
    lim.has_min = uv_limit(el, box, vertical, false, &lim.min);
    lim.has_max = uv_limit(el, box, vertical, true, &lim.max);
    DCHECK(lim.has_min,
           "§10.4 step 3's FLOOR came back absent, and there is no arm that can answer that: `none` is only on "
           "`max-width`/`max-height` (css-sizing-3 §3.1.3), and §3.2 resolves the `min-` pair's `auto` to a "
           "used value of 0. So the floor is always present — an absence here is `uv_limit`'s two properties "
           "having come apart, and reading `min` past it would clamp against an uninitialised limit");
    return lim;
}

/* §10.4's SECOND ALGORITHM — the CONSTRAINT-VIOLATION TABLE — whose ANTECEDENT is tested here and is FALSE in
 * this build, so what stands here is the crash that names it rather than eleven rows nothing can exercise.
 *
 * §10.4's own words: "However, for replaced elements with an INTRINSIC RATIO and BOTH 'width' AND 'height'
 * SPECIFIED AS 'auto', the algorithm is as follows: select from the table the resolved height and width values
 * for the appropriate constraint violation. Take the max-width and max-height as max(min, max) so that
 * min <= max holds true." §10.7 does not restate it — "for replaced elements with both 'width' and 'height'
 * computed as 'auto', use the algorithm under Minimum and maximum widths above to find the used width and
 * height" — so the two axes are ONE joint solve and the per-axis clamp below is not merely a different order
 * of the same arithmetic, it is a different answer: the table PRESERVES THE RATIO, which two independent
 * clamps cannot.
 *
 * IT IS REACHED BY A `canvas`, AND THE SENTENCE THAT STOOD HERE SAID IT COULD NOT BE. That sentence was an
 * argument about THIS TREE and not about the spec — "core/layout/replaced_element.c mints `has_ratio` in
 * exactly one place, `rep_sized(0.0, 0.0)`, whose degenerate ratio css-images-3 §4.1 denies" — and it stopped
 * being true the moment that component derived HTML §4.12.5 "The canvas element"'s bitmap: a `<canvas>` has a
 * natural width of 300 and a natural height of 150 from its own content attributes, so it has a natural ratio
 * of 2, which is the first non-degenerate one this engine has ever produced. THE TRIGGER IS ORDINARY
 * RESPONSIVE CSS — `canvas { max-width: 100% }` with neither `width` nor `height` declared — so this abort is
 * a work queue rather than an edge case, and the per-axis clamp it stands in front of is the WRONG answer for
 * exactly the element that now reaches it.
 *
 * HTML §15.4.3 "Attributes for embedded content and images" WOULD MOVE IT OUT OF REACH AGAIN, and that is
 * worth knowing before anyone reads a green run as this being fixed: §15.4.3 maps the `width`/`height` CONTENT
 * attributes to presentational hints on the `width`/`height` PROPERTIES, which makes the antecedent's "both
 * specified as 'auto'" FALSE for every `<canvas width=… height=…>`. A bare `<canvas>` under a `max-width`
 * still reaches it, and so does an `<img>` the day a DECODER gives §15.4.2's first rule real natural
 * dimensions — which is the case that matters most, because it is what makes a narrow viewport scale a
 * photograph's height instead of leaving it at its natural one. BUILD the eleven rows. */
static void uv_require_no_ratio_table(lxb_dom_element_t *el, const UvLimits *lim)
{
    ReplacedElement rep;

    /* The cheap conjuncts first, and the classification last — `replaced_element_of` reads an image request's
       state, and the table's subject is a shape that answers no to one of these two tests long before that. */
    if (!lim->has_max && !(lim->has_min && lim->min.px > 0.0)) return;
    if (!uv_length_is(el, "width", "auto") || !uv_length_is(el, "height", "auto")) return;
    rep = replaced_element_of(el);
    if (!rep.replaced || !rep.has_ratio) return;
    DFAIL("CSS 2.1 §10.4 \"Minimum and maximum widths: 'min-width' and 'max-width'\" has a SECOND algorithm "
          "for exactly this element — a REPLACED element with an INTRINSIC RATIO and both `width` and `height` "
          "specified as `auto`, under a declared limit — and the per-axis clamp is the WRONG one for it: two "
          "independent clamps distort the ratio, and the table is what preserves it. §10.7 delegates to the "
          "same table (\"for replaced elements with both 'width' and 'height' computed as 'auto', use the "
          "algorithm under Minimum and maximum widths above\"), so it resolves BOTH axes in one step. Its "
          "eleven rows are over `w` and `h`, \"the results of the width and height computations IGNORING the "
          "'min-width', 'min-height', 'max-width' and 'max-height' properties\" — which is the tentative pass "
          "this component already runs on each axis — after \"take the max-width and max-height as "
          "max(min, max) so that min <= max holds true\". WHAT REACHED THIS IS AN INTRINSIC RATIO, AND THE "
          "ELEMENT THAT HAS ONE IS A `canvas`: core/layout/replaced_element.c derives HTML §4.12.5 \"The "
          "canvas element\"'s bitmap from the element's own `width` and `height` content attributes, whose "
          "§4.12.5 defaults are 300 and 150, so its ratio is 2 — the first non-degenerate one in this build. "
          "`canvas { max-width: 100% }` with neither size declared is the whole trigger. BUILD the eleven "
          "rows over the two tentative values, and note that the row conditions are a DISJOINT partition — the "
          "four single-violation rows must be tested AFTER the six double-violation ones or a box violating "
          "both takes the wrong one");
}

/* ---- CSS 2.1 §10.3.2's LAST ARM AND §10.6.2's, WHICH ARE ONE RECTANGLE STATED TWICE ------------------------
   §10.3.2 "Inline, replaced elements": "Otherwise, if 'width' has a computed value of 'auto', but none of the
   conditions above are met, then the used value of 'width' becomes 300px. If 300px is TOO WIDE TO FIT THE
   DEVICE, UAs should use the width of the largest rectangle that has a 2:1 ratio and fits the device instead."
   §10.6.2 "Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced
   elements in normal flow and floating replaced elements": "Otherwise, if 'height' has a computed value of
   'auto', but none of the conditions above are met, then the used value of 'height' must be set to the height
   of the largest rectangle that has a 2:1 ratio, has a height not greater than 150px, and has a WIDTH NOT
   GREATER THAN THE DEVICE WIDTH."
   The two sentences describe ONE 2:1 rectangle — 300 by 150 — capped by the device, which is why they are one
   function and not a constant written at each of the two arms.

   THE DEVICE IS THE OUTPUT DISPLAY, NOT THE VIEWPORT, and CSS 2.1 says so in its own vocabulary: §9.1.1 "The
   viewport" defines the viewport as "a window or other viewing area ON THE SCREEN", so the screen is the thing
   the window is on and the two are different rectangles. This engine already answers "the device" from
   core/frame/screen.h at the one other place that asks for it — the `device-width` media feature, whose
   evaluator reads that component and whose own header records that it held a second literal 1920 until it
   existed — and one fact answered from two places is the defect CLAUDE.md §per-realm names.
   Reading it from the VIEWPORT would also be a definition of itself here: core/frame/viewport.c derives a
   child navigable's viewport FROM this number, so a default replaced size that was a function of the viewport
   would be a function of the default replaced size.

   THE ANSWER CARRIES NO ENVIRONMENT FACT, AND THAT IS THE ASSERT RATHER THAN AN OMISSION. css_length.h makes
   `CSS_ENV_NONE` a POSITIVE statement — the value's domain is a single point — and it is one here only because
   the device clause does NOT bind: the modelled display is far larger than the rectangle, so the answer is 300
   by 150 at every point of the domain the model admits and there is no arm a page could take. The moment a
   narrower display is modelled that stops being true, and the DCHECK below fires naming what has to be built:
   the device's own dimensions would then have to become a `CssEnvFact` (core/css/css_length.h) with a row in
   core/frame/viewport.c's seam, and that row's source key must be screen.c's own `screen.width` rather than a
   per-document one, because a Screen member IS its own source and a page reading `screen.width` and a page
   measuring an image would otherwise fork two identities for one fact. */
#define UV_DEFAULT_REPLACED_WIDTH  300.0
#define UV_DEFAULT_REPLACED_HEIGHT 150.0

CssPx used_value_default_replaced_size(bool vertical)
{
    /* The largest 2:1 rectangle that fits a `w` by `h` device is `min(w, 2h)` wide, so the clause binds
       exactly when the display is narrower than 300 or shorter than 150. */
    DCHECK(screen_width() >= UV_DEFAULT_REPLACED_WIDTH && screen_height() >= UV_DEFAULT_REPLACED_HEIGHT,
           "CSS 2.1 §10.3.2's \"if 300px is TOO WIDE TO FIT THE DEVICE\" and §10.6.2's \"a width not greater "
           "than the device width\" have become REACHABLE: core/frame/screen.h now models a display smaller "
           "than the 300 x 150 rectangle, so the default replaced size is the largest 2:1 rectangle that fits "
           "it and is a FUNCTION OF THE DEVICE. Two things follow and neither is arithmetic. The size is no "
           "longer a single-point domain, so it must carry the device's dimensions as environment facts — a "
           "`CssEnvFact` row in core/css/css_length.h and a row in core/frame/viewport.c's seam whose member "
           "name is screen.c's own `screen.width`/`screen.height` and NOT a per-document key, because a Screen "
           "member is its own source (core/frame/screen.c) and two spellings of one fact fork one predicate "
           "twice. And core/frame/viewport.c derives a CHILD NAVIGABLE's viewport from this number, so the "
           "viewport would become a function of the device too. BUILD the fact, then this becomes "
           "`css_px_min` over it");
    return css_px(vertical ? UV_DEFAULT_REPLACED_HEIGHT : UV_DEFAULT_REPLACED_WIDTH);
}

/* ---- the used value ------------------------------------------------------------------------------------- */

/* THE ONE PARAMETER THAT MAKES §10.4 A SECOND PASS RATHER THAN A `min`. Every function below whose name starts
 * `uv_pass_` runs §10.3/§10.6's rules WITH A GIVEN COMPUTED SIZE, handed in as a `CssLength`, and §10.4/§10.7
 * are then literally their own sentence: "the rules above are applied again, but this time USING THE COMPUTED
 * VALUE OF 'max-width' AS THE COMPUTED VALUE FOR 'width'". `uv_sized` is that outer algorithm.
 *
 * WHY THE SIZE IS A PARAMETER AND NOT A READ, WHICH IS THE WHOLE DESIGN. §10.3.3's margin rules BRANCH ON IT:
 * rule 5 ("if 'width' is set to 'auto', any other 'auto' values become '0'") gives an `auto` margin 0, while
 * rules 4 and 6 give it the SLACK. So `margin: 0 auto; max-width: 1200px` on an `auto`-width container — the
 * centred wrapper on most of the web — is centred only if the margin rules see §10.4's SUBSTITUTED width and
 * not the computed `auto`. If each function re-read the property instead, the tentative pass (step 1, run
 * WITHOUT the limits) would have to ask §10.4 for the very answer it is computing, and the recursion would not
 * terminate. Threading it makes both passes well-founded: step 1 runs with the raw computed value, and the
 * substituted pass runs with an absolute length, which takes the declared arm and asks for no margins at all.
 *
 * IT IS ALSO WHY THE SECOND PASS COSTS NOTHING ON THE HEIGHT AXIS. §10.6.3's content-based height is a walk
 * over the whole subtree; the substituted pass never runs it, because the substituted value is a length. So a
 * declared `min-height` on every level of a tree adds one walk in total, not one per level. */
static CssPx uv_margin(lxb_dom_element_t *el, const char *name, const char *opposite, CssLength len, UvBox box,
                       const CssLength *size_len);
static CssPx uv_pass_size(lxb_dom_element_t *el, CssLength len, UvBox box, bool vertical);

/* CSS 2.1 §10.3.3's rules 2, 4 and 6 — the used value of a horizontal `auto` margin on a block-level box in
   normal flow whose `width` is NOT `auto`, which is one computation and not three:
     rule 4, "if there is exactly one value specified as 'auto', its used value follows from the equality" —
       the margin takes all the slack;
     rule 6, "if both 'margin-left' and 'margin-right' are 'auto', their used values are equal" — they split
       it, which is what `margin: 0 auto` means;
     rule 2, "If 'width' is not 'auto' and 'border-left-width' + 'padding-left' + 'width' + 'padding-right' +
       'border-right-width' (plus any of 'margin-left' or 'margin-right' that are not 'auto') is larger than
       the width of the containing block, then any 'auto' values for 'margin-left' or 'margin-right' are, for
       the following rules, treated as zero" — which is exactly the slack being negative. THE FIVE PER-SIDE
       TERMS ARE THE SPEC'S OWN AND WERE COMPRESSED HERE TO "border + padding + width", which read as a
       paraphrase of the equation above and is not one: the equation is over SEVEN terms including both
       margins, and rule 2's antecedent is the same sum with the two margins moved into its parenthetical. The
       clause beneath this list was right either way, so what the compression cost was a reader's ability to
       check it.
   THE SIGN TEST RUNS ON THE EXAMPLE, and that is css_length.h's stated layering rather than a shortcut past
   it: the containing block's width may be the viewport's, so `slack < 0` is a question the environment could
   answer either way, and it is decided here on the modelled viewport exactly as Media Queries §4 decides
   `(max-width: 768px)` on the same number. The fact rides the result either way, so the page's own branch on
   the margin still forks both worlds.
   `used_size` IS THE PASS'S OWN USED WIDTH, not a re-read of the property — see the note above `uv_margin`'s
   declaration. It is the size in the box css-sizing-3 §3.3 exposes, which is why the surround is added only
   under `content-box`: the equation's `inner` is the box's OUTER width and under `border-box` the used value
   already is one. Asking `uv_content_size` for it instead would re-enter §10.4 through `used_value_px` and
   step 1 would ask for its own answer. */
static CssPx uv_block_auto_margin(lxb_dom_element_t *el, const char *name, const char *opposite, UvBox box,
                                  const CssLength *size_len, CssPx used_size)
{
    UvSurround s = uv_surround(el, false);
    CssPx cb = used_value_containing_block_width(el);
    CssPx inner = uv_is_border_box(el) ? used_size : css_px_add(used_size, uv_surround_total(s));
    CssLength ol = css_computed_length(el, opposite);
    bool both = uv_len_is_auto(ol);
    /* The OTHER margin is resolved in THIS pass — `uv_margin` with the same `size_len` — and never through
       `used_value_px`, which would run §10.4's whole algorithm again for a value this pass already fixed. It
       cannot recurse back here: this margin is `auto`, so §10.3.3's over-constrained case (which needs BOTH
       margins non-auto) is false for it and the opposite's non-auto arms answer from its own length. */
    CssPx other = both ? css_px(0.0) : uv_margin(el, opposite, name, ol, box, size_len);
    CssPx slack = css_px_sub(css_px_sub(cb, inner), other);

    if (slack.px < 0.0) return css_px(0.0);              /* rule 2 */
    return both ? css_px_scale(slack, 0.5) : slack;      /* rule 6, then rule 4 */
}

/* `opposite` is the OTHER horizontal margin's property name — `margin-right` when this is `margin-left` and
   the reverse — or NULL when this is a vertical margin, which is also how this function knows which pair it
   is in; `name` is this margin's own and is NULL on the same axis. §10.3.3's over-constraint is a statement
   about the three horizontal values TOGETHER, so the one that is not this margin and is not `width` has to be
   read — and the pair of names is what lets `uv_block_auto_margin` above resolve it inside the same pass.
   `size_len` IS THE `width` §10.3.3's RULES ARE BEING RUN WITH, which on the horizontal axis is §10.4's
   substituted value whenever the clamp bound. Every branch below that asks "is `width` auto" asks it of THIS
   length and never of the property, because after §10.4 substitutes they are different answers and the rules
   are stated over the substituted one. IT IS NULL WHERE NO SECTION READS IT, and that is a POSITIVE statement
   rather than a hole css_length.h would warn about: §10.6.3 gives a vertical `auto` margin the used value 0
   outright, with no reference to the height at all, so there is no size for this axis to be run with and
   passing a plausible one — the margin's own length, the raw computed height — would make an unread parameter
   look like a read one and would cost §10.6.3's whole subtree walk to produce it.
   THE HORIZONTAL AXIS SAYS THE SAME OF THREE BOX TYPES and used not to, which is the defect this parameter's
   own contract was hiding. §10.3.1, §10.3.5 and §10.3.9 state a horizontal `auto` margin as 0 outright and
   have no equation for a non-`auto` one to be over-constrained by, so they read no width either — and
   §10.3.1's box HAS none, because its first sentence is "The 'width' property does not apply." Resolving one
   anyway to fill this parameter ran §10.3.3's constraint equation on exactly that box. `uv_margin_reads_width`
   is the one place that question is answered; this parameter is present exactly when it says yes. */
static CssPx uv_margin(lxb_dom_element_t *el, const char *name, const char *opposite, CssLength len, UvBox box,
                       const CssLength *size_len)
{
    bool vertical = opposite == NULL;

    DCHECK((name == NULL) == (opposite == NULL),
           "a margin was resolved with half a horizontal PAIR. The two property names go together — both NULL "
           "is the vertical axis, both present is the horizontal one — because §10.3.3's over-constrained case "
           "is a statement about those two values TOGETHER, so a caller that filled in one of them has an axis "
           "it has not decided");
    /* AND THE WIDTH IS PRESENT EXACTLY WHERE A SECTION READS IT, which is the half of this contract that used
       to be stated as "all three go together" and was therefore satisfied by a caller resolving a used width
       for a box that has none. Two-sided on purpose: a NULL where a section reads it is a dereference this
       function would make, and a value where none does is §10.3.3's equation run to produce a number nothing
       looks at — which for an inline box is not merely wasted, it is unanswerable. */
    DCHECK((size_len != NULL) == (!vertical && uv_margin_reads_width(box)),
           "a margin was resolved with the pass's `width` present where no section reads it, or absent where "
           "one does. §10.3.3's and §10.3.7's margin rules are stated over a constraint equation with 'width' "
           "in it and the two box types §10 does not own solve one of their own; §10.3.1, §10.3.5 and §10.3.9 "
           "state a horizontal `auto` margin as 0 outright and §10.6.3 does the same on the vertical axis, so "
           "neither has a width to be run with — and §10.3.1's box does not have one at all");
    if (len.kind == CSS_LENGTH_ABSOLUTE) {
        /* §10.3.3's OVER-CONSTRAINED case is the one configuration in CSS 2.1 in which a non-auto margin's
           used value differs from its computed value: a block-level non-replaced box in normal flow whose
           `width` and both horizontal margins are all non-auto cannot satisfy the equation, and the spec
           resolves it by IGNORING one of the two — "if the 'direction' property of the containing block has
           the value 'ltr', the specified value of 'margin-right' is ignored and the value is calculated so as
           to make the equality true. If the value of 'direction' is 'rtl', this happens to 'margin-left'
           instead" — and recomputing it so the equality holds.
           ALL THREE HAVE TO BE NON-AUTO for the box to be over-constrained, which is why the OTHER margin is
           read: "if there is exactly one value specified as 'auto', its used value follows from the equality",
           so `margin-left: 10px; margin-right: auto; width: 100px` is not over-constrained at all and
           `margin-left`'s used value is the 10px it computed to.
           THE `width` HERE MAY BE §10.4's SUBSTITUTED ONE: a declared `max-width` that bound turns an `auto`
           width into a length for the whole second pass, so a box that was not over-constrained on step 1 can
           be over-constrained on step 2 — which is the case the spec's own "the rules above are applied again"
           creates and is why this test reads the pass's length rather than the property.
           THE MARGIN THAT IS *NOT* IGNORED KEEPS ITS COMPUTED VALUE, which is why this arm returns before the
           equality is solved for the other one. The ignored margin's used value is the containing block's
           width minus the six terms the equation's other side names, and every one of those is read HERE
           rather than through `used_value_px` — that entry would re-run §10.4's whole algorithm for values
           this pass has already fixed, and its own §10.3.3 pass would re-enter this test with the two margins'
           roles swapped, which is a recursion with no base case. */
        if (!vertical && box == UV_BOX_BLOCK_FLOW && !uv_len_is_auto(*size_len) &&
            !uv_length_is(el, opposite, "auto")) {
            bool rtl = used_value_containing_block_is_rtl(el);
            bool ignored = strcmp(name, rtl ? "margin-left" : "margin-right") == 0;

            DCHECK(strcmp(name, "margin-left") == 0 || strcmp(name, "margin-right") == 0,
                   "§10.3.3's over-constrained case was reached for a margin that is not one of the two the "
                   "equation names. The horizontal pair is `margin-left` and `margin-right`; the caller's own "
                   "`vertical` test is what separates the axes, so the two have come apart");
            if (ignored) {
                UvSurround s = uv_surround(el, false);
                CssPx used_size = uv_pass_size(el, *size_len, box, false);
                CssPx inner = uv_is_border_box(el) ? used_size : css_px_add(used_size, uv_surround_total(s));
                CssLength ol = css_computed_length(el, opposite);
                CssPx other = ol.kind == CSS_LENGTH_ABSOLUTE
                                  ? ol.px
                                  : css_length_resolve_pct(ol, used_value_containing_block_width(el));

                DCHECK(ol.kind == CSS_LENGTH_ABSOLUTE || ol.kind == CSS_LENGTH_PERCENTAGE ||
                           ol.kind == CSS_LENGTH_CALCULATED,
                       "§10.3.3's over-constrained case read the OPPOSITE margin and found a keyword. The test "
                       "above has already established that it is not `auto`, and CSS 2.1 §8.3's <margin-width> "
                       "grammar admits a length, a percentage and `auto` and nothing else");
                /* NOT FLOORED AT ZERO, and that is §10.3.3's own arithmetic rather than an omission: the
                   sentence is "the value is calculated so as to make the equality true", and a box whose
                   declared width and other margin already exceed the containing block makes that value
                   negative. css-sizing-3 §3.3's floor is stated of the CONTENT box, which this is not. */
                return css_px_sub(css_px_sub(used_value_containing_block_width(el), inner), other);
            }
        }
        return len.px;
    }
    /* CSS 2.1 §8.3: a percentage margin "is calculated with respect to the WIDTH of the generated box's
       containing block. NOTE THAT THIS IS TRUE FOR 'margin-top' AND 'margin-bottom' AS WELL." So the axis is
       not asked: a vertical margin resolves against the same horizontal measure, which is the counter-intuitive
       half of the rule and the reason this arm takes no `vertical`. A math function carrying BOTH terms
       (`margin-left: calc(50% - 1rem)`) resolves against that same basis in one step — css-values-4 §10.11
       "Computed Value" left the percentage in the function and §5.6 "Mixing Percentages and Dimensions" adds
       the two here.
       AND IT IS THE ONE BOX-MODEL LENGTH THAT IS NOT CLAMPED, which is §8.3's own sentence and not an omission:
       "negative values for margin properties are allowed, but there may be implementation-specific limits". So
       css-values-4 §9.1 "Numeric Functions"'s clamp has no range to clamp to on this property, and a
       `calc(1rem - 100%)` that comes out negative IS the used value. */
    if (len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED)
        return css_length_resolve_pct(len, used_value_containing_block_width(el));
    DCHECK(strcmp(len.keyword, "auto") == 0,
           "a margin's computed value is neither a length, nor a percentage, nor `auto` — CSS 2.1 §8.3's "
           "<margin-width> grammar admits exactly those three, and lexbor validates the declaration against "
           "it, so a fourth form here is a serializer that produced something the grammar does not");
    if (vertical) {
        /* §10.6.3: "If 'margin-top', or 'margin-bottom' are 'auto', their used value is 0." */
        if (box == UV_BOX_BLOCK_FLOW) return css_px(0.0);
        DFAIL("a VERTICAL margin computes to `auto` on a box CSS 2.1 §10.6.3 does not cover. §10.6.3 is the "
              "block-level-in-normal-flow section and it is the only one that gives `margin-top: auto` a used "
              "value of 0 outright; §10.6.4 solves the vertical `auto` margins of an ABSOLUTELY POSITIONED box "
              "from its own constraint equation (they centre the box between `top` and `bottom` when both are "
              "given), §10.6.1 says nothing at all about an INLINE box's vertical margins, and css-flexbox §9.6 "
              "gives a flex item's `auto` cross-axis margins the free space. BUILD the section this box's type "
              "names — uv_box_kind above says which it is");
        return css_px(0.0);
    }
    /* THREE SECTIONS STATE THE MARGIN OUTRIGHT and the rest solve an equation instead, which is why the box
       type is what this arm branches on. §10.3.1 "Inline, non-replaced elements" and §10.3.9 "'Inline-block',
       non-replaced elements in normal flow" each say "A computed value of 'auto' for 'margin-left' or
       'margin-right' becomes a used value of '0'", and §10.3.5 "Floating, non-replaced elements" says the same
       thing in its own words — "If 'margin-left', or 'margin-right' are computed as 'auto', their used value
       is '0'" — which is why it is three sections and not one sentence quoted three times;
       §10.3.3 says it too, but only in its rule 5, "if 'width' is set to 'auto', any other 'auto' values
       become '0'" — with a non-auto `width` its `auto` margins take the slack instead. A FOURTH BOX TYPE
       REACHES THE SAME 0 THROUGH §10.3.9 rather than through a fourth sentence: css-grid-1 §5.2 sizes an
       inline-level grid container "as an atomic inline-level box (such as an inline-block)" and css-flexbox-1
       §9.2 sends an inline-level flex container to its formatting context's rules, so §10.3.9 IS their margin
       section — and this is the arm where that delegation pays, because it answers `margin-left: auto` on an
       `inline-flex` with the 0 the spec states while the box's WIDTH is still an unbuilt intrinsic size.
       IT IS `uv_margin_reads_width`'S OWN LIST, negated, and it is spelled that way rather than repeated
       because it IS the same question: a section that states the margin outright is exactly a section with no
       equation to read a width from, so a box type added to one list and not the other would take this arm's
       0 while the caller resolved a width for it, or the reverse. */
    if (!uv_margin_reads_width(box)) return css_px(0.0);
    if (box == UV_BOX_BLOCK_FLOW) {
        /* Rule 5's condition is asked of the PASS's width, so `margin: 0 auto` under a `max-width` that bound
           reaches rules 4 and 6 and centres the box — with the raw computed `auto` it would take rule 5 and
           both margins would be 0, which is the whole of what a centred wrapper looks wrong as. The pass's
           USED width is derived from that same length by the same rules, which for a non-auto length is the
           declared arm and therefore neither a walk nor a recursion. */
        if (uv_len_is_auto(*size_len)) return css_px(0.0);
        return uv_block_auto_margin(el, name, opposite, box, size_len,
                                    uv_pass_size(el, *size_len, box, false));
    }
    if (box == UV_BOX_TABLE) {
        TableBoxKind kind = uv_table_box_kind(el);
        char nbuf[160];

        /* THIS ARM COVERS NINE of CSS 2.1 §17.2 The CSS table model's ten box types and they do not have one
           answer, which is what the single message that stood here got wrong: it named the table wrapper box
           for a `<td>` as readily as for a `<table>`, and §17.5 Visual layout of table contents says of the
           internal boxes that "internal table elements do not have margins" at all.
           THE TENTH IS THE CAPTION AND IT NO LONGER REACHES THIS ARM, because `uv_box_kind` classifies it as
           block-level in normal flow (§17.4 Tables in the visual formatting model: "rendered as normal block
           boxes inside the table wrapper box"). Its `auto` margins take §10.3.3's rules 4 and 6 over the
           block-level arm above, and the SLACK those rules divide is the wrapper's content edge, which
           `used_value_containing_block_width` answers. The arm that stood here for it is deleted with the
           classification that sent it here. */
        if (table_box_kind_is_internal(kind))
            DFAILF("%s: a horizontal `auto` margin on an INTERNAL table box, which CSS 2.1 §17.5 Visual "
                   "layout of table contents says has NO margin to resolve — \"internal table elements do not "
                   "have margins\" — so this is not an unbuilt algorithm but a call that should not have been "
                   "made: CSSOM §9 Resolved Values' first conjunct is that the property APPLIES to the "
                   "element, and core/css/css_property_applies.c decides that same sentence. The two have "
                   "come apart, and the used value §10.3.3 would compute here is a share of a slack this box "
                   "has no margin to take",
                   box_subject(el, nbuf, sizeof nbuf));
        DCHECK(kind != TABLE_BOX_CAPTION,
               "a CAPTION box reached CSS 2.1 §17.5's `auto`-margin refusals. §17.4 Tables in the visual "
               "formatting model gives it its own margin areas and renders it as a normal block box in the "
               "wrapper, so §10.3.3's rules 4 and 6 divide the wrapper's slack for it and `uv_box_kind` sends "
               "it to the block-level arm above. A caption here is that classification having come apart");
        /* A TABLE BOX'S `auto` MARGIN IS THE WRAPPER'S, AND IT IS ANSWERED HERE RATHER THAN REFUSED — CSS 2.1
           §17.5.2 Table width algorithms: the 'table-layout' property says so in its own last sentence.
           §17.5.2's second paragraph is the handover ("Note that this section overrides the rules that apply
           to calculating widths as described in section 10.3"), and its handback names THIS case as the
           worked example: "However, once the calculated value of 'width' for the table is found (using the
           algorithms given below or, when appropriate, some other UA dependent algorithm) then the other
           parts of section 10.3 do apply. Therefore a table can be centered using left and right 'auto'
           margins, for instance."
           SO §10.3.3's RULES RUN, AND THEY RUN OVER THE WRAPPER, WHICH IS WHY THIS IS THE BLOCK-LEVEL ARM'S
           OWN CODE AND NOT A SECOND EQUATION. §17.4 Tables in the visual formatting model uses `margin-*` on
           the wrapper and every other non-inheritable value on the table box, and gives the wrapper's width as
           "the border-edge width of the table box inside it" — so the equation's `width` term IS the table
           box's border box, which is exactly the `inner` `uv_block_auto_margin` derives from the used size it
           is handed. The wrapper has no border and no padding of its own to add (§17.4's "the initial values
           are used instead"), so the two boxes' equations have the identical seven terms and there is nothing
           here to write a second time.
           RULE 5 IS THE ONE THING THE BLOCK ARM DOES THAT THIS MUST NOT. "If 'width' is set to 'auto', any
           other 'auto' values become '0'" is stated over a width §10.3 SOLVES, and §17.5.2 has already found
           this one — its own sentence above is that a table with `width: auto` "will not automatically size
           to fill its containing block" and can still be centred. Guarding on the declared length here would
           answer 0 for `table { margin: 0 auto }`, which is the single most common table declaration on the
           web. */
        {
            char *d = uv_computed(el, "display");
            bool block_level = table_wrapper_is_block_level(d);

            free(d);
            /* §17.4: "The table wrapper box is a 'block' box if the table is block-level, and an
               'inline-block' box if the table is inline-level." An `inline-table`'s wrapper is therefore
               §10.3.9 "'Inline-block', non-replaced elements in normal flow"'s box, whose own sentence is "A
               computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of '0'" — no
               equation, no slack, and nothing of §10.3.3 to run. The two halves of §17.4's one sentence are
               two different sections here, which is why the predicate is asked and not assumed. */
            if (!block_level) return css_px(0.0);
            return uv_block_auto_margin(el, name, opposite, box, size_len,
                                        uv_pass_size(el, *size_len, box, false));
        }
    }
    if (box == UV_BOX_ITEM)
        DFAIL("a horizontal `auto` margin on a FLEX or GRID ITEM, which css-flexbox §9.5 answers before "
              "alignment does: 'if the remaining free space is positive and at least one main-axis auto margin "
              "is on the line, distribute it equally among those margins'. It is the container's FREE SPACE "
              "and not §10.3.3's slack — the two differ because the container has already flexed every item. "
              "BUILD the flex layout algorithm over the container's own used content size");
    DFAIL("a HORIZONTAL margin computes to `auto` on an ABSOLUTELY POSITIONED box, whose used value CSS 2.1 "
          "§10.3.7 solves from its own constraint equation — 'left + margin-left + border-left-width + "
          "padding-left + width + padding-right + border-right-width + margin-right + right = width of "
          "containing block' — under the section's own "
          "ordered rules: an `auto` `left` or `right` replaces an `auto` margin with 0 first, both `auto` "
          "margins then get EQUAL values 'unless this would make them negative', and an over-constrained set "
          "ignores one offset depending on the containing block's `direction`. Three things are missing and "
          "none of them is the containing block's WIDTH, which §10.1's chain answers now, and none is "
          "`direction`, which core/css/css_computed_value.c models now: the used `left` and `right` (CSSOM §9's "
          "inset arm crashes on the same equation), and the STATIC POSITION its `auto` cases fall back to. "
          "§9.4.1's NORMAL FLOW IS BUILT (core/layout/flow_position.h) and it places every IN-FLOW block-level "
          "box; what no part of it produces is a position for an OUT-OF-FLOW one, because §10.6.3 tells "
          "core/layout/block_flow.c's walk to skip exactly those children and it does. EXTEND that walk to "
          "report where a skipped child WOULD have been placed, then §10.3.7 over it");
    return css_px(0.0);
}

/* CSS 2.1 §8.4: a percentage padding "is calculated with respect to the WIDTH of the generated box's
   containing block, EVEN FOR 'padding-top' and 'padding-bottom'" — so, as for a margin, the axis is not asked.
   §8.4's other sentence is the one this component cannot yet be asked to break: "if the containing block's
   width depends on this element, then the resulting layout is undefined in CSS 2.1". A SHRINK-TO-FIT width IS
   such a containing block — §10.3.5's two intrinsic terms are functions of the box's own content — so the
   sentence is now live rather than vacuous, and what keeps it unreachable is a property of the walk that
   produces one: core/layout/intrinsic_size.c crashes for EVERY element child, so a box whose shrink-to-fit
   width exists at all has no element descendant, and there is therefore no element for it to be the containing
   block of. The day §5.2's contributions make an element child measurable, this becomes reachable in the same
   diff, and §8.4's "undefined" is what it has to answer with — a real arm to build, not a case that vanished. */
static CssPx uv_padding(lxb_dom_element_t *el, CssLength len)
{
    CssPx used;

    /* §8.4's THIRD shape, and it is the same sentence as the second: css-values-4 §10.11 "Computed Value"
       leaves a math function's percentage term unresolved at computed-value time, so `padding: calc(1rem + 2%)`
       arrives as a length AND a percentage and resolves against §8.4's one basis in one step. */
    if (len.kind == CSS_LENGTH_ABSOLUTE) {
        used = len.px;
    }
    else {
        DCHECK(len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED,
               "a padding's computed value is neither a length, a percentage, nor a math function carrying "
               "both. CSS 2.1 §8.4's <padding-width> grammar has no `auto` and no keyword at all — a padding "
               "is a length or a percentage and nothing else — so this is a value lexbor's own validation "
               "should have dropped");
        used = css_length_resolve_pct(len, used_value_containing_block_width(el));
    }
    /* css-values-4 §9.1 "Numeric Functions"'s CLAMP, at §9.1's own "at USED VALUE TIME otherwise" — this is the
       first point at which the percentage has a basis, so it is the first point at which there is a number to
       range-check. CSS 2.1 §8.4 states the range in words: "unlike margin properties, values for padding
       values cannot be negative", and §5.1 "Range Restrictions and Range Definition Notation" makes a negative
       LITERAL a dropped declaration — so a negative here is a math function's top-level result and §9.1 says
       outright that one "never cause[s] a declaration to become invalid". `css_px_max` and not an `if`, so the
       clamped-away operand's environment facts stay in the domain: `calc(2% - 1rem)` is a function of the
       containing block AND of the reader's default font size at the viewports where it floors as well as at
       the ones where it does not. */
    return css_px_max(used, css_px(0.0));
}

/* CSS 2.1 §10.3.3's RULE 5 — "if 'width' is set to 'auto', any other 'auto' values become '0' and 'width'
   follows from the resulting equality", which is the constraint equation
       margin-left + border-left-width + padding-left + width + padding-right + border-right-width +
       margin-right = width of containing block
   solved for the one unknown. Every other term is read back through this component's own arms, so the `auto`
   margins rule 5 zeroes are zeroed by uv_margin and not a second time here.
   THE FLOOR AT ZERO IS css-sizing-3 §3.3's AND NOT §10.4's, which is the correction the clamp forced. §3.3
   states it of the CONTENT box outright — "as the content width and height cannot be negative, this
   computation is floored at zero" — and it belongs here because the equation can produce a negative content
   width from margins and borders alone, with no limit declared at all. §10.4's step 3 is a SEPARATE pass and
   is `uv_sized`'s: it re-runs these rules with `min-width` substituted, which for a non-auto substitution is
   the declared arm and not this equation. Writing the floor as "§10.4's min-width: 0 running early" was true
   of the number and wrong about which section owns it, and it made the real step 3 look already done.
   AND THE ANSWER IS THE BORDER BOX under `box-sizing: border-box`, because css-sizing-3 §3.3 says the used
   value "as exposed for instance through getComputedStyle()" refers to that box. The equation solves for the
   CONTENT width either way — its seven terms are CSS 2.1's, which knows only the content box — so §3.3's
   conversion is applied to the result rather than to the equation.
   THE MARGINS ARE RESOLVED IN THIS PASS, with `size_len` — which on this arm is `auto`, so §10.3.3's rule 5
   zeroes any that are. Reading them through `used_value_px` would run §10.4's whole algorithm to answer a
   question this pass has already fixed, and step 1 would be asking for its own result. */
static CssPx uv_block_auto_width(lxb_dom_element_t *el, CssLength size_len, UvBox box)
{
    UvSurround s;
    CssPx cb;
    CssPx margins, content;

    DCHECK(uv_len_is_auto(size_len),
           "§10.3.3's rule 5 was reached with a `width` that is not `auto`. The rule's own condition is \"if "
           "'width' is set to 'auto'\", and after §10.4 substitutes a limit the pass's width is a LENGTH and "
           "the declared arm is what runs — so a non-auto length here is `uv_pass_size`'s dispatch and this "
           "equation having come apart");
    s = uv_surround(el, false);
    cb = used_value_containing_block_width(el);
    margins = css_px_add(uv_margin(el, "margin-left", "margin-right",
                                   css_computed_length(el, "margin-left"), box, &size_len),
                         uv_margin(el, "margin-right", "margin-left",
                                   css_computed_length(el, "margin-right"), box, &size_len));
    content = css_px_max(css_px_sub(css_px_sub(cb, uv_surround_total(s)), margins), css_px(0.0));
    if (!uv_is_border_box(el)) return content;
    return css_px_add(content, uv_surround_total(s));
}

/* CSS 2.2 §10.3.5 "Floating, non-replaced elements"'s SHRINK-TO-FIT WIDTH — the section's own formula, over its
 * own three terms:
 *     "Thirdly, find the AVAILABLE WIDTH: in this case, this is the width of the containing block minus the
 *      used values of 'margin-left', 'border-left-width', 'padding-left', 'padding-right',
 *      'border-right-width', 'margin-right', and the widths of any relevant scroll bars. Then the shrink-to-fit
 *      width is: min(max(preferred minimum width, available width), preferred width)."
 * §10.3.9 "'Inline-block', non-replaced elements in normal flow" is the same algorithm by reference and not a
 * second one — "if 'width' is 'auto', the used value is the shrink-to-fit width AS FOR FLOATING ELEMENTS" — so
 * one function serves both box types and the `auto` margin rule they also share is already `uv_margin`'s.
 *
 * TWO OF THE THREE TERMS ARE css-sizing-3's, UNDER ITS NAMES, and §2.1 "Auto Box Sizes" is what pins the
 * vocabularies together by name rather than by resemblance: the "preferred width" IS the max-content inline
 * size ("this is called the 'preferred width' in CSS2.1§10.3.5") and the "preferred minimum width" IS the
 * min-content inline size ("this is called the 'preferred minimum width' in CSS2.1§10.3.5"). The AVAILABLE
 * WIDTH is §2.1's stretch-fit inline size, and §2.1 says that too — "for the inline axis, this is called the
 * 'available width' in CSS2.1§10.3.5 and computed by the rules in CSS2.1§10.3.3" — which is why the subtraction
 * below is the same six-term one `uv_block_auto_width` performs and not a second spelling of it.
 *
 * THE SCROLL BAR TERM IS ZERO AND THAT IS A DERIVATION rather than an omitted operand: this user agent renders
 * no scroll bar, which core/dom/element_view.h states as a UA CHOICE the model makes and which every CSSOM VIEW
 * member there already answers under. Writing the term as a literal zero here would be one component deciding
 * that again; it is absent because there is nothing to subtract.
 *
 * WHY THE FORMULA IS WRITTEN IN CSS 2.2's ORDER AND NOT css-sizing-3 §2.1's `clamp`. §2.1 states the same
 * result as `clamp(min-content size, stretch-fit size, max-content size)`, i.e. `max(min-content, min(max-
 * content, stretch-fit))`, and the two are the SAME FUNCTION only while `min-content <= max-content` — which
 * core/layout/text_run.c asserts at the read for exactly this reason. Under a violated relation they differ:
 * CSS 2.2's spelling would return the min-content size and §2.1's the max-content one. Taking the section's own
 * order keeps the code checkable against the sentence quoted above, and the assert one level down keeps the
 * other spelling from being a different algorithm behind an equals sign.
 *
 * THE ANSWER IS THE CONTENT WIDTH, and css-sizing-3 §3.3's `box-sizing` conversion is applied to the RESULT
 * exactly as it is to §10.3.3's equation above: every term §10.3.5 names is CSS 2.2's, and CSS 2.2 knows only
 * the content box. */
static CssPx uv_shrink_to_fit_width(lxb_dom_element_t *el, CssLength size_len, UvBox box)
{
    UvSurround s = uv_surround(el, false);
    IntrinsicInlineSizes in = intrinsic_inline_sizes(el);
    CssPx margins, available, content;

    DCHECK(box == UV_BOX_FLOAT || box == UV_BOX_INLINE_BLOCK,
           "the shrink-to-fit width was computed for a box neither CSS 2.2 §10.3.5 nor §10.3.9 covers. §10.3.7 "
           "sends an absolutely positioned box's `auto` width here too, in its rules 1 and 3, but with a "
           "DIFFERENT available width — the containing block's minus `left` and `right` as well as the six "
           "terms below — so it is not this function with a different box type");
    DCHECK(uv_len_is_auto(size_len),
           "§10.3.5's shrink-to-fit was reached with a `width` that is not `auto`. The section's own condition "
           "is \"if 'width' is computed as 'auto'\", and after §10.4 substitutes a limit the pass's width is a "
           "LENGTH and the declared arm is what runs — so a non-auto length here is `uv_pass_size`'s dispatch "
           "and this formula having come apart");
    /* §10.3.5's first sentence, resolved in THIS pass for the reason `uv_block_auto_width` gives: reading the
       margins through `used_value_px` would re-enter §10.4's whole algorithm for a value this pass has already
       fixed. For these two box types `uv_margin` answers 0 for an `auto` margin outright — §10.3.5's "If
       'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'" and §10.3.9's "A
       computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of '0'" — so it cannot
       recurse back into this width.
       AND THAT IS WHY NO WIDTH IS HANDED OVER. Those two sentences are the WHOLE of what either section says
       about a margin: neither states a constraint equation, so neither has a `width` term for a margin rule to
       read, which is exactly what `uv_margin_reads_width` answers `false` for these two box types. Passing the
       pass's length anyway is the state `uv_margin`'s own two-sided DCHECK names — "a value where none does is
       §10.3.3's equation run to produce a number nothing looks at" — and it was this call site, not the check,
       that was wrong: the parameter's contract was narrowed to the sections that READ a width, `used_value_px`'s
       margin arm was moved onto `uv_margin_reads_width` with it, and this caller kept handing one over. The
       width still reaches `uv_margin` from `uv_block_auto_width`, whose §10.3.3 box does read it. */
    margins = css_px_add(uv_margin(el, "margin-left", "margin-right",
                                   css_computed_length(el, "margin-left"), box, NULL),
                         uv_margin(el, "margin-right", "margin-left",
                                   css_computed_length(el, "margin-right"), box, NULL));
    available = css_px_sub(css_px_sub(used_value_containing_block_width(el), uv_surround_total(s)), margins);
    content = css_px_min(css_px_max(in.min_content, available), in.max_content);
    /* THE RESULT NEEDS NO FLOOR, and that is the formula's own arithmetic rather than a clamp left out:
       `max(min-content, available)` is at least the min-content size, which core/layout/intrinsic_size.c
       asserts non-negative, and `min` with the max-content size cannot take it below that same non-negative
       floor. css-sizing-3 §3.3's "the content width and height cannot be negative" is therefore already true
       here, where §10.3.3's equation genuinely can produce a negative and does need it. */
    DCHECK(content.px >= 0.0,
           "CSS 2.2 §10.3.5's shrink-to-fit width came out NEGATIVE, which its own three terms cannot produce: "
           "`max(preferred minimum width, available width)` is at least the preferred minimum width and both "
           "intrinsic sizes are non-negative, so this is one of the operands having been derived rather than "
           "measured");
    if (!uv_is_border_box(el)) return content;
    return css_px_add(content, uv_surround_total(s));
}

/* CSS 2.1 §10.3.2 "Inline, replaced elements", ITS FIVE ARMS IN THE SPEC'S OWN ORDER — reached only with a
   computed `width` of `auto`, which is the condition every one of them carries and the caller has established.
   THE ORDER IS LOAD-BEARING AND THE ARMS ARE NOT INDEPENDENT TESTS. Arm 2's second disjunct ("'width' has a
   computed value of 'auto', 'height' has some other computed value, and the element does have an intrinsic
   ratio") can be true at the same time as arm 4's condition, and it comes first: a replaced element with an
   intrinsic width of 50, an intrinsic ratio of 2 and a declared `height: 100px` is 200 wide and not 50.
   THE ANSWER IS THE CONTENT WIDTH. Every term §10.3.2 names is CSS 2.1's, and CSS 2.1 knows only the content
   box; css-sizing-3 §3.3's `box-sizing` conversion is applied to the RESULT by the caller, exactly as it is to
   §10.3.3's equation. Which is also why arm 2's "(used height)" is read through `uv_content_size` and not
   through `used_value_px` — under `border-box` the latter is the BORDER box (css-sizing-3 §3.3's exposed used value), and
   multiplying that by an aspect ratio would be a ratio of two different boxes. */
static CssPx uv_replaced_width(lxb_dom_element_t *el, const ReplacedElement *rep)
{
    bool h_auto = uv_length_is(el, "height", "auto");

    /* "If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic
       width, then that intrinsic width is the used value of 'width'." */
    if (h_auto && rep->has_width) return rep->width;
    /* "If 'height' and 'width' both have computed values of 'auto' and the element has no intrinsic width, but
       does have an intrinsic height and intrinsic ratio; or if 'width' has a computed value of 'auto',
       'height' has some other computed value, and the element does have an intrinsic ratio; then the used
       value of 'width' is: (used height) * (intrinsic ratio)."
       THIS DOES NOT RECURSE, and the reason is the spec's conditions rather than a guard here: §10.6.2's own
       ratio arm — the one that would ask back for the used width — runs only when its FIRST arm did not, and
       that first arm takes every case in which both sizes are `auto` and there is an intrinsic height. The
       both-auto disjunct here REQUIRES an intrinsic height, so it is precisely the case §10.6.2 answers
       without asking; and the other disjunct has a non-auto `height`, which §10.6.2 never reaches at all. */
    if ((h_auto && !rep->has_width && rep->has_height && rep->has_ratio) || (!h_auto && rep->has_ratio)) {
        DCHECK(!h_auto || rep->has_height,
               "CSS 2.1 §10.3.2's intrinsic-ratio arm was reached with both sizes `auto` and NO intrinsic "
               "height, which is the one shape that would make it and §10.6.2's ratio arm ask each other for "
               "the answer forever — the arm's own condition names the intrinsic height, so reaching here "
               "without one is that condition and this test having come apart");
        return css_px_scale(uv_content_size(el, true, uv_surround(el, true)), rep->ratio);
    }
    /* "If 'height' and 'width' both have computed values of 'auto' and the element has an intrinsic ratio but
       no intrinsic height or width, then the used value of 'width' is UNDEFINED IN CSS 2.1." */
    if (h_auto && rep->has_ratio)
        DFAIL("CSS 2.1 §10.3.2 \"Inline, replaced elements\" reached the one arm it declines to define: both "
              "`width` and `height` compute to `auto` and the element has an INTRINSIC RATIO but neither an "
              "intrinsic width nor an intrinsic height — a scalable SVG image is the object css-images-3 §4.1 "
              "\"Object-Sizing Terminology\" names for this. The spec's words are \"the used value of 'width' "
              "is undefined in CSS 2.1\", followed by a SUGGESTION and not a rule: \"however, it is suggested "
              "that, if the containing block's width does not itself depend on the replaced element's width, "
              "then the used value of 'width' is calculated from the constraint equation used for block-level, "
              "non-replaced elements in normal flow\". That equation is `uv_block_auto_width` above and it "
              "would run today. It is NOT written here because nothing in this build can produce an object "
              "with a ratio and no sizes — there is no image decoder and no SVG document sizing — so the arm "
              "would be a choice among several UAs' answers with no way to exercise it, which is the plausible "
              "datum CLAUDE.md refuses. BUILD the object that has this shape first (an SVG image with a "
              "`viewBox` and no `width`/`height`), then write the suggestion beside a test that fires");
    /* "Otherwise, if 'width' has a computed value of 'auto', and the element has an intrinsic width, then that
       intrinsic width is the used value of 'width'." */
    if (rep->has_width) return rep->width;
    /* "Otherwise … the used value of 'width' becomes 300px." */
    return used_value_default_replaced_size(false);
}

/* CSS 2.1 §10.6.2's FOUR ARMS, in its order and reached only with a computed `height` of `auto`. The section's
   title names four box types — "Inline replaced elements, block-level replaced elements in normal flow,
   'inline-block' replaced elements in normal flow and floating replaced elements" — and §10.6.5 "Absolutely
   positioned, replaced elements" adds the fifth by reference ("the used value of 'height' is determined as for
   inline replaced elements"), so every replaced box this engine can classify is covered by this one function.
   ITS SECOND ARM READS THE USED WIDTH AS A CONTENT EXTENT, for the reason `uv_replaced_width` states. */
static CssPx uv_replaced_height(lxb_dom_element_t *el, const ReplacedElement *rep)
{
    bool w_auto = uv_length_is(el, "width", "auto");

    /* "If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic
       height, then that intrinsic height is the used value of 'height'." */
    if (w_auto && rep->has_height) return rep->height;
    /* "Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic ratio then the
       used value of 'height' is: (used width) / (intrinsic ratio)." */
    if (rep->has_ratio) {
        DCHECK(rep->ratio > 0.0,
               "CSS 2.1 §10.6.2 divides the used width by an INTRINSIC RATIO that is not positive. "
               "css-images-3 §4.1 states that an object with a degenerate ratio — \"at least one part being "
               "zero or infinity\" — is treated as having NO natural aspect ratio, so a ratio that reached a "
               "division is one core/layout/replaced_element.c's own test let through");
        return css_px_scale(uv_content_size(el, false, uv_surround(el, false)), 1.0 / rep->ratio);
    }
    /* "Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic height, then
       that intrinsic height is the used value of 'height'." */
    if (rep->has_height) return rep->height;
    /* "Otherwise … the height of the largest rectangle that has a 2:1 ratio, has a height not greater than
       150px, and has a width not greater than the device width." */
    return used_value_default_replaced_size(true);
}

/* THE REPLACED-ELEMENT SIZE, WHICH IS ONE ALGORITHM FOR FIVE BOX TYPES — the shape §10.3 does not have anywhere
   else. §10.3.4 "Block-level, replaced elements in normal flow" ("the used value of 'width' is determined as
   for inline replaced elements. Then the rules for non-replaced block-level elements are applied to determine
   the MARGINS"), §10.3.6 "Floating, replaced elements", §10.3.8 "Absolutely positioned, replaced elements" and
   §10.3.10 "'Inline-block', replaced elements in normal flow" ("exactly as inline replaced elements") every one
   of them delegates the SIZE to §10.3.2 and keeps only its own MARGIN rules — which is why the box type is not
   a branch here and is still the branch in `uv_margin`. Two box types genuinely disagree and crash below.
   §10.4/§10.7's CLAMP IS `uv_sized`'s AND NOT THIS FUNCTION'S, and this is one of the two arms where that
   split has a consequence rather than being tidiness: §10.4's SECOND algorithm — the constraint-violation
   table, "for replaced elements with an INTRINSIC RATIO and both 'width' and 'height' specified as 'auto'" —
   replaces the ordinary three steps for exactly the subject this function computes. Its antecedent is tested
   in `uv_require_no_ratio_table`, one level up, because it is a fact about BOTH axes at once and this function
   sees one; a `canvas` under a declared limit satisfies it and aborts there, for the reason stated there. */
static CssPx uv_replaced_size(lxb_dom_element_t *el, const ReplacedElement *rep, UvBox box, bool vertical)
{
    CssPx content;

    if (box == UV_BOX_TABLE)
        DFAIL("a REPLACED element whose computed `display` makes it a TABLE BOX. CSS 2.1 §17.5 owns a table's "
              "width and height and §10.3.2's intrinsic-dimension arms do not apply to it — §17.5.2's two "
              "algorithms derive the width from the COLUMNS, and a replaced element has none. §17.5.2 IS "
              "BUILT AND IS NOT WHAT THIS IS WAITING ON, which is what an earlier form of this line said: "
              "§17.2.1 Anonymous table objects' first two stages are core/layout/table_box.h's, its grid is "
              "core/layout/table_grid.h's, and both of §17.5.2's algorithms run over that grid in "
              "core/layout/table_width.h — `uv_table_used_width` above reaches them for every NON-replaced "
              "table box. WHAT NEITHER ALGORITHM HAS A RULE FOR is this element: §17.5.2.2 Automatic table "
              "layout's four steps walk the CELLS of the grid and a replaced table box has no cell to walk, "
              "while §17.5.2.1 Fixed table layout divides a declared width over columns that do not exist. So "
              "what a table box "
              "does with a replaced "
              "element's own natural dimensions is their question to answer and not this section's. CSS 2.1 §17.2 "
              "The CSS table model is the sentence that puts this element here at all: \"Replaced elements with these "
              "'display' values are treated as their given display types during layout\"");
    if (box == UV_BOX_ITEM)
        DFAIL("a REPLACED element that is a FLEX or GRID ITEM, whose used main and cross sizes come from its "
              "container's algorithm. css-flexbox §9.7 makes a declared size the FLEX BASE SIZE and then "
              "flexes it, and for an `auto` one §9.2 makes the item's natural size its content contribution — "
              "so §10.3.2's arms are an INPUT to that algorithm rather than the answer, and returning one here "
              "would report an unflexed size as the used value. BUILD the flex layout over the container's own "
              "used content size");
    content = vertical ? uv_replaced_height(el, rep) : uv_replaced_width(el, rep);
    DCHECK(content.px >= 0.0,
           "CSS 2.1 §10.3.2 or §10.6.2 produced a NEGATIVE used size for a replaced element. Every arm of both "
           "is either a natural dimension (core/layout/replaced_element.c asserts those non-negative at their "
           "origin), a product of a non-negative extent with a positive ratio, or the 300 x 150 default — so a "
           "negative here is a derivation that lost an operand");
    /* css-sizing-3 §3.3's conversion, applied to the RESULT — the arms above solve for the CONTENT box, which
       is the only box CSS 2.1 knows, and css-sizing-3 §3.3 makes the used value "as exposed for instance
       through getComputedStyle()" the border box's. Identical to `uv_block_auto_width`'s last two lines and stated
       here rather than shared with them because the two solve different equations for the same box. */
    if (!uv_is_border_box(el)) return content;
    return css_px_add(content, uv_surround_total(uv_surround(el, vertical)));
}

/* §10.3 AND §10.6, RUN WITH `len` AS THE COMPUTED SIZE — "Calculating widths and margins" and "Calculating
   heights and margins", which is exactly the unit §10.4 and §10.7 re-run. It reads no size property: `len` is
   the pass's, and after §10.4 substitutes a limit the two are different values. */
static CssPx uv_pass_size(lxb_dom_element_t *el, CssLength len, UvBox box, bool vertical)
{
    /* THE BASIS A PERCENTAGE SIZE RESOLVES AGAINST, AND WHETHER THERE IS ONE — the one difference between the
       two axes, and the reason a percentage `height` reaches this function at all rather than having been
       decided one stage earlier. CSS 2.1 §10.2 makes the horizontal basis the containing block's WIDTH, which
       §10.1's chain always answers. §10.5 makes the vertical one its HEIGHT, which exists only when that
       height is definite — and when it is not, css-sizing-3 §3.2.1 "“Behaving as auto”" says the property
       BEHAVES AS AUTO, so the percentage falls through to the `auto` arms below rather than resolving. */
    CssPx basis = css_px(0.0);
    bool pct = len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED;
    bool resolves = false;

    /* A TABLE CELL'S USED WIDTH IS ANSWERED BEFORE `len` IS EVEN LOOKED AT, and the position is the whole
       point rather than an optimisation. §17.5.2's second paragraph overrides §10.3 for everything inside a
       table — "Note that this section overrides the rules that apply to calculating widths as described in
       section 10.3" — and §17.5 Visual layout of table contents then makes a cell's width a fact about the
       GRID: it is the used width of the columns its rectangle covers, whatever `width` says. §17.5.2.2
       Automatic table layout's step 1 is the only place the declaration is read at all and it reads it as a
       FLOOR ("If the specified 'width' (W) of the cell is greater than MCW, W is the minimum cell width"),
       inside the algorithm `uv_table_cell_used_width` runs.
       SO A PERCENTAGE ON A CELL MUST NOT REACH THE BASIS BELOW. §10.1's containing block for a cell is a
       rectangle no element's content edge is, so resolving one would run a walk that crashes to produce a
       number no rule consults — and `width: 50%` and `width: 50px` on one `<td>` would then answer through
       two different mechanisms, one of them an abort. The DECLARED and `auto` arms further down are where the
       TABLE BOX's own two routes live, because a table's declared `width` IS an operand of §17.5.2's final
       comparison and its percentage does resolve. */
    if (!vertical && box == UV_BOX_TABLE && uv_table_box_kind(el) == TABLE_BOX_CELL)
        return uv_table_cell_used_width(el);
    /* AND THE WHOLE BLOCK AXIS OF A TABLE BOX IS ANSWERED HERE, WHICH IS ONE ROUTE WHERE THE INLINE AXIS NEEDS
       THREE. CSS 2.1 §17.5.3 Table height algorithms admits the declaration and the `auto` value into the SAME
       comparison — "A value of 'auto' means that the height is the sum of the row heights plus any cell spacing
       or borders. Any other value is treated as a minimum height" — so a declared height is an INPUT to that
       section and never the used value, for the table box, for a row ("the maximum of the row's computed
       'height', …") and for a cell alike ("The table cell's 'height' property can influence the height of the
       row (see above), but it does not increase the height of the cell box"). There is therefore nothing for a
       later declared-height arm to do with any of the three, and the arms that used to crash for them below are
       gone rather than left standing behind this one.
       SO A PERCENTAGE HEIGHT ON ANY OF THEM MUST NOT REACH THE BASIS BELOW, exactly as a cell's percentage
       width must not: §17.5.3 declines the case in its own words for the two internal boxes ("CSS 2.1 does not
       define how the height of table cells and table rows is calculated when their height is specified using
       percentage values"), and core/layout/table_height.c takes that decline as a recorded choice inside the
       algorithm — where a resolution here would have run §10.1's walk to produce a number no rule consults. The
       TABLE BOX's own percentage is the one case that is neither: it is a real minimum §17.5.3 would use, and
       `th_declared_minimum` crashes for it — naming a BASIS this file does not export, not a box nothing can
       name. §17.4 Tables in the visual formatting model states the rectangle in one sentence: "Percentages on
       'width' and 'height' on the table are relative to the table wrapper box's containing block, not the table
       wrapper box itself", which is the ordinary ancestor box §10.1's walk already answers for a table element.
       WHAT THIS LINE USED TO SAY, AND WHY IT IS RECORDED RATHER THAN DELETED: that the wrapper was an ANONYMOUS
       box §10.5 would have to resolve against. It is not — §17.4 calls it the table's principal block box — and
       even for an anonymous one CSS 2 §9.2.1.1 Anonymous block boxes orders the resolution past it. A reader
       who re-derives "the wrapper is anonymous" from §17.4's own silence about an element naming it would
       re-introduce the whole reading, so the refutation stays beside the rule. What is genuinely missing is the
       HEIGHT twin of `used_value_containing_block_width`: this file's own `uv_cb_height` computes it over the
       same `uv_cb` walk and only the width half of that pair is exported. */
    if (vertical && box == UV_BOX_TABLE) {
        TableBoxKind kind = uv_table_box_kind(el);

        if (table_box_kind_generates_table_box(kind)) return uv_table_used_height(el);
        if (kind == TABLE_BOX_CELL) return uv_table_cell_used_height(el);
        if (kind == TABLE_BOX_ROW) return uv_table_row_used_height(el);
        uv_table_non_cell_height_fail(el, kind);
    }
    if (pct && !vertical) {
        basis = used_value_containing_block_width(el);
        resolves = true;
    } else if (pct) {
        resolves = uv_cb_height(el, &basis);
    }
    /* CSS 2.1 §10.2: a percentage `width` "is calculated with respect to the width of the generated box's
       containing block", which §10.1 answers — and past that resolution it is a declared length like any
       other, which is why the two arms join here rather than each carrying its own copy of css-sizing-3 §3.3's
       `box-sizing` conversion. §10.2's other sentence, "if the containing block's width depends on this
       element's width, then the resulting layout is undefined in CSS 2.1", is unreachable for the reason
       uv_padding states. A percentage HEIGHT joins the SAME arm when it resolves, which is what makes
       `height: 100%` inside a sized container a declared length here and not a second algorithm. */
    if (len.kind == CSS_LENGTH_ABSOLUTE || resolves) {
        /* `width: calc(100% - 2rem)` joins §10.2's percentage arm and not the length one, because css-values-4
           §10.11 "Computed Value" left the percentage term in the function and §5.6 "Mixing Percentages and
           Dimensions" resolves BOTH terms against §10.2's one basis here — "width: calc(500px + 50%); is
           allowed — both values are converted to absolute lengths and added". It joins the SAME branch as a
           bare percentage rather than getting one of its own, so the table-box and flex-item crashes below run
           for it too; §10.11's own last paragraph is why that matters on a table ("math expressions mixing both
           percentages and non-zero lengths for widths and heights on table columns … MUST be treated as if auto
           had been specified"), and a §17.5 that answered without knowing would get that rule wrong silently.
           THE CLAMP IS css-values-4 §9.1 "Numeric Functions"'s, at its own "at USED VALUE TIME otherwise" — a
           percentage has no number to range-check before this point. CSS 2.1 §10.2 states the range in words
           ("negative values for 'width' are illegal") and §5.1 "Range Restrictions and Range Definition
           Notation" makes a negative LITERAL a dropped declaration, so a negative reaching here is a math
           function's top-level result, which §9.1 says "never cause[s] a declaration to become invalid" and
           clamps instead. That is exactly `width: calc(50% - 100px)` on a narrow viewport, which is the single
           most common thing calc() is written for. `css_px_max` keeps the clamped-away operand's environment
           facts in the domain, so the wide-viewport arm where the subtraction is positive survives the floor. */
        CssPx declared = css_px_max(len.kind == CSS_LENGTH_ABSOLUTE
                                        ? len.px
                                        : css_length_resolve_pct(len, basis),
                                    css_px(0.0));

        /* §10.3.3, §10.3.5, §10.3.7 and §10.3.9 all agree on this one case and each says it in its own words:
           the equation solves for `width` only when `width` is `auto`, so a declared length IS the used value.
           §10.6.2 and §10.6.3 say the same for `height`. What does NOT agree is a table box and a flex or grid
           item, and both crash below. */
        /* CSS 2.1 §17.5 owns a TABLE box's width and height and §10 does not apply to either, so a declared
           length is NOT the used value here: it is an INPUT to §17.5's own algorithms. The two axes are two
           different sections and only one of them is built, which is why they split here rather than sharing an
           arm — §17.5.2 Table width algorithms: the 'table-layout' property takes the declared width as a
           minimum ("the used width is the greater of W, CAPMIN, and … MIN") and core/layout/table_width.h runs
           it, while §17.5.3 Table height algorithms says the same of a declared height in its own words and has
           no component yet. A TABLE-INTERNAL box is a third case: §17.5.2 is stated over "the 'table' or
           'inline-table' element", and a cell's or a row's width is a different number. THE CELL DOES NOT
           REACH THIS LINE AT ALL — `uv_pass_size` answered it before the declaration was resolved, because
           §17.5.2.2 Automatic table layout's step 1 makes a cell's declared `width` a FLOOR under its column
           ("If the specified 'width' (W) of the cell is greater than MCW, W is the minimum cell width") and
           never the used value, so there is nothing for this arm to do with it. */
        if (box == UV_BOX_TABLE) {
            TableBoxKind kind = uv_table_box_kind(el);

            DCHECK(!vertical,
                   "CSS 2.1 §17.5.3 Table height algorithms owns the BLOCK axis of every one of CSS 2.1 §17.2 "
                   "The CSS table model's box types, and `uv_pass_size` routes or refuses all ten of them "
                   "before this function is reached — so a vertical size arriving at §17.5.2 Table width "
                   "algorithms: the 'table-layout' property's arm is that route having been lost, and the "
                   "number it would answer with is this box's WIDTH reported as its height");
            if (table_box_kind_generates_table_box(kind)) return uv_table_used_width(el);
            uv_table_non_cell_width_fail(el, kind);
        }
        /* A TABLE BOX's, a ROW's and a CELL's declared `height` NEVER REACH THIS ARM — `uv_pass_size`
           answered all three above, because CSS 2.1 §17.5.3 Table height algorithms takes the
           declaration as an INPUT to its own comparison rather than as the used value ("Any other
           value is treated as a minimum height"), so the declared and `auto` cases are ONE route.
           The crash that stood here told its reader to build §17.5.3, which is built. */
        if (box == UV_BOX_ITEM)
            DFAIL("this box is a FLEX or GRID ITEM, so its used main and cross sizes come from its container's "
                  "algorithm and not from CSS 2.1 §10 at all — css-flexbox §9.7 resolves the flexible lengths "
                  "(a declared `width` is only the FLEX BASE SIZE that `flex-grow` and `flex-shrink` then "
                  "adjust against the container's free space), and css-grid §11 sizes a grid item to its "
                  "track. BUILD the flex layout algorithm, which needs the container's own used content size "
                  "first — the same §10.3.3 subproblem, one level up");
        /* css-sizing-3 §3.3 decides which BOX EDGE the declared length is on, and the used value it exposes.
           It runs on §10.4's SUBSTITUTED limit too — §3.3 says the property "affects the interpretation of ALL
           SIZING PROPERTIES", so a `max-width` under `border-box` bounds the border box and its own content
           box floors at zero, which is what this arm's floor produces. */
        if (uv_is_border_box(el)) return uv_border_box_size(el, declared, vertical);
        return declared;
    }
    /* WHAT FALLS THROUGH IS `auto` OR A PERCENTAGE HEIGHT BEHAVING AS ONE, and the two take the same arms
       below because css-sizing-3 §3.2.1 "“Behaving as auto”" says they are one case: the property "is said to
       BEHAVE AS AUTO" both "when width/height computes to auto" and "when it is defined to behave as if auto
       were specified (as in the case of block percentage heights resolving against an indefinite size, see
       CSS2§10.5)". So `height: 100%` inside an `auto`-height parent reaches §10.6.3's content-based height,
       which is what every user agent does with it, and it does so WITHOUT its computed value having been
       rewritten — §3.1.1's `Computed value: as specified, with <length-percentage> values computed` keeps the
       percentage, so `getComputedStyle` still reports it.
       A MATH FUNCTION TAKES THE SAME ROUTE, and css-values-4 §10.11 "Computed Value" says so by name: "if
       there are special rules for computing percentages in a value (e.g. the height property), they apply
       whenever a math function contains percentages". `height: calc(100% - 2rem)` therefore behaves as auto
       WHOLE under the same antecedent rather than having its percentage half treated separately, which is why
       `pct` above admits both kinds and neither is split. */
    DCHECK((len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, "auto") == 0) || (pct && vertical),
           "a `width` or `height` reached the `auto` arms as something that is neither `auto` nor a vertical "
           "percentage behaving as one. CSS 2.1 §10.2 and §10.5 admit `<length> | <percentage> | auto`, a "
           "horizontal percentage always has §10.2's basis and resolved above, and css-sizing-3 §3.2's "
           "`min-content`/`max-content`/`fit-content()` are level-3 additions this engine records no "
           "computed-value rule for");
    /* THE REPLACED QUESTION IS ASKED BEFORE THE BOX TYPE because §10.3 answers it before the box type too:
       five of its ten sections delegate a replaced element's size to §10.3.2, and only the box types §10 does
       not own at all (a table box, a flex or grid item) disagree. It is asked ONLY on this arm — a DECLARED
       length is the used value for a replaced element and a non-replaced one alike, so a `<video
       style="width:100px">` must not pay for a classification whose answer cannot change its size. */
    {
        ReplacedElement rep = replaced_element_of(el);

        if (rep.replaced) return uv_replaced_size(el, &rep, box, vertical);
    }
    if (vertical) {
        /* CSS 2.1 §10.6.3's CONTENT-BASED HEIGHT — "the distance from its top content edge to … the bottom
           edge of the bottom (possibly collapsed) margin of its last in-flow child" — is core/layout/
           block_flow.h's walk, and so is §10.6.6's redirection of a float's and an inline-block's own `auto`
           height to §10.6.7. Which of the two sections runs is the SAME walk under §8.3.1's escape flags, so
           the three box types below share this arm; every other one has its height from a different spec and
           says so.
           css-sizing-3 §3.3's CONVERSION IS APPLIED TO THE RESULT, exactly as it is to §10.3.3's equation two
           functions down: the walk answers the CONTENT box, which is what CSS 2.1 knows, and css-sizing-3 §3.3
           makes the used value "as exposed for instance through getComputedStyle()" the border box's. */
        if (box == UV_BOX_ABS)
            DFAIL("CSS 2.1 §10.6.4 solves an ABSOLUTELY POSITIONED box's `height: auto` from its own constraint "
                  "equation — 'top + margin-top + border-top-width + padding-top + height + padding-bottom + "
                  "border-bottom-width + margin-bottom + bottom = height of containing block' — and its rules 1 "
                  "and 3, where `top` or `bottom` is `auto` alongside `height`, send it on to §10.6.7's "
                  "content-based height instead. TWO things are missing and the walk is not one of them: the "
                  "used `top` and `bottom` (CSSOM §9's inset arm crashes on the same equation) and the STATIC "
                  "POSITION its `auto` cases fall back to, which is where the box would have been in normal "
                  "flow. §9.4.1's normal flow is BUILT for IN-FLOW boxes (core/layout/flow_position.h) and "
                  "§10.6.3 is what keeps an out-of-flow child out of its walk, so EXTEND core/layout/"
                  "block_flow.c to report a skipped child's would-be position, then §10.6.4 over it");
        /* A TABLE BOX with `height: auto` does not reach here either, for the same reason and through
           the same route: CSS 2.1 §17.5.3 Table height algorithms owns both of its arms and
           `uv_pass_size` takes them together. §10.6.3's stack of block-level children is not a
           table's height under any value of the property. */
        if (box == UV_BOX_ITEM)
            DFAIL("a FLEX or GRID ITEM with `height: auto`. Its cross size is its CONTAINER's algorithm — "
                  "css-flexbox §9.4 collects the items into flex lines and §9.7 resolves the flexible lengths, "
                  "css-grid §11 sizes the item to its TRACK — and CSS 2.1 §10.6.3's stack of block-level "
                  "children is not it. BUILD the flex layout over the container's own used content size");
        if (box == UV_BOX_INLINE_FLEX_GRID)
            DFAIL("an INLINE-LEVEL FLEX OR GRID CONTAINER with `height: auto`. Both modules give the container "
                  "the same automatic block size and neither of them is §10.6.3's walk: css-grid-1 §5.2 "
                  "\"Sizing Grid Containers\" says \"in both inline and block formatting contexts, the grid "
                  "container's auto block size is its MAX-CONTENT SIZE\", which the same section states as "
                  "\"the sum of the grid container's track sizes (including gutters) in the appropriate axis\", "
                  "and css-flexbox-1 §9.9 \"Intrinsic Sizes\" says the container's content-based logical "
                  "heights \"use the max-content size\", which §9.9.2 \"Flex Container Intrinsic Cross Sizes\" "
                  "derives from the FLEX LINES. Two grid items in one row do not stack and two flex items on "
                  "one line do not either, so §10.6.3's collapsing column of block-level children would answer "
                  "a number from the wrong algorithm. BUILD the module this box's `display` names — its track "
                  "sizing (css-grid-1 §11.3 \"Track Sizing Algorithm\") or its line and cross sizing "
                  "(css-flexbox-1 §9.4 \"Cross Size Determination\") — over the container's own used main size, "
                  "which is the arm below and is the same absence one axis over");
        DCHECK(box == UV_BOX_BLOCK_FLOW || box == UV_BOX_FLOAT || box == UV_BOX_INLINE_BLOCK,
               "an `auto` height reached §10.6.3's walk on a box type whose own section is elsewhere — every "
               "one of them has left through its own crash above, so uv_box_kind's list and this one have come "
               "apart");
        {
            CssPx content = block_flow_auto_height(el);

            if (!uv_is_border_box(el)) return content;
            return css_px_add(content, uv_surround_total(uv_surround(el, true)));
        }
    }
    /* §10.3.5 and §10.3.9 send an `auto` width to SHRINK-TO-FIT, which is a different algorithm from §10.3.3's
       equation over different terms. It was always reachable through CSSOM §9's resolved value; CSSOM VIEW §6's
       `clientWidth` on a floated or inline-block box is the ordinary way a page arrives here. */
    if (box == UV_BOX_FLOAT || box == UV_BOX_INLINE_BLOCK) return uv_shrink_to_fit_width(el, len, box);
    if (box == UV_BOX_ABS)
        DFAIL("CSS 2.2 §10.3.7 \"Absolutely positioned, non-replaced elements\" sends this box's `auto` width to "
              "the shrink-to-fit formula in its rules 1 and 3 — the ones where `left` or `right` is `auto` "
              "alongside `width` — and its rule 5, with both offsets given, solves the constraint equation for "
              "`width` instead. THE FORMULA ITSELF IS BUILT (uv_shrink_to_fit_width above) and so are the two "
              "intrinsic terms it needs, so what is missing is neither: it is §10.3.7's OWN AVAILABLE WIDTH, "
              "which is not §10.3.5 \"Floating, non-replaced elements\"'s — and it is NOT the containing block "
              "minus BOTH offsets. §10.3.7 states it as \"this is found by solving for 'width' after setting "
              "'left' (in case 1) or 'right' (in case 3) to 0\", and cases 1 and 3 ARE rules 1 and 3, each of "
              "which has exactly ONE `auto` offset: that one is ZERO and the other is declared. So this arm "
              "needs no offset this engine cannot supply. DO NOT compute it from a static position — §10.3.7 "
              "uses the static position for the ALL-THREE-`auto` entry alone, \"if the 'direction' property of "
              "the element establishing the static-position containing block is 'ltr' set 'left' to the static "
              "position and apply rule number three below; otherwise, set 'right' to the static position and "
              "apply rule number one below\", and even there the rule it then applies zeroes the OTHER offset. "
              "THAT entry is the one needing \"the position an element would have had in the normal flow\": "
              "§9.4.1's normal flow is BUILT for IN-FLOW boxes (core/layout/flow_position.h) and §10.6.3 is "
              "exactly what keeps an out-of-flow child out of its walk, so EXTEND core/layout/block_flow.c to "
              "report a skipped child's would-be position FOR THE ALL-THREE-`auto` ENTRY, and write THIS arm's "
              "available width from the constraint equation with the `auto` offset at 0 — after which this arm "
              "is the same three-term `min(max(...))` the function above already computes");
    /* The two box types §10 does not own at all reach the `auto` arm as well as the declared one, and their
       `auto` case is a DIFFERENT algorithm from their declared case, so each says which. */
    /* A TABLE box with `width: auto` is CSS 2.1 §17.5.2.2 Automatic table layout's, and §17.5.2.1 Fixed table
       layout sends its own `auto` case there too ("A value of 'auto' (for both 'display: table' and 'display:
       inline-table') means use the automatic table layout algorithm"), so `table-layout` does not reach this
       line at all — core/layout/table_width.h owns that dispatch. §10.3.3's equation does not apply to a table
       and never did. */
    /* A CELL never reaches here either — `uv_pass_size` answers it before this function is called, because
       §17.5's grid decides a cell's width whether the declaration is `auto` or a length and the two arms are
       therefore ONE route. What is left is the caption and the row/column machinery, which is the same three
       questions the declared arm has and is refused by the same function. */
    if (box == UV_BOX_TABLE) {
        TableBoxKind kind = uv_table_box_kind(el);

        DCHECK(!vertical,
               "CSS 2.1 §17.5.3 Table height algorithms owns the BLOCK axis of every one of CSS 2.1 §17.2 "
               "The CSS table model's box types, and `uv_pass_size` routes or refuses all ten of them "
               "before this function is reached — so a vertical size arriving at §17.5.2 Table width "
               "algorithms: the 'table-layout' property's arm is that route having been lost, and the "
               "number it would answer with is this box's WIDTH reported as its height");
        if (table_box_kind_generates_table_box(kind)) return uv_table_used_width(el);
        uv_table_non_cell_width_fail(el, kind);
    }
    if (box == UV_BOX_ITEM)
        DFAIL("a FLEX or GRID ITEM with `width: auto`. css-flexbox §9.7 makes the FLEX BASE SIZE the item's "
              "max-content contribution and then flexes it against the container's free space; css-grid §11 "
              "sizes the item to its TRACK, which is itself sized from the items in it. Both are intrinsic "
              "sizes and neither is §10.3.3's equation. BUILD the flex layout over the container's own used "
              "content size, which §10.1 and §10.3.3 answer now");
    if (box == UV_BOX_INLINE_FLEX_GRID)
        DFAIL("an INLINE-LEVEL FLEX OR GRID CONTAINER with `width: auto`. It is CSS 2.2 §9.2.2's ATOMIC "
              "INLINE-LEVEL box, so its own module sends it to the section this file already runs for an "
              "inline-block — css-grid-1 §5.2 \"Sizing Grid Containers\" says it \"is sized as an atomic "
              "inline-level box (such as an inline-block)\" and css-flexbox-1 §9.2 \"Line Length "
              "Determination\" says \"determine the main size of the flex container using the rules of the "
              "formatting context in which it participates\" — which is §10.3.9's shrink-to-fit. WHAT IS "
              "MISSING IS NEITHER THE FORMULA NOR THE AVAILABLE WIDTH, and that is the whole difference "
              "between this arm and the absolutely positioned one above: §10.3.9 sends this box to §10.3.5's "
              "three-term `min(max(preferred minimum width, available width), preferred width)`, which "
              "`uv_shrink_to_fit_width` computes, over an available width that IS §10.3.5's six-term one. It "
              "is the TWO INTRINSIC TERMS. core/layout/intrinsic_size.c measures them by laying the box's own "
              "text out under CSS 2.2 §9.4.2's line boxes and crashes for a box that is not a block container, "
              "and this one is not: css-flexbox-1 §3 says \"flex containers are not block containers\" and "
              "css-grid-1 §5.1 says \"grid containers are not block containers\", and what is inside one is "
              "flex lines or grid tracks rather than line boxes. So the "
              "two terms are the MODULE's — css-flexbox-1 §9.9.1 \"Flex Container Intrinsic Main Sizes\" (the "
              "max-content main size is the largest per-line sum of the items' flexed sizes; §9.9.1.3 gives a "
              "multi-line container's min-content main size as the largest min-content contribution) and "
              "css-grid-1 §5.2 (\"the max-content size (min-content size) of a grid container is the sum of "
              "the grid container's track sizes (including gutters) in the appropriate axis, when the grid is "
              "sized under a max-content constraint (min-content constraint)\"). BUILD the one this box's "
              "`display` names as a second producer of `IntrinsicInlineSizes`; the shrink-to-fit above then "
              "runs unchanged over it");
    DCHECK(box == UV_BOX_BLOCK_FLOW,
           "an `auto` width reached §10.3.3's constraint equation on a box that is not block-level in normal "
           "flow — every other box type in uv_box_kind's list has left through its own section above, so the "
           "two lists have come apart");
    return uv_block_auto_width(el, len, box);
}

/* CSS 2.1 §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'" and §10.7 "Minimum and maximum
 * heights: 'min-height' and 'max-height'" — THE ORDINARY ALGORITHM, three steps, in the spec's own order.
 * §10.4: "The tentative used width is calculated (without 'min-width' and 'max-width') following the rules
 * under 'Calculating widths and margins' above. If the tentative used width is greater than 'max-width', the
 * rules above are applied again, but this time using the computed value of 'max-width' as the computed value
 * for 'width'. If the resulting width is smaller than 'min-width', the rules above are applied again, but this
 * time using the value of 'min-width' as the computed value for 'width'." §10.7 is the same three sentences
 * about `height`, which is why one function takes `vertical`.
 *
 * THE ORDER IS NOT COMMUTATIVE AND IS NOT A `clamp()`. Step 3 is stated over "the RESULTING width" — the
 * output of step 2 — so `min-width` wins when the two conflict, and running them the other way round or as one
 * three-argument min/max would give the box `max-width` instead. That is the whole of what "take the max-width
 * as max(min, max)" achieves in the OTHER algorithm, and it is achieved here by the sequence.
 *
 * WHY THE SUBSTITUTION IS THE ANSWER AND `min`/`max` OVER TWO NUMBERS IS NOT. The re-run is over "the rules
 * above", which is all of §10.3 — so it re-solves whichever MARGIN was `auto`. `margin: 0 auto; max-width:
 * 1200px` on an `auto`-width block is the case: step 1 gives the box the containing block's whole width with
 * both margins at rule 5's zero, and step 2 makes the width 1200 and the margins rules 4 and 6's SLACK, which
 * is what centres it. Clamping the number alone leaves the margins at zero and the box hard against the left
 * edge. It also re-runs css-sizing-3 §3.3's `box-sizing` conversion, which is not the identity: under
 * `border-box` a `max-width` smaller than the paddings and borders floors the CONTENT box at zero and the
 * border box ends up at the surround, not at the limit.
 *
 * AND THE CLAMPED VALUE STAYS CONCOLIC, which is why the substitution goes through `css_px_min`/`css_px_max`
 * rather than taking the limit's `CssPx` whole. Those two are documented for exactly this caller: the result
 * carries the UNION of both operands' environment facts, "because the operand that lost at this viewport is
 * the one that wins at another". A `max-width: 400px` on a `width: 50vw` box picks 400 at 1280 and the ICB's
 * fact still rides it, so a page branching on the used width still forks the narrow-viewport world where 50vw
 * wins. Taking the limit alone would delete that arm exactly as a `min(50vw, 400px)` collapsing to 400 would.
 * The NUMBER is still the second pass's — `css_px_min` picks the limit because the step's own condition says
 * the tentative exceeds it, and `uv_pass_size` then runs the rules on it — so nothing here is arithmetic
 * standing in for the re-run. */
typedef struct {
    CssLength len;   /* the computed size the FINAL pass ran with — the raw value, or §10.4's substituted limit */
    CssPx     used;  /* that pass's used value, in the box css-sizing-3 §3.3 exposes */
} UvSized;

static UvSized uv_sized(lxb_dom_element_t *el, UvBox box, bool vertical)
{
    UvSized r;
    UvLimits lim;

    r.len = css_computed_length(el, vertical ? "height" : "width");
    /* STEP 1 — the TENTATIVE used value, "calculated WITHOUT 'min-width' and 'max-width'". It runs before the
       limits are even read, which is also what sends the box types §10 does not own to their own section — a
       flex or grid item to its crash, and a table box to CSS 2.1 §17.5.2 Table width algorithms: the
       'table-layout' property — rather than to `uv_limits`' assert that they never arrive. */
    r.used = uv_pass_size(el, r.len, box, vertical);
    /* §10.4's THREE STEPS DO NOT RUN OVER A TABLE BOX, and that is the section's own sentence rather than a
       shortcut: "In CSS 2.1, the effect of 'min-width' and 'max-width' on tables, inline tables, table cells,
       table columns, and column groups is undefined", with §10.7 saying the same of rows and row groups. So the
       tentative value IS the used value here, and `uv_limits`' assert that no table box ever reaches it stays
       true — which is what it is for. The comment above used to say the crashes were what kept a table out of
       that assert; §17.5.2 now ANSWERS for a table's width, so the return is what keeps it out. */
    if (box == UV_BOX_TABLE) return r;
    lim = uv_limits(el, box, vertical);
    uv_require_no_ratio_table(el, &lim);
    /* STEP 2 — "if the tentative used width is greater than 'max-width' … using the computed value of
       'max-width' as the computed value for 'width'". */
    if (lim.has_max && r.used.px > lim.max.px) {
        r.len = uv_len_px(css_px_min(r.used, lim.max));
        r.used = uv_pass_size(el, r.len, box, vertical);
    }
    /* STEP 3 — "if the RESULTING width is smaller than 'min-width' …", over step 2's output and not over
       step 1's, which is what makes `min-width` win a conflict. */
    if (lim.has_min && r.used.px < lim.min.px) {
        r.len = uv_len_px(css_px_max(r.used, lim.min));
        r.used = uv_pass_size(el, r.len, box, vertical);
    }
    DCHECK(!lim.has_max || !lim.has_min || lim.min.px <= lim.max.px || r.used.px >= lim.min.px,
           "§10.4/§10.7 ran with a `min-` limit ABOVE the `max-` one and the floor did not win. The two steps "
           "are ordered so that it does — step 3 is stated over step 2's result — and this is the "
           "configuration the OTHER algorithm normalises away with \"take the max-width and max-height as "
           "max(min, max)\". A used value below the floor here means the two steps ran in the wrong order or "
           "step 3's own re-run did not take");
    return r;
}

CssPx used_value_px(lxb_dom_element_t *el, const char *name)
{
    static const char *const MARGINS[] = { "margin-top", "margin-right", "margin-bottom", "margin-left" };
    static const char *const PADDINGS[] = { "padding-top", "padding-right", "padding-bottom", "padding-left" };
    CssLength len;
    UvBox box;
    CssPx out;
    bool vertical = false;
    unsigned i;
    int group = -1;   /* 0 = margin, 1 = padding, 2 = size */
    int side = -1;    /* the index into the two four-side tables above: top, right, bottom, left */

    DCHECK(el != NULL && name != NULL, "a used value was asked for with no element or no property name");
    for (i = 0; i < sizeof(MARGINS) / sizeof(MARGINS[0]); i++) {
        if (strcmp(MARGINS[i], name) == 0)  { group = 0; side = (int)i; }
        if (strcmp(PADDINGS[i], name) == 0) { group = 1; side = (int)i; }
    }
    if (side >= 0) vertical = (side % 2) == 0;   /* top and bottom are the even entries */
    if (group < 0 && (strcmp(name, "width") == 0 || strcmp(name, "height") == 0)) {
        group = 2;
        vertical = strcmp(name, "height") == 0;
    }
    if (group < 0) {
        DFAIL("CSSOM §9 routed a property to CSS 2.1 §10's used value that this component does not compute. It "
              "carries the ten PHYSICAL box-model lengths — the four margins, the four paddings, `width` and "
              "`height`. §9's own used-if-rendered list also names the LOGICAL spellings (`inline-size`, "
              "`margin-block-start`, `padding-inline-end`), which need css-writing-modes §6's mapping to a "
              "physical property before §10 can be asked anything, and `transform-origin`, whose used value is "
              "a position in the TRANSFORM REFERENCE BOX (css-transforms §5) and not a length at all");
        return css_px(0.0);
    }
    len = css_computed_length(el, name);
    box = uv_box_kind(el);
    /* CSS 2.1 §10.3.1 "Inline, NON-REPLACED elements" and §10.6.1 are what make `width` and `height` not apply
       to an inline box, and the word that used to be missing from this assert is the one in their titles:
       §10.3.2 "Inline, REPLACED elements" gives an inline `img` a real used width, and §10.2's own
       "Applies to:" line says so too — "all elements but NON-REPLACED inline elements". So the assert is over
       the pair, and the replaced question is asked only when the first two conjuncts have already failed,
       which is exactly the shape whose answer it can change. core/css/css_property_applies.c decides the same
       pair; if these two disagree, one of them read a different image request state than the other. */
    DCHECK(box != UV_BOX_INLINE || group != 2 || replaced_element_of(el).replaced,
           "CSS 2.1 §10.3.1 and §10.6.1 say `width` and `height` do not apply to an inline NON-REPLACED box, "
           "so CSSOM §9's first conjunct is false and the resolved value is the computed value — this call "
           "should never have been made. css_property_applies.c decides it, and the two have come apart");
    /* §10.3.3's margin rules read the `width` §10.4 has already SUBSTITUTED, so a horizontal margin asks
       `uv_sized` for the pass's length before resolving — that is what makes `margin: 0 auto` under a
       `max-width` centre the box. A VERTICAL margin reads no size at all (§10.6.3 gives its `auto` the used
       value 0 outright), so the height is not resolved for it: doing so would run §10.6.3's whole subtree walk
       to answer a question that does not consult it.
       AND THE SAME SENTENCE DECIDES THE HORIZONTAL AXIS PER BOX TYPE — `uv_margin_reads_width` — which this
       arm used not to ask, and the cost was not a wasted walk. §10.3.1's first sentence is "The 'width'
       property does not apply", so an inline non-replaced box has NO used width, and resolving one to hand to
       rules that never read it ran §10.3.3's constraint equation on exactly the box §10.3.3 is not about. Two
       call sites reached it that way — CSSOM VIEW §6's fragment rects and §9.4.2's line-extent test, both
       asking an inline box for the margins §10.3.1 answers with 0. */
    if (group == 0) {
        /* The two property names are the AXIS and are present together on the horizontal one whatever the box
           type is — §10.3.1's box still HAS a `margin-left` and a `margin-right`, and its section still has a
           rule for them. What varies is only whether that rule reads a width. */
        const char *self = vertical ? NULL : MARGINS[side];
        const char *opposite = vertical ? NULL : MARGINS[(side + 2) % 4];

        if (!vertical && uv_margin_reads_width(box)) {
            CssLength width = uv_sized(el, box, false).len;

            out = uv_margin(el, self, opposite, len, box, &width);
        } else {
            out = uv_margin(el, self, opposite, len, box, NULL);
        }
    }
    else if (group == 1) out = uv_padding(el, len);
    else                 out = uv_sized(el, box, vertical).used;
    return out;
}

/* CSS 2.1 §8's BOX MODEL — "the padding edge surrounds the box padding", and the padding box is the content box
   plus the padding on each side — over css-sizing-3 §3.3, which is the only thing that varies: WHICH BOX the used
   size is the size of. That is why the paddings are added ONCE below, to the content box `uv_content_size`
   derives, rather than in two arms that would each have to remember the other's convention.
   THE ASSERT IS THE WHOLE MECHANISM AND IT IS TWO-SIDED. §5 floors the content box at zero — "as the content
   width and height cannot be negative, this computation is floored at zero" — and `uv_border_box_size`
   IMPLEMENTS that floor, by returning the LARGER of the declared length and the four-term surround rather than
   the declared length alone. So under `border-box` the used size is a number that same sum already dominates,
   and subtracting the sum back out cannot go below zero: the floored case cancels to exactly zero because it is
   the identical `uv_surround_total` result going back, and the other case is a subtraction the `>` that chose
   it already decided the sign of. A negative content box here is therefore not a strange page and not a
   rounding artifact — it is the two derivations having stopped describing the same box: a used size that did
   not come through §5's floor, or a surround computed from different terms than the one that produced it.
   Under `content-box` the same assert says something simpler and just as necessary — CSS 2.1 §10.2's `width`
   is a non-negative <length>, so a negative used size is a derivation that lost an operand.
   AND THE TWO EDGES ARE ONE DERIVATION, which is why §8.1's border edge is a `with_border` here and not a
   second function that adds the border widths to what this one returned. §8.1 states the box model as one
   nesting — content, then padding, then border — so the border edge is the padding edge plus the same two
   border widths `uv_surround` already computed for it, and reading them a second time from a second surround
   is the one way the two edges could come to describe different boxes. */
static CssPx uv_edge_px(lxb_dom_element_t *el, bool vertical, bool with_border)
{
    UvSurround s;
    CssPx content;

    s = uv_surround(el, vertical);
    content = uv_content_size(el, vertical, s);
    DCHECK(content.px >= 0.0,
           "the CONTENT box derived from a used size is NEGATIVE. Under `box-sizing: border-box` css-sizing-3 §3.3 "
           "floors it at zero and this component's border-box arm implements that floor by taking the larger of "
           "the declared length and the four-term surround, so subtracting that same sum back out cannot reach "
           "a negative — a negative one means the used size and this surround were computed from "
           "different terms and no longer describe the same box. Under `content-box` the used size IS the "
           "content box, and CSS 2.1 §10.2's `width` is a non-negative <length>, so a negative one is a "
           "derivation that lost an operand");
    return css_px_add(content, with_border ? uv_surround_total(s) : s.padding);
}

CssPx used_value_padding_edge_px(lxb_dom_element_t *el, bool vertical)
{
    DCHECK(el != NULL, "a padding edge's extent was asked for with no element");
    return uv_edge_px(el, vertical, false);
}

CssPx used_value_border_edge_px(lxb_dom_element_t *el, bool vertical)
{
    DCHECK(el != NULL, "a border edge's extent was asked for with no element");
    return uv_edge_px(el, vertical, true);
}

/* CSS 2 §8.1's OUTERMOST NESTING, stated over the border edge above rather than over a third surround — "the
   margin edge surrounds the box margin", and the two margins on the axis are the only terms between the two
   boxes. It is written here and not at the caller for `uv_edge_px`'s reason one level out: `used_value_px`'s
   answer for a margin is CSS 2.1 §10.3's, whose `auto` rule differs by BOX TYPE (§10.3.1, §10.3.5 and §10.3.9
   each make a horizontal `auto` margin 0 while §10.3.3 may give it the slack), so a caller adding "the
   margins" would be choosing a section it has no reason to know it was choosing.
   NO FLOOR AND NO ASSERT ON THE SIGN, which is CSS 2.2 §8.3's own "negative values for margin properties are
   allowed" — see used_value.h. The border edge inside it IS asserted non-negative, by `uv_edge_px`. */
CssPx used_value_margin_edge_px(lxb_dom_element_t *el, bool vertical)
{
    CssPx border;

    DCHECK(el != NULL, "a margin edge's extent was asked for with no element");
    border = uv_edge_px(el, vertical, true);
    border = css_px_add(border, used_value_px(el, vertical ? "margin-top" : "margin-left"));
    return css_px_add(border, used_value_px(el, vertical ? "margin-bottom" : "margin-right"));
}

/* §10.4/§10.7's CLAMP RUNS HERE TOO, and it is the one place it runs over a tentative value the caller
   computed rather than one `uv_pass_size` produced. That is not a second implementation of the algorithm: the
   extent handed in IS §10.6.3's answer, which is step 1's "tentative used height", and steps 2 and 3 substitute
   a LENGTH — whose pass is the declared arm and therefore needs no walk and cannot re-enter the caller. So the
   substitution is written as the same `css_px_min`/`css_px_max` the outer algorithm uses, for the same reason:
   the clamped extent stays a joint function of both operands' environment facts.
   IT MUST RUN, because the caller stacks this box inside its parent's own §10.6.3 walk — a child with
   `min-height: 400px` that reported its content height unclamped would make its parent's height wrong as well
   as its own, and nothing downstream would say so. */
CssPx used_value_border_edge_from_content_px(lxb_dom_element_t *el, CssPx content, bool vertical)
{
    UvSurround s;
    UvLimits lim;
    UvBox box;
    CssPx used;

    DCHECK(el != NULL, "a border edge was asked for from a content extent with no element");
    DCHECK(content.px >= 0.0,
           "CSS 2.1 §8.1's border edge was derived from a NEGATIVE content extent. The caller is §10.6.3's "
           "walk, whose answer is a running sum of used heights and collapsed margins — and a collapsed margin "
           "is a maximum of positives minus a maximum of negatives, so a negative total is a run whose "
           "operands were not the box's own margins");
    s = uv_surround(el, vertical);
    box = uv_box_kind(el);
    /* A `table-cell` is a BLOCK CONTAINER (core/layout/block_flow.h's own list), so the walk can hand one to
       this entry — and CSS 2.1 §17.5.3 owns its height, not §10.6.3. It crashes with §17.5's message here
       rather than through `uv_limits`' assert, which is about the two classifications agreeing and would say
       the wrong thing about a real page. A `table-caption` is a block container too and is NOT one of these:
       `uv_box_kind` classifies it as block-level in normal flow, so it arrives with `box` already answering
       §10.6.3 and this conversion is the right one for it. */
    if (box == UV_BOX_TABLE)
        DFAIL("a TABLE-INTERNAL box reached §8.1's border edge through §10.6.3's walk, and the content extent "
              "handed in is NOT this box's height. CSS 2.1 §17.5.3 Table height algorithms is BUILT "
              "(core/layout/table_height.h) and it is precisely what makes this a crash rather than an "
              "arithmetic: a CELL's used box height is the ROW's, not the cell's own content — \"Cell boxes "
              "that are smaller than the height of the row receive extra top or bottom padding\" — and the "
              "row is \"the maximum of the row's computed 'height', the computed 'height' of each cell in the "
              "row, and the minimum height (MIN) required by the cells\", so every OTHER cell in the row is "
              "an input to this one's box. Converting the extent in hand would answer with the cell's own "
              "content height, which is one term of one term of the answer. `used_value_px` on a cell routes "
              "to §17.5.3 and gets the whole of it. A CAPTION CANNOT REACH THIS ARM AT ALL ANY MORE and the "
              "sentence that stood here said it could: §17.4 Tables in the visual formatting model renders it "
              "as a normal block box in the table WRAPPER, so `uv_box_kind` classifies it as block-level in "
              "normal flow and it arrives with a `box` that is not `UV_BOX_TABLE` — §10.6.3 owns its height "
              "and this conversion is simply the right one for it. §10.4/§10.7 say outright that their own "
              "effect on table boxes is undefined, so the clamp below is not what this arm is skipping "
              "either. WHO CAN REACH THIS: core/layout/block_flow.c's stack holds no cell, because a cell is "
              "a block container and not a block-level box (§9.2.1) — so a box here is that walk having "
              "descended somewhere §9.4.1 does not go");
    lim = uv_limits(el, box, vertical);
    /* The tentative value in the box css-sizing-3 §3.3 exposes, which is the box the two limits are measured
       in — "it affects the interpretation of all sizing properties". */
    used = uv_is_border_box(el) ? css_px_add(content, uv_surround_total(s)) : content;
    if (lim.has_max && used.px > lim.max.px) used = css_px_min(used, lim.max);
    if (lim.has_min && used.px < lim.min.px) used = css_px_max(used, lim.min);
    if (!uv_is_border_box(el)) return css_px_add(used, uv_surround_total(s));
    /* §3.3's floor: "as the content width and height cannot be negative, this computation is floored at zero",
       so a limit smaller than the surround leaves the border box AT the surround. Identical to the declared
       arm's `uv_border_box_size`, and stated through it so the two cannot come to disagree. */
    return uv_border_box_size(el, used, vertical);
}
