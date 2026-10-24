/* css-variables-1 §3 "Using Cascading Variables: the var() notation", the syntax half — see css_var.h for the
 * split, for why a NULL here is CSS Cascade 5 §7.3.3's `unset`, and for what is deliberately not covered. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_var.h"

/* ---- the scan ------------------------------------------------------------------------------------------- */

/* CSS Syntax 3 §4.2 "Definitions"' ident code point, as the question this file actually asks: may this byte
   CONTINUE an identifier? It decides two things and they are the same thing — where a `<custom-property-name>`
   ends, and whether a `var(` is a function token of its own or the tail of a longer ident. `-var(` is the
   function `-var`, not `var`, and a scan that missed that would substitute into somebody else's function.
   A byte at or above 0x80 is admitted whole rather than decoded: §4.2 makes every non-ASCII code point an
   ident code point, so every byte of a UTF-8 sequence is one and no decode can change the answer. */
static bool cv_ident_byte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '\\' || c >= 0x80;
}

static bool cv_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Past the string that OPENS at `i` — CSS Syntax 3 §4.3.5 "Consume a string token", whose only two enders are
   the matching quote and a newline (an unterminated string). The backslash is honoured so that a `"\""` does
   not end early, which is what would otherwise let the rest of a declaration be scanned as though it were
   outside the string, and a `var(` inside somebody's `content` string would then be substituted. */
static size_t cv_skip_string(const char *s, size_t i)
{
    char q = s[i++];

    while (s[i] != '\0') {
        if (s[i] == '\\' && s[i + 1] != '\0') { i += 2; continue; }
        if (s[i] == '\n') return i;          /* §4.3.5's bad-string-token arm: the newline is not consumed */
        if (s[i] == q) return i + 1;
        i++;
    }
    return i;
}

/* Is there a `var(` FUNCTION TOKEN at `i` — the name ASCII case-insensitively, the `(` immediately after it
   (CSS Syntax 3 §4.3.1 admits no whitespace between an ident and the `(` that makes it a function), and `i`
   at an ident BOUNDARY so that the tail of a longer ident is not mistaken for one. */
static bool cv_at_var(const char *s, size_t i)
{
    const char *p = s + i;

    if (i > 0 && cv_ident_byte((unsigned char)s[i - 1])) return false;
    return (p[0] == 'v' || p[0] == 'V') && (p[1] == 'a' || p[1] == 'A') &&
           (p[2] == 'r' || p[2] == 'R') && p[3] == '(';
}

/* The index of the `)` closing a `(` whose CONTENTS begin at `i`, or the length of `s` when the value ends
   first — an unbalanced value, which CSS Syntax 3 §5.4.1 closes implicitly at EOF and which this file treats
   the same way rather than refusing, since the cascaded value it is handed came out of a parser. Strings are
   skipped whole for `cv_skip_string`'s reason and nesting is counted so that `var(--a, rgb(1,2,3))` ends at
   its own paren and not at the inner one. */
static size_t cv_match_paren(const char *s, size_t i)
{
    size_t depth = 1;

    while (s[i] != '\0') {
        char c = s[i];

        if (c == '\\' && s[i + 1] != '\0') { i += 2; continue; }
        if (c == '"' || c == '\'') { i = cv_skip_string(s, i); continue; }
        if (c == '(') depth++;
        else if (c == ')' && --depth == 0) return i;
        i++;
    }
    return i;
}

bool css_var_references(const char *value)
{
    size_t i = 0;

    if (value == NULL) return false;
    while (value[i] != '\0') {
        if (value[i] == '\\' && value[i + 1] != '\0') { i += 2; continue; }
        if (value[i] == '"' || value[i] == '\'') { i = cv_skip_string(value, i); continue; }
        if (cv_at_var(value, i)) return true;
        i++;
    }
    return false;
}

/* ---- the output buffer ---------------------------------------------------------------------------------- */

typedef struct { char *p; size_t n, cap; } CvBuf;

