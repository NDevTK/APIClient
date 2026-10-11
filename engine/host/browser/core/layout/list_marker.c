/* css-lists-3 §3.2 "Generating Marker Contents", read for which arm an element is on. See list_marker.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
/* Lexbor's property registry, only for the premise check of §3.2's first arm (`lxb_css_property_by_name`). As
   core/layout/box_tree.c argues for its generated enum, a layout unit takes a lookup, not the parser header. */
#include <lexbor/css/property.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_counter_style.h"
#include "core/css/css_shorthand.h"
#include "core/layout/box_subject.h"
#include "core/layout/list_marker.h"

/* A computed value; every property read here has an initial value, so NULL is a cascade failure, asserted.
   The caller frees the result. */
static char *lm_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECKF(v != NULL, "`%s`: the cascade produced no computed value for a property css-lists-3 §3.2 "
                       "\"Generating Marker Contents\" reads off the originating element, and every property "
                       "this entry asks for has an `Initial:` line css-cascade-5 §7.1 \"Initial Values\" can "
                       "fall to", name);
    return v;
}

/* Checks the premise that §3.2's first arm ("content on the ::marker itself is not normal") is unreachable,
   since an upstream Lexbor sync can falsify either half silently (like core/layout/box_tree.c's
   `__UNDEF == 0` static assert):
     - Lexbor's registry has no `content` property, so css-content-3 §1 "Inserting and Replacing Content: the
       content property"' initial value holds ("For ::marker, ::placeholder, and ::file-selector-button, this
       computes to itself (normal).").
     - No pseudo-element selector parses (`lxb_css_selectors_state_pseudo_element` refuses every name; see
       core/css/css_style_declaration.c), so `li::marker { … }` is Selectors 4 §17.1's `failure`.
   Two-sided, with `color` as a control, so a lookup answering NULL for everything is not read as a confirmed
   premise. The guard is inside the body because a release `DCHECK` type-checks its condition, and a
   dev-only predicate would be an implicit declaration under `-Werror=implicit-function-declaration`. */
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

/* Refuses §3.2's second arm ("list-style-image on the originating element defines a marker image") for any
   value but `none`: css-images-3 §2 "Image Values: the <image> type" makes it a question about the image's
   validity, which only a fetch decides (see list_marker.h). The release arm falls through to the third arm,
   which is §2's error handling for an invalid image ("allowing the list-style-type to render in its place"),
   the answer a browser reaches once the load fails; dev refuses deciding that before any load. */
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

/* Refuses a `list-style-type` outside css-lists-3 §3.4 "Text-based Markers: the list-style-type property"'s
   `Value:` line, `<counter-style> | <string> | none`: a guard, since core/css/css_shorthand.c's
   `list_style_longhand_value` is that grammar and drops anything else. Each arm is asked of its owning
   production (core/css/css_counter_style.h; css-values-4 §4.4 "Quoted Strings: the <string> type"'s
   predicate). It makes the entry below sound: "defines a marker string" is read as "is not `none`", true only
   while every other value of the line defines one. */
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

    /* css-lists-3 §3.1 "The ::marker Pseudo-Element"'s last sentence: "Marker boxes only exist for list items: on
       any other element, the ::marker pseudo-element’s content property must compute to none, which suppresses
       its creation." A list item is a box whose computed `display` carries css-display-3 §2.3 "Generating Marker
       Boxes: the list-item keyword"'s flag, whatever its outer and inner types. */
    {
        CssDisplay d = css_computed_display(el);

        if (!(d.kind == CSS_DISPLAY_PAIR && d.list_item)) return false;
    }

    /* §3.2 is ordered ("the first of these conditions that is true"), so each arm is reached only after the
       ones above it are false. */
    lm_require_marker_content_cannot_be_set();

    v = lm_computed(el, "list-style-image");
    if (strcmp(v, "none") != 0) lm_require_marker_image_is_decidable(el, v);
    free(v);

    v = lm_computed(el, "list-style-type");
    lm_require_type_is_in_the_grammar(el, v);
    /* §3.2's third arm, then its last: every value but `none` defines a marker string (css-counter-styles-3 §5;
       see list_marker.h), so false is "The marker box has no contents and ::marker does not generate a box". */
    yes = strcmp(v, "none") != 0;
    free(v);
    return yes;
}

ListMarkerPosition list_marker_position(lxb_dom_element_t *list_item)
{
    char bbuf[160];

    DCHECK(list_item != NULL && list_marker_box_generated(list_item),
           "css-lists-3 §3.5 \"Positioning Markers: The list-style-position property\" was asked where the "
           "marker box of an element that has none is placed — §3.1 generates one only for a list item, and "
           "§3.2's last arm gives some list items none");
    /* §3.5's inline-box arm: a list item whose outer display type is not block. */
    DCHECKF(css_computed_display(list_item).outer == CSS_KEYWORD_BLOCK,
            "%s: css-lists-3 §3.5 reads `outside` as \"equivalent to inside\" for a list item that is an inline "
            "box, and this list item's computed `display` is not block-level — BUILD §3.5's inline-box arm here",
            box_subject(list_item, bbuf, sizeof bbuf));
    (void) bbuf;
    return css_computed_keyword_is(list_item, "list-style-position", CSS_KEYWORD_INSIDE)
               ? LIST_MARKER_POSITION_INSIDE : LIST_MARKER_POSITION_OUTSIDE;
}
