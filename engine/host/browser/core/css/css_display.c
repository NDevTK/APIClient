/* css-display-3 §2's display value — see core/css/css_display.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_display.h"

/* §2's summary table, the rows whose "Short display" is one keyword: §2.6's precomposed `<display-legacy>`
   values and the single keywords §2.1's and §2.2's defaults expand. The parse reads it one way and the
   serialization the other, so the two cannot disagree about a spelling. */
static const struct { CssKeyword shortk, outer, inner; } CSS_DISPLAY_SHORT[] = {
    { CSS_KEYWORD_BLOCK,        CSS_KEYWORD_BLOCK,  CSS_KEYWORD_FLOW },
    { CSS_KEYWORD_FLOW_ROOT,    CSS_KEYWORD_BLOCK,  CSS_KEYWORD_FLOW_ROOT },
    { CSS_KEYWORD_INLINE,       CSS_KEYWORD_INLINE, CSS_KEYWORD_FLOW },
    { CSS_KEYWORD_INLINE_BLOCK, CSS_KEYWORD_INLINE, CSS_KEYWORD_FLOW_ROOT },
    { CSS_KEYWORD_RUN_IN,       CSS_KEYWORD_RUN_IN, CSS_KEYWORD_FLOW },
    { CSS_KEYWORD_TABLE,        CSS_KEYWORD_BLOCK,  CSS_KEYWORD_TABLE },
    { CSS_KEYWORD_INLINE_TABLE, CSS_KEYWORD_INLINE, CSS_KEYWORD_TABLE },
    { CSS_KEYWORD_FLEX,         CSS_KEYWORD_BLOCK,  CSS_KEYWORD_FLEX },
    { CSS_KEYWORD_INLINE_FLEX,  CSS_KEYWORD_INLINE, CSS_KEYWORD_FLEX },
    { CSS_KEYWORD_GRID,         CSS_KEYWORD_BLOCK,  CSS_KEYWORD_GRID },
    { CSS_KEYWORD_INLINE_GRID,  CSS_KEYWORD_INLINE, CSS_KEYWORD_GRID },
    { CSS_KEYWORD_RUBY,         CSS_KEYWORD_INLINE, CSS_KEYWORD_RUBY },
};

#define CSS_DISPLAY_N(t) (sizeof(t) / sizeof((t)[0]))
#define CSS_DISPLAY_MAX_TOKENS 3

static CssDisplay css_display_pair(CssKeyword outer, CssKeyword inner, bool list_item)
{
    CssDisplay d = { CSS_DISPLAY_PAIR, outer, inner, list_item, CSS_KEYWORD_INVALID };

    return d;
}

static CssDisplay css_display_keyword(CssDisplayKind kind, CssKeyword k)
{
    CssDisplay d = { kind, CSS_KEYWORD_INVALID, CSS_KEYWORD_INVALID, false, k };

    return d;
}

/* The whitespace-separated keywords of `text`, at most CSS_DISPLAY_MAX_TOKENS of them. -1 for more, or for
   a token that is no keyword at all. */
static int css_display_tokens(const char *text, CssKeyword *out)
{
    const char *p = text;
    int n = 0;

    for (;;) {
        const char *b;

        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        if (*p == '\0') return n;
        if (n == CSS_DISPLAY_MAX_TOKENS) return -1;
        b = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) p++;
        out[n] = css_keyword_of(b, (size_t)(p - b));
        if (out[n] == CSS_KEYWORD_INVALID) return -1;
        n++;
    }
}

