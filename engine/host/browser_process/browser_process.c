/* THE BROWSER PROCESS — one WASM instance per EXTENSION, beside the per-document RENDERER instances.
 *
 * WHAT IT IS. `engine/host/main.c` is the renderer entry: one instance per origin-keyed agent cluster, running
 * an untrusted bundle over a real DOM (SECURITY.md). This is the OTHER entry — the trusted counterpart that
 * runs the algorithms a real browser keeps OUT of the renderer, and it holds no document, no realm, no page
 * script and no frontier. It is what a Chromium reader means by the browser process: the network service's
 * algorithms live here, the renderer is TOLD their results, and CORB is the reason the direction matters.
 *
 * WHO CREATES IT AND WHO OWNS ITS LIFETIME. The OFFSCREEN document, once, when it starts — the same zone that
 * provisions a renderer instance per document and routes between them (SECURITY.md: "the engine NAMES its own
 * child documents; the offscreen ROUTES them"). There is exactly ONE, because it is keyed on nothing: it holds
 * no per-document state to be keyed by, and a second one would be a second answer to a question with one
 * answer. It dies with the offscreen document, which is also when every renderer instance dies, so it never
 * outlives or is outlived by the instances that ask it. Nothing else may hold its Module handle: a renderer is
 * a separate WASM instance and has no way to call these entries at all — its requests reach the zone as
 * `qjs_host_requests` lines and the zone is what relays them here.
 *
 * AND TODAY THE ZONE THAT PROVISIONS ONE IS `engine/browser_process.mjs`, WHICH IS A STATEMENT ABOUT THIS TREE
 * AND NOT ABOUT THE DESIGN. That driver plays the offscreen — provisions the instance, asks as the zone, and
 * relays a renderer's request as the zone would — for the same reason `engine/route.mjs` is the only thing that
 * provisions a second renderer: a transport no host has driven is a design that has never run (SECURITY.md).
 * `extension/offscreen-brain.js` does not create one yet, and it will on the day it has something to do with
 * the answer — which is the reply record's computed type, read by `solver/reply_decode.c` and written by
 * nobody. A `createBrowserProcess()` in the offscreen ahead of that consumer would be an instance nothing asks.
 *
 * WHY THE ASKER IS AN ARGUMENT AND NOT A FIELD OF THE RECORD. `requester` is stated by the TRUSTED ZONE on
 * every call, exactly as `qjs_route`'s `sender_origin` is and for the identical reason: an identity the
 * untrusted side can write into the text it sends is not an identity. NULL is the zone speaking for itself —
 * a positive statement, not a hole a caller left — and a non-NULL value is the document id of the renderer
 * instance whose request line the zone is relaying. The zone knows which instance handed it a line; the line
 * cannot say.
 *
 * WHO MAY ASK WHAT IS DECLARED BESIDE THE OPERATION, never asked at the call — the same rule §7.2.5.1's
 * cross-origin allowlist follows, and for the same reason: a check written at the entry is one check for a
 * growing table, and the first operation added without a thought about the asker inherits whatever that one
 * check happened to say. Today the table has one row and it is zone-only, so a renderer may ask for NOTHING,
 * and the answer to "what is a renderer allowed to ask for" is a column in the table rather than a sentence
 * here that will go stale.
 *
 * WHAT CROSSES IS TEXT AND IT CARRIES ITS TYPE (SECURITY.md). A request is one TAB-separated record —
 * `<operation><TAB><operands…>` — which is the grammar `qjs_host_requests` and `qjs_perform` already speak, so
 * a relayed renderer request needs no re-encoding on the way through the zone. Bytes travel BESIDE the record
 * and never inside it, exactly as `qjs_provide` and `qjs_host_answer` carry a response body: §7's input is a
 * byte sequence and every way of putting one in text is an encode or a decode performed by a zone that has no
 * business doing either. The ANSWER is the operation's own; `mime.sniff` answers with WHATWG MIME Sniffing
 * §4.5's serialization of a MIME type, which is the standard's own string form of the record and parses back
 * through §4.4 on the far side — one grammar, owned by one component, on both sides of the boundary.
 *
 * NO REALM, AND THAT IS NOT THE SAME QUESTION AS THE ONE SECURITY.md ANSWERS ELSEWHERE. "A PEER ANSWERS BY
 * RUNNING A PROGRAM, never by reading a property from C" is about an AGENT answering a cross-document read:
 * the read is an IDL getter, it belongs to a document's timelines, and it may have to suspend. This process
 * holds no document and no timeline — §7 is a pure function of a header value and some bytes — so an answer
 * here is a return value and the entry is synchronous. The day an operation here has to WAIT (a real network
 * fetch), it gains the ask-then-answer split the renderer seam already has; inventing that split now for an
 * algorithm that cannot use it would be scaffolding with nothing behind it.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "browser_process/network/mime_sniff.h"
#include "core/mime/mime_type.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define BP_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define BP_EXPORT
#endif

/* ── the operations ──────────────────────────────────────────────────────────────────────────────────────── */

