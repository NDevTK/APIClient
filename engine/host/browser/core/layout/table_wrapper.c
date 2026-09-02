/* CSS 2.1 §17.4 Tables in the visual formatting model — the table wrapper box. See table_wrapper.h for the
   contract, for §17.4's declaration split, and for which of the wrapper's own numbers are still §17.5's. */
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "core/layout/table_wrapper.h"

bool table_wrapper_generates(const char *display)
{
    DCHECK(display != NULL,
           "CSS 2.1 §17.4's table wrapper box was asked about a NULL computed `display`. §17.2 The CSS table "
           "model assigns table formatting rules by that value and by nothing else, so there is no table and "
           "therefore no wrapper to answer for without one");
    return strcmp(display, "table") == 0 || strcmp(display, "inline-table") == 0;
}

bool table_wrapper_is_block_level(const char *display)
{
    DCHECK(table_wrapper_generates(display),
           "CSS 2.1 §17.4's \"'block' box if the table is block-level, and an 'inline-block' box if the table "
           "is inline-level\" was asked of an element that generates NO table wrapper box. The two answers "
           "here are BLOCK-LEVEL and INLINE-LEVEL, and neither of them is \"there is no box\" — a caller that "
           "let this predicate answer that third question would read a false as \"inline-level\" for every "
           "element in the document. `table_wrapper_generates` is the question that was skipped");
    return strcmp(display, "table") == 0;
}

bool table_wrapper_owns_property(const char *name)
{
    /* §17.4's list, verbatim and closed: "The computed values of properties 'position', 'float', 'margin-*',
       'top', 'right', 'bottom', and 'left' on the table element are used on the table wrapper box and not the
       table box". `margin-*` is the four longhands; the four offsets are named individually by the section
       itself, so this table is the sentence and not a reading of it. */
    static const char *const WRAPPER[] = {
        "position", "float",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "top", "right", "bottom", "left",
    };
    size_t i;

    DCHECK(name != NULL, "CSS 2.1 §17.4's declaration split was asked about a NULL property name");
    for (i = 0; i < COUNTOF(WRAPPER); i++)
        if (strcmp(name, WRAPPER[i]) == 0) return true;
    /* "all other values of non-inheritable properties are used on the table box and not the table wrapper
       box" — so every name outside the list above belongs to the table box, and there is no third answer for
       this predicate to have. The INHERITED properties the sentence excludes are the caller's to keep out
       (see the header): they are inherited by both boxes from the same parent, so the question does not arise
       for them and a `false` here would read as "the table box's alone", which is wrong in a way nothing
       downstream could see. */
    return false;
}
