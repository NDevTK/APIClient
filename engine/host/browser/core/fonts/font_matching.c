/* See font_matching.h. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_code_point.h"
#include "core/css/css_font_family.h"
#include "core/fonts/font_matching.h"

/* WHICH FACE EACH GENERIC FAMILY AND EACH ALIAS ANSWERS IS THIS HOST'S FONTCONFIG ANSWER AS CHROME RENDERS IT,
 * not a universal mapping: css-fonts-4 §2.1.2 "Syntax of <generic-font-family>" leaves every generic to the
 * user agent, and §5.2 lets it look a name up among "available installed fonts (this may include font
 * aliases)". Each row is the face Chrome on this project's hosts measures a 100-character string at for that
 * family (the scratch page and the commit that changes a row carry the measurement); a generic absent here
 * (`cursive`, `fantasy`, `math` and the `ui-*` four) is one Chrome falls through to the next family for.
 * Another host's fontconfig answers differently, which is the reader's own installed set and not this table's
 * concern. */
/* NAMED RESIDUAL. Not covered: Chrome's default fixed font size, which gives an element whose family list is
   exactly `monospace` a `medium` of 13px. Next diff builds: that size as a second picked default in
   core/css/font_size_functions.h, chosen by the computed family list. Absence shows as `<pre>` and `<code>`
   computing a 16px `font-size` here where Chrome computes 13px. */
static const struct { const char *generic; InstalledFont face; } GENERIC_FONT_FAMILY_SETTINGS[] = {
    { "serif",      INSTALLED_FONT_LIBERATION_SERIF },
    { "sans-serif", INSTALLED_FONT_LIBERATION_SANS },
    { "monospace",  INSTALLED_FONT_DEJAVU_SANS_MONO },
    { "system-ui",  INSTALLED_FONT_DEJAVU_SANS },
};

/* NAMED RESIDUAL. Not covered: the hosts' metric aliases whose face is not installed here, `Courier New` and
   `Courier` (Liberation Mono), and the host faces this user agent does not ship (DejaVu Serif among them).
   Next diff builds: Liberation Mono as an installed face and its two rows. Absence shows as a `"Courier New",
   monospace` run measuring at DejaVu Sans Mono's advances where Chrome measures Liberation Mono's. */
static const struct { const char *name; InstalledFont face; } FONT_ALIAS[] = {
    { "times new roman", INSTALLED_FONT_LIBERATION_SERIF },
    { "times",           INSTALLED_FONT_LIBERATION_SERIF },
    { "arial",           INSTALLED_FONT_LIBERATION_SANS },
    { "helvetica",       INSTALLED_FONT_LIBERATION_SANS },
};

/* §5.2's "user agent's default font if none are available": the face of this user agent's initial family
   (core/css/css_style_declaration.c's `font-family` initial value), which is Chrome's default font here. */
#define FONT_MATCHING_DEFAULT INSTALLED_FONT_LIBERATION_SERIF

/* The installed font fallback's order: the broadest repertoire first, then the rest of the installed set. */
static const InstalledFont FONT_FALLBACK_ORDER[INSTALLED_FONT_COUNT] = {
    INSTALLED_FONT_DEJAVU_SANS, INSTALLED_FONT_LIBERATION_SERIF, INSTALLED_FONT_LIBERATION_SANS,
    INSTALLED_FONT_DEJAVU_SANS_MONO,
};

/* Unicode's case folding (CaseFolding.txt, status C and F) for the non-ASCII code points whose folding is
   ASCII. §5.1 "Localized name matching" matches "using the "Default Caseless Matching" algorithm" with "the
   case mappings with status field "C" or "F"", "without normalizing the strings involved". Every installed
   name and alias is ASCII, so a name matches only if each of its code points folds to ASCII: these rows, the
   ASCII letters, and nothing else. Re-derived from the UCD file by keeping the C and F rows whose mapping is
   all below U+0080 and whose code point is not. */
static const struct { uint32_t cp; const char *fold; } CASE_FOLD_TO_ASCII[] = {
    { 0x00DF, "ss" }, { 0x017F, "s" }, { 0x1E9E, "ss" }, { 0x212A, "k" },
    { 0xFB00, "ff" }, { 0xFB01, "fi" }, { 0xFB02, "fl" }, { 0xFB03, "ffi" }, { 0xFB04, "ffl" },
    { 0xFB05, "st" }, { 0xFB06, "st" }, { 0x1DF95, "ss" },
};

/* `name`'s full case folding into `out` (NUL-terminated, `cap` bytes), true when every code point folds to
   ASCII. A name that does not is no installed name, so false is a non-match and not an error. */
