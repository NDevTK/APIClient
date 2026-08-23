/* CSS Nesting Module Level 1 §3 "Nesting Style Rules", §3.1 "Syntax", §4 "Nesting Selector: the & selector"
 * and §6 "CSSOM". See css_nesting.h for why the nesting selector is desugared to `:is()` rather than
 * concatenated, for why absolutize runs once and resolve runs per cascade read, and for why the scan is over
 * CSS Syntax's tokens rather than over bytes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_nesting.h"

/* ---- the buffer ---------------------------------------------------------------------------------------- */

typedef struct { char *s; size_t len, cap; } NBuf;

static void nbuf_add_n(NBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        char *grown;

        while (cap < b->len + n + 1) cap *= 2;
        grown = realloc(b->s, cap);
        CHECK(grown != NULL, "cssom: OOM resolving a CSS Nesting selector");
        b->s = grown;
        b->cap = cap;
    }
    if (n) memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void nbuf_add(NBuf *b, const char *s) { nbuf_add_n(b, s, strlen(s)); }

/* A buffer nothing was ever written into still has to come back as a STRING, because both entries below
   promise one. The EMPTY selector is a real answer and not an error here — `.a { , { } }` has an empty complex
   selector in its prelude, which no production of Selectors 4 §16 "Grammar" admits, and it is the cascade's own
   re-parse that refuses the rule rather than this file inventing a stand-in for it. */
static char *nbuf_finish(NBuf *b)
{
    if (!b->s) nbuf_add_n(b, "", 0);
    return b->s;
}

/* ---- §3.1's token scan ---------------------------------------------------------------------------------- */

/* ONE CSS SYNTAX TOKEN'S WORTH OF TEXT, as far as §3.1's question needs to distinguish them: the index after
   the construct starting at `i`. Exactly three constructs hide an ampersand from being a <delim-token>, and
   each is one of CSS Syntax's own:
     - a STRING (`"..."` / `'...'`), whose contents are a <string-token>'s and not selector syntax at all. A raw
       newline inside one ends it — CSS Syntax's bad-string — so the scan does not run past a line on an
       unterminated quote and swallow the rest of the sheet.
     - a COMMENT, which CSS Syntax removes before tokenizing.
     - an ESCAPE (backslash then ampersand), which is a code point of an <ident-token> rather than a delim.
       §3.1's test is explicitly about the DELIM, so an escaped ampersand is not the nesting selector.
   Everything else advances one byte, which is all the callers need: they only ever ask about `&`, `,`, the
   three combinator characters and bracket depth, none of which is multi-byte and none of which can be a UTF-8
   sequence's continuation byte. */
static size_t nest_step(const char *s, size_t n, size_t i)
{
    DCHECK(i < n, "the nesting-selector scan stepped from a position that is not inside the text");
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
        size_t j = i + 2;

        while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
        return (j + 1 < n) ? j + 2 : n;
    }
    if (s[i] == '"' || s[i] == '\'') {
        char q = s[i];
        size_t j = i + 1;

        while (j < n && s[j] != q) {
            if (s[j] == '\n' || s[j] == '\r' || s[j] == '\f') return j;   /* CSS Syntax's bad-string */
            j += (s[j] == '\\' && j + 1 < n) ? 2 : 1;
        }
        return (j < n) ? j + 1 : n;
    }
    if (s[i] == '\\') return (i + 1 < n) ? i + 2 : n;
    return i + 1;
}

/* Is the construct at `i` — which `nest_step` reported ends at `next` — the nesting <delim-token>? */
static bool nest_is_amp(const char *s, size_t i, size_t next) { return next == i + 1 && s[i] == '&'; }

bool css_nesting_contains(const char *sel, size_t len)
{
    size_t i = 0;

    DCHECK(sel != NULL || len == 0,
           "§3.1's contains-the-nesting-selector test was asked about a NULL span of non-zero length");
    if (!sel) return false;
    while (i < len) {
        size_t j = nest_step(sel, len, i);

        if (nest_is_amp(sel, i, j)) return true;
        i = j;
    }
    return false;
}

/* ---- §6's absolutize ------------------------------------------------------------------------------------ */

static bool nest_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* Selectors 4 §16 "Grammar": `<combinator> = '>' | '+' | '~'`. The descendant combinator is §14 "Combinators"'
   whitespace and cannot begin a TRIMMED complex selector, so it is not one of these. */
static bool nest_combinator(char c) { return c == '>' || c == '+' || c == '~'; }

/* §3.1 for ONE `<relative-selector>`, appended to `b` in its absolutized form. `s`/`n` is the part the comma
   split produced, already trimmed of the whitespace around it. */
