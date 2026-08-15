/* THE UA COLOUR THEME — CSS Color 4 §6.2's `<system-color>` keywords. See css_system_color.h.
 *
 * WHERE THE NUMBERS COME FROM. §6.2 defines each keyword by the UI ROLE it names — "Background of application
 * content or documents", "The base border color for push buttons" — and deliberately does not print numbers,
 * because the values are the UA's. So the table below is this UA's answer, chosen from the traditional desktop
 * colours the roles have carried since the first CSS UA stylesheets (a white document canvas with black text,
 * §18's `:link`/`:visited`/`:active` blue/purple/red, a yellow `mark`, a grey disabled text, a blue accent and
 * selection with white text on it) and then MOVED where the tradition fails §6.2's contrast requirement. Two
 * were: the traditional pure `#ff0000` active-link red measures 4.00:1 on white and the traditional bright
 * `#3399ff` selection blue measures 2.94:1 under white text, both below WCAG AA's 4.5:1 for normal text. The
 * darker `#ee0000` and `#0060df` clear it at 4.53:1 and 5.62:1 and keep the same hue. That is the point of
 * asserting the requirement rather than reciting a palette: it is what caught them. */
#include <math.h>

#include "check.h"
#include "core/css/css_system_color.h"

/* One entry per §6.2 keyword, indexed by CssSystemColorId, as the packed sRGB byte triple the roles are
   traditionally written in. The comment on each is the spec's own definition of the role, because the role is
   what a future edit has to keep answering — the number is only this UA's current answer to it. */
static const unsigned CSS_SYSTEM_COLOR_THEME[CSS_SYSTEM_COLOR__COUNT] = {
    0x0060df,   /* AccentColor       — background of accented user interface controls. §6.2 also ties this to
                                        `accent-color`, whose initial `auto` is the UA's own accent: this. */
    0xffffff,   /* AccentColorText   — text of accented controls; §6.2's "contrasting foreground to AccentColor". */
    0xee0000,   /* ActiveText        — text in active links; "for light backgrounds, traditionally red". */
    0x767676,   /* ButtonBorder      — the base border color for push buttons. */
    0xefefef,   /* ButtonFace        — the face background color for push buttons. */
    0x000000,   /* ButtonText        — text on push buttons. */
    0xffffff,   /* Canvas            — background of application content or documents. */
    0x000000,   /* CanvasText        — text in application content or documents. */
    0xffffff,   /* Field             — background of input fields. */
    0x000000,   /* FieldText         — text in input fields. */
    0x6d6d6d,   /* GrayText          — disabled text; "often, but not necessarily, gray". */
    0x0060df,   /* Highlight         — background of selected text, for example from ::selection. */
    0xffffff,   /* HighlightText     — text of selected text. */
    0x0000ee,   /* LinkText          — text in non-active, non-visited links; "traditionally blue". */
    0xffff00,   /* Mark              — background of text specially marked, such as by HTML's `mark`. */
    0x000000,   /* MarkText          — text that has been specially marked. */
    0x0060df,   /* SelectedItem      — background of selected items, for example a selected checkbox. */
    0xffffff,   /* SelectedItemText  — text of selected items. */
    0x551a8b    /* VisitedText       — text in visited links; "traditionally purple". */
};

#if APICLIENT_DEV
/* WCAG 2.2's RELATIVE LUMINANCE of an sRGB colour: L = 0.2126·R + 0.7152·G + 0.0722·B over components
   linearized by the sRGB transfer function. WCAG's own note records that its threshold was corrected from
   0.03928 to 0.04045 in May 2021 to match [SRGB], so this is the sRGB curve exactly and not a near-miss of it. */
