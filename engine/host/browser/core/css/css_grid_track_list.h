/* css-grid-1 §7.2 "Explicit Track Sizing: the grid-template-rows and grid-template-columns properties": the
 * `<track-list>` grammar, and the property's computed value over it ("as specified, with lengths made
 * absolute", §7.2.5 "Computed Value of a Track Listing").
 *
 * Lexbor's property registry carries neither property, so a declared track list reaches the cascade as raw
 * text that nothing has checked, and this parse is the first thing that looks. The two ways a value can fail
 * are answered differently, as core/css/css_transform_function.h answers them for `transform`: text outside
 * the grammar is CSS Syntax 3's invalid declaration and the parse returns false, so the property computes to
 * its initial `none`; a production §7.2 defines that is not built here crashes by name. */
/* Named residual. Not covered: `minmax()`, `fit-content()`, `repeat()`, `min-content`, `max-content` and line
 * names — every `<track-size>` but a bare `<track-breadth>` of `<length-percentage>`, `<flex>` or `auto`. Next
 * diff builds `minmax()` with separate min and max sizing functions on a track. How its absence would show: a
 * page's grid aborting at this file's refusal of a function or bracket. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_GRID_TRACK_LIST_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_GRID_TRACK_LIST_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

#include "core/css/css_length.h"

/* One track's sizing function. `auto` is minmax(auto, auto); a `<flex>` "outside a minmax() notation, implies
   an automatic minimum (i.e. minmax(auto, <flex>))" (§7.2.1 "Track Sizes"); a `<length-percentage>` is both
   the min and the max. */
typedef enum {
    CSS_GRID_TRACK_AUTO = 0,
    CSS_GRID_TRACK_BREADTH,      /* `length`: absolute, a percentage, or a math function holding one */
    CSS_GRID_TRACK_FLEX          /* `flex`: the flex factor, non-negative */
} CssGridTrackKind;

typedef struct {
    CssGridTrackKind kind;
    CssLength        length;
    double           flex;
} CssGridTrackSize;

/* The explicit tracks, in order. `tracks` is owned and released by `css_grid_track_list_free`. */
typedef struct {
    CssGridTrackSize *tracks;
    size_t            n;
} CssGridTrackList;

/* `text` against §7.2's `<track-list>`, lengths absolutized with `font` and `realm` as css_length.h's parse
   does. False for `none` and for text outside the grammar, with `*out` empty either way. */
bool css_grid_track_list_parse(JSContext *realm, const CssFontMetrics *font, const char *text,
                               CssGridTrackList *out);
void css_grid_track_list_free(CssGridTrackList *list);

#endif
