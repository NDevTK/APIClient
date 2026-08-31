/* WHERE A SEQUENCE OF CHARACTERS MAY BREAK — [UAX14] Unicode Line Breaking Algorithm §6 "Line Breaking
 * Algorithm", rules LB1 through LB31, over the Line_Break property core/layout/line_break_class.h carries.
 *
 * WHY THIS IS A COMPONENT AND NOT A PREDICATE INSIDE THE MEASUREMENT. css-text-3 §5 "Line Breaking and Word
 * Boundaries" makes the soft wrap opportunity a property of the CONTENT, and its note names the baseline:
 * "[UAX14] defines a baseline behavior for line breaking for all scripts in Unicode, which is expected to be
 * further tailored." §5.5 "Line Breaking Details" then makes four parts of it MANDATORY for a CSS UA rather
 * than advisory — BK and NL are forced line breaks "regardless of the white-space value"; the behavior defined
 * for WJ, ZW, GL and ZWJ "must be honored"; and "CSS never allows soft wrap opportunities within typographic
 * character units. Thus, the CM and SG Unicode line breaking classes must always be honored." So the algorithm
 * is a spec obligation with its own oracle (the UCD's LineBreakTest.txt), and a copy of it living inside a
 * width accumulator would be an obligation with no way to be checked on its own.
 *
 * IT TAKES THE WHOLE SEQUENCE, AND THAT IS FORCED BY THE RULES RATHER THAN CHOSEN FOR CONVENIENCE. A streaming
 * decider that is handed one character at a time cannot answer, because UAX14's rules look FORWARD past the
 * boundary they decide: LB25's `PO × OP IS NU` reads three characters to the right of the boundary, LB15c's
 * `SP ÷ IS NU` and LB28a's `(AK | [◌] | AS) × (AK | [◌] | AS) VF` read two, and LB15b and LB19a read one plus
 * eot. Worse, LB9 lets an UNBOUNDED run of combining marks sit between the boundary and the character that
 * decides it — `PO OP CM CM … CM IS NU` is one pattern with any number of marks in the middle — so no fixed
 * pipeline depth is enough and a bounded one would be a cap on which documents get the right answer. Blink
 * resolves this the same way, by collecting an inline formatting context's text content into one buffer and
 * running the break iterator over that; this interface is that shape.
 *
 * IT ANSWERS FOR EVERY POSITION INCLUDING eot, because LB3 "Always break at the end of text" is a rule and not
 * an edge case: a caller that stopped at the last character would be implementing LB2 and LB3 itself, in a
 * file whose asserts do not cover them.
 *
 * WHAT IT DOES NOT DO IS TAILOR. UAX14 §8 "Customization" and css-text-3 §5.1 "Breaking Rules for Letters: the
 * word-break property", §5.2 "Line Breaking Strictness: the line-break property", §5.3 "Hyphenation: the
 * hyphens property" and §5.4 "Overflow Wrapping: the overflow-wrap (word-wrap) property" are the
 * four knobs a CSS UA turns, and this engine's cascade derives a computed value for none of them — so there
 * are no criteria to tailor with and the untailored default is the answer. core/layout/text_run.c asserts that
 * at the one place the cascade is read; this file holds no CSS at all, which is why the assert is there and
 * not here. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BREAK_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BREAK_H

#include <stddef.h>
#include <stdint.h>

#include "core/layout/line_break_class.h"

/* UAX14 §6's three outcomes at a position, which are the annex's own three notations. `!` is a MANDATORY break
   (LB3, LB4, LB5) and is not a soft wrap opportunity at all — css-text-3 §5.5 calls it a forced line break and
   §5 separates the two by name. `÷` is an OPPORTUNITY, css-text-3 §5's soft wrap opportunity. `×` is
   PROHIBITED, which is not "no rule matched": LB31 "Break everywhere else" means every position a rule does
   not prohibit IS an opportunity, so a prohibition is always some rule's positive statement. */
typedef enum {
    LINE_BREAK_PROHIBITED = 0,
    LINE_BREAK_OPPORTUNITY = 1,
    LINE_BREAK_MANDATORY = 2,
} LineBreakAction;

/* [UAX14] LB1's resolved Line_Break class of one code point. Exported because a CALLER's own asserts are
   stated in these terms — core/layout/text_run.c has to know that a character it is about to measure is not a
   forced break before it can claim its max-content answer is one line — and because a caller re-deriving the
   class from a second copy of the table is the one way the two could disagree. */
LineBreakClass line_break_class_of(uint32_t cp);

/* THE ACTION AT EVERY POSITION of `cps[0 .. count-1]`, written into `out[0 .. count]` — ONE MORE THAN THERE
   ARE CHARACTERS. `out[i]` is the action at the boundary IMMEDIATELY BEFORE `cps[i]`, so `out[0]` is LB2
   "Never break at the start of text" (always PROHIBITED) and `out[count]` is LB3 "Always break at the end of
   text" (always MANDATORY). A ZERO-LENGTH sequence is therefore not a degenerate call and its one position is
   not MANDATORY: UAX14 says outright that "an empty string would consist of sot followed immediately by eot",
   and LB2 is applied BEFORE LB3, so sot decides it and `out[0]` is PROHIBITED. */
void line_break_actions(const uint32_t *cps, size_t count, LineBreakAction *out);

#endif
