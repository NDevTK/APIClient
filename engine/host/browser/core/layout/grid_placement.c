/* css-grid-1 §8 "Placing Grid Items". See grid_placement.h. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "core/css/css_style_declaration.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_item.h"
#include "core/layout/grid_placement.h"

lxb_dom_element_t *grid_item_of(lxb_dom_element_t *container, BoxTreeChild c)
{
    char buf[160];

    /* css-grid-1 §6 "Grid Items" states css-flexbox-1 §4's rule word for word — "Each in-flow child of a grid
       container becomes a grid item, and each child text sequence is wrapped in an anonymous block container
       grid item", a white-space-only sequence not rendered — and §9 takes an absolutely positioned child out of
       the grid's layout, as §4.1 does a flex container's, so §4's classification answers for a grid. */
    if (box_tree_child_is_pseudo(c))
        DFAILF("%s has a pseudo-element child box, which css-grid-1 §6 \"Grid Items\" makes a grid item. "
               "BUILD the pseudo-element as an item of the track sizing",
               box_subject(container, buf, sizeof buf));
    switch (flex_item_child_kind(container, c.node)) {
    case FLEX_ITEM_CHILD_NONE:
        return NULL;
    case FLEX_ITEM_CHILD_TEXT:
        DFAILF("%s has a child text sequence, which css-grid-1 §6 wraps in an anonymous block container grid "
               "item. BUILD that item's contributions (core/layout/intrinsic_size.h's run sizes)",
               box_subject(container, buf, sizeof buf));
        return NULL;
    case FLEX_ITEM_CHILD_ELEMENT:
        break;
    }
    return lxb_dom_interface_element(c.node);
}

/* One `<grid-line>` (§8.3): `auto`, a line number, or a span. */
typedef enum { GP_AUTO = 0, GP_LINE, GP_SPAN } GpKind;
typedef struct {
    GpKind kind;
    long   n;
} GpLine;

/* `v` (its first `len` bytes) as a `<grid-line>`. A value outside the grammar is an invalid declaration, which
   leaves the property at its initial `auto`; `<custom-ident>` (named lines and areas) is refused by name. */
static GpLine gp_parse_line(lxb_dom_element_t *el, const char *v, size_t len)
{
    GpLine out = { GP_AUTO, 0 };
    char tok[2][32];
    int ntok = 0, i;
    size_t at = 0;
    bool span = false, have_int = false;
    long n = 0;
    char buf[160];

    while (at < len && ntok < 3) {
        size_t s;

        while (at < len && isspace((unsigned char)v[at])) at++;
        if (at >= len) break;
        s = at;
        while (at < len && !isspace((unsigned char)v[at])) at++;
        if (ntok == 2)
            DFAILF("%s's grid placement `%.*s` has three components, which css-grid-1 §8.3's grammar admits only "
                   "with a `<custom-ident>`. BUILD named lines", box_subject(el, buf, sizeof buf), (int)len, v);
        if (at - s >= sizeof tok[0]) return out;
        memcpy(tok[ntok], v + s, at - s);
        tok[ntok][at - s] = '\0';
        ntok++;
    }
    if (ntok == 0) return out;
    if (ntok == 1 && strcasecmp(tok[0], "auto") == 0) return out;
    for (i = 0; i < ntok; i++) {
        char *end;
        long x;

        if (strcasecmp(tok[i], "span") == 0) {
            if (span) return out;
            span = true;
            continue;
        }
        x = strtol(tok[i], &end, 10);
        if (*end == '\0' && end != tok[i] && (isdigit((unsigned char)tok[i][0]) || tok[i][0] == '-' ||
                                              tok[i][0] == '+')) {
            if (have_int) return out;
            have_int = true;
            n = x;
            continue;
        }
        DFAILF("%s's grid placement `%.*s` names a line or area by `<custom-ident>` (css-grid-1 §8.3 "
               "\"Line-based Placement\"), or is outside its grammar. BUILD named lines and areas",
               box_subject(el, buf, sizeof buf), (int)len, v);
    }
    /* "An <integer> value of zero makes the declaration invalid", and a span takes `<integer [1,∞]>`. */
    if (!have_int || n == 0 || (span && n < 0)) return out;
    out.kind = span ? GP_SPAN : GP_LINE;
    out.n = n;
    return out;
}

