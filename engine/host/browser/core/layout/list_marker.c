/* css-lists-3 §3.2 "Generating Marker Contents", read for which arm an element is on. See list_marker.h for
   the contract, for why the three consumers of that answer are one component, and for why the image arm is the
   one that refuses. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
/* THE PROPERTY REGISTRY, FOR THE PREMISE OF §3.2's FIRST ARM AND FOR NOTHING ELSE — `lxb_css_property_by_name`
   is declared here and nowhere narrower, and the assert below is the one place this file asks lexbor anything
   about CSS. The same argument core/layout/box_tree.c makes for taking one generated enum rather than the
   whole-parser header: a LAYOUT translation unit has no business compiling the parser, and what is needed here
   is a lookup. */
#include <lexbor/css/property.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_counter_style.h"
#include "core/css/css_shorthand.h"
#include "core/layout/box_subject.h"
#include "core/layout/list_marker.h"

/* A COMPUTED VALUE, WITH THE ONE THING THE CASCADE CANNOT ANSWER ASSERTED AT THE READ: core/css/
   css_style_declaration.c's UA layer answers `inline` for every element it does not name, so a NULL here is
   the cascade failing rather than a property being unset. OWNED: the caller frees. */
static char *lm_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECKF(v != NULL, "`%s`: the cascade produced no computed value for a property css-lists-3 §3.2 "
                       "\"Generating Marker Contents\" reads off the originating element, and every property "
                       "this entry asks for has an `Initial:` line css-cascade-5 §7.1 \"Initial Values\" can "
                       "fall to", name);
    return v;
}

/* REFUSES TO READ §3.2's FIRST ARM'S PREMISE AS STILL TRUE WITHOUT CHECKING IT. That arm is "content on the
   ::marker itself is not normal", and it is unreachable in this engine for two INDEPENDENT reasons, either of
   which an upstream sync of engine/lexbor can falsify in silence — which is why the premise is a check at the
   site that rests on it rather than a sentence in a header, exactly as core/layout/box_tree.c's `__UNDEF == 0`
   static assert is:
     - THE PROPERTY DOES NOT PARSE. lexbor's property registry has no `content` entry, so a declaration of it
       is one CSS Syntax drops and no cascade holds. css-content-3 §1 "Inserting and Replacing Content: the
       content property" is what makes the initial value the answer for ::marker — "For ::before and ::after,
       this computes to none. For ::marker, ::placeholder, and ::file-selector-button, this computes to itself
       (normal)." — so with nothing able to set it, `normal` is what §3.2's first arm tests against.
     - NO PSEUDO-ELEMENT SELECTOR PARSES AT ALL, so even a `content` property could not be TARGETED at a
       ::marker. core/css/css_style_declaration.c's getComputedStyle crash derives that in full: lexbor's
       `lxb_css_selectors_state_pseudo_element` refuses every one of the twelve names its own lookup table can
       return, so `li::marker { … }` answers Selectors 4 §17.1's `failure` and enters no cascade.
   THE CHECK IS TWO-SIDED BECAUSE A ONE-SIDED ONE CANNOT FAIL FOR THE RIGHT REASON: "the registry has no
   `content`" is satisfied by a lookup that answers NULL for EVERYTHING, which is a broken probe reported as a
   confirmed premise. The control is a property that must be there, so the two answers tell a falsified premise
   from an unarmed question — and the message prints both operands because the remedies are different work.
   IT IS A `require` FUNCTION WITH THE GUARD IN THE BODY, which is this directory's spelling for the reason
   core/layout/box_tree.c states at its own: a release `DCHECK` type-checks its condition without evaluating
   it, so a predicate hidden behind `#if APICLIENT_DEV` is an undeclared call in the shipped build and
   engine/build.mjs's `-Werror=implicit-function-declaration` makes that a hard failure of the release program
   alone. */
