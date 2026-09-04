/* SUBRESOURCE INTEGRITY §3.3.2 "Parse metadata" — a request's integrity metadata, read as the hash
 * expressions it names. See subresource_integrity.c.
 *
 * WHY THIS IS ITS OWN COMPONENT AND NOT A FEW LINES INSIDE THE ONE ALGORITHM THAT ASKS. The caller that
 * brought it here is CSP §6.7.2.4 "Does integrity metadata match source list?", whose step 4 is "Let integrity
 * sources be the result of parsing metadata given integrity metadata" and which cites [SRI] for it — so the
 * grammar being read is ANOTHER STANDARD'S, over a string Fetch §2.2.5 "Requests" puts on the request rather
 * than one CSP composed. core/frame/csp_source_list.h says why a matcher that re-derives a grammar is the
 * second reading of it; a matcher that re-derives a DIFFERENT STANDARD'S grammar inside its own file is that
 * defect with the standard's name removed as well, and the next reader has no way to know which document the
 * bytes are being read against.
 *
 * AND IT HAS A SECOND CALLER ALREADY WRITTEN DOWN, which is what settles the question rather than taste:
 * Fetch §4.1 "Main fetch" reads the same field — "If request's integrity metadata is not the empty string" —
 * and hands the bytes to §3.3.4 "Do bytes match metadataList?", which is this same parse followed by a digest
 * this engine already has (core/crypto/secure_hash.h). That check is not built here and is not this file's
 * subject; the parse it will need is.
 *
 * IT IS A CURSOR AND NOT AN OWNED SET, and both halves of that are deliberate. The bytes every expression
 * names are SLICES of the caller's own string, exactly as core/frame/csp_directive_list.h's CspToken is a
 * slice of a policy's text, so there is nothing to allocate, nothing to free and no OOM arm on an algorithm
 * whose whole job is to read. And §3.3.2 returns a SET while this yields a STREAM that can repeat a member:
 * that difference is unobservable to both questions asked of it — whether it is EMPTY, and whether every
 * member of it appears in some other list — because a repeated member that matches still matches and a
 * repeated member that does not still fails. A consumer that needs §3.3.3 "Get the strongest metadata from
 * set"'s ORDERING would need the set itself, and would say so here rather than de-duplicating at its own site.
 *
 * THE STRING IS THE REQUEST'S AND MAY BE ANYTHING AT ALL — it is an author-written attribute value, so a
 * malformed one is INPUT and not a violated invariant. §3.3.2 answers every string: an item whose algorithm
 * is not a valid SRI hash algorithm token is skipped by its own step 2.6, and a string of nothing but spaces
 * yields no expressions at all, which is the empty set its callers read as a positive statement. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_SUBRESOURCE_INTEGRITY_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_SUBRESOURCE_INTEGRITY_H
#include <stdbool.h>
#include <stddef.h>

/* SRI §3.3.2's step 2.8 ordered map, whose two entries are the first two pieces of information SRI §3.1
   "Integrity metadata" lists: the cryptographic hash function, keyed `alg`, and the digest, keyed `val`. The
   third piece SRI §3.1 lists — options, keyed `opt` — is NOT here and its absence is the standard's: the note
   under that step says that since no options are defined a corresponding entry is not set in metadata, and
   SRI §3.3.2's step 2.1 splits them off so that a future version can read them.
   BOTH SPANS ARE SLICES of the string handed to sri_parse_metadata and live exactly as long as it does.
   `alg` is the token AS WRITTEN, in the author's own case — step 2.5 keeps `algorithm-and-value[0]` and it is
   §2's ASCII-lowercase rule that decides validity, not a fold applied to the stored bytes. `val` MAY BE EMPTY:
   step 2.3 initializes base64-value to the empty string and step 2.7 only overwrites it "If
   algorithm-and-value[1] exists", so a bare `sha256` parses to a real expression naming no digest. */
typedef struct {
    const char *alg;
    size_t      alg_len;
    const char *val;
    size_t      val_len;
} SriHashExpression;

/* The cursor. Its fields are this component's and no caller reads them; it is spelled here rather than behind
   an opaque pointer so that it lives on the reader's stack and the parse allocates nothing. */
typedef struct {
    const char *p;
    size_t      n;
    size_t      i;
} SriMetadataParse;

/* §3.3.2's step 1, over `metadata` — Fetch §2.2.5's integrity metadata of some request. The bytes are BORROWED
   and are not required to be NUL-terminated. `len` may be 0, which is the field's own initial value and the
   state every caller that never set it is in. */
void sri_parse_metadata(SriMetadataParse *it, const char *metadata, size_t len);

/* §3.3.2's step 2, ONE TURN OF ITS LOOP — true when `out` was filled with the next expression, false when the
   loop has ended, which is its step 3. Items step 2.6 skips are consumed inside this call, so a `false` means
   there are no more VALID expressions and never that an invalid one is waiting. */
bool sri_parse_metadata_next(SriMetadataParse *it, SriHashExpression *out);

#endif
