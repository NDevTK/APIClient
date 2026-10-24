/* css-grid-1 §8 "Placing Grid Items". See grid_placement.h. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_style_declaration.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_item.h"
#include "core/layout/grid_explicit_tracks.h"
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

/* One `<grid-line>` (§8.3): `auto`, a line number, or a span, either of which may count only the lines of one
   name. A lone `<custom-ident>` is a line number of 1 with `ident` set, which §8.3 first matches against a
   named area's edge and §8.4's shorthands copy across. `name` is owned (`gp_line_free`). */
typedef enum { GP_AUTO = 0, GP_LINE, GP_SPAN } GpKind;
typedef struct {
    GpKind kind;
    long   n;
    char  *name;
    bool   ident;
} GpLine;

static const GpLine GP_LINE_AUTO = { GP_AUTO, 0, NULL, false };

static void gp_line_free(GpLine *l)
{
    free(l->name);
    *l = GP_LINE_AUTO;
}

static GpLine gp_line_copy(const GpLine *l)
{
    GpLine out = *l;

    if (l->name != NULL) {
        out.name = strdup(l->name);
        CHECK(out.name != NULL, "css-grid-1 §8.4: out of memory copying a `<custom-ident>` across a shorthand");
    }
    return out;
}

static bool gp_token_is(const lxb_css_syntax_token_string_t *s, const char *lower)
{
    return s->length == strlen(lower) && strncasecmp((const char *)s->data, lower, s->length) == 0;
}

/* `v` (its first `len` bytes) as a `<grid-line>`, over CSS Syntax 3's tokens so an escaped `<custom-ident>`
   arrives unescaped: `auto | <custom-ident> | [ <integer> && <custom-ident>? ] | [ span && [ <integer> ||
   <custom-ident> ] ]`, where the `<custom-ident>` "additionally excludes the keywords span and auto". A value
   outside the grammar is an invalid declaration, which leaves the property at its initial `auto`. */
static GpLine gp_parse_line(const char *v, size_t len)
{
    GpLine out = GP_LINE_AUTO;
    lxb_css_syntax_tokenizer_t *tkz = lxb_css_syntax_tokenizer_create();
    lxb_css_syntax_token_t *t;
    bool ok = true, span = false, have_int = false, is_auto = false;
    long n = 0;
    int count = 0;
    char *name = NULL;

    CHECK(tkz != NULL && lxb_css_syntax_tokenizer_init(tkz) == LXB_STATUS_OK,
          "css-grid-1 §8.3: the placement tokenizer could not be created");
    lxb_css_syntax_tokenizer_buffer_set(tkz, (const lxb_char_t *)v, len);
    for (t = lxb_css_syntax_token(tkz); ok; t = lxb_css_syntax_token(tkz)) {
        CHECK(t != NULL, "css-grid-1 §8.3: the placement tokenizer failed to allocate a token");
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        if (t->type != LXB_CSS_SYNTAX_TOKEN_WHITESPACE) {
            count++;
            if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
                const lxb_css_syntax_token_string_t *s = lxb_css_syntax_token_string(t);

                if (gp_token_is(s, "span")) {
                    ok = !span;
                    span = true;
                } else if (gp_token_is(s, "auto")) {
                    is_auto = true;
                } else if (name != NULL) {
                    ok = false;
                } else {
                    name = malloc(s->length + 1);
                    CHECK(name != NULL, "css-grid-1 §8.3: out of memory copying a `<custom-ident>`");
                    memcpy(name, s->data, s->length);
                    name[s->length] = '\0';
                }
            } else if (t->type == LXB_CSS_SYNTAX_TOKEN_NUMBER && !lxb_css_syntax_token_number(t)->is_float &&
                       !have_int) {
                have_int = true;
                n = (long)lxb_css_syntax_token_number(t)->num;
            } else {
                ok = false;
            }
        }
        lxb_css_syntax_token_consume(tkz);
    }
    lxb_css_syntax_tokenizer_destroy(tkz);
    /* "An <integer> value of zero makes the declaration invalid", and a span takes `<integer [1,∞]>`. */
    if (is_auto && count > 1) ok = false;
    if (span && (have_int ? n < 1 : name == NULL)) ok = false;
    if (!span && have_int && n == 0) ok = false;
    if (!ok || count == 0 || is_auto) {
        free(name);
        return out;
    }
    out.kind = span ? GP_SPAN : GP_LINE;
    out.n = have_int ? n : 1;
    out.name = name;
    out.ident = !span && !have_int;
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

/* The `i`th slash-separated part of `v`, parsed; false when there is none. */
static bool gp_part(const char *v, int i, GpLine *out)
{
    const char *p = v;

    for (; i > 0; i--) {
        p = strchr(p, '/');
        if (p == NULL) return false;
        p++;
    }
    *out = gp_parse_line(p, strchr(p, '/') ? (size_t)(strchr(p, '/') - p) : strlen(p));
    return true;
}

