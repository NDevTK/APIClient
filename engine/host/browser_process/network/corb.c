/* Cross-Origin Read Blocking — see corb.h for whose algorithm this is and why it lives in this program. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "network/corb.h"
#include "network/json_sniff.h"
#include "network/mime_sniff.h"

/* ── the protected type set, as ESSENCES ────────────────────────────────────────────────────────────────────
 *
 * It is compared against a §4.2 essence, which §4.4's parser has already ASCII-lowercased, so there is no case
 * folding here and no `;`-splitting either: the record was parsed, not cut at the first semicolon. That is the
 * difference from the JS this replaces, which did `String(mime||"").split(";")[0].trim().toLowerCase()` on a
 * raw header value and therefore disagreed with the parser about `text/html;charset="a;b"`.
 *
 * THE JAVASCRIPT SET LEFT THIS FILE. It stood here as Chromium's `IsJavaScriptMimeType`, and the moment a
 * second component needed the same answer (resource_kind.c, telling a script from API data) a private copy
 * became the pair that can disagree about one response. It is §4.6's JavaScript MIME type group and it is now
 * `mime_type_is_javascript`, beside the other groups, asked of the RECORD rather than of a string — which also
 * closed the two ways this copy had drifted from the group it was quoting: it carried `application/node`, which
 * is in no version of the list, and it lacked `text/livescript`, which is in every one. */

static bool suffix_is(const char *e, const char *suf)
{
    size_t n = strlen(e), m = strlen(suf);
    return n >= m && !strcmp(e + n - m, suf);
}

/* The CORB-PROTECTED types: what a cross-origin load must never deliver to a code loader. Chromium's analyzer
   calls these the HTML/XML/JSON canonical types plus the never-sniffed set; `multipart/` is here because a
   multipart body is a container whose parts carry their own types and no part of it is script. */
static bool essence_is_protected(const char *e)
{
    return !strcmp(e, "text/html") || !strcmp(e, "text/xml") || !strcmp(e, "application/xml") ||
           suffix_is(e, "+xml") || !strcmp(e, "application/json") || suffix_is(e, "+json") ||
           !strncmp(e, "multipart/", 10);
}

/* ── the confirmation sniffs ────────────────────────────────────────────────────────────────────────────────
 *
 * These decide the MISLABELLED case, which is the whole reason CORB reads bytes at all: a body DECLARED as
 * JavaScript that is in fact a document. §7 will not catch it and is right not to — its step 1 exists precisely
 * so sniffing can never upgrade a resource INTO a scriptable type, and the symmetric refusal is that it never
 * downgrades one out. So the declared type decides the first two rules and the bytes decide the last. */

/* MARKUP, through MIME Sniffing §7.1's own scriptable table — the rows `<!doctype html`, `<script`, `<html`,
   `<iframe`, `<!--`, `<?xml` and the rest, which is the same signature list Chromium's `SniffForHTML` carries a
   hand-written copy of.
   IT ASKS FOR THE TABLE BY NAME NOW. What stood here ran a whole `mime_sniff_compute(&m, NULL, false, …)` — a
   §7 pass whose NULL supplied type existed only to reach step 2, whose `no_sniff` was false only to reach the
   scriptable branch, and whose every other answer was then thrown away. That is six steps of an algorithm
   nobody was asking, spelling one table; mime_sniff.h exposes the table, and resource_kind.c reaching for the
   same one is what made the difference matter.
   ONLY MARKUP COUNTS AS A CONFIRMATION, unchanged. The table also answers `application/pdf`, and §7.1 around it
   answers image/*, the archive types and (as its last two steps) text/plain or application/octet-stream — none
   of which is evidence that a body declared as script is a DOCUMENT, and treating a fallback answer as a
   confirmation would block every plain-text chunk in existence. */
static const char *sniff_markup(const unsigned char *header, size_t header_n)
{
    const char *m = mime_sniff_scriptable_pattern(header, header_n);

    if (!m) return NULL;
    if (!strcmp(m, "text/html") || !strcmp(m, "text/xml")) return m;
    return NULL;
}

/* JSON is not in MIME Sniffing's tables at all, so §7 cannot be asked and the sniff is Chromium's
   `CrossOriginReadBlocking::SniffForJSON`. IT WAS STATIC HERE AND IS NOW `network/json_sniff.c`, for the reason
   this file's own JavaScript list moved: resource_kind.c needs the identical answer about the identical bytes —
   a body under a JavaScript MIME type that opens a JSON object is API data, not a script — and two static
   copies of one algorithm is the pair that can disagree about one response. The JS this replaced ran
   `JSON.parse` over the WHOLE body and blocked on either shape, which also meant a multi-megabyte bundle was
   decoded and parsed to answer a question its first byte settles. */

/* ── the decision ──────────────────────────────────────────────────────────────────────────────────────── */

static void verdict(CorbVerdict *out, bool allow, const char *computed, const char *reason)
{
    DCHECK(computed != NULL && *computed, "a CORB verdict was built with no computed type — §7 has no failure "
                                          "outcome, so every path through it names one");
    out->allow = allow;
    snprintf(out->computed, sizeof out->computed, "%s", computed);
    snprintf(out->reason, sizeof out->reason, "%s", reason);
}

void corb_check(CorbVerdict *out, const char *content_type_value, bool no_sniff, bool same_origin,
                const unsigned char *header, size_t header_n)
{
    MimeType computed;
    char *essence;
    const char *markup;

    DCHECK(out != NULL, "CORB was asked to decide into nothing");
    DCHECK(header != NULL || header_n == 0,
           "a resource header of non-zero length was passed as a null pointer — a caller that has no bytes "
           "says so with a length of zero, which is a body it read and found empty");

    /* §7 FIRST, and the rules below then run on WHAT THE RESOURCE IS rather than on what its header claimed.
       That is a strictly stronger statement of the same rule than the JS's `mime.split(";")[0]`: §7 answers the
       supplied essence unchanged in every case except the ones the standard says otherwise about — the four
       Apache-bug values whose body decides, an image or media type re-matched against §6's patterns, and an
       absent or wildcard type identified from the bytes. Each of those is a case where the declared string is
       the WRONG answer to "is this a protected type", and each was decided by the string before. */
    mime_sniff_compute(&computed, content_type_value, no_sniff, header, header_n);
    essence = mime_type_essence(&computed);
    CHECK(essence, "corb: OOM reading the computed essence");

    if (same_origin) {
        /* The page's OWN data, which it may read by definition — CORB protects across origins and nowhere
           else. The one thing skipped is the page's own non-JS data being handed to a CODE loader, which is a
           load that could not have executed anyway. */
        verdict(out, !(essence_is_protected(essence) && !mime_type_is_javascript(&computed)),
                essence, "same-origin");
        goto done;
    }
    if (essence_is_protected(essence)) {
        verdict(out, false, essence, "protected-type");
        goto done;
    }
    /* `nosniff` is the server saying the declared type is the final word, so a browser refuses to execute a
       cross-origin non-JS type under it and this refuses to ingest one. */
    if (no_sniff && !mime_type_is_javascript(&computed)) {
        verdict(out, false, essence, "nosniff-not-js");
        goto done;
    }
    if ((markup = sniff_markup(header, header_n)) != NULL) {
        verdict(out, false, essence, !strcmp(markup, "text/html") ? "sniffed-html" : "sniffed-xml");
        goto done;
    }
    if (json_sniff(header, header_n)) {
        verdict(out, false, essence, "sniffed-json");
        goto done;
    }
    verdict(out, true, essence, "allowed");

done:
    free(essence);
    mime_type_free(&computed);
}