static void lm_require_marker_content_cannot_be_set(void)
{
#if APICLIENT_DEV
    bool content_absent = lxb_css_property_by_name((const lxb_char_t *) "content", 7) == NULL;
    bool control_present = lxb_css_property_by_name((const lxb_char_t *) "color", 5) != NULL;

    if (content_absent && control_present) return;
    DFAILF("css-lists-3 §3.2 \"Generating Marker Contents\"' first arm, \"content on the ::marker itself is "
           "not normal\", is read here as unreachable, and the premise that makes it so no longer holds: "
           "lexbor answers %s for the `content` property and %s for the `color` control. A PRESENT `content` "
           "means the premise is FALSIFIED and §3.2's first arm is now a real condition this entry must ask — "
           "BUILD it, over a ::marker whose declarations reach the cascade (core/css/css_style_declaration.c's "
           "getComputedStyle crash states what the pseudo-element side of that needs). AN ABSENT `color` means "
           "the LOOKUP is broken rather than the premise: the probe answers NULL for every name, so it "
           "establishes nothing about `content` and no conclusion may be drawn from it",
           content_absent ? "no entry" : "AN ENTRY", control_present ? "an entry" : "NO ENTRY");
#endif
}

/* REFUSES §3.2's SECOND ARM WHERE THIS ENGINE CANNOT ANSWER IT. The arm is "list-style-image on the
   originating element defines a marker image", and css-images-3 §2 "Image Values: the <image> type" makes that
   a question about the IMAGE rather than about the property — see list_marker.h for both of its sentences. A
   computed value of `none` answers the arm FALSE without asking anything; any other value is an `<image>` whose
   validity only a fetch decides.
   THE RELEASE ARM IS §2's OWN ERROR-HANDLING CLAUSE AND IS COHERENT: the entry falls through to §3.2's third
   arm, which is what §2 says an invalid image does ("allowing the list-style-type to render in its place"), so
   no consumer is left holding a state it has no step for — a page with an unresolvable `list-style-image` gets
   the marker its `list-style-type` defines, which is the answer a real user agent reaches once the load
   fails. What dev refuses is the engine DECIDING that outcome before anything has been loaded. */
static void lm_require_marker_image_is_decidable(lxb_dom_element_t *el, const char *image)
{
#if APICLIENT_DEV
    char bbuf[160];

    DFAILF("%s has `list-style-image: %s`: css-lists-3 §3.2 \"Generating Marker Contents\"' second arm is "
           "\"list-style-image on the originating element defines a marker image\", and whether this one does "
           "is not a cascade question. css-images-3 §2 \"Image Values: the <image> type\" ties it to the "
           "image's VALIDITY — \"In some cases an image is invalid, such as a <url> pointing to a resource "
           "that is not a valid image format or that has failed to load\" — and an image this engine has not "
           "fetched has not failed to load, so it is neither valid nor invalid and §3.2's second arm has no "
           "answer. BUILD the load: core/css/css_image.h's `css_image_kind` already says which arm of §2's "
           "`<image>` production this value took, and css-values-4 §4.5.1 \"Relative URLs\" is where the "
           "`<url>` arm's absolute form comes from — core/css/css_font_src.h holds this engine's decision that "
           "the cascade does not resolve one, so the resolution belongs to the fetching component and the base "
           "it resolves against is a named residual at core/css/css_style_sheet.c. ANSWERING INSTEAD WOULD "
           "PICK AN ARM: calling the image valid states contents nothing has read, and calling it invalid "
           "states a load failure that has not happened",
           box_subject(el, bbuf, sizeof bbuf), image);
#else
    (void) el;
    (void) image;
#endif
}

