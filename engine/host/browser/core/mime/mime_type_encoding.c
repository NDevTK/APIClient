/* Fetch §3.5 "legacy extract an encoding" — the one algorithm that reads a §4 MIME record and answers in the
 * ENCODING registry's vocabulary. Declared in core/mime/mime_type.h beside `extract a MIME type`, because Fetch
 * states both in §3.5 and HTML §8.1.4.2's "fetch a classic script" runs them back to back on one header list.
 *
 * WHY IT IS ITS OWN TRANSLATION UNIT, which is a LAYERING statement and not a size one. §4's record is a pure
 * value type over strings: it allocates, it compares bytes, and it needs nothing but the C library. §4.2's "get
 * an encoding" is equally pure — a sorted label table and a binary search — but it is compiled inside
 * core/encoding/encoding.c, which is the whole TextEncoder/TextDecoder component and therefore includes
 * quickjs.h, solver/cow.h, core/realm.h and core/idl_args.h. One line in mime_type.c reached it, and that one
 * line put the entire JS realm behind the MIME record.
 *
 * That mattered the moment a program without a realm needed the record. WHATWG MIME Sniffing §7 is the NETWORK
 * SERVICE'S algorithm (browser_process/network/mime_sniff.c), it computes into a `MimeType`, and the browser
 * process links neither quickjs nor a realm — so with the two in one object the sniffer could not be built at
 * all outside the renderer, which is exactly the housing problem §7 had. Splitting the reach-out is the root
 * fix: core/mime's record now has no dependency outside the C library, and this file is where the dependency
 * that does exist is declared, in one place a reader can see.
 */
#include <string.h>

#include "core/encoding/encoding.h"   /* §4.2's get an encoding, which §3.5 step 3 runs on the charset */
#include "core/mime/mime_type.h"

/* The four steps, in order, each with its own return. Every one of them answers `fallbackEncoding`, which is
   why the algorithm is "legacy": the header gets to narrow the answer and never to widen it into a failure. */
int mime_type_legacy_extract_encoding(const MimeType *m, int fallback_encoding)
{
    const char *charset;
    int tentative;

    /* "If mimeType is failure, then return fallbackEncoding." */
    if (!m) return fallback_encoding;
    /* "If mimeType["charset"] does not exist, then return fallbackEncoding." — the record's own ordered map,
       and `mime_type_parameter` answers NULL for exactly the standard's "does not exist". A charset present and
       EMPTY is not this case: §4.4 drops a parameter with no value, so it never reaches the record. */
    charset = mime_type_parameter(m, "charset");
    if (!charset) return fallback_encoding;
    /* "Let tentativeEncoding be the result of getting an encoding from mimeType["charset"]. If
       tentativeEncoding is failure, then return fallbackEncoding. Return tentativeEncoding." A label this
       registry does not know is the standard's failure, and the fallback stands. */
    tentative = encoding_lookup(charset, strlen(charset));
    return tentative < 0 ? fallback_encoding : tentative;
}