/* §8.4's rule for an omitted part: "if [the other] is a <custom-ident>", it is set to that `<custom-ident>`;
   otherwise to `auto`. */
static GpLine gp_part_or_copy(const char *v, int i, const GpLine *from)
{
    GpLine out;

    if (gp_part(v, i, &out)) return out;
    return from->ident ? gp_line_copy(from) : GP_LINE_AUTO;
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
    char buf[160];

    *start = *end = GP_LINE_AUTO;
    if ((vsh != NULL) + (varea != NULL) + (vs != NULL || ve != NULL) > 1)
        DFAILF("%s declares a css-grid-1 §8.4 placement shorthand beside a property it sets, which only the "
               "cascade order of the two declarations resolves. BUILD the shorthands' expansion in the cascade",
               box_subject(el, buf, sizeof buf));
    if (vs != NULL) *start = gp_parse_line(vs, strlen(vs));
    if (ve != NULL) *end = gp_parse_line(ve, strlen(ve));
    /* §8.4: `grid-row`/`grid-column` set start before the slash and end after it; `grid-area` sets row-start,
       column-start, row-end, column-end in that order, and an omitted column-start copies a row-start
       `<custom-ident>` ("all four longhands are set to that value"). */
    if (vsh != NULL) {
        (void)gp_part(vsh, 0, start);
        *end = gp_part_or_copy(vsh, 1, start);
    }
    if (varea != NULL) {
        GpLine row_start = GP_LINE_AUTO;

        (void)gp_part(varea, 0, &row_start);
        if (rows) {
            *start = row_start;
            *end = gp_part_or_copy(varea, 2, &row_start);
        } else {
            *start = gp_part_or_copy(varea, 1, &row_start);
            *end = gp_part_or_copy(varea, 3, start);
            gp_line_free(&row_start);
        }
    }
    free(vs);
    free(ve);
    free(vsh);
    free(varea);
}

/* One axis of the explicit grid (§7.1 "The Explicit Grid"): its track count and the template whose areas
   name its lines. */
typedef struct {
    const CssGridTemplateAreas *areas;
    bool rows;
    size_t explicit_tracks;
} GpAxis;

/* The line §7.3.2 "Implicitly-Assigned Line Names" names `name` on the axis: "For each named grid area foo,
   four implicitly-assigned line names are created", `foo-start` on its start lines and `foo-end` on its end
   lines. One area answers a name at most once on an axis, so a name names at most one line. */
static bool gp_named_line(const GpAxis *ax, const char *name, long *out)
{
    size_t i, n = strlen(name);

    for (i = 0; i < ax->areas->n; i++) {
        const CssGridArea *a = &ax->areas->areas[i];
        size_t k = strlen(a->name);

        if (n == k + 6 && strncmp(name, a->name, k) == 0 && strcmp(name + k, "-start") == 0) {
            *out = (long)(ax->rows ? a->row_start : a->col_start);
            return true;
        }
        if (n == k + 4 && strncmp(name, a->name, k) == 0 && strcmp(name + k, "-end") == 0) {
            *out = (long)(ax->rows ? a->row_end : a->col_end);
            return true;
        }
    }
    return false;
}

/* `l` as a zero-based line index. A number counts from the explicit grid's start, or "in reverse, starting
   from the end edge of the explicit grid"; with a name, "only lines with that name are counted", and "if not
   enough lines with that name exist, all implicit grid lines are assumed to have that name". A lone
   `<custom-ident>` first matches "a grid line whose line name is <custom-ident>-start (for grid-*-start) /
   <custom-ident>-end (for grid-*-end)". A negative answer is a line before the explicit grid. */
static long gp_line_at(const GpAxis *ax, const GpLine *l, bool end_side)
{
    long at, have;

    DCHECK(l->kind == GP_LINE, "a grid line index was asked of a placement that is not a line");
    if (l->name == NULL) return l->n > 0 ? l->n - 1 : (long)ax->explicit_tracks + 1 + l->n;
    if (l->ident) {
        size_t k = strlen(l->name);
        char *edge = malloc(k + 7);

        CHECK(edge != NULL, "css-grid-1 §8.3: out of memory naming a grid area's edge");
        memcpy(edge, l->name, k);
        strcpy(edge + k, end_side ? "-end" : "-start");
        have = gp_named_line(ax, edge, &at);
        free(edge);
        if (have) return at;
    }
    have = gp_named_line(ax, l->name, &at);
    if (l->n > 0) return have >= l->n ? at : (long)ax->explicit_tracks + (l->n - have);
    return have >= -l->n ? at : -((-l->n) - have);
}

/* The line a span `l` reaches from line `from`, endward or startward: "N lines from its opposite edge in the
   corresponding direction", counting only lines of `l->name` when it has one, and assuming "all implicit grid
   lines on the side of the explicit grid corresponding to the search direction" have that name. */
