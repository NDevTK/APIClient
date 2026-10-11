/* CSS 2.1 §17.4 Tables in the visual formatting model: the table wrapper box, the box §9.4.1's stack places.
 *
 * A `display: table` element generates two boxes: "the table generates a principal block box called the table
 * wrapper box that contains the table box itself and any caption boxes (in document order)", and §17.4 splits
 * the element's declarations between them. The split is answered here once so no walk reads `margin-top` and
 * `border-top-width` off one merged box. "The table wrapper box is a 'block' box if the table is block-level,
 * and an 'inline-block' box if the table is inline-level": a table is block-level without being a block
 * container (§9.2.1), and the wrapper "establishes a block formatting context", so §8.3.1's runs stop at it.
 *
 * Not here: the wrapper's height (§10.6.3 over captions and table box; core/layout/block_flow.c's stack; with
 * no caption it is the table box's border-box height, since the wrapper has no border or padding and the table
 * box no margin); its width ("the border-edge width of the table box inside it, as described by section
 * 17.5.2", composed from core/layout/used_value.h and core/layout/table_width.h); caption order (§17.4.1
 * Caption position and alignment, a placement question for core/layout/flow_position.h); and which captions it
 * holds (core/layout/table_box.h's `table_box_captions`). */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_WRAPPER_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_WRAPPER_H

#include <stdbool.h>

#include "core/css/css_display.h"

/* Whether an element with this computed `display` generates a table wrapper box: true for `table` and
   `inline-table`, §17.2 The CSS table model's two table values. */
bool table_wrapper_generates(CssDisplay display);

/* Whether that wrapper is block-level (§17.4): `table` true, `inline-table` false. A wrapper must exist
   (asserted), since "not block-level" and "no box" are different answers. */
bool table_wrapper_is_block_level(CssDisplay display);

/* §17.4's declaration split: is the computed value of `name` on the table element used on the wrapper (true)
   or on the table box (false)? The predicate is §17.4's closed list: "The computed values of properties
   'position', 'float', 'margin-*', 'top', 'right', 'bottom', and 'left' on the table element are used on the
   table wrapper box and not the table box; all other values of non-inheritable properties are used on the
   table box and not the table wrapper box." — widened by css-tables-3 §3.6.1 Overrides applying in all modes
   with `overflow`, `opacity`, `filter`, `clip`, `clip-path`, `isolation`, `mask-*`, `mix-blend-mode`,
   `transform-*` and `perspective`.
   Ask about non-inherited properties only: an inherited one reaches both boxes from the parent
   (core/css/css_defaulting.c has the inherited list). The other box gets the initial value ("the initial
   values are used instead"), which the consumer supplies. */
bool table_wrapper_owns_property(const char *name);

#endif