static void nest_absolutize_one(NBuf *b, const char *s, size_t n)
{
    size_t before = b->len;

    if (n == 0) return;
    /* §3.1's two arms, in the order §3.1 states them. A part beginning with a combinator is a RELATIVE
       selector whatever else it holds, so `> &` absolutizes to `& > &`; only a part that does NOT begin with
       one and DOES contain the nesting selector "is interpreted as a non-relative selector" and goes in
       untouched. Everything else is relative with Selectors 4 §3.4 "Relative Selectors"' implied descendant
       combinator, and the SPACE in `"& "` is that combinator. */
    if (nest_combinator(s[0]) || !css_nesting_contains(s, n)) nbuf_add(b, "& ");
    nbuf_add_n(b, s, n);
    DCHECK(css_nesting_contains(b->s + before, b->len - before),
           "§6's absolutize produced a complex selector that does not contain the nesting selector — the whole "
           "point of the operation is that every later reader has ONE shape to handle, and css_nesting_resolve "
           "asserts the same premise from the other side");
}

bool css_nesting_is_relative(const char *sel, size_t len)
{
    size_t i = 0, depth = 0;
    bool at_start = true;

    DCHECK(sel != NULL || len == 0, "§3.1's relative-shape test was asked about a NULL span of non-zero length");
    if (!sel) return false;
    if (css_nesting_contains(sel, len)) return true;
    /* The remaining half of §3.1's question: does any complex selector in the list BEGIN with a combinator.
       `at_start` is true at the head of the list and after every top-level comma, and the leading whitespace a
       trim would have removed is skipped rather than removed because nothing here builds a string. */
    while (i < len) {
        size_t j = nest_step(sel, len, i);
        char c = sel[i];
        bool one = (j == i + 1);

        if (one && nest_space(c)) { i = j; continue; }
        if (one && depth == 0 && c == ',') { at_start = true; i = j; continue; }
        if (at_start && one && nest_combinator(c)) return true;
        at_start = false;
        if (one) {
            if (c == '(' || c == '[') depth++;
            else if ((c == ')' || c == ']') && depth) depth--;
        }
        i = j;
    }
    return false;
}

char *css_nesting_absolutize(const char *sel, size_t len)
{
    NBuf b = { NULL, 0, 0 };
    size_t i = 0, start = 0, depth = 0, parts = 0;

    DCHECK(sel != NULL || len == 0, "§6's absolutize was handed a NULL span of non-zero length");
    if (!sel) { sel = ""; len = 0; }
    /* THE SPLIT IS ON TOP-LEVEL COMMAS ONLY, because Selectors 4 §16's `<relative-selector-list>` is
       `<relative-selector>#` and a comma inside `:is(a, b)` or `[title="a,b"]` belongs to that construct
       rather than to the list. Brackets are COUNTED and not matched: an unbalanced one is invalid author CSS,
       and refusing it is the cascade's own re-parse rather than this scan's. */
    while (i <= len) {
        size_t j = (i < len) ? nest_step(sel, len, i) : len + 1;
        bool split = (i == len) || (depth == 0 && j == i + 1 && sel[i] == ',');

        if (split) {
            size_t a = start, z = i;

            while (a < z && nest_space(sel[a])) a++;
            while (z > a && nest_space(sel[z - 1])) z--;
            if (parts++) nbuf_add(&b, ", ");
            nest_absolutize_one(&b, sel + a, z - a);
            start = i + 1;
        } else if (j == i + 1) {
            if (sel[i] == '(' || sel[i] == '[') depth++;
            else if ((sel[i] == ')' || sel[i] == ']') && depth) depth--;
        }
        i = j;
    }
    DCHECK(parts >= 1,
           "§6's absolutize emitted no complex selector at all — the loop above ends BY splitting at the end "
           "of the text, so it writes one part per comma and one more besides");
    return nbuf_finish(&b);
}

/* ---- §4's desugaring ------------------------------------------------------------------------------------ */

char *css_nesting_resolve(const char *sel, size_t len, const char *parent, size_t parent_len)
{
    NBuf b = { NULL, 0, 0 };
    size_t i = 0;

    DCHECK(sel != NULL && parent != NULL,
           "§4's desugaring was asked to replace the nesting selector with nothing, or in nothing");
    DCHECK(parent_len > 0,
           "§4's desugaring was handed an EMPTY parent selector list. `&` is defined as \"the elements matched "
           "by the parent rule\", and a style rule with no selector list matches nothing and cannot be one — "
           "core/css/css_rule.c asserts the same premise from the other side before it emits a rule at all");
    DCHECK(css_nesting_contains(sel, len),
           "§4's desugaring was handed a nested selector that contains NO nesting selector. Every nested rule "
           "stores the §6 ABSOLUTIZED form, in which the implied `&` has already been inserted, so a selector "
           "without one reached this call without going through css_nesting_absolutize");
    if (!sel || !parent) return nbuf_finish(&b);
    while (i < len) {
        size_t j = nest_step(sel, len, i);

        if (nest_is_amp(sel, i, j)) {
            nbuf_add(&b, ":is(");
            nbuf_add_n(&b, parent, parent_len);
            nbuf_add(&b, ")");
        } else {
            nbuf_add_n(&b, sel + i, j - i);
        }
        i = j;
    }
    DCHECK(!css_nesting_contains(b.s, b.len),
           "§4's desugaring left a nesting selector in its own output — every `&` is replaced by `:is(...)` "
           "and the parent selector list it substitutes in is an ABSOLUTE one, so an `&` surviving here means "
           "a caller resolved against a parent that was itself still nested");
    return nbuf_finish(&b);
}
