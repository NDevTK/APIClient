/* THE UA COLOUR THEME — the nineteen `<system-color>` keywords of CSS Color Module Level 4 §6.2.
 *
 * A HEADLESS UA STILL HAS A COLOUR THEME, AND THIS ENGINE IS THE UA. §6.2 says the keywords "reflect default
 * color choices made by the user, the browser, or the OS"; there is no user and no OS here, so every one of
 * them is a BROWSER choice, and a browser choice is a value this component simply has. The spec then makes the
 * fixed answer the recommended one rather than a concession: "User agents may, to mitigate privacy and
 * security risks such as fingerprinting, elect to return fixed values for the used value of system colors
 * which do not reflect customisation or theming choices made by the user." So a fixed table is the spec's own
 * privacy-preserving option, not a stand-in for a missing display.
 *
 * ONE THEME, BECAUSE THE COLOUR SCHEME RESOLVES TO ONE. §6.2's keywords "respond to the element color scheme";
 * `color-scheme`'s initial value is `normal` (CSS Color Adjust 1 §2.2), which gives a null colour-scheme
 * support, and §2.3's `used color scheme` returns the UA DEFAULT colour scheme for a null support. This UA's
 * default colour scheme is light, so the table below is the light theme and there is exactly one of it. It is
 * a module static and that is correct rather than the defect CLAUDE.md's per-realm rule names: the UA's own
 * theme is one fact about the UA, identical in every realm by construction, not one realm's answer handed to
 * another's question.
 *
 * THE VALUES ARE HELD TO §6.2'S CONTRAST REQUIREMENT, WHICH IS WHY THEY ARE CHECKABLE. §6.2 requires that
 * "when the values of <system-color> keywords come from the browser ... the browser should ensure that
 * matching foreground/background pairs have a minimum of WCAG AA contrast", and then LISTS the pairs it means.
 * That list is this component's invariant: the theme is asserted against it at every read, in a DEV build,
 * using WCAG 2.2's own relative-luminance and contrast-ratio definitions. Without that assertion the table is
 * nineteen numbers nothing can be wrong about; with it, an edit that makes a pair illegible crashes naming the
 * pair. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SYSTEM_COLOR_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SYSTEM_COLOR_H
#include "core/css/css_color.h"

/* §6.2's nineteen keywords, in the order the spec defines them (alphabetical, as the spec lists them). The
   `_COUNT` member is what makes "the table has an entry for every keyword" a thing a DCHECK can state. */
typedef enum {
    CSS_SYSTEM_COLOR_ACCENTCOLOR = 0,
    CSS_SYSTEM_COLOR_ACCENTCOLORTEXT,
    CSS_SYSTEM_COLOR_ACTIVETEXT,
    CSS_SYSTEM_COLOR_BUTTONBORDER,
    CSS_SYSTEM_COLOR_BUTTONFACE,
    CSS_SYSTEM_COLOR_BUTTONTEXT,
    CSS_SYSTEM_COLOR_CANVAS,
    CSS_SYSTEM_COLOR_CANVASTEXT,
    CSS_SYSTEM_COLOR_FIELD,
    CSS_SYSTEM_COLOR_FIELDTEXT,
    CSS_SYSTEM_COLOR_GRAYTEXT,
    CSS_SYSTEM_COLOR_HIGHLIGHT,
    CSS_SYSTEM_COLOR_HIGHLIGHTTEXT,
    CSS_SYSTEM_COLOR_LINKTEXT,
    CSS_SYSTEM_COLOR_MARK,
    CSS_SYSTEM_COLOR_MARKTEXT,
    CSS_SYSTEM_COLOR_SELECTEDITEM,
    CSS_SYSTEM_COLOR_SELECTEDITEMTEXT,
    CSS_SYSTEM_COLOR_VISITEDTEXT,
    CSS_SYSTEM_COLOR__COUNT
} CssSystemColorId;

/* §15.5's used value for one `<system-color>`: "the corresponding color in its color space". Opaque, and in
   sRGB — every colour in this UA's theme is an sRGB colour, so the alpha the caller then pairs with it (§15.5
   keeps the specified one) is the only component a caller has to supply. */
CssColor css_system_color(CssSystemColorId id);

#endif
