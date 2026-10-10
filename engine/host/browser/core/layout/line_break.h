/* Where a sequence of characters may break: [UAX14] Unicode Line Breaking Algorithm §6 "Line Breaking
 * Algorithm", rules LB1 through LB31, over the Line_Break property core/layout/line_break_class.h carries.
 *
 * css-text-3 §5 "Line Breaking and Word Boundaries" makes the soft wrap opportunity a property of the content
 * with [UAX14] as the baseline, and §5.5 "Line Breaking Details" makes parts of it mandatory for a CSS UA (BK
 * and NL break "regardless of the white-space value"; WJ, ZW, GL, ZWJ, CM and SG "must be honored"). It is a
 * component so it can be checked on its own against the UCD's LineBreakTest.txt.
 *
 * It takes the whole sequence because the rules look forward past the boundary they decide (LB25 reads three
 * characters right, LB15c and LB28a two) and LB9 lets an unbounded run of combining marks sit in between, so no
 * fixed-depth streaming decider is correct (Blink likewise breaks an inline formatting context's collected text).
 *
 * It does not tailor (UAX14 §8 "Customization"; css-text-3 §5.1-§5.4: word-break, line-break, hyphens,
 * overflow-wrap): the cascade computes none of those properties, so the untailored default is the answer.
 * core/layout/text_run.c asserts that where it reads the cascade; this file holds no CSS. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BREAK_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BREAK_H

#include <stddef.h>
#include <stdint.h>

#include "core/layout/line_break_class.h"

/* UAX14 §6's three outcomes at a position. `!` is mandatory (LB3, LB4, LB5): css-text-3's forced line break,
   not a soft wrap opportunity. `÷` is a soft wrap opportunity. `×` is prohibited, always by some rule's
   positive statement, since LB31 "Break everywhere else" makes every unprohibited position an opportunity. */
typedef enum {
    LINE_BREAK_PROHIBITED = 0,
    LINE_BREAK_OPPORTUNITY = 1,
    LINE_BREAK_MANDATORY = 2,
} LineBreakAction;

/* [UAX14] LB1's resolved Line_Break class of one code point. Exported so a caller's asserts (core/layout/
   text_run.c) read the same table this algorithm does rather than a second copy. */
LineBreakClass line_break_class_of(uint32_t cp);

/* The action at every position of `cps[0 .. count-1]`, written into `out[0 .. count]` (one more entry than
   characters). `out[i]` is the boundary immediately before `cps[i]`: `out[0]` is LB2 "Never break at the start
   of text" (prohibited) and `out[count]` is LB3 "Always break at the end of text" (mandatory). For an empty
   sequence "an empty string would consist of sot followed immediately by eot" and LB2 applies before LB3, so
   its one position is prohibited. */
void line_break_actions(const uint32_t *cps, size_t count, LineBreakAction *out);

#endif