/* The cascaded text of `name` on `el`, or NULL when it is undeclared or at its initial `auto`. */
static char *gp_declared(lxb_dom_element_t *el, const char *name)
{
    char *v = cssom_cascaded_value(el, name);

    if (v != NULL && (strcasecmp(v, "auto") == 0 || strcasecmp(v, "initial") == 0 || strcasecmp(v, "unset") == 0)) {
        free(v);
        return NULL;
    }
    return v;
}

/* The `i`th slash-separated part of `v`, as a span of it; false when there is none. */
static bool gp_part(const char *v, int i, const char **s, size_t *len)
{
    const char *p = v;

    for (; i > 0; i--) {
        p = strchr(p, '/');
        if (p == NULL) return false;
        p++;
    }
    *s = p;
    *len = strchr(p, '/') ? (size_t)(strchr(p, '/') - p) : strlen(p);
    return true;
}

/* The start and end `<grid-line>`s of one axis of `el`, from the longhands or §8.4's shorthands. The cascade
   does not expand these shorthands, so one declared beside its own longhand would need their cascade order
   to resolve and is refused by name. */
static void gp_axis_lines(lxb_dom_element_t *el, bool rows, GpLine *start, GpLine *end)
{
    const char *ls = rows ? "grid-row-start" : "grid-column-start";
    const char *le = rows ? "grid-row-end" : "grid-column-end";
    const char *sh = rows ? "grid-row" : "grid-column";
    char *vs = gp_declared(el, ls), *ve = gp_declared(el, le), *vsh = gp_declared(el, sh);
    char *varea = gp_declared(el, "grid-area");
    const char *p;
    size_t n;
    char buf[160];

    start->kind = end->kind = GP_AUTO;
    start->n = end->n = 0;
    if ((vsh != NULL) + (varea != NULL) + (vs != NULL || ve != NULL) > 1)
        DFAILF("%s declares a css-grid-1 §8.4 placement shorthand beside a property it sets, which only the "
               "cascade order of the two declarations resolves. BUILD the shorthands' expansion in the cascade",
               box_subject(el, buf, sizeof buf));
    if (vs != NULL) *start = gp_parse_line(el, vs, strlen(vs));
    if (ve != NULL) *end = gp_parse_line(el, ve, strlen(ve));
    /* §8.4: `grid-row`/`grid-column` set start before the slash and end after it; `grid-area` sets row-start,
       column-start, row-end, column-end in that order. An omitted part is `auto` once `<custom-ident>`, the one
       value that would copy across, is refused by the parse. */
    if (vsh != NULL) {
        if (gp_part(vsh, 0, &p, &n)) *start = gp_parse_line(el, p, n);
        if (gp_part(vsh, 1, &p, &n)) *end = gp_parse_line(el, p, n);
    }
    if (varea != NULL) {
        if (gp_part(varea, rows ? 0 : 1, &p, &n)) *start = gp_parse_line(el, p, n);
        if (gp_part(varea, rows ? 2 : 3, &p, &n)) *end = gp_parse_line(el, p, n);
    }
    free(vs);
    free(ve);
    free(vsh);
    free(varea);
}

/* A line number as a zero-based line index, over an explicit grid of `explicit_tracks` tracks: a negative one
   "counts in reverse, starting from the end edge of the explicit grid". */
static long gp_line_index(long n, size_t explicit_tracks)
{
    return n > 0 ? n - 1 : (long)explicit_tracks + 1 + n;
}

/* One axis's placement: its span, and whether its start track is definite. §8.3.1 "Grid Placement Conflict
   Handling" first: "If the start line is further end-ward than the end line, swap the two lines. If the start
   line is equal to the end line, remove the end line. If the placement contains two spans, remove the one
   contributed by the end grid-placement property." A missing span is 1 (§8.5: "if they're not explicitly
   specified, they default to 1"). */
