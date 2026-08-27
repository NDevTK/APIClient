/* SNIFFING — WHATWG MIME Sniffing §5 "Handling a resource", §6 "Matching a MIME type pattern" and §7
   "Determining the computed MIME type of a resource". See mime_sniff.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_MIME_MIME_SNIFF_H
#define ENGINE_HOST_BROWSER_CORE_MIME_MIME_SNIFF_H
#include <stdbool.h>
#include <stddef.h>

#include "core/mime/mime_type.h"

/* THIS IS core/mime's OTHER HALF, NOT A SECOND MIME COMPONENT. mime_type.h is §4 — the RECORD, a pure value
   type over a string. This file is §5–§7 — the algorithms over BYTES that decide which record a resource
   actually has. They are split because the contracts are different (a record has no bytes and a sniff has no
   realm), for the same reason mime_type_encoding.c is split out of mime_type.c, and they are one component
   because §7 is stated entirely in terms of §4's groups.
   IT IS NOT THE SNIFFING IN `extension/lib/safe-fetch.js` EITHER, AND IT DOES NOT WEAKEN IT. That one is the
   TRUSTED ZONE's CORB gate: it decides whether bytes are handed to this sandbox AT ALL, it runs before any
   flow exists, and SECURITY.md assigns it there by name — which is why solver/reply_decode.h refuses to
   re-derive a type for a REPLY and reads the one the host stamped. The warning that file states is about a
   renderer mining a body a real renderer would never have been shown, and it does not reach here: this runs
   only over the bytes of a NAVIGATION this instance is about to PARSE INTO ITS OWN DOCUMENT — bytes the CORB
   gate has already released and that html_encoding_sniff is already reading a BOM and 1024-byte prescan out
   of. Sniffing them shows this heap nothing it does not already hold.
   AND NO HOST CAN ANSWER IT INSTEAD. HTML §7.4.5's dispatch is a C algorithm, the hosts that answer
   `document.fetch` include C runners with no trusted zone in the picture at all, and the answer decides which
   parse a FLOW performs mid-execution — CLAUDE.md's own test for what belongs in the engine. */

/* §5.2 "Reading the resource header": "Read bytes of the resource into buffer until … the number of bytes in
   buffer is greater than or equal to 1445." The number is here rather than at each caller because the sniff
   ASSERTS it — a caller that hands over a whole body is asking a different question from the one §7 answers
   (§7.1's "contains no binary data bytes" scans the resource header, so a binary byte at offset 5000 changes
   `text/plain` into `application/octet-stream` and nothing would say so). */
#define MIME_SNIFF_RESOURCE_HEADER_MAX 1445

/* §5.1 "Interpreting the resource metadata" — the SUPPLIED MIME TYPE DETECTION ALGORITHM, for a resource
   retrieved via HTTP. `last_content_type_value` is §5.1's "the value of the LAST `Content-Type` header
   associated with the resource", or NULL for a resource that carried none.
   IT IS THE LAST HEADER AND NOT Fetch §2.2.2 "Headers"'s "extract a MIME type" over the joined list. The two algorithms
   answer the same question differently and this one is what §7's input is defined as: Fetch's merges a charset
   forward across values, §5.1's takes one value whole, and §5's check-for-apache-bug flag is a BYTE-EXACT
   comparison against four literal header values that only the unjoined one can ever equal.
   Returns §5.1's "the supplied MIME type is undefined" as FALSE (`out` is left initialised-and-empty, so the
   caller frees it either way), and `*apache_bug` is §5's check-for-apache-bug flag — written on every call,
   including the false one, because a flag a caller has to remember to clear is a flag that reads as set. */
bool mime_sniff_supplied(MimeType *out, bool *apache_bug, const char *last_content_type_value);

/* §5's per-resource metadata, as one record, because §7 reads all of it and asking for the pieces separately
   is asking a caller to keep four facts about one response in step by hand. */
typedef struct {
    /* §5.1's answer. NULL IS "the supplied MIME type is undefined" — a POSITIVE statement §7 step 2 branches
       on, never a hole: a response that carried no `Content-Type` is exactly this. */
    const MimeType *supplied;
    bool apache_bug;   /* §5's check-for-apache-bug flag, as mime_sniff_supplied wrote it */
    bool no_sniff;     /* §5's no-sniff flag — Fetch §3.6's "determine nosniff" over the response's headers */
    /* §7 steps 5 and 7's "SUPPORTED BY THE USER AGENT": does this user agent render resources of the SUPPLIED
       type. It is an input because it is a UA CAPABILITY and this component does not own one — core/mime knows
       what a byte stream IS (§4.6's groups, applied here), and which of those a build can decode is a fact its
       decoding component owns and states once. The group half of each step stays here; only this conjunct
       crosses. False when `supplied` is NULL, which is the only value "does the UA render the supplied type"
       can have when there is no supplied type. */
    bool ua_renders_supplied;
    const unsigned char *header;   /* §5.2's RESOURCE HEADER — see MIME_SNIFF_RESOURCE_HEADER_MAX */
    size_t header_len;
} MimeSniffResource;

/* §7 "Determining the computed MIME type of a resource" — the MIME TYPE SNIFFING ALGORITHM, which is also
   §8.1's "Sniffing in a browsing context" ("Use the MIME type sniffing algorithm") and therefore what HTML
   §7.4.5 "Populating a session history entry" means by "the computed type of navigationParams's response".
   THE COMPUTED MIME TYPE IS NEVER UNDEFINED, and that is a property of the algorithm rather than of this
   implementation: every arm that keeps the supplied type is reachable only after step 2 has established that
   the supplied type is defined, and every other arm returns one of §7.1's or §7.2's literal types. `out` is
   therefore always a parsed record and the caller always frees it. */
void mime_sniff_computed(MimeType *out, const MimeSniffResource *r);

#endif