static bool fm_fold_ascii(const char *name, char *out, size_t cap)
{
    const char *p = name, *end = name + strlen(name);
    size_t n = 0;

    while (p < end) {
        size_t len = 0, i;
        uint32_t cp = css_cp_at(p, end, &len);
        const char *fold = NULL;
        char one[2] = { 0, 0 };

        p += len;
        if (cp < 0x80) {
            one[0] = (char)(cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp);
            fold = one;
        } else {
            for (i = 0; i < sizeof(CASE_FOLD_TO_ASCII) / sizeof(CASE_FOLD_TO_ASCII[0]); i++)
                if (CASE_FOLD_TO_ASCII[i].cp == cp) fold = CASE_FOLD_TO_ASCII[i].fold;
            if (fold == NULL) return false;
        }
        if (n + strlen(fold) + 1 > cap) return false;   /* longer than every installed name */
        memcpy(out + n, fold, strlen(fold));
        n += strlen(fold);
    }
    out[n] = '\0';
    return true;
}

/* One family name of the list against the installed set by §5.1, its own family names first and then the
   aliases. False when it names no installed face, which §5.2 answers by moving to the next family. */
static bool fm_match_name(const char *name, InstalledFont *out)
{
    char folded[64], installed[64];
    size_t i;
    int f;

    if (!fm_fold_ascii(name, folded, sizeof folded)) return false;
    for (f = 0; f < INSTALLED_FONT_COUNT; f++) {
        bool ascii = fm_fold_ascii(installed_font_family((InstalledFont)f), installed, sizeof installed);

        DCHECKF(ascii, "installed font %s has a family name that is not ASCII, which §5.1's match here assumes",
                installed_font_family((InstalledFont)f));
        if (strcmp(folded, installed) == 0) { *out = (InstalledFont)f; return true; }
    }
    for (i = 0; i < sizeof(FONT_ALIAS) / sizeof(FONT_ALIAS[0]); i++)
        if (strcmp(folded, FONT_ALIAS[i].name) == 0) { *out = FONT_ALIAS[i].face; return true; }
    return false;
}

static bool fm_match_generic(const char *generic, InstalledFont *out)
{
    size_t i;

    for (i = 0; i < sizeof(GENERIC_FONT_FAMILY_SETTINGS) / sizeof(GENERIC_FONT_FAMILY_SETTINGS[0]); i++)
        if (strcmp(generic, GENERIC_FONT_FAMILY_SETTINGS[i].generic) == 0) {
            *out = GENERIC_FONT_FAMILY_SETTINGS[i].face;
            return true;
        }
    return false;
}

void font_matching_resolve(const char *computed_family, FontFallbackList *out)
{
    CssFontFamilyList list = { 0 };
    bool parsed;
    size_t i;

    DCHECK(computed_family != NULL && out != NULL, "css-fonts-4 §5.2's family walk was asked of no value");
    out->n = 0;
    parsed = css_font_family_list(computed_family, &list);
    DCHECKF(parsed, "an element's computed `font-family` `%s` is outside css-fonts-4 §2.1's grammar. Its "
            "computed value is its specified value, which the cascade parsed by that grammar, so this is a "
            "derivation that let a value through which it should have refused", computed_family);
    for (i = 0; i < list.n; i++) {
        InstalledFont face;
        bool found = list.v[i].generic ? fm_match_generic(list.v[i].generic, &face)
                                       : fm_match_name(list.v[i].name, &face);
        unsigned k;

        if (!found) continue;
        for (k = 0; k < out->n && out->face[k] != face; k++) {}
        if (k == out->n) out->face[out->n++] = face;
    }
    css_font_family_list_free(&list);
    /* §5.2's "user agent's default font if none are available": a list that matched nothing renders with the
       default font, its characters included, before the installed font fallback is asked. */
    if (out->n == 0) out->face[out->n++] = FONT_MATCHING_DEFAULT;
}

InstalledFont font_matching_first_available(const FontFallbackList *l)
{
    DCHECK(l != NULL && l->n > 0 && l->n <= INSTALLED_FONT_COUNT,
           "a fallback list holds no face, or more than are installed — font_matching_resolve gives a list that "
           "matched nothing the default font");
    return l->face[0];
}

InstalledFont font_matching_font_for_character(const FontFallbackList *l, uint32_t cp)
{
    unsigned i;

    DCHECK(cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF),
           "css-fonts-4 §5.2 was asked for a face for something that is not a Unicode scalar value");
    for (i = 0; i < l->n; i++)
        if (open_type_metrics_covers(installed_font_metrics(l->face[i]), cp)) return l->face[i];
    /* §5.2: "If there are no more font families to be evaluated and no matching face has been found, then the
       user agent performs an installed font fallback procedure to find the best match for the character". */
    for (i = 0; i < INSTALLED_FONT_COUNT; i++)
        if (open_type_metrics_covers(installed_font_metrics(FONT_FALLBACK_ORDER[i]), cp))
            return FONT_FALLBACK_ORDER[i];
    return font_matching_first_available(l);
}