/* REFUSES A `list-style-type` OUTSIDE §3.4's OWN `Value:` LINE — a GUARD and not a gap, because the value is
   one this codebase enumerates: css-lists-3 §3.4 "Text-based Markers: the list-style-type property" has a
   `Value:` line of `<counter-style> | <string> | none`, `css_shorthand_validates_longhand` answers TRUE for
   the property, and core/css/css_shorthand.c's `list_style_longhand_value` IS that grammar — so a declaration
   outside it is one CSS Syntax drops and never one the cascade holds. The three arms are asked of the
   productions that own them (core/css/css_counter_style.h and css-values-4 §4.4 "Quoted Strings: the <string>
   type"'s predicate) rather than restated here, so this guard and the grammar that admits a value cannot come
   to disagree about `symbols(cyclic "*")`.
   IT IS WHAT MAKES THE EXISTENCE ANSWER CHECKABLE RATHER THAN ARGUED: the entry below reads "defines a marker
   string" as "is not the `none` keyword", and that is only sound while every OTHER value of the line defines
   one. This states the line. */
static void lm_require_type_is_in_the_grammar(lxb_dom_element_t *el, const char *type)
{
#if APICLIENT_DEV
    size_t n = strlen(type);
    char bbuf[160];

    if (strcmp(type, "none") == 0 || css_counter_style_is_counter_style(type, n) ||
        css_shorthand_string(type, n))
        return;
    DFAILF("%s has `list-style-type: %s`: a cascaded value reached css-lists-3 §3.2 \"Generating Marker "
           "Contents\"' third arm that is outside css-lists-3 §3.4 \"Text-based Markers: the list-style-type "
           "property\"' own `Value:` line of `<counter-style> | <string> | none` — so it is neither the keyword "
           "nor a value css-counter-styles-3 §5 \"Extending list-style-type, counter(), and counters()\"' "
           "`<counter-style>` admits nor one css-values-4 §4.4 \"Quoted Strings: the <string> type\" does. "
           "`css_shorthand_validates_longhand` answers TRUE for this property and this engine's grammar for it "
           "is core/css/css_shorthand.c's, which admits nothing else — so the declaration a page wrote was "
           "DROPPED and this value did not come from one. Find what put it in the cascade; §3.2's condition is "
           "not the defect",
           box_subject(el, bbuf, sizeof bbuf), type);
#else
    (void) el;
    (void) type;
#endif
}

bool list_marker_box_generated(lxb_dom_element_t *el)
{
    char *v;
    bool yes;

    DCHECK(el != NULL, "css-lists-3 §3.2 \"Generating Marker Contents\" was asked of no element — §3.2 states "
                       "every one of its arms over \"the originating element\", so there is no answer without "
                       "one");

    /* css-lists-3 §3.1 "The ::marker Pseudo-Element"' LAST SENTENCE, which is the whole population and not an
       approximation of it: "Marker boxes only exist for list items: on any other element, the ::marker
       pseudo-element’s content property must compute to none, which suppresses its creation." The keyword is
       css-display-3 §2.3 "Generating Marker Boxes: the list-item keyword"'s and the comparison is one, not a
       prefix test: css-display-3 §2.3's two-value `block flow list-item` form is not a computed value this
       engine produces, and core/css/css_computed_value.c's `blockified` crashes on it by name long before this
       entry. */
    v = lm_computed(el, "display");
    yes = strcmp(v, "list-item") == 0;
    free(v);
    if (!yes) return false;

    /* §3.2 IS ORDERED — "The contents of a marker box are determined by the first of these conditions that is
       true" — so the arms are asked in the section's own order and an arm is reached only once every arm above
       it is false. Reading the third alone would answer a condition whose predecessors nothing had asked. */
    lm_require_marker_content_cannot_be_set();

    v = lm_computed(el, "list-style-image");
    if (strcmp(v, "none") != 0) lm_require_marker_image_is_decidable(el, v);
    free(v);

    v = lm_computed(el, "list-style-type");
    lm_require_type_is_in_the_grammar(el, v);
    /* §3.2's THIRD ARM AND THEN ITS LAST. "defines a marker string" is every value of §3.4's line but its own
       `none` keyword, by css-counter-styles-3 §5's sentence that a name denoting no counter style still
       denotes one — see list_marker.h. So a FALSE here is §3.2's `otherwise`: "The marker box has no contents
       and ::marker does not generate a box". */
    yes = strcmp(v, "none") != 0;
    free(v);
    return yes;
}
