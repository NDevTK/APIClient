/* CROSS-ORIGIN READ BLOCKING — the decision SECURITY.md §Network states as "CORB by load type (`opts.as`):
 * `script` (a chunk/import that becomes executable code) must be JS-typed when cross-origin — never read a
 * cross-origin HTML/JSON body as code".
 *
 * WHOSE ALGORITHM THIS IS. CORB has no WHATWG specification: it is Chromium's, in
 * `services/network/public/cpp/cross_origin_read_blocking.cc`, and it runs in the NETWORK SERVICE — which is
 * why it is in this program and beside `network/mime_sniff.c` rather than anywhere in the renderer. CLAUDE.md
 * §Browser-half's rule is to follow the established system rather than coin one, so where Chromium's analyzer
 * asks a question this file asks Chromium's version of it, and where the question has a WHATWG answer it asks
 * that instead: the confirmation sniff for markup IS MIME Sniffing §7.1's scriptable table (Chromium's own
 * `SniffForHTML` comment says its signature list is the MIME-sniffing one), so it is run through
 * `mime_sniff_compute` rather than written a second time here. Only the JSON sniff is Chromium's alone, because
 * §7 has no JSON row at all and cannot be asked.
 *
 * WHAT IT REPLACED, AND WHAT DID NOT CROSS WITH IT. `extension/lib/safe-fetch.js` held `_jsMime`,
 * `_corbProtectedMime`, `_sniffsProtected` and `_corbAllowsScript`. All four are deleted; the chokepoint now
 * asks this program and reads the verdict. What stayed in the chokepoint is the ORIGIN COMPARISON, and that is
 * deliberate rather than left over: SECURITY.md makes the same-origin principal `opts.pageOrigin` — the
 * browser's own `MessageSender.origin`, opaque-unique, "NEVER re-parsed from a URL" — so the fact that reaches
 * this file is `same_origin`, a browser-stated boolean, exactly as a delivered message carries a sender origin
 * the trusted zone stamped. This component never sees a URL and has no way to invent one.
 *
 * THE THREE SNIFFS ARE OVER A PREFIX, which is what a network service has: Chromium sniffs at most
 * `net::kMaxBytesToSniff` (1024) bytes and §5.2's resource header is 1445, so the caller sends the first
 * MIME_SNIFF_HEADER_MAX bytes and both algorithms get the whole of what they are defined over. The JS this
 * replaced ran `JSON.parse` over the ENTIRE body — a whole-bundle decode and parse on every chunk fetch to
 * answer a question the first line settles.
 */
#ifndef ENGINE_HOST_BROWSER_PROCESS_NETWORK_CORB_H
#define ENGINE_HOST_BROWSER_PROCESS_NETWORK_CORB_H

#include <stdbool.h>
#include <stddef.h>

/* Long enough for any §4.2 essence a real server sends and bounded because the record is a fixed struct the
   caller owns. An essence longer than this is TRUNCATED in the report and never in the decision: every rule
   below runs on the full string, and this field is what the verdict is explained with. */
#define CORB_TEXT_MAX 160

typedef struct {
    bool allow;                     /* may the trusted zone hand these bytes to a code loader */
    char computed[CORB_TEXT_MAX];   /* §7's COMPUTED essence — what the resource is, not what it claimed */
    char reason[CORB_TEXT_MAX];     /* which rule decided; one of the fixed strings corb.c spells */
} CorbVerdict;

/* `content_type_value` is the response's `Content-Type` as "get a header" joined it, or NULL for absent — the
   same positive statement §5.1 makes and mime_sniff_compute takes. `no_sniff` is `X-Content-Type-Options:
   nosniff`, read by the caller. `same_origin` is the browser-stated comparison of the loading document's
   origin with the response's; an opaque origin is same-origin with nothing, so it arrives here as false.
   `header` is the resource header — the response's first bytes, up to MIME_SNIFF_HEADER_MAX of them. */
void corb_check(CorbVerdict *out, const char *content_type_value, bool no_sniff, bool same_origin,
                const unsigned char *header, size_t header_n);

#endif
