/* THE INK — the mark vocabulary and the ordered sequence. See display_list.h for why the order is the list's
   whole statement, why a rectangle carries the environment facts it is a function of, and why a colour is not
   quantized here. */
#include <stddef.h>
#include <stdlib.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_length.h"
#include "core/paint/display_list.h"

void display_list_init(DisplayList *dl)
{
    DCHECK(dl != NULL, "a display list was initialised through no list");
    dl->v = NULL;
    dl->n = dl->cap = 0;
}

void display_list_free(DisplayList *dl)
{
    DCHECK(dl != NULL, "a display list was released through no list");
    free(dl->v);
    dl->v = NULL;
    dl->n = dl->cap = 0;
}

void display_list_append(DisplayList *dl, const DisplayMark *mark)
{
    DCHECK(dl != NULL, "a mark was appended to no display list");
    DCHECK(mark != NULL, "a display list was appended to with no mark — an append is a COPY of the caller's "
                         "mark, so there is no arm here under which the absence of one is a statement");
    DCHECK(mark->kind == DISPLAY_MARK_FILL_RECT,
           "a display mark carries a kind this component does not define. The enum is a closed VOCABULARY this "
           "engine writes, so a value outside it is this engine's own uninitialised memory rather than a "
           "document's; see display_list.h's residual for which kinds CSS 2.1 §E.2 \"Painting order\"'s "
           "sub-lists still name and which diff builds each");
    /* THE SPACE AND THE ALPHA ARE THIS ENGINE'S OWN AND ARE THEREFORE ASSERTABLE, which is the line
       CLAUDE.md draws and which a colour read out of a page makes worth drawing explicitly. The page states
       the SPECIFIED colour and this engine states the used one: the space is sRGB because the painter ran CSS
       Color 4 §11 "Converting Colors" on it, and the alpha is in range because CSS Color 4 §16.1.2 puts it
       there — core/css/css_color.h records that sentence in its own words, "<alpha-value>s which were
       specified outside the valid range are clamped at parse time". Neither is a claim about what the
       document declared. */
    DCHECK(mark->color.space == CSS_COLOR_SPACE_SRGB,
           "a display mark's colour is not in sRGB. CSS Color 4 §11 \"Converting Colors\" is the painter's to "
           "run, once, where the cascade is read; a mark whose space were free to vary would make every "
           "consumer of a list ask a question the painter already answered, and two answers to one question is "
           "the shape that drifts");
    DCHECK(mark->color.a >= 0.0 && mark->color.a <= 1.0,
           "a display mark's alpha is outside [0, 1]. CSS Color 4 §16.1.2 clamps an <alpha-value> at parse "
           "time, so a value outside the range did not come from a declaration — it came from arithmetic this "
           "engine performed on one");
    /* NOTHING HERE ASSERTS THE GEOMETRY, and that is a deliberate refusal rather than an omission. The one
       invariant worth having over a rectangle — that an extent is a distance between parallel edges and
       cannot be negative — is core/dom/element_view.h's, asserted at `ev_length_long` where §6's six extents
       are produced. Restating it here would be a second copy of one rule, free to drift from the first and
       reported at the consumer instead of at the producer. */
    if (dl->n == dl->cap) {
        size_t cap = dl->cap ? dl->cap * 2 : 8;
        DisplayMark *v = realloc(dl->v, cap * sizeof *v);

        CHECK(v != NULL, "paint: OOM growing a display list — a dropped mark is ink that is silently absent "
                         "from a sequence whose whole statement is that it is complete and in CSS 2.1 §E.2 "
                         "\"Painting order\"'s order, and no consumer could tell that list from a document "
                         "with less in it");
        dl->v = v;
        dl->cap = cap;
    }
    dl->v[dl->n++] = *mark;
}

CssEnvSet display_list_env(const DisplayList *dl)
{
    CssEnvSet env = CSS_ENV_NONE;
    size_t i;
    unsigned k;

    DCHECK(dl != NULL, "the environment set of no display list was asked for");
    /* THE UNION OVER EVERY COORDINATE OF EVERY MARK, which is css_length.h's own rule for a sum read over a
       sequence instead of over two operands: "the sum of two lengths derived from DIFFERENT facts is a
       function of BOTH, which is the union". The COLOUR contributes nothing — a used colour is a function of
       the cascade and of `currentcolor`'s walk up the tree, and neither is one of core/frame/viewport.h's
       picked facts — so a list of ink whose geometry is determined is a list with no arm to explore however
       many colours are in it. */
    for (i = 0; i < dl->n; i++)
        for (k = 0; k < 4; k++) env |= dl->v[i].rect[k].env;
    return env;
}