static double css_wcag_luminance(unsigned rgb)
{
    static const double W[3] = { 0.2126, 0.7152, 0.0722 };
    double l = 0.0;
    int i;

    for (i = 0; i < 3; i++) {
        double c = ((rgb >> (16 - i * 8)) & 0xffu) / 255.0;

        l += W[i] * (c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
    }
    return l;
}

/* WCAG 2.2's CONTRAST RATIO: (L1 + 0.05) / (L2 + 0.05) with L1 the lighter of the two. */
static double css_wcag_contrast(unsigned a, unsigned b)
{
    double la = css_wcag_luminance(a), lb = css_wcag_luminance(b), t;

    if (la < lb) { t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

/* §6.2'S OWN LIST OF PAIRINGS, as the invariant the theme is held to. `floor` is what the pairing has to
   reach: WCAG AA's 4.5:1 for the pairs §6.2 states as foreground TEXT on a background, and 1.4.11 Non-text
   Contrast's 3:1 for the two things that are not text — ButtonBorder, which §6.2 lists as a BORDER against a
   background and an adjacent colour, and GrayText, which §6.2 separately says is "expected to be readable,
   though possibly at a lower contrast rating, over any of the backgrounds".
   GrayText is checked against the three document/control backgrounds only, and that is arithmetic rather than
   a shrug: to clear 3:1 against Canvas (#ffffff, luminance 1) a grey needs luminance at most 0.30, and to
   clear 3:1 against AccentColor (luminance 0.137) it needs at least 0.51, so NO single grey can satisfy both.
   §6.2's "expected to be readable ... over any of the backgrounds" is unreachable for a fixed palette, which
   is why the spec words it as an expectation and this list does not pretend otherwise. */
typedef struct { CssSystemColorId bg, fg; double floor_ratio; const char *what; } CssSystemColorPair;

static const CssSystemColorPair CSS_SYSTEM_COLOR_PAIRS[] = {
    { CSS_SYSTEM_COLOR_CANVAS,       CSS_SYSTEM_COLOR_CANVASTEXT,       4.5, "Canvas with CanvasText" },
    { CSS_SYSTEM_COLOR_CANVAS,       CSS_SYSTEM_COLOR_LINKTEXT,         4.5, "Canvas with LinkText" },
    { CSS_SYSTEM_COLOR_CANVAS,       CSS_SYSTEM_COLOR_VISITEDTEXT,      4.5, "Canvas with VisitedText" },
    { CSS_SYSTEM_COLOR_CANVAS,       CSS_SYSTEM_COLOR_ACTIVETEXT,       4.5, "Canvas with ActiveText" },
    { CSS_SYSTEM_COLOR_BUTTONFACE,   CSS_SYSTEM_COLOR_BUTTONTEXT,       4.5, "ButtonFace with ButtonText" },
    { CSS_SYSTEM_COLOR_FIELD,        CSS_SYSTEM_COLOR_FIELDTEXT,        4.5, "Field with FieldText" },
    { CSS_SYSTEM_COLOR_MARK,         CSS_SYSTEM_COLOR_MARKTEXT,         4.5, "Mark with MarkText" },
    { CSS_SYSTEM_COLOR_HIGHLIGHT,    CSS_SYSTEM_COLOR_HIGHLIGHTTEXT,    4.5, "Highlight with HighlightText" },
    { CSS_SYSTEM_COLOR_SELECTEDITEM, CSS_SYSTEM_COLOR_SELECTEDITEMTEXT, 4.5, "SelectedItem with SelectedItemText" },
    { CSS_SYSTEM_COLOR_ACCENTCOLOR,  CSS_SYSTEM_COLOR_ACCENTCOLORTEXT,  4.5, "AccentColor with AccentColorText" },
    { CSS_SYSTEM_COLOR_CANVAS,       CSS_SYSTEM_COLOR_BUTTONBORDER,     3.0, "Canvas with a ButtonBorder border" },
    { CSS_SYSTEM_COLOR_BUTTONFACE,   CSS_SYSTEM_COLOR_BUTTONBORDER,     3.0, "ButtonFace with a ButtonBorder border" },
    { CSS_SYSTEM_COLOR_FIELD,        CSS_SYSTEM_COLOR_BUTTONBORDER,     3.0, "Field with a ButtonBorder border" },
    { CSS_SYSTEM_COLOR_CANVAS,       CSS_SYSTEM_COLOR_GRAYTEXT,         3.0, "Canvas with GrayText" },
    { CSS_SYSTEM_COLOR_BUTTONFACE,   CSS_SYSTEM_COLOR_GRAYTEXT,         3.0, "ButtonFace with GrayText" },
    { CSS_SYSTEM_COLOR_FIELD,        CSS_SYSTEM_COLOR_GRAYTEXT,         3.0, "Field with GrayText" },
    { CSS_SYSTEM_COLOR_MARK,         CSS_SYSTEM_COLOR_GRAYTEXT,         3.0, "Mark with GrayText" }
};

static void css_system_color_check_theme(void)
{
    size_t i;

    for (i = 0; i < sizeof CSS_SYSTEM_COLOR_PAIRS / sizeof CSS_SYSTEM_COLOR_PAIRS[0]; i++) {
        const CssSystemColorPair *p = &CSS_SYSTEM_COLOR_PAIRS[i];

        DCHECK(css_wcag_contrast(CSS_SYSTEM_COLOR_THEME[p->bg], CSS_SYSTEM_COLOR_THEME[p->fg])
               >= p->floor_ratio,
               "a pairing CSS Color 4 §6.2 lists as legible is below its WCAG contrast floor in this UA's "
               "colour theme — §6.2 requires the browser to ensure matching foreground/background pairs have "
               "a minimum of WCAG AA contrast, so move the value that changed until the pair clears it");
    }
}
#endif

CssColor css_system_color(CssSystemColorId id)
{
    CssColor c;
    unsigned rgb;

    DCHECK((unsigned)id < (unsigned)CSS_SYSTEM_COLOR__COUNT,
           "a <system-color> id outside the nineteen CSS Color 4 §6.2 defines reached the UA colour theme — "
           "the keyword-to-id mapping produced an id this table has no entry for");
#if APICLIENT_DEV
    /* AT THE READ, not at a first-use initialiser. A lazily-checked table is one whichever caller happened to
       arrive first validated, and the whole table is const data with no initialisation step to hang this on;
       checking here costs nothing in release, where the whole function body above vanishes. */
    css_system_color_check_theme();
#endif
    rgb = CSS_SYSTEM_COLOR_THEME[id];
    c.r = ((rgb >> 16) & 0xffu) / 255.0;
    c.g = ((rgb >> 8) & 0xffu) / 255.0;
    c.b = (rgb & 0xffu) / 255.0;
    c.a = 1.0;
    return c;
}
