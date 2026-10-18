/* css-grid-1 §7.3 "Named Areas: the grid-template-areas property": the `none | <string>+` grammar and the
 * named grid areas it defines. Lexbor's property registry does not carry the property, so a declaration
 * reaches the cascade as raw text, and this parse is the first thing that checks it. Text outside the grammar,
 * a trash token, rows of unequal length, or an area that is not a filled-in rectangle make the declaration
 * invalid ("or else the declaration is invalid"), and the property computes to its initial `none`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_GRID_TEMPLATE_AREAS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_GRID_TEMPLATE_AREAS_H

#include <stdbool.h>
#include <stddef.h>

/* One named grid area, as zero-based grid line indices: it covers rows `[row_start, row_end)` and columns
   `[col_start, col_end)`. `name` is owned by the list. */
typedef struct {
    char  *name;
    size_t row_start, row_end, col_start, col_end;
} CssGridArea;

/* The template: `rows` strings of `columns` cells each, and its named areas in order of first appearance.
   Owned, and released by `css_grid_template_areas_free`. */
typedef struct {
    CssGridArea *areas;
    size_t       n, rows, columns;
} CssGridTemplateAreas;

/* `text` against §7.3's grammar. False for `none` and for an invalid declaration, with `*out` empty. */
bool css_grid_template_areas_parse(const char *text, CssGridTemplateAreas *out);
void css_grid_template_areas_free(CssGridTemplateAreas *a);

#endif
