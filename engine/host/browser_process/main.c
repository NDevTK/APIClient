/* THE BROWSER PROCESS'S ENTRY — the ABI `extension/browser-process.js` drives through `ccall`, and the whole
 * of it. `engine/host/main.c` is the RENDERER's entry and this is its counterpart: a different program, linked
 * from a different source list into a different artifact, instantiated in a different realm on a different
 * thread. The two share source files (core/mime/mime_type.c is compiled into both) and share no objects, no
 * linear memory and no handle — which is the difference between this and the `browser_process/` directory that
 * was deleted at 58ba66a7, where two wasm-ld invocations over ONE object set produced two file names.
 *
 * WHY THERE IS NO SCHEDULER HERE, no realm and no quickjs. A network service answers questions about bytes.
 * Every entry below is a pure function of its arguments — no state survives a call, so there is nothing to
 * park, nothing to fork and no flow to be fair to. The moment an entry here needs to SUSPEND (a peer read, a
 * fetch of its own) that stops being true and the answer is the one CLAUDE.md §scheduler already gives, not a
 * second scheduler invented in this file.
 *
 * WHAT CROSSES, AND WHY IT IS TEXT. `extension/renderer-host.js` states the discipline this follows: a record
 * of primitives carrying its TYPE, with BYTES BESIDE IT. Here the bytes go IN (a resource header, placed in
 * this module's linear memory by the worker) and a RECORD comes back, and the record crosses as ONE JSON
 * document that the worker does ONE `JSON.parse` of — the same shape `qjs_result`'s `@RESULT` has, and for the
 * same reason: a decision with three fields answered as three separate calls is three chances for a caller to
 * read a stale one beside a fresh one.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "network/corb.h"
#include "network/mime_sniff.h"
#include "network/nosniff.h"
#include "network/resource_kind.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define BP_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define BP_EXPORT
#endif

/* ONE ANSWER BUFFER FOR ALL ENTRIES, because there is one caller and it is synchronous: `ccall` converts this
   C string to a JS string before returning, and JavaScript is run-to-completion, so no second call — of this
   entry or of any other — can begin while a first answer is still being read. That is a property of the
   TRANSPORT (a Worker serving one postMessage at a time through one synchronous handler) rather than of the
   number of entries, which is why a second entry shares it rather than needing a second buffer. A malloc'd
   answer would put a free on the far side of a postMessage. */
static char g_answer[512];

/* THE ONE PLACE A C STRING BECOMES JSON, and it ASSERTS the property that lets it be a `%s`. §4.4 restricts a
   MIME type's type and subtype to HTTP token code points, which contain neither `"` nor `\` nor any control
   character, and every reason string is a literal spelled in corb.c — so an escape routine here would be dead
   code guarding an invariant, and the invariant is worth more asserted than defended. */
static void json_safe(const char *s)
{
    size_t i;
    for (i = 0; s[i]; i++)
        DCHECK(s[i] != '"' && s[i] != '\\' && (unsigned char)s[i] >= 0x20,
               "a CORB verdict field carried a character JSON cannot hold unescaped — a §4.2 essence is two "
               "HTTP-token strings and a solidus, and a reason is a literal, so this is a component upstream "
               "answering with something that did not come out of the MIME parser");
}

/* THE TWO ENTRIES TAKE THE SAME THREE FACTS ABOUT A RESPONSE, and they take them in the same form on purpose:
   both are questions about one resource, and a boundary on which the same fact is spelled two ways is a
   boundary on which the two answers can disagree about one response.
   `content_type` is the joined `Content-Type` value or NULL for absent (§5.1's "the supplied MIME type is
   undefined" — a positive statement, never a hole the caller filled with an empty string).
   `x_content_type_options` is that header's value, joined the same way, or NULL for absent — THE VALUE AND NOT
   THE FLAG. The flag is Fetch's determine-nosniff over it, and it is computed here (network/nosniff.c) because
   deriving it on the far side put a spec algorithm in `extension/lib/safe-fetch.js`, spelled as a SUBSTRING
   test where the standard splits the header and matches its FIRST value: `foo, nosniff` set the flag there and
   does not set it under Fetch. The trusted zone reads headers; this program decides what they mean.
   `header` is the response's first bytes, of which each entry reads at most MIME_SNIFF_HEADER_MAX. */