bool css_display_parse(const char *text, CssDisplay *out)
{
    CssKeyword t[CSS_DISPLAY_MAX_TOKENS], outer = CSS_KEYWORD_INVALID, inner = CSS_KEYWORD_INVALID;
    bool list_item = false;
    int n, i;
    unsigned k;

    DCHECK(text != NULL && out != NULL, "a display value was parsed from no text or into nowhere");
    n = css_display_tokens(text, t);
    if (n <= 0) return false;
    if (n == 1) {
        if (css_keyword_set_has(CSS_KEYWORDS_DISPLAY_INTERNAL, t[0])) {
            *out = css_display_keyword(CSS_DISPLAY_INTERNAL, t[0]);
            return true;
        }
        if (css_keyword_set_has(CSS_KEYWORDS_DISPLAY_BOX, t[0])) {
            *out = css_display_keyword(CSS_DISPLAY_BOX, t[0]);
            return true;
        }
        /* §2.6's precomposed values and §2.1's and §2.2's single keywords. */
        for (k = 0; k < CSS_DISPLAY_N(CSS_DISPLAY_SHORT); k++)
            if (CSS_DISPLAY_SHORT[k].shortk == t[0]) {
                *out = css_display_pair(CSS_DISPLAY_SHORT[k].outer, CSS_DISPLAY_SHORT[k].inner, false);
                return true;
            }
    }
    /* `[ <display-outside> || <display-inside> ] | <display-listitem>`, where `<display-listitem> =
       <display-outside>? && [ flow | flow-root ]? && list-item`: each component at most once, in any order. */
    for (i = 0; i < n; i++) {
        if (t[i] == CSS_KEYWORD_LIST_ITEM && !list_item) list_item = true;
        else if (css_keyword_set_has(CSS_KEYWORDS_DISPLAY_OUTSIDE, t[i]) && outer == CSS_KEYWORD_INVALID)
            outer = t[i];
        else if (css_keyword_set_has(CSS_KEYWORDS_DISPLAY_INSIDE, t[i]) && inner == CSS_KEYWORD_INVALID)
            inner = t[i];
        else
            return false;
    }
    if (list_item && inner != CSS_KEYWORD_INVALID && inner != CSS_KEYWORD_FLOW && inner != CSS_KEYWORD_FLOW_ROOT)
        return false;
    /* §2.1: "If a <display-outside> value is specified but <display-inside> is omitted, the element’s inner
       display type defaults to flow." §2.2: "If a <display-inside> value is specified but <display-outside> is
       omitted, the element’s outer display type defaults to block—​except for ruby, which defaults to inline."
       §2.3 states the same two defaults for `list-item`. */
    if (inner == CSS_KEYWORD_INVALID) inner = CSS_KEYWORD_FLOW;
    if (outer == CSS_KEYWORD_INVALID) outer = inner == CSS_KEYWORD_RUBY ? CSS_KEYWORD_INLINE : CSS_KEYWORD_BLOCK;
    *out = css_display_pair(outer, inner, list_item);
    return true;
}

CssDisplay css_display_blockify(CssDisplay d)
{
    switch (d.kind) {
    /* "This has no effect on display types that generate no box at all, such as none or contents." */
    case CSS_DISPLAY_BOX:
        return d;
    /* "If a layout-internal box is blockified, its inner display type converts to flow so that it becomes a
       block container." */
    case CSS_DISPLAY_INTERNAL:
        return css_display_pair(CSS_KEYWORD_BLOCK, CSS_KEYWORD_FLOW, false);
    /* "Sets the box’s computed outer display type to block", and "if an inline block box (inline flow-root) is
       blockified, it becomes a block box (losing its flow-root nature). For consistency, a run-in flow-root box
       also blockifies to a block box." */
    case CSS_DISPLAY_PAIR:
        if (d.inner == CSS_KEYWORD_FLOW_ROOT && d.outer != CSS_KEYWORD_BLOCK) d.inner = CSS_KEYWORD_FLOW;
        d.outer = CSS_KEYWORD_BLOCK;
        return d;
    }
    DFAILF("a display value of kind %u, none of the three css_display.h declares", (unsigned)d.kind);
    return d;
}

static char *css_display_strdup(const char *s)
{
    char *out = strdup(s);

    CHECK(out != NULL, "css-display-3 §2: OOM serializing a display value");
    return out;
}

char *css_display_serialize(CssDisplay d)
{
    char buf[64];
    unsigned k;

    switch (d.kind) {
    case CSS_DISPLAY_INTERNAL:
    case CSS_DISPLAY_BOX:
        return css_display_strdup(css_keyword_name(d.keyword));
    case CSS_DISPLAY_PAIR:
        break;
    }
    DCHECK(d.kind == CSS_DISPLAY_PAIR, "a display value of a kind css_display.h does not declare was serialized");
    if (d.list_item) {
        /* §2.3's defaults are omitted, `list-item` last, as the table's `list-item` and `inline list-item`. */
        snprintf(buf, sizeof buf, "%s%s%s%slist-item",
                 d.outer != CSS_KEYWORD_BLOCK ? css_keyword_name(d.outer) : "",
                 d.outer != CSS_KEYWORD_BLOCK ? " " : "",
                 d.inner != CSS_KEYWORD_FLOW ? css_keyword_name(d.inner) : "",
                 d.inner != CSS_KEYWORD_FLOW ? " " : "");
        return css_display_strdup(buf);
    }
    for (k = 0; k < CSS_DISPLAY_N(CSS_DISPLAY_SHORT); k++)
        if (CSS_DISPLAY_SHORT[k].outer == d.outer && CSS_DISPLAY_SHORT[k].inner == d.inner)
            return css_display_strdup(css_keyword_name(CSS_DISPLAY_SHORT[k].shortk));
    /* `block ruby`, and a `run-in` box of an inner type other than flow: no single keyword says them. */
    snprintf(buf, sizeof buf, "%s %s", css_keyword_name(d.outer), css_keyword_name(d.inner));
    return css_display_strdup(buf);
}

bool css_display_is_box(CssDisplay d, CssKeyword k)
{
    DCHECKF(css_keyword_set_has(CSS_KEYWORDS_DISPLAY_BOX, k),
            "a display value was asked whether it is keyword %u, which is not a `<display-box>`", (unsigned)k);
    return d.kind == CSS_DISPLAY_BOX && d.keyword == k;
}
