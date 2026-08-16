/* FETCH'S "determine nosniff" — the one algorithm that turns `X-Content-Type-Options` into the flag §5 of MIME
 * Sniffing calls the no-sniff flag.
 *
 * WHY IT IS A COMPONENT AND NOT AN ARGUMENT THE CALLER DERIVES. `mime_sniff.h` states, and still states, that
 * §7 takes the ANSWER rather than reaching back into a header list for a second fact about one response — that
 * contract is between §7 and ITS callers (corb.c, resource_kind.c), and it is unchanged. What was wrong is one
 * level up: the ABI took the flag, so the DERIVATION lived in `extension/lib/safe-fetch.js`, spelled
 * `_cto.toLowerCase().indexOf("nosniff") >= 0`. That is a substring test where Fetch specifies "getting,
 * decoding, and splitting" the header and then an ASCII case-insensitive match of the FIRST value: a response
 * carrying `X-Content-Type-Options: foo, nosniff` sets the flag under the substring test and does not set it
 * under the standard, and a second ABI entry needing the same fact would have made that a second copy of a
 * wrong algorithm rather than a first. So the boundary carries the header VALUE, which is a fact the trusted
 * zone READ, and the flag is computed here, which is a fact the standard DEFINES.
 *
 * THE VALUE IS AS "get a header" JOINS IT — the list's duplicates already run together with 0x2C 0x20 between
 * them — which is exactly the string `mime_type_extract` takes for the sibling header, and for the same reason:
 * the two zones that hold header lists hold them differently and both can produce the joined value. */
#ifndef ENGINE_HOST_BROWSER_PROCESS_NETWORK_NOSNIFF_H
#define ENGINE_HOST_BROWSER_PROCESS_NETWORK_NOSNIFF_H

#include <stdbool.h>

/* Fetch's DETERMINE NOSNIFF, over the joined `X-Content-Type-Options` value, or NULL for the header being
   absent — which is the spec's "values is null" and its own first step, so NULL is a positive statement that
   no such header arrived and never a hole a caller filled with an empty string (an EMPTY header value is a
   real thing a server can send, and it is a value that does not match, not an absent one). */
bool nosniff_determine(const char *x_content_type_options_value);

#endif
