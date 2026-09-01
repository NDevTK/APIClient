/* compose.h — the ONE way a census, a shape or a document becomes bytes in this engine: a printf whose buffer
 * is the length it MEASURES rather than a number somebody counted.
 *
 * WHAT THIS REPLACES, AND WHY IT IS A COMPONENT RATHER THAN A HABIT. Every composer on the result seam used to
 * carry `size_t n = <constant>` beside a paragraph deriving that constant by hand from its own format string —
 * the fixed bytes with the conversion specifiers removed, plus each counter's widest decimal form — and under
 * it the instruction RE-DO THE COUNTS WHEN YOU ADD A ROW. Six composers carried one, and TWO of them had
 * already been got wrong, both in the safe-looking direction:
 *
 *   `result_json`'s said "467 with its conversion specifiers and 407 without" and "nineteen counters … 335"
 *   against a string measuring 508/442 over twenty-one counters at 375 — five numbers wrong, a stated worst
 *   case of 742, and a real one of 818 that the 768-byte buffer those numbers justified did not fit.
 *
 *   `result_cold_json`'s said "fixed bytes 503 … the thirty-nine numbers' widest forms are 753" against 521
 *   over FORTY, so the honest sum was 1295 inside a 1280-byte buffer already fifteen bytes short.
 *
 * NEITHER WAS CAUGHT BY THE ASSERT WRITTEN TO CATCH IT, and that is the part worth keeping. Each site had a
 * `DCHECK(m > 0 && m < n)` under its snprintf, and a fit assert fires only on a document WIDE ENOUGH to reach
 * the end of the buffer — the numbers a real page produces are small, so an under-count sits inside the slack
 * for as long as the slack lasts. The margin that was raised to protect a bad count is the same margin that
 * hides one. An arithmetic and an assert that can only fire once the arithmetic has already been wrong for a
 * while are not two halves of a mechanism; they are one mechanism and a witness who was looking elsewhere.
 *
 * SO THE QUESTION CHANGES RATHER THAN THE ANSWER GETTING BETTER. "What is the WIDEST expansion of this format
 * string" is what no portable question can be asked, which is exactly what `result_cold_json`'s paragraph said
 * before concluding the counts had to stay by hand. "What is the ACTUAL expansion of this format string with
 * THESE arguments" is answered exactly, by C, in the library every host already links: C99 §7.19.6.12 "The
 * vsnprintf function" — "The vsnprintf function returns the number of characters that would have been written
 * had n been sufficiently large, not counting the terminating null character, or a negative value if an
 * encoding error occurred" — and §7.19.6.5 "The snprintf function" — "If n is zero, nothing is written, and s
 * may be a null pointer" — which is what makes the measuring pass legal with no buffer in existence. Measure,
 * allocate exactly, write. There is then no count to keep in step with a string, no host-dependence (the old
 * terms were a 64-bit host's `long` at 20 digits, generous by nine on the WASM32 host that actually ships, so
 * a miscount could not surface first where the code runs), and no slack for a miscount to hide in.
 *
 * IT IS THE RULE THIS TREE ALREADY CHOSE, TWICE, ONE LAYER DOWN. check.h's own emitter records it in these
 * words — "A LONGER PER-SITE BUFFER IS NOT THE FIX, because the next author picks the next number. The emitter
 * owns the sizing, so no call site has a buffer at all" — and solver/concolic.c's `shapef` is the same two
 * passes for the same reason, after fixed 192- and 224-byte shape buffers gave two different sources one
 * provenance. The result-seam composers were the last holdouts. This file is not a new idea; it is that idea
 * with one implementation instead of three.
 *
 * THE FORMAT ATTRIBUTE IS LOAD-BEARING AND NOT DECORATION. Hiding a composition behind a helper is what would
 * otherwise cost its call site `-Wformat` checking, and a twenty-six-argument list the compiler no longer
 * matches against its string is a worse defect than the one this replaces: a `%ld` handed an `int` is
 * undefined behaviour a hand count cannot even be wrong about. `APICLIENT_PRINTF(1, 2)` keeps every argument
 * list checked exactly as an inline `snprintf` was — verified by mismatching one deliberately and reading the
 * warning, which is the only way to know an attribute is attached to the declaration the caller sees.
 *
 * `static inline` RATHER THAN A TRANSLATION UNIT, following check.h's own helpers: there is no state here to
 * have one copy of, and every caller composing the same bytes from the same two passes is the point. */
#ifndef ENGINE_HOST_SOLVER_COMPOSE_H
#define ENGINE_HOST_SOLVER_COMPOSE_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "check.h"

static inline char *composef(const char *fmt, ...) APICLIENT_PRINTF(1, 2);

/* The composed string, exactly as long as it needs to be, on the heap; NULL only on allocation failure, which
   every caller on this seam already treats as "this census is absent" rather than as a reason to fail a run. */
static inline char *composef(const char *fmt, ...)
{
    va_list ap;
    char *out;
    int need, wrote;

    DCHECK(fmt != NULL, "a document was composed from no format string at all — there is nothing to measure "
                        "and nothing to write, and the length that came back would be a fact about whatever "
                        "the caller passed instead");
    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    /* AN ENCODING ERROR IS THE ONE WAY OUT OF vsnprintf THAT IS NOT A LENGTH, and it is fatal in EVERY build
       rather than dev-only: there is no measurement to allocate against, so the alternative to aborting is
       choosing a buffer size — which is the thing this component exists to stop anyone doing. */
    CHECK(need >= 0, "a document could not be MEASURED — vsnprintf reported an encoding error, so there is no "
                     "length to allocate and any size chosen here would be the hand-counted guess this "
                     "composer replaced");
    out = malloc((size_t)need + 1);
    if (!out) return NULL;
    va_start(ap, fmt);
    wrote = vsnprintf(out, (size_t)need + 1, fmt, ap);
    va_end(ap);
    /* THE RELATIONSHIP, AT ITS ORIGIN, AND IT IS THE INVARIANT THAT REPLACES THE BYTE COUNT. Two passes over
       one argument list can only disagree if an argument was consumed differently between them, and the
       consequence of a silent disagreement is what every one of the deleted fit asserts named: a truncation
       here does not lose a digit, it loses the CLOSING BRACE, so the host is handed a document that will not
       parse and reports NOTHING for the page — the loudest possible consequence arriving as the quietest
       possible bug. Unlike those asserts this one has ZERO slack in front of it, so it fires on the first byte
       of a disagreement rather than after a document has grown into a margin that was itself a guess. */
    DCHECK(wrote == need,
           "a document was WRITTEN to a different length than it was MEASURED for — the two passes read one "
           "argument list, so they can only disagree if an argument was consumed differently between them, and "
           "the document about to be published is truncated at its closing brace");
    (void)wrote;
    return out;
}

#endif /* ENGINE_HOST_SOLVER_COMPOSE_H */