static long gp_span_from(const GpAxis *ax, const GpLine *l, long from, bool endward)
{
    long at, have = 0;

    DCHECK(l->kind == GP_SPAN, "a grid span was asked of a placement that is not a span");
    if (l->name == NULL) return endward ? from + l->n : from - l->n;
    if (gp_named_line(ax, l->name, &at) && (endward ? at > from : at < from)) have = 1;
    if (have >= l->n) return at;
    if (endward) return (from > (long)ax->explicit_tracks ? from : (long)ax->explicit_tracks) + (l->n - have);
    return (from < 0 ? from : 0) - (l->n - have);
}

/* One axis's placement: its span, and whether its start track is definite. §8.3.1 "Grid Placement Conflict
   Handling" first: "If the start line is further end-ward than the end line, swap the two lines. If the start
   line is equal to the end line, remove the end line. If the placement contains two spans, remove the one
   contributed by the end grid-placement property. If the placement contains only a span for a named line,
   replace it with a span of 1." A missing span is 1 (§8.5: "if they're not explicitly specified, they default
   to 1"). */
static bool gp_axis_resolve(lxb_dom_element_t *el, const GpAxis *ax, size_t *track, size_t *span)
{
    GpLine s, e;
    long a = 0, b = 1;
    bool definite = true;
    char buf[160];

    gp_axis_lines(el, ax->rows, &s, &e);
    if (s.kind == GP_SPAN && e.kind == GP_SPAN) gp_line_free(&e);
    if (s.kind == GP_LINE && e.kind == GP_LINE) {
        a = gp_line_at(ax, &s, false);
        b = gp_line_at(ax, &e, true);
        if (a > b) { long t = a; a = b; b = t; }
        if (b == a) b = a + 1;
    } else if (s.kind == GP_LINE) {
        a = gp_line_at(ax, &s, false);
        b = e.kind == GP_SPAN ? gp_span_from(ax, &e, a, true) : a + 1;
    } else if (e.kind == GP_LINE) {
        b = gp_line_at(ax, &e, true);
        a = s.kind == GP_SPAN ? gp_span_from(ax, &s, b, false) : b - 1;
    } else {
        const GpLine *sp = s.kind == GP_SPAN ? &s : &e;

        definite = false;
        b = sp->kind == GP_SPAN && sp->name == NULL ? sp->n : 1;
    }
    gp_line_free(&s);
    gp_line_free(&e);
    if (definite && a < 0)
        DFAILF("%s is placed before the explicit grid's first line, which css-grid-1 §8.5 meets by adding implicit "
               "tracks at the start. BUILD those tracks", box_subject(el, buf, sizeof buf));
    DCHECK(b > a, "a grid area resolved to no tracks after css-grid-1 §8.3.1's conflict handling");
    *track = definite ? (size_t)a : 0;
    *span = (size_t)(b - a);
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

/* §7.1: "The size of the explicit grid is determined by the larger of the number of rows/columns defined by
   grid-template-areas and the number of rows/columns sized by grid-template-rows/grid-template-columns." */
static size_t gp_explicit_tracks(lxb_dom_element_t *container, const CssGridTemplateAreas *areas, bool rows,
                                 bool intrinsic)
{
    GridExplicitTracks t;
    size_t n = 0, from_areas = rows ? areas->rows : areas->columns;

    if (grid_explicit_tracks(container, rows, intrinsic, &t)) n = t.list.n;
    grid_explicit_tracks_free(&t);
    return n > from_areas ? n : from_areas;
}

void grid_placement_run(lxb_dom_element_t *container, bool intrinsic, GridPlacement *out)
{
    BoxTreeChild c;
    CssGridTemplateAreas areas;
    GpAxis row_axis, col_axis;
    size_t n = 0, cap = 0, i, explicit_columns, explicit_rows, columns, rows;
    size_t cursor_row = 0, cursor_col = 0;
    lxb_dom_element_t **items = NULL;
    bool *row_def, *col_def;
    size_t *row_at, *col_at, *rs, *cs, *row_cursor = NULL, row_cursors = 0;
    GpOccupancy o = { NULL, 0, 0 };

    DCHECK(out != NULL, "a grid was placed with nowhere to put it");
    (void)css_computed_grid_template_areas(container, &areas);
    explicit_columns = gp_explicit_tracks(container, &areas, false, intrinsic);
    explicit_rows = gp_explicit_tracks(container, &areas, true, intrinsic);
    row_axis.areas = col_axis.areas = &areas;
    row_axis.rows = true;
    col_axis.rows = false;
    row_axis.explicit_tracks = explicit_rows;
    col_axis.explicit_tracks = explicit_columns;
    columns = explicit_columns ? explicit_columns : 1;
    rows = explicit_rows;
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
        row_def[i] = gp_axis_resolve(items[i], &row_axis, &row_at[i], &rs[i]);
        col_def[i] = gp_axis_resolve(items[i], &col_axis, &col_at[i], &cs[i]);
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
    css_grid_template_areas_free(&areas);
}