static bool gp_axis_resolve(lxb_dom_element_t *el, bool rows, size_t explicit_tracks, size_t *track, size_t *span)
{
    GpLine s, e;
    long a = 0, b;
    bool definite = true;
    char buf[160];

    gp_axis_lines(el, rows, &s, &e);
    if (s.kind == GP_SPAN && e.kind == GP_SPAN) e.kind = GP_AUTO;
    *span = 1;
    if (s.kind == GP_LINE && e.kind == GP_LINE) {
        a = gp_line_index(s.n, explicit_tracks);
        b = gp_line_index(e.n, explicit_tracks);
        if (a > b) { long t = a; a = b; b = t; }
        if (b > a) *span = (size_t)(b - a);
    } else if (s.kind == GP_LINE) {
        a = gp_line_index(s.n, explicit_tracks);
        if (e.kind == GP_SPAN) *span = (size_t)e.n;
    } else if (e.kind == GP_LINE) {
        if (s.kind == GP_SPAN) *span = (size_t)s.n;
        a = gp_line_index(e.n, explicit_tracks) - (long)*span;
    } else {
        definite = false;
        if (s.kind == GP_SPAN) *span = (size_t)s.n;
        else if (e.kind == GP_SPAN) *span = (size_t)e.n;
    }
    if (definite && a < 0)
        DFAILF("%s is placed before the explicit grid's first line, which css-grid-1 §8.5 meets by adding implicit "
               "tracks at the start. BUILD those tracks", box_subject(el, buf, sizeof buf));
    *track = definite ? (size_t)a : 0;
    return definite;
}

/* The occupied cells, a dense bitmap over `columns` growing by rows. */
typedef struct {
    unsigned char *bits;
    size_t columns, rows;
} GpOccupancy;

/* Whether the area of `rs` rows by `cs` columns at (`row`, `col`) overlaps an occupied cell. */
static bool gp_overlaps(const GpOccupancy *o, size_t row, size_t col, size_t rs, size_t cs)
{
    size_t r, c;

    for (r = row; r < row + rs && r < o->rows; r++)
        for (c = col; c < col + cs; c++)
            if (o->bits[r * o->columns + c]) return true;
    return false;
}

static void gp_occupy(GpOccupancy *o, size_t row, size_t col, size_t rs, size_t cs)
{
    size_t r, c;

    DCHECK(col + cs <= o->columns, "a grid area was occupied past the implicit grid's columns");
    if (row + rs > o->rows) {
        size_t rows = row + rs > 2 * o->rows ? row + rs : 2 * o->rows;
        unsigned char *grown = realloc(o->bits, rows * o->columns);

        CHECK(grown != NULL, "css-grid-1 §8.5: out of memory marking occupied grid cells");
        memset(grown + o->rows * o->columns, 0, (rows - o->rows) * o->columns);
        o->bits = grown;
        o->rows = rows;
    }
    for (r = row; r < row + rs; r++)
        for (c = col; c < col + cs; c++) o->bits[r * o->columns + c] = 1;
}

void grid_placement_free(GridPlacement *p)
{
    DCHECK(p != NULL, "a grid placement was released with no placement");
    free(p->cells);
    p->cells = NULL;
    p->n = 0;
}

