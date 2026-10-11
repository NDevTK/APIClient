/* css-align-3's alignment value — see core/css/css_align.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_align.h"

#define CSS_ALIGN_MAX_TOKENS 3

bool css_align_property(CssPropertyId id)
{
    return id == CSS_PROPERTY_ALIGN_CONTENT || id == CSS_PROPERTY_JUSTIFY_CONTENT ||
           id == CSS_PROPERTY_ALIGN_ITEMS || id == CSS_PROPERTY_JUSTIFY_ITEMS || id == CSS_PROPERTY_ALIGN_SELF ||
           id == CSS_PROPERTY_JUSTIFY_SELF;
}

/* The whitespace-separated keywords of `text`, at most CSS_ALIGN_MAX_TOKENS. -1 for more, or for a word that
   is no keyword at all. */
static int css_align_tokens(const char *text, CssKeyword *out)
{
    const char *p = text;
    int n = 0;

    for (;;) {
        const char *b;

        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        if (*p == '\0') return n;
        if (n == CSS_ALIGN_MAX_TOKENS) return -1;
        b = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) p++;
        out[n] = css_keyword_of(b, (size_t)(p - b));
        if (out[n] == CSS_KEYWORD_INVALID) return -1;
        n++;
    }
}

static CssAlign css_align_of(CssKeyword value)
{
    CssAlign a = { value, CSS_KEYWORD_INVALID, CSS_KEYWORD_INVALID, false };

    return a;
}

/* css-align-3 §4.2's `<baseline-position> = [ first | last ]? && baseline`; a bare `baseline` "Computes to first
   baseline". */
static bool css_align_baseline(const CssKeyword *t, int n, CssAlign *out)
{
    CssKeyword pref;

    if (n == 1 && t[0] == CSS_KEYWORD_BASELINE) pref = CSS_KEYWORD_FIRST;
    else if (n == 2 && t[1] == CSS_KEYWORD_BASELINE && (t[0] == CSS_KEYWORD_FIRST || t[0] == CSS_KEYWORD_LAST))
        pref = t[0];
    else if (n == 2 && t[0] == CSS_KEYWORD_BASELINE && (t[1] == CSS_KEYWORD_FIRST || t[1] == CSS_KEYWORD_LAST))
        pref = t[1];
    else
        return false;
    *out = css_align_of(CSS_KEYWORD_BASELINE);
    out->baseline = pref;
    return true;
}

/* `<overflow-position>? X`, where X is one keyword of `set`, or `left`/`right` where `left_right`, or `normal`
   where `normal_too`. css-align-3 §4.4's `<overflow-position> = unsafe | safe` precedes the position. */
static bool css_align_positional(const CssKeyword *t, int n, CssKeywordSet set, bool left_right, bool normal_too,
                                 CssAlign *out)
{
    CssKeyword overflow = CSS_KEYWORD_INVALID, v;

    if (n == 2 && css_keyword_set_has(CSS_KEYWORDS_OVERFLOW_POSITION, t[0])) {
        overflow = t[0];
        t++;
        n--;
    }
    if (n != 1) return false;
    v = t[0];
    if (!(css_keyword_set_has(set, v) || (left_right && (v == CSS_KEYWORD_LEFT || v == CSS_KEYWORD_RIGHT)) ||
          (normal_too && v == CSS_KEYWORD_NORMAL)))
        return false;
    *out = css_align_of(v);
    out->overflow = overflow;
    return true;
}

static bool css_align_one(const CssKeyword *t, int n, CssKeyword a, CssKeyword b, CssKeyword c, CssAlign *out)
{
    if (n != 1 || t[0] == CSS_KEYWORD_INVALID || (t[0] != a && t[0] != b && t[0] != c)) return false;
    *out = css_align_of(t[0]);
    return true;
}

