/* css-grid-1 §7.2 "Explicit Track Sizing: the grid-template-rows and grid-template-columns properties": the
 * `<track-list>` grammar, and the property's computed value over it ("as specified, with lengths made
 * absolute", §7.2.5 "Computed Value of a Track Listing").
 *
 * Lexbor's property registry carries neither property, so a declared track list reaches the cascade as raw
 * text that nothing has checked, and this parse is the first thing that looks. The two ways a value can fail
 * are answered differently, as core/css/css_transform_function.h answers them for `transform`: text outside
 * the grammar is CSS Syntax 3's invalid declaration and the parse returns false, so the property computes to
 * its initial `none`; a production §7.2 defines that is not built here crashes by name. */
/* Named residual. Not covered: line names (§7.2.2), inside `repeat()` as outside it. Next diff builds them as
 * names on the lines between tracks, which core/layout/grid_placement.h resolves. How its absence would show: a
 * page's grid aborting at this file's refusal of `[`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_GRID_TRACK_LIST_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_GRID_TRACK_LIST_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

#include "core/css/css_length.h"

/* One of a track's two sizing functions (§7.2.1 "Track Sizes"). A `<flex>` and `fit-content()` are max sizing
   functions only; `fit-content()`'s argument is in `length`. */
typedef enum {
    CSS_GRID_BREADTH_LENGTH = 0,   /* `<length-percentage>`: absolute, a percentage, or a math function */
    CSS_GRID_BREADTH_FLEX,         /* `<flex>`: the flex factor in `flex`, non-negative */
    CSS_GRID_BREADTH_AUTO,
    CSS_GRID_BREADTH_MIN_CONTENT,
    CSS_GRID_BREADTH_MAX_CONTENT,
    CSS_GRID_BREADTH_FIT_CONTENT
} CssGridBreadthKind;

typedef struct {
    CssGridBreadthKind kind;
    CssLength          length;
    double             flex;
} CssGridBreadth;

/* A `<track-size>` as its min and max sizing functions: `auto` is minmax(auto, auto); a `<flex>` "outside a
   minmax() notation, implies an automatic minimum (i.e. minmax(auto, <flex>))"; any other `<track-breadth>` is
   both; `fit-content(L)` is minmax(auto, fit-content(L)), "the smaller of minmax(auto, max-content) and
   minmax(auto, limit)". */
typedef struct {
    CssGridBreadth min, max;
} CssGridTrackSize;

/* The explicit tracks, in order, with an integer `repeat()` expanded (§7.2.3.1 "Syntax of repeat()"). An
   `<auto-repeat>` cannot be expanded here, because §7.2.3.2 "Repeat-to-fill: auto-fill and auto-fit repetitions"
   counts its repetitions from the grid container's size: its tracks are held once, as the `auto_n` tracks from
   `auto_at`, and `auto_n` is 0 for a list without one. `auto_fit` is `auto-fit` rather than `auto-fill`.
   `tracks` is owned and released by `css_grid_track_list_free`. */
typedef struct {
    CssGridTrackSize *tracks;
    size_t            n, auto_at, auto_n;
    bool              auto_fit;
} CssGridTrackList;

/* `text` against §7.2's `<track-list>`, lengths absolutized with `font` and `realm` as css_length.h's parse
   does. False for `none` and for text outside the grammar, with `*out` empty either way. */
bool css_grid_track_list_parse(JSContext *realm, const CssFontMetrics *font, const char *text,
                               CssGridTrackList *out);
void css_grid_track_list_free(CssGridTrackList *list);

#endif