/* WHATWG MIME Sniffing §7 — the COMPUTED MIME type of a response.
 *
 * `args` is the record's remainder: `<no-sniff><TAB><Content-Type value>`, and the Content-Type field is LAST
 * because it is the remainder — a header value may contain HTAB, and the §7.4-notice grammar puts a policy
 * container last for the same reason.
 *
 * A RESPONSE WITH NO `Content-Type` HEADER AT ALL AND ONE WITH AN EMPTY VALUE ARE DIFFERENT FACTS, and the
 * record says which by its FIELD COUNT: two fields is Fetch's "get a header" returning null, three fields is a
 * header whose value is the third. §5.1 happens to reach the same "supplied type is undefined" for both, and
 * that is exactly why the distinction has to live here rather than in whichever caller first noticed it did
 * not matter — a record that cannot say a header is absent has already lost the fact.
 *
 * `no_sniff` is §5's no-sniff flag, whose one source is `X-Content-Type-Options: nosniff`. The ZONE reads that
 * header and states the answer, because it is the zone that holds the response's header list; this process
 * never reaches back for a second fact about a response it was handed one fact about. */
static char *op_mime_sniff(const char *args, const unsigned char *header, size_t header_n)
{
    MimeType computed;
    const char *content_type_value;
    char *serialized;

    /* THE GRAMMAR IS CHECKED, NOT DCHECKED, and the two run in this order for a reason beyond diagnosis: the
       first establishes that `args[0]` is not the terminator, which is what makes reading `args[1]` a read
       inside the string. A dev-only assertion would leave the release build parsing a record it cannot parse —
       an out-of-bounds byte, then a Content-Type field silently read as absent — and there is no answer to give
       for a record this process does not understand while the caller is about to read one. */
    CHECK(args[0] == '0' || args[0] == '1',
          "a `mime.sniff` record's no-sniff field is not a flag — it is §5's no-sniff flag and crosses as `0` "
          "or `1` so that it carries its type, and a zone that wrote anything else stated no flag at all");
    CHECK(args[1] == '\0' || args[1] == '\t',
          "a `mime.sniff` record's no-sniff field ran on past its flag — the field is one byte and the next "
          "byte is either the end of the record or the separator before the Content-Type value, so anything "
          "else means the operands were assembled in an order this operation does not read");

    /* §5.1's supplied-type input: NULL for "the response carries no Content-Type header". */
    content_type_value = (args[1] == '\t') ? args + 2 : NULL;

    mime_sniff_compute(&computed, content_type_value, args[0] == '1', header, header_n);
    /* §4.5's SERIALIZATION IS THE ANSWER, and §7 has no failure outcome — every path through it ends in a MIME
       type, down to `application/octet-stream` — so there is no empty answer for a caller to read as absence. */
    serialized = mime_type_serialize(&computed);
    CHECK(serialized != NULL, "browser process: OOM serializing §7's computed MIME type");
    mime_type_free(&computed);
    return serialized;
}