bool css_align_parse(CssPropertyId id, const char *text, CssAlign *out)
{
    CssKeyword t[CSS_ALIGN_MAX_TOKENS];
    const CssKeyword X = CSS_KEYWORD_INVALID;
    int n;

    DCHECKF(css_align_property(id) && text != NULL && out != NULL,
            "an alignment value was parsed for row %u, which is none of css-align-3's six, or from no text",
            (unsigned)id);
    n = css_align_tokens(text, t);
    if (n <= 0) return false;
    switch (id) {
    /* css-align-3 §5.1 "The justify-content and align-content Properties": `normal | <baseline-position> |
       <content-distribution> | <overflow-position>? <content-position>`. */
    case CSS_PROPERTY_ALIGN_CONTENT:
        if (css_align_one(t, n, CSS_KEYWORD_NORMAL, X, X, out) || css_align_baseline(t, n, out)) return true;
        if (n == 1 && css_keyword_set_has(CSS_KEYWORDS_CONTENT_DISTRIBUTION, t[0])) {
            *out = css_align_of(t[0]);
            return true;
        }
        return css_align_positional(t, n, CSS_KEYWORDS_CONTENT_POSITION, false, false, out);
    /* css-align-3 §5.1: `normal | <content-distribution> | <overflow-position>? [ <content-position> | left |
       right ]`. */
    case CSS_PROPERTY_JUSTIFY_CONTENT:
        if (css_align_one(t, n, CSS_KEYWORD_NORMAL, X, X, out)) return true;
        if (n == 1 && css_keyword_set_has(CSS_KEYWORDS_CONTENT_DISTRIBUTION, t[0])) {
            *out = css_align_of(t[0]);
            return true;
        }
        return css_align_positional(t, n, CSS_KEYWORDS_CONTENT_POSITION, true, false, out);
    /* css-align-3 §7.2 "Block-Axis (or Cross-Axis) Default Alignment: the align-items property": `normal |
       stretch | <baseline-position> | <overflow-position>? <self-position>`. */
    case CSS_PROPERTY_ALIGN_ITEMS:
        if (css_align_one(t, n, CSS_KEYWORD_NORMAL, CSS_KEYWORD_STRETCH, X, out) || css_align_baseline(t, n, out))
            return true;
        return css_align_positional(t, n, CSS_KEYWORDS_SELF_POSITION, false, false, out);
    /* css-align-3 §7.1 "Inline-Axis (or Main-Axis) Default Alignment: the justify-items property": `normal |
       stretch | <baseline-position> | <overflow-position>? [ <self-position> | left | right ] | legacy | legacy
       && [ left | right | center ]`. */
    case CSS_PROPERTY_JUSTIFY_ITEMS:
        if (css_align_one(t, n, CSS_KEYWORD_NORMAL, CSS_KEYWORD_STRETCH, CSS_KEYWORD_LEGACY, out) ||
            css_align_baseline(t, n, out))
            return true;
        if (n == 2 && (t[0] == CSS_KEYWORD_LEGACY || t[1] == CSS_KEYWORD_LEGACY)) {
            CssKeyword v = t[0] == CSS_KEYWORD_LEGACY ? t[1] : t[0];

            if (v != CSS_KEYWORD_LEFT && v != CSS_KEYWORD_RIGHT && v != CSS_KEYWORD_CENTER) return false;
            *out = css_align_of(v);
            out->legacy = true;
            return true;
        }
        return css_align_positional(t, n, CSS_KEYWORDS_SELF_POSITION, true, false, out);
    /* css-align-3 §6.2 "Block-Axis (or Cross-Axis) Self-Alignment: the align-self property": `auto |
       <overflow-position>? [ normal | <self-position> ] | stretch | <baseline-position>`. */
    case CSS_PROPERTY_ALIGN_SELF:
        if (css_align_one(t, n, CSS_KEYWORD_AUTO, CSS_KEYWORD_STRETCH, X, out) || css_align_baseline(t, n, out))
            return true;
        return css_align_positional(t, n, CSS_KEYWORDS_SELF_POSITION, false, true, out);
    /* css-align-3 §6.1 "Inline-Axis (or Main-Axis) Self-Alignment: the justify-self property": `auto |
       <overflow-position>? [ normal | <self-position> | left | right ] | stretch | <baseline-position>`. */
    case CSS_PROPERTY_JUSTIFY_SELF:
        if (css_align_one(t, n, CSS_KEYWORD_AUTO, CSS_KEYWORD_STRETCH, X, out) || css_align_baseline(t, n, out))
            return true;
        return css_align_positional(t, n, CSS_KEYWORDS_SELF_POSITION, true, true, out);
    default:
        DFAILF("row %u passed css_align_property and has no grammar arm", (unsigned)id);
        return false;
    }
}

char *css_align_serialize(CssAlign a)
{
    char buf[64], *out;

    DCHECK(a.value != CSS_KEYWORD_INVALID, "an alignment value with no keyword was serialized");
    if (a.value == CSS_KEYWORD_BASELINE)
        snprintf(buf, sizeof buf, "%s", a.baseline == CSS_KEYWORD_LAST ? "last baseline" : "baseline");
    else if (a.legacy)
        snprintf(buf, sizeof buf, "legacy %s", css_keyword_name(a.value));
    else if (a.overflow != CSS_KEYWORD_INVALID)
        snprintf(buf, sizeof buf, "%s %s", css_keyword_name(a.overflow), css_keyword_name(a.value));
    else
        snprintf(buf, sizeof buf, "%s", css_keyword_name(a.value));
    out = strdup(buf);
    CHECK(out != NULL, "css-align-3: OOM serializing an alignment value");
    return out;
}
