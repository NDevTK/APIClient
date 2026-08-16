/* Cross-Origin Read Blocking — see corb.h for whose algorithm this is and why it lives in this program. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "network/corb.h"
#include "network/mime_sniff.h"

/* ── the two type sets, as ESSENCES ─────────────────────────────────────────────────────────────────────────
 *
 * Both are compared against a §4.2 essence, which §4.4's parser has already ASCII-lowercased, so there is no
 * case folding here and no `;`-splitting either: the record was parsed, not cut at the first semicolon. That is
 * the difference from the JS these replace, which did `String(mime||"").split(";")[0].trim().toLowerCase()` on
 * a raw header value and therefore disagreed with the parser about `text/html;charset="a;b"`. */

static bool suffix_is(const char *e, const char *suf)
{
    size_t n = strlen(e), m = strlen(suf);
    return n >= m && !strcmp(e + n - m, suf);
}

/* Chromium's `IsJavaScriptMimeType`, which is HTML's "JavaScript MIME type" list. */
static bool essence_is_javascript(const char *e)
{
    static const char *const JS[] = {
        "application/ecmascript", "application/javascript", "application/x-ecmascript",
        "application/x-javascript", "text/ecmascript", "text/javascript", "text/jscript",
        "text/x-ecmascript", "text/x-javascript", "application/node",
    };
    size_t i;
    for (i = 0; i < sizeof JS / sizeof *JS; i++)
        if (!strcmp(e, JS[i])) return true;
    /* HTML's legacy `text/javascript1.0` … `text/javascript1.5`, written as the range it is rather than as the
       regex the JS carried — a range test cannot accidentally match `text/javascript1.5x`. */
    if (!strncmp(e, "text/javascript1.", 17) && e[17] >= '0' && e[17] <= '5' && e[18] == 0) return true;
    return false;
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

/* MARKUP, through MIME Sniffing §7.1's own scriptable table. Passing a NULL supplied type is §5.1's "the
   supplied MIME type is undefined", which sends §7 to step 2 and §7.1 to the table whose rows are `<!doctype
   html`, `<script`, `<html`, `<iframe`, `<!--`, `<?xml` and the rest — the same signature list Chromium's
   `SniffForHTML` carries a hand-written copy of. `no_sniff` is false here on purpose: the caller has already
   applied the nosniff rule as its own step, and this call is asking what the BYTES are, which is a question
   the response's own headers do not get a vote on. */
static const char *sniff_markup(const unsigned char *header, size_t header_n)
{
    MimeType m;
    char *essence;
    const char *hit = NULL;

    mime_sniff_compute(&m, NULL, false, header, header_n);
    essence = mime_type_essence(&m);
    CHECK(essence, "corb: OOM reading the sniffed essence");
    /* Only markup counts as a confirmation. §7.1 also answers image/*, application/pdf, the archive types and
       (as its last two steps) text/plain or application/octet-stream — none of which is evidence that a body
       declared as script is a document, and treating a fallback answer as a confirmation would block every
       plain-text chunk in existence. */
    if (!strcmp(essence, "text/html")) hit = "text/html";
    else if (!strcmp(essence, "text/xml")) hit = "text/xml";
    free(essence);
    mime_type_free(&m);
    return hit;
}

/* §7 has no JSON row — JSON is not in the standard's tables at all — so this one is Chromium's
   `CrossOriginReadBlocking::SniffForJSON` and nothing else: an opening brace, then a double-quoted string, then
   a colon, with ASCII whitespace and JS comments skipped outside the string literal.
   IT DELIBERATELY DOES NOT SNIFF AN ARRAY, and that is Chromium's position rather than an omission: a
   top-level `[1,2,3]` IS a valid JavaScript expression, so it is not evidence that the server mislabelled
   anything, while a top-level `{"a":` is a syntax error as a JS statement and therefore is. The JS this
   replaces ran `JSON.parse` over the whole body and blocked on either, which also meant a multi-megabyte
   bundle was decoded and parsed to answer a question its first byte settles. */
static bool sniff_json(const unsigned char *d, size_t n)
{
    enum { START, LEFT_BRACE, IN_STRING, ESCAPE, RIGHT_QUOTE } state = START;
    size_t i = 0;

    while (i < n) {
        unsigned char c = d[i];
        if (state != IN_STRING && state != ESCAPE) {
            if (c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20) { i++; continue; }
            /* Chromium's `AdvancePastComments`, in both of JavaScript's comment forms. An unterminated comment
               consumes the rest of the header, which ends the loop as "ran out of data" below. */
            if (c == '/' && i + 1 < n && d[i + 1] == '/') {
                i += 2;
                while (i < n && d[i] != 0x0A) i++;
                continue;
            }
            if (c == '/' && i + 1 < n && d[i + 1] == '*') {
                i += 2;
                while (i + 1 < n && !(d[i] == '*' && d[i + 1] == '/')) i++;
                i = (i + 1 < n) ? i + 2 : n;
                continue;
            }
        }
        switch (state) {
        case START:       if (c != '{') return false; state = LEFT_BRACE; break;
        case LEFT_BRACE:  if (c != '"') return false; state = IN_STRING; break;
        case IN_STRING:   if (c == '"') state = RIGHT_QUOTE; else if (c == '\\') state = ESCAPE; break;
        case ESCAPE:      state = IN_STRING; break;
        case RIGHT_QUOTE: return c == ':';
        }
        i++;
    }
    /* Chromium's third answer is `kMaybe` — the sniff ran out of data before deciding — and for a response
       whose bytes are all here that is a NO. This caller always holds the whole resource header, so there is
       no more data coming and nothing to wait for. */
    return false;
}

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
        verdict(out, !(essence_is_protected(essence) && !essence_is_javascript(essence)),
                essence, "same-origin");
        goto done;
    }
    if (essence_is_protected(essence)) {
        verdict(out, false, essence, "protected-type");
        goto done;
    }
    /* `nosniff` is the server saying the declared type is the final word, so a browser refuses to execute a
       cross-origin non-JS type under it and this refuses to ingest one. */
    if (no_sniff && !essence_is_javascript(essence)) {
        verdict(out, false, essence, "nosniff-not-js");
        goto done;
    }
    if ((markup = sniff_markup(header, header_n)) != NULL) {
        verdict(out, false, essence, !strcmp(markup, "text/html") ? "sniffed-html" : "sniffed-xml");
        goto done;
    }
    if (sniff_json(header, header_n)) {
        verdict(out, false, essence, "sniffed-json");
        goto done;
    }
    verdict(out, true, essence, "allowed");

done:
    free(essence);
    mime_type_free(&computed);
}
