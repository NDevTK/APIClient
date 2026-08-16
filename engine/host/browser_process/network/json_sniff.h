/* SNIFFING FOR JSON — Chromium's `CrossOriginReadBlocking::SniffForJSON`, and nothing else.
 *
 * WHOSE ALGORITHM, AND WHY IT IS NOT IN mime_sniff.c. WHATWG MIME Sniffing has NO JSON row: §6's tables are
 * binary magic and §7.1's are markup, so the standard cannot be asked this question at all. Putting it in the
 * file whose header says "WHATWG MIME Sniffing §5-§7" would make that sentence false, and the file next to it
 * would then be the place a reader goes to check the standard and finds something else. It is Chromium's, so
 * it sits beside corb.c's other Chromium rules, in a translation unit of its own.
 *
 * WHY IT IS A TRANSLATION UNIT AT ALL. It had ONE caller and was static inside corb.c, which was right while
 * that was true. It has TWO now — CORB blocks a cross-origin body that sniffs as JSON from reaching a code
 * loader, and resource_kind.c reads the same answer to decide that a body served under a JavaScript MIME type
 * is API DATA rather than a script — and a second static copy is exactly the pair of implementations that can
 * disagree about one response. The move is what CLAUDE.md means by splitting until each piece is one assertable
 * contract: what this file promises is a single sentence, and both callers get the same answer to it.
 *
 * IT DELIBERATELY DOES NOT SNIFF AN ARRAY, which is Chromium's position rather than an omission: a top-level
 * `[1,2,3]` IS a valid JavaScript expression, so it is not evidence that a server mislabelled anything, while a
 * top-level `{"a":` is a syntax error as a JS statement and therefore is. */
#ifndef ENGINE_HOST_BROWSER_PROCESS_NETWORK_JSON_SNIFF_H
#define ENGINE_HOST_BROWSER_PROCESS_NETWORK_JSON_SNIFF_H

#include <stdbool.h>
#include <stddef.h>

/* True when the bytes OPEN a JSON object: `{`, then a double-quoted string, then `:`, with ASCII whitespace and
   JavaScript comments skipped outside the string literal.
   `d` is a PREFIX of the resource — whatever the caller holds of the resource header — and running out of data
   is a NO. That is Chromium's third answer (`kMaybe`, "sniff again when more arrives") collapsed by a fact both
   callers have and it does not: the whole resource header is already in hand, so no more is coming. */
bool json_sniff(const unsigned char *d, size_t n);

#endif
