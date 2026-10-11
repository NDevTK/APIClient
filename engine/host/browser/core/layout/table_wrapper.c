/* CSS 2.1 §17.4 Tables in the visual formatting model: the table wrapper box. See table_wrapper.h. */
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "core/layout/table_wrapper.h"

bool table_wrapper_generates(CssDisplay display)
{
    return css_display_spells(display, CSS_KEYWORD_TABLE) || css_display_spells(display, CSS_KEYWORD_INLINE_TABLE);
}

bool table_wrapper_is_block_level(CssDisplay display)
{
    DCHECK(table_wrapper_generates(display),
           "CSS 2.1 §17.4's \"'block' box if the table is block-level, and an 'inline-block' box if the table "
           "is inline-level\" was asked of an element that generates NO table wrapper box. The two answers "
           "here are BLOCK-LEVEL and INLINE-LEVEL, and neither of them is \"there is no box\" — a caller that "
           "let this predicate answer that third question would read a false as \"inline-level\" for every "
           "element in the document. `table_wrapper_generates` is the question that was skipped");
    return css_display_spells(display, CSS_KEYWORD_TABLE);
}

bool table_wrapper_owns_property(const char *name)
{
    /* §17.4's closed list; `margin-*` is the four longhands. */
    static const char *const WRAPPER[] = {
        "position", "float",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "top", "right", "bottom", "left",
        /* css-tables-3 §3.6.1 Overrides applying in all modes adds "the properties whose use could force the
           used value of transform-style to flat (see list) and their shorthands/longhands relatives: this list
           currently includes overflow, opacity, filter, clip, clip-path, isolation, mask-*, mix-blend-mode,
           transform-* and perspective". The `-*` families are matched by prefix below. */
        "overflow", "overflow-x", "overflow-y", "opacity", "filter", "clip", "clip-path", "isolation",
        "mix-blend-mode", "perspective",
    };
    static const char *const WRAPPER_FAMILY[] = { "mask", "transform" };
    size_t i;

    DCHECK(name != NULL, "CSS 2.1 §17.4's declaration split was asked about a NULL property name");
    for (i = 0; i < COUNTOF(WRAPPER); i++)
        if (strcmp(name, WRAPPER[i]) == 0) return true;
    for (i = 0; i < COUNTOF(WRAPPER_FAMILY); i++) {
        size_t n = strlen(WRAPPER_FAMILY[i]);
        if (strncmp(name, WRAPPER_FAMILY[i], n) == 0 && (name[n] == '\0' || name[n] == '-')) return true;
    }
    /* "all other values of non-inheritable properties are used on the table box". Inherited properties are
       the caller's to keep out (see the header); a false for one would wrongly read as "the table box's". */
    return false;
}