void grid_placement_run(lxb_dom_element_t *container, size_t explicit_columns, size_t explicit_rows,
                        GridPlacement *out)
{
    BoxTreeChild c;
    size_t n = 0, cap = 0, i, columns = explicit_columns ? explicit_columns : 1, rows = explicit_rows;
    size_t cursor_row = 0, cursor_col = 0;
    lxb_dom_element_t **items = NULL;
    bool *row_def, *col_def;
    size_t *row_at, *col_at, *rs, *cs, *row_cursor = NULL, row_cursors = 0;
    GpOccupancy o = { NULL, 0, 0 };

    DCHECK(out != NULL, "a grid was placed with nowhere to put it");
    for (c = box_tree_first_child(container); box_tree_child_exists(c); c = box_tree_next_sibling(container, c)) {
        lxb_dom_element_t *it = grid_item_of(container, c);

        if (it == NULL) continue;
        if (n == cap) {
            lxb_dom_element_t **grown = realloc(items, (cap = cap ? 2 * cap : 8) * sizeof *grown);

            CHECK(grown != NULL, "css-grid-1 §8.5: out of memory listing grid items");
            items = grown;
        }
        items[n++] = it;
    }
    out->cells = calloc(n ? n : 1, sizeof *out->cells);
    row_def = calloc(n ? n : 1, sizeof *row_def);
    col_def = calloc(n ? n : 1, sizeof *col_def);
    row_at = calloc(n ? n : 1, sizeof *row_at);
    col_at = calloc(n ? n : 1, sizeof *col_at);
    rs = calloc(n ? n : 1, sizeof *rs);
    cs = calloc(n ? n : 1, sizeof *cs);
    CHECK(out->cells && row_def && col_def && row_at && col_at && rs && cs,
          "css-grid-1 §8.5: out of memory placing grid items");
    for (i = 0; i < n; i++) {
        row_def[i] = gp_axis_resolve(items[i], true, explicit_rows, &row_at[i], &rs[i]);
        col_def[i] = gp_axis_resolve(items[i], false, explicit_columns, &col_at[i], &cs[i]);
        /* "Determine the columns in the implicit grid": every definite column position, and "if the largest
           column span among all the items without a definite column position is larger than the width of the
           implicit grid, add columns to the end of the implicit grid to accommodate that column span". */
        if (col_def[i] && col_at[i] + cs[i] > columns) columns = col_at[i] + cs[i];
    }
    for (i = 0; i < n; i++)
        if (!col_def[i] && cs[i] > columns) columns = cs[i];
    o.columns = columns;
    /* "Position anything that's not auto-positioned." */
    for (i = 0; i < n; i++)
        if (row_def[i] && col_def[i]) {
            out->cells[i].row = row_at[i];
            out->cells[i].col = col_at[i];
            gp_occupy(&o, row_at[i], col_at[i], rs[i], cs[i]);
        }
    /* "Process the items locked to a given row": the earliest column that does not overlap an occupied cell
       "and that is past any grid items previously placed in this row by this step". */
    for (i = 0; i < n; i++) {
        size_t col;

        if (!row_def[i] || col_def[i]) continue;
        if (row_at[i] >= row_cursors) {
            size_t want = row_at[i] + 1;
            size_t *grown = realloc(row_cursor, want * sizeof *grown);

            CHECK(grown != NULL, "css-grid-1 §8.5: out of memory holding per-row cursors");
            memset(grown + row_cursors, 0, (want - row_cursors) * sizeof *grown);
            row_cursor = grown;
            row_cursors = want;
        }
        for (col = row_cursor[row_at[i]]; col + cs[i] <= columns && gp_overlaps(&o, row_at[i], col, rs[i], cs[i]);
             col++) {}
        if (col + cs[i] > columns) {
            char buf[160];

            DFAILF("%s is locked to a row with no room for it, which css-grid-1 §8.5 meets by adding implicit "
                   "columns. BUILD that growth", box_subject(items[i], buf, sizeof buf));
        }
        out->cells[i].row = row_at[i];
        out->cells[i].col = col;
        row_cursor[row_at[i]] = col + cs[i];
        gp_occupy(&o, row_at[i], col, rs[i], cs[i]);
    }
    /* "Position the remaining grid items", sparse packing, the cursor starting at the start-most lines. */
    for (i = 0; i < n; i++) {
        if (row_def[i]) continue;
        if (col_def[i]) {
            /* "Set the column position of the cursor to the grid item's column-start line. If this is less than
               the previous column position of the cursor, increment the row position by 1. Increment the
               cursor's row position until a value is found where the grid item does not overlap any occupied
               grid cells." */
            if (col_at[i] < cursor_col) cursor_row++;
            cursor_col = col_at[i];
            while (gp_overlaps(&o, cursor_row, cursor_col, rs[i], cs[i])) cursor_row++;
        } else {
            /* "Increment the column position of the auto-placement cursor until either this item's grid area
               does not overlap any occupied grid cells, or the cursor's column position, plus the item's column
               span, overflow the number of columns in the implicit grid … Otherwise, increment the auto-placement
               cursor's row position …, set its column position to the start-most column line". */
            while (cursor_col + cs[i] > columns || gp_overlaps(&o, cursor_row, cursor_col, rs[i], cs[i])) {
                if (cursor_col + cs[i] > columns) { cursor_row++; cursor_col = 0; }
                else cursor_col++;
            }
        }
        out->cells[i].row = cursor_row;
        out->cells[i].col = cursor_col;
        gp_occupy(&o, cursor_row, cursor_col, rs[i], cs[i]);
    }
    for (i = 0; i < n; i++) {
        out->cells[i].rows = rs[i];
        out->cells[i].cols = cs[i];
        if (out->cells[i].row + rs[i] > rows) rows = out->cells[i].row + rs[i];
    }
    out->n = n;
    out->columns = columns;
    out->rows = rows;
    free(items);
    free(row_def);
    free(col_def);
    free(row_at);
    free(col_at);
    free(rs);
    free(cs);
    free(row_cursor);
    free(o.bits);
}