static void cv_put(CvBuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t want = (b->cap ? b->cap * 2 : 64);

        while (want < b->n + n + 1) want *= 2;
        b->p = realloc(b->p, want);
        CHECK(b->p != NULL, "css-variables-1 §3: OOM composing a substituted declaration value — a dropped "
                            "value would read as a property nobody declared rather than as a failure");
        b->cap = want;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

/* ---- §3's substitution ---------------------------------------------------------------------------------- */

/* What a frame of the walk below substitutes. */
typedef enum {
    CV_TOP,        /* the value the caller handed in */
    CV_PROPERTY,   /* a custom property's own value, on the cyclic-substitution chain under `name` */
    CV_FALLBACK    /* a var()'s fallback, `<declaration-value>` */
} CvKind;

/* One text being substituted. `text` is owned except CV_TOP's; `name` points into the parent's text, which
   outlives the frame. `comma` and `close` are the var() at the cursor whose substitution a child computes. */
typedef struct {
    CvKind      kind;
    char       *text;
    size_t      i, n;
    CvBuf       out;
    const char *name;
    size_t      name_n;
    const char *pend_name;
    size_t      pend_name_n, comma, close;
} CvFrame;

typedef struct { CvFrame *v; size_t n, cap; } CvWalk;

typedef enum { CV_DONE, CV_FAIL, CV_NEED_PROPERTY, CV_NEED_FALLBACK } CvStep;

static void cv_push(CvWalk *w, CvKind kind, char *text, const char *name, size_t name_n)
{
    CvFrame *f;

    if (w->n == w->cap) {
        CvFrame *grown;

        w->cap = w->cap ? 2 * w->cap : 8;
        grown = realloc(w->v, w->cap * sizeof *grown);
        CHECK(grown != NULL, "css-variables-1 §3: OOM growing the substitution walk");
        w->v = grown;
    }
    f = &w->v[w->n++];
    memset(f, 0, sizeof *f);
    f->kind = kind;
    f->text = text;
    f->n = strlen(text);
    f->name = name;
    f->name_n = name_n;
    cv_put(&f->out, "", 0);                    /* so a value that is entirely one var() still owns a buffer */
}

/* §2.2's arm for the var() at `f`'s cursor, whose property is guaranteed-invalid: "the fallback, if any, is
   used instead". The fallback is everything after the first comma — commas included, which is why it is
   taken as a span and not tokenized — and it is `<declaration-value>`, so it may itself contain a `var()` and
   is substituted by the same steps (`*fb`, owned). A `var()` with no comma has no fallback to use, and that
   is where the whole declaration becomes invalid at computed-value time. */
static CvStep cv_fallback(const CvFrame *f, char **fb)
{
    size_t at = f->comma + 1, fb_n;

    if (f->comma >= f->close || f->text[f->comma] != ',') return CV_FAIL;
    while (at < f->close && cv_space((unsigned char)f->text[at])) at++;
    fb_n = f->close > at ? f->close - at : 0;
    *fb = malloc(fb_n + 1);
    CHECK(*fb != NULL, "css-variables-1 §3: OOM taking a `var()` fallback");
    memcpy(*fb, f->text + at, fb_n);
    (*fb)[fb_n] = '\0';
    return CV_NEED_FALLBACK;
}

/* Scans the top frame of `w` until its text is done or fails, or until a var() needs a child: its property's
   own value (`*child`, owned, from `lookup`) or its fallback. A name already on the chain of CV_PROPERTY
   frames is css-values-5's cyclic substitution context: this text fails, with no fallback of its own. The
   chain is the stack and not a seen-set, so `margin: var(--gap) var(--gap)` names one property twice. */
static CvStep cv_scan(CvWalk *w, CssVarLookup lookup, void *ud, char **child)
{
    CvFrame *f = &w->v[w->n - 1];
    const char *value = f->text;
    size_t k;

    while (value[f->i] != '\0') {
        size_t i = f->i, open, close, j, name, name_n;

        if (value[i] == '\\' && value[i + 1] != '\0') { cv_put(&f->out, value + i, 2); f->i += 2; continue; }
        if (value[i] == '"' || value[i] == '\'') {
            size_t e = cv_skip_string(value, i);

            cv_put(&f->out, value + i, e - i);
            f->i = e;
            continue;
        }
        if (!cv_at_var(value, i)) { cv_put(&f->out, value + i, 1); f->i++; continue; }

        open  = i + 4;                          /* past `var(` */
        close = cv_match_paren(value, open);

        /* §3's grammar: `var( <custom-property-name> , <declaration-value>? )`. The first argument is a
           custom property name and nothing else, so a `var(--)`, a `var(width)` or an empty one is a value
           this engine cannot compute — and the honest answer for it is the one §2.2 gives a guaranteed-invalid
           value rather than passing the text through to a property grammar that will report it as a keyword. */
        j = open;
        while (j < close && cv_space((unsigned char)value[j])) j++;
        name = j;
        if (!(j + 1 < close && value[j] == '-' && value[j + 1] == '-')) return CV_FAIL;
        j += 2;
        while (j < close && cv_ident_byte((unsigned char)value[j])) j++;
        name_n = j - name - 2;                  /* the name WITHOUT its `--`, which css_var.h states */
        if (name_n == 0) return CV_FAIL;
        while (j < close && cv_space((unsigned char)value[j])) j++;
        if (j < close && value[j] != ',') return CV_FAIL;

        f->pend_name = value + name + 2;
        f->pend_name_n = name_n;
        f->comma = j;
        f->close = close;
        for (k = 0; k < w->n; k++)
            if (w->v[k].kind == CV_PROPERTY && w->v[k].name_n == name_n &&
                memcmp(w->v[k].name, f->pend_name, name_n) == 0)
                return CV_FAIL;
        *child = lookup(ud, f->pend_name, name_n);
        if (*child != NULL) return CV_NEED_PROPERTY;
        return cv_fallback(f, child);
    }
    return CV_DONE;
}

/* The texts being substituted are walked on a heap stack of frames, so a deep chain of custom properties or
   of nested fallbacks costs no C stack. A frame that ends hands its parent what css-variables-1's "replace a
   var() function" steps return: its text when it substituted, and otherwise, for a custom property, §2.2's
   guaranteed-invalid value, whose var() then takes its fallback; a fallback that failed fails its parent. */
char *css_var_substitute(const char *value, CssVarLookup lookup, void *ud)
{
    CvWalk w = { NULL, 0, 0 };
    CvFrame *f;
    CvKind kind;
    CvStep r;
    char *child = NULL, *out;

    DCHECK(lookup != NULL,
           "css-variables-1 §3's substitution was asked to run with no lookup — every `var()` names a custom "
           "property and this file answers for none of them by itself, so a NULL lookup is a caller that has "
           "not said which element the names belong to");
    if (value == NULL) return NULL;
    cv_push(&w, CV_TOP, (char *)value, NULL, 0);
    for (;;) {
        r = cv_scan(&w, lookup, ud, &child);
        for (;;) {
            f = &w.v[w.n - 1];
            if (r == CV_NEED_PROPERTY) {
                cv_push(&w, CV_PROPERTY, child, f->pend_name, f->pend_name_n);
                break;
            }
            if (r == CV_NEED_FALLBACK) {
                cv_push(&w, CV_FALLBACK, child, NULL, 0);
                break;
            }
            kind = f->kind;
            out = f->out.p;
            if (r == CV_FAIL) { free(out); out = NULL; }
            if (kind != CV_TOP) free(f->text);
            if (--w.n == 0) {
                free(w.v);
                return out;
            }
            f = &w.v[w.n - 1];
            if (out != NULL) {
                cv_put(&f->out, out, strlen(out));
                free(out);
                f->i = f->close < f->n ? f->close + 1 : f->close;
                break;
            }
            /* A custom property whose own value did not substitute is guaranteed-invalid, so the var() that
               asked for it may still use its fallback; a fallback that did not substitute fails its var(). */
            r = kind == CV_PROPERTY ? cv_fallback(f, &child) : CV_FAIL;
        }
    }
}
