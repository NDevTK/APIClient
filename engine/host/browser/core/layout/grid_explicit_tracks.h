/* css-grid-1 §7.1 "The Explicit Grid"'s tracks on one axis of a grid container: the computed track list
 * (core/css/css_grid_track_list.h) with its `<auto-repeat>` expanded by §7.2.3.2 "Repeat-to-fill: auto-fill and
 * auto-fit repetitions"' count, which reads the container's size. core/layout/grid_placement.h sizes the
 * explicit grid from it and core/layout/grid_track.h sizes its tracks, so the two see one count. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_EXPLICIT_TRACKS_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_EXPLICIT_TRACKS_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_grid_track_list.h"

/* `list` holds every explicit track, its `auto_n` 0. The repeated tracks are `[repeat_lo, repeat_hi)`, an empty
   range for a list without an `<auto-repeat>`; `auto_fit` is `auto-fit`, whose empty repeated tracks the track
   sizing collapses after placement. Owned, released by `grid_explicit_tracks_free`. */
typedef struct {
    CssGridTrackList list;
    size_t           repeat_lo, repeat_hi;
    bool             auto_fit;
} GridExplicitTracks;

/* The explicit tracks of `container`'s columns (`vertical` false) or rows. `intrinsic` is a run sizing the
   container under a min-/max-content constraint, in which a size that depends on its containing block is
   indefinite. False, with `out` empty, for a `none` list. */
bool grid_explicit_tracks(lxb_dom_element_t *container, bool vertical, bool intrinsic, GridExplicitTracks *out);
void grid_explicit_tracks_free(GridExplicitTracks *t);

#endif
