/* css-grid-1 §7.3 "Named Areas: the grid-template-areas property". See css_grid_template_areas.h. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "core/css/css_grid_template_areas.h"

/* CSS Syntax 3 §4.2's ident code point over UTF-8 bytes: a letter, a digit, `_`, `-`, or any byte of a
   non-ASCII code point. */
static bool gta_ident_byte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c >= 0x80;
}

static bool gta_whitespace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* The cells of one template string, appended to `names` (NULL for a null cell token) as newly allocated
   names. False for a trash token. */
static bool gta_row(const unsigned char *s, size_t len, char ***names, size_t *n, size_t *cap)
{
    size_t at = 0;

    while (at < len) {
        size_t from = at;
        char *name = NULL;

        if (gta_whitespace(s[at])) {
            at++;
            continue;
        }
        if (gta_ident_byte(s[at])) {
            while (at < len && gta_ident_byte(s[at])) at++;
            name = malloc(at - from + 1);
            CHECK(name != NULL, "css-grid-1 §7.3: out of memory copying a named cell token");
            memcpy(name, s + from, at - from);
            name[at - from] = '\0';
        } else if (s[at] == '.') {
            while (at < len && s[at] == '.') at++;
        } else {
            return false;   /* "A trash token is a syntax error, and makes the declaration invalid." */
        }
        if (*n == *cap) {
            char **grown = realloc(*names, (*cap = *cap ? 2 * *cap : 8) * sizeof *grown);

            CHECK(grown != NULL, "css-grid-1 §7.3: out of memory listing template cells");
            *names = grown;
        }
        (*names)[(*n)++] = name;
    }
    return true;
}

/* The cells' named areas, each checked to be "a single filled-in rectangle". */
static bool gta_areas(char **cells, size_t rows, size_t columns, CssGridTemplateAreas *out)
{
    size_t i, r, c, cap = 0;

    for (i = 0; i < rows * columns; i++) {
        CssGridArea a;
        size_t k, count = 0;
        bool seen = false;

        if (cells[i] == NULL) continue;
        for (k = 0; k < out->n; k++)
            if (strcmp(out->areas[k].name, cells[i]) == 0) seen = true;
        if (seen) continue;
        a.row_start = a.row_end = i / columns;
        a.col_start = a.col_end = i % columns;
        for (k = i; k < rows * columns; k++) {
            if (cells[k] == NULL || strcmp(cells[k], cells[i]) != 0) continue;
            count++;
            if (k / columns > a.row_end) a.row_end = k / columns;
            if (k % columns < a.col_start) a.col_start = k % columns;
            if (k % columns > a.col_end) a.col_end = k % columns;
        }
        a.row_end++;
        a.col_end++;
        if (count != (a.row_end - a.row_start) * (a.col_end - a.col_start)) return false;
        for (r = a.row_start; r < a.row_end; r++)
            for (c = a.col_start; c < a.col_end; c++)
                if (cells[r * columns + c] == NULL || strcmp(cells[r * columns + c], cells[i]) != 0) return false;
        a.name = strdup(cells[i]);
        CHECK(a.name != NULL, "css-grid-1 §7.3: out of memory naming a grid area");
        if (out->n == cap) {
            CssGridArea *grown = realloc(out->areas, (cap = cap ? 2 * cap : 4) * sizeof *grown);

            CHECK(grown != NULL, "css-grid-1 §7.3: out of memory listing named grid areas");
            out->areas = grown;
        }
        out->areas[out->n++] = a;
    }
    return true;
}

void css_grid_template_areas_free(CssGridTemplateAreas *a)
{
    size_t i;

    DCHECK(a != NULL, "a grid template was released with no template");
    for (i = 0; i < a->n; i++) free(a->areas[i].name);
    free(a->areas);
    a->areas = NULL;
    a->n = a->rows = a->columns = 0;
}

bool css_grid_template_areas_parse(const char *text, CssGridTemplateAreas *out)
{
    lxb_css_syntax_tokenizer_t *tkz;
    lxb_css_syntax_token_t *t;
    char **cells = NULL;
    size_t n = 0, cap = 0, rows = 0, columns = 0, i;
    bool ok = true;

    DCHECK(text != NULL && out != NULL, "a grid template was parsed with no text or nowhere to put it");
    out->areas = NULL;
    out->n = out->rows = out->columns = 0;
    tkz = lxb_css_syntax_tokenizer_create();
    CHECK(tkz != NULL, "css-grid-1 §7.3: the template tokenizer allocation failed");
    CHECK(lxb_css_syntax_tokenizer_init(tkz) == LXB_STATUS_OK, "css-grid-1 §7.3: the template tokenizer init failed");
    lxb_css_syntax_tokenizer_buffer_set(tkz, (const lxb_char_t *)text, strlen(text));
    /* `none` is the initial value; the tokenizer has already unescaped each `<string>`. */
    for (t = lxb_css_syntax_token(tkz); ok; t = lxb_css_syntax_token(tkz)) {
        const lxb_css_syntax_token_string_t *s;
        size_t before = n;

        CHECK(t != NULL, "css-grid-1 §7.3: the template tokenizer failed to allocate a token");
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        if (t->type == LXB_CSS_SYNTAX_TOKEN_WHITESPACE) {
            lxb_css_syntax_token_consume(tkz);
            continue;
        }
        if (t->type != LXB_CSS_SYNTAX_TOKEN_STRING) {
            ok = false;
            break;
        }
        s = lxb_css_syntax_token_string(t);
        ok = gta_row(s->data, s->length, &cells, &n, &cap);
        /* "All strings must define the same number of cell tokens … and at least one cell token". */
        if (ok && rows == 0) columns = n - before;
        if (ok && (n - before == 0 || n - before != columns)) ok = false;
        rows++;
        lxb_css_syntax_token_consume(tkz);
    }
    lxb_css_syntax_tokenizer_destroy(tkz);
    if (rows == 0) ok = false;
    if (ok) {
        out->rows = rows;
        out->columns = columns;
        ok = gta_areas(cells, rows, columns, out);
    }
    for (i = 0; i < n; i++) free(cells[i]);
    free(cells);
    if (!ok) css_grid_template_areas_free(out);
    return ok;
}