/* WHO MAY ASK, BESIDE WHAT THEY ASK FOR. `renderer_may_ask` is the whole authorization surface of this process:
   an operation declares it at its own row, and a row added without one does not compile. It reads `false`
   everywhere today because a renderer has nothing it is entitled to ask this process for yet — a renderer's
   business with the browser process is network and storage, and neither of those operations exists here. */
typedef struct {
    const char *op;
    bool        renderer_may_ask;
    char      *(*run)(const char *args, const unsigned char *header, size_t header_n);
} BpOperation;

static const BpOperation OPERATIONS[] = {
    { "mime.sniff", false, op_mime_sniff },
};

/* ── the ABI ─────────────────────────────────────────────────────────────────────────────────────────────── */

/* THE ANSWER LIVES UNTIL THE NEXT REQUEST, which is the contract `qjs_pending` and `qjs_result` already have:
   the zone reads the string out of linear memory before it asks anything else. Freed at the TOP of the next
   call rather than at the bottom of this one, so the pointer this entry returns is never a pointer into freed
   memory. */
static char *g_answer;

BP_EXPORT const char *bp_perform(const char *requester, const char *record,
                                 const char *header, unsigned header_n)
{
    const char *tab;
    size_t op_n, i;

    DCHECK(requester == NULL || *requester,
           "a request arrived from a renderer with an empty name — NULL is how the zone says it is asking for "
           "itself, so an empty string is a name the zone failed to state rather than an absence it stated");
    DCHECK(record != NULL && *record, "the browser process was asked to perform nothing at all");
    DCHECK(header != NULL || header_n == 0,
           "a request's bytes arrived as a null pointer with a length — the pointer and the length describe ONE "
           "byte sequence, so a request carrying no bytes passes both as nothing");

    free(g_answer);
    g_answer = NULL;

    tab = strchr(record, '\t');
    /* CHECKED FOR THE SAME REASON THE FIELD SHAPE ABOVE IS: with no separator there is no operation name to
       find and no operands to run it on, and the pointer arithmetic below would be performed on a null. Every
       operation this process has takes operands, so a record without a TAB is one whose operands were dropped
       somewhere on the way — a caller and a build shipped out of step, not a state to continue past. */
    CHECK(tab != NULL,
          "a request record carries an operation and no operands — the two are separated by a TAB, so a record "
          "without one is either an operation name that lost its arguments or a caller writing a grammar this "
          "process does not speak");
    op_n = (size_t)(tab - record);

    for (i = 0; i < sizeof OPERATIONS / sizeof *OPERATIONS; i++) {
        if (strlen(OPERATIONS[i].op) != op_n || memcmp(record, OPERATIONS[i].op, op_n) != 0)
            continue;
        /* THE SECURITY BOUNDARY, AND IT IS FATAL IN RELEASE TOO. A renderer reaching an operation it is not
           entitled to is the untrusted side deciding what it may be told, which is the one thing this process
           exists to stop — so it is a CHECK and not a DCHECK, and proceeding past it in a shipped build would
           be worse than crashing. */
        CHECK(requester == NULL || OPERATIONS[i].renderer_may_ask,
              "a RENDERER asked the browser process for an operation declared zone-only — the renderer runs the "
              "untrusted bundle, and an answer it is not entitled to is surface the page could never have "
              "obtained, reported as a fact about the page");
        g_answer = OPERATIONS[i].run(tab + 1, (const unsigned char *)header, (size_t)header_n);
        DCHECK(g_answer != NULL,
               "an operation returned no answer — every operation here answers, because a caller that cannot "
               "tell an absent answer from an empty one will read the next request's answer as this one's");
        return g_answer;
    }
    /* AN OPERATION THIS BUILD DOES NOT HAVE IS FATAL IN RELEASE TOO, because there is no answer to give and the
       caller is about to read one: the extension ships this wasm and the zone that drives it together, so a
       name they disagree on is not a state to continue past. */
    CHECK_FAIL("the browser process was asked for an operation it does not have — the `OPERATIONS` table above "
               "is the whole of what this process answers, and a zone naming something else is a caller and a "
               "build that shipped out of step with each other");
}