/* THE CORB DECISION, over one response. `same_origin` is the trusted zone's own comparison of the
   browser-stated page origin with the response's, which SECURITY.md keeps on that side.
   The answer is `{"allow":<bool>,"computed":<§7's essence>,"reason":<the rule that decided>}`. */
BP_EXPORT const char *bp_corb_check(const char *content_type, const char *x_content_type_options,
                                    int same_origin, const unsigned char *header, int header_n)
{
    CorbVerdict v;
    int n;

    DCHECK(header_n >= 0, "a resource header was passed a negative length — the caller measures a byte "
                          "sequence it holds, so a negative one is a subtraction that went past the start");
    DCHECK(header != NULL || header_n == 0,
           "a resource header of non-zero length arrived as a null pointer — the worker places the bytes in "
           "this module's memory and passes what it placed, so a null with a length is a placement that failed "
           "silently and would be classified as an empty body");
    corb_check(&v, content_type, nosniff_determine(x_content_type_options), same_origin != 0, header,
               (size_t)(header_n > MIME_SNIFF_HEADER_MAX ? MIME_SNIFF_HEADER_MAX : header_n));
    json_safe(v.computed);
    json_safe(v.reason);
    n = snprintf(g_answer, sizeof g_answer, "{\"allow\":%s,\"computed\":\"%s\",\"reason\":\"%s\"}",
                 v.allow ? "true" : "false", v.computed, v.reason);
    DCHECK(n > 0 && (size_t)n < sizeof g_answer,
           "the CORB verdict did not fit its answer buffer — both of its strings are bounded by CORB_TEXT_MAX "
           "and the buffer is sized for both plus the record around them, so a truncation here is a field that "
           "grew without this buffer growing and would be delivered as malformed JSON");
    return g_answer;
}

/* WHAT THE RESOURCE IS FOR — does this response body carry API structure to learn from, or is it a static asset
   whose bytes a decoder turns into pixels, samples, glyphs or a program? See network/resource_kind.h for whose
   judgement that is and for the three JS functions it replaced.
   `opaque` is Fetch §2.2.6: the response is an opaque filtered response, so its body is null and its header
   list is empty. It is a fact the trusted zone HOLDS — it has the Response object and this program has neither
   a URL nor a principal — which is the same shape `same_origin` has above.
   The answer is `{"asset":<bool>,"reason":<the rule that decided>}`. There is no computed-type field, and its
   absence is the rule CLAUDE.md states about a reader with no writer read from the other end: §7's essence has
   no consumer on this path, and a field written for nobody is a capability the surface only looks like it has.
   The rule name is what the popup tags a method with and what the probe compares. */
BP_EXPORT const char *bp_classify(const char *content_type, const char *x_content_type_options, int opaque,
                                  const unsigned char *header, int header_n)
{
    ResourceKind k;
    int n;

    DCHECK(header_n >= 0, "a resource header was passed a negative length — the caller measures a byte "
                          "sequence it holds, so a negative one is a subtraction that went past the start");
    DCHECK(header != NULL || header_n == 0,
           "a resource header of non-zero length arrived as a null pointer — the worker places the bytes in "
           "this module's memory and passes what it placed, so a null with a length is a placement that failed "
           "silently and would be classified as an empty body");
    resource_kind_classify(&k, content_type, nosniff_determine(x_content_type_options), opaque != 0, header,
                           (size_t)(header_n > MIME_SNIFF_HEADER_MAX ? MIME_SNIFF_HEADER_MAX : header_n));
    json_safe(k.reason);
    n = snprintf(g_answer, sizeof g_answer, "{\"asset\":%s,\"reason\":\"%s\"}",
                 k.asset ? "true" : "false", k.reason);
    DCHECK(n > 0 && (size_t)n < sizeof g_answer,
           "the resource-kind verdict did not fit its answer buffer — its one string is bounded by "
           "RESOURCE_KIND_REASON_MAX and the buffer is sized for it plus the record around it, so a truncation "
           "here is a field that grew without this buffer growing and would be delivered as malformed JSON");
    return g_answer;
}
