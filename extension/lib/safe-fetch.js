// safe-fetch.js — THE single external-fetch entry point for the analyzer.
//
// ALL external requests (lazy chunks, source maps, discovery probes, anything the
// analyzer pulls off the network) go through safeFetch so the security invariants
// live in ONE auditable place:
//   • cookies OMITTED by default (credentials:"omit") — no credentialed exfiltration.
//                        In CREDENTIALED mode (opts.credentialed) the user's cookies
//                        ARE attached, so the bytes are the REAL authenticated ones
//                        (the logged-in API surface) — but the reply is gated by
//                        safeFetch's OWN SOP/CORS check (a host-permission fetch
//                        bypasses the browser's; see below).
//                        THE MODE HAS A CALLER: the custom browser's DOCUMENT LOAD.
//                        CLAUDE.md §A-REAL-NAVIGABLE — "EVERY ONE OF THEM SENDS
//                        COOKIES: safeFetch supports credentialed loads, and a
//                        SAME-ORIGIN navigation carries them, exactly as a browser's
//                        does" — so bridge.js's navigationLoad asks for them wherever
//                        the address is same-origin with the BROWSER-STATED principal
//                        of the document being loaded. A session-less tab models
//                        nothing: not the person's browser and not a clean client.
//   • GET only         — method is forced to GET: forced execution explores many
//                        paths; a real POST/PUT/DELETE replay would mutate server
//                        state. A well-designed server never mutates on GET, so even
//                        the credentialed replay is side-effect-free; POST/PUT/DELETE
//                        endpoints are only RECORDED by forced exec, never issued.
//   • destructive-path — that "well-designed" is an ASSUMPTION about someone else's
//     deny list          server, and RFC 9110 §9.2.1 Safe Methods makes it theirs to
//                        honour rather than ours to verify. So a credentialed GET whose
//                        path or query carries a session-ending or resource-destroying
//                        token is REFUSED before it is sent: the one place this project
//                        matches on a name, matching only to refuse, because an
//                        accidental logout is a CSRF this tool commits against its own
//                        user. See _destructiveToken for why the deny direction escapes
//                        §RUN-DON'T-MATCH and what it may never be read as.
//   • HTTP(S) only     — scheme must be http:/https:; file:/data:/blob:/chrome-
//                        extension:/etc. are rejected so a crafted URL can't read
//                        local/extension resources.
//   • origin-relative  — the analyzer acts with the analyzed DOCUMENT's own origin,
//     SSRF guard         passed PER CALL as opts.pageUrl (the browser's MessageSender
//                        .url for THAT document, never `sender.tab.url`) —
//                        never a shared global (concurrent grinds). NORMAL web rules:
//                        a page may load cross-origin PUBLIC JS (CDN/imports) AND
//                        a localhost/intranet page may fetch its OWN private
//                        network (localhost->localhost). The ONLY thing blocked is
//                        a PUBLIC page reaching the user's PRIVATE network via the
//                        extension's host perms (confused-deputy). Replaces a
//                        duplicated _isPublicScriptUrl that also left source-map/
//                        discovery fetches unprotected.
//
// This is a DIRECT fetch from the offscreen document or its Web Worker — there is
// NO service-worker relay. crossOriginIsolated / COEP `require-corp` does NOT block
// fetch (it gates SharedArrayBuffer + high-res timers, and requires CORP only for
// no-cors *subresources*); a CORS fetch with the extension's host_permissions
// reaches any host straight from here. The only thing that ever blocked it was an
// over-restrictive `connect-src` — the CSP must allow https:/http:.
//
// Returns a plain object { ok, status, statusText, headers (lowercased), body
// (BYTES — a Uint8Array), urlList, computedType } — NOT a Response — so it is
// identical in the offscreen document and the Worker. `computedType` is §4.2's
// ESSENCE of what this function decided the resource IS, and it is on the record
// because this is the zone that read the bytes: it is TOLD to the renderer, which
// therefore never sniffs and never re-parses a raw header for a type of its own.
//
// THE BODY IS BYTES, AND THAT IS A LAYERING RULE RATHER THAN A TYPE PREFERENCE.
// It was `await resp.text()`, which is Fetch §5.3 "Body mixin"'s `text()`, whose
// steps are "to return the result of running consume body with this and UTF-8
// decode". (§5.2 stood here and is "BodyInit unions", which EXTRACTS a body rather
// than consuming one, so the number named the opposite direction.)
// A decode is a SEMANTIC, and CLAUDE.md §Architecture
// puts every semantic in the C engine and leaves this zone a BRIDGE, never logic —
// so this chokepoint was running an algorithm that is not its, and running the
// WRONG one: UTF-8 always, the response's charset ignored. HTML §8.1.4.2's "fetch a
// classic script" says "let sourceText be the result of DECODING bodyBytes to
// Unicode, using encoding as the fallback encoding", whose whole point is that the
// `Content-Type` charset (Fetch §3.5's legacy extract an encoding) and a BOM decide
// the decoder; the engine implements exactly that in core/loader/script_fetch.c, and
// it was being handed bytes that algorithm's label had never touched. A
// `charset=windows-1252` chunk arrived pre-mangled and there was no assert anywhere
// downstream that could ever have caught it — the evidence was destroyed in
// TRANSIT, so what the engine saw was a plausible string.
// Nothing about the SECURITY invariants moves with it: SOP/CORS/PNA/method/
// credentials are decided from the URL, the principal and the headers and never from
// the body, and the one check that does read the body — CORB's sniff below — still
// runs here, on these bytes, at the same point in this function. It decodes what it
// needs for its own comparison, which is a check reading its evidence rather than a
// transform applied to what crosses.
// urlList is Fetch §2.2.6's RESPONSE URL LIST, and this is the ONLY zone that can
// report it: the redirect chain exists here and nowhere else. §5.5 defines
// `response.url` as its LAST item and says "The redirected getter steps are to
// return true if this's response's URL list's size is greater than 1; otherwise
// false", so an engine that never receives it cannot compute either — which is why
// `redirected` was the literal false. The spec also says "Except for the first and
// last URL, if any, a response's URL list is not directly exposed to script as that
// would violate atomic HTTP redirect handling", and first + last is exactly what a
// browser fetch exposes to US (the requested href and resp.url), so this list is
// [requested] when nothing redirected and [requested, final] when something did —
// the whole of what any caller may ever observe. A blocked or failed read reports
// [requested]: the request URL is a fact even when the reply is not.
// LOADED IN EXACTLY ONE PLACE — ast-worker.html, the offscreen document, after check.js. This line named a
// second one ("ast-thread.js via importScripts") for a file that is not on disk and has no jsaudit row, which
// is the stale-pointer failure mode: it reads as authoritative while describing a tree that no longer exists,
// and here it also describes the wrong ENVIRONMENT — the DCHECK below is only defined because check.js is the
// FIRST script that page loads, and a worker reached by importScripts would have had none.
// CORB/ORB for a SCRIPT-LIKE destination (Fetch §2.2.5): a `<script src>`, an
// injected script or an import() becomes executable code under QuickJS control, so
// the response must be JS-typed (or same-origin) — never a cross-origin HTML/JSON/etc.
// DATA body read as code. Lives here (the chokepoint) so every code-loader gets it and
// a new one can't forget it — and the class is now read off the REQUEST rather than
// from a keyword the caller had to remember, which is what a caller did forget.
//
// TYPE SNIFFING LIVES HERE, AND IT IS THE ONLY PLACE IN THE EXTENSION THAT SNIFFS.
// CLAUDE.md §Architecture states it by name: "TYPE SNIFFING STAYS IN JAVASCRIPT, in
// `safeFetch`, where SECURITY.md puts it." The four functions below — `_jsMime`,
// `_corbProtectedMime`, the sniff, and the CORB rule that was `_corbAllowsScript` —
// were taken out of this file into a C program that had never been compiled, and the
// reasoning that took them — a migration mandate — is deleted; what a flow needs
// MID-EXECUTION belongs in the engine, and this is not that. It is what the trusted zone decides
// ONCE, between flows, about a reply it fetched, and the failure mode of getting it
// wrong is a wrong answer rather than a corrupted heap.
// SO THIS FILE ANSWERS THE QUESTION ONCE AND TELLS THE RENDERER WHAT IT DECIDED.
// `computedType` on the record below is that statement, and it is the reason there
// is no second sniffer downstream: `engine/host/solver/reply_decode.c` used to pull
// the raw `Content-Type` off the header list and re-derive a type for itself, so two
// zones were answering one question about one response and nothing made them agree.
// The chokepoint that read the bytes is the one that may answer it, exactly as the
// trusted zone — and never the untrusted engine — is what stamps a sender's origin
// onto a delivered message.
//
// A REAL (tuple) origin — scheme://host[:port] — usable for same-origin and CORS.
// An OPAQUE origin reports "null" (sandboxed iframe / data: / sandboxed doc) or
// our minted "null:<uuid>" token (per-document, and for a mixed-origin buffer);
// per the spec each opaque origin is UNIQUE, so it is NEVER same-origin with
// anything — not even another "null". A real origin is the only form containing
// "://"; "null" / "" / "null:<uuid>" do not, so this one test distinguishes them.
function _isRealOrigin(o) { return typeof o === "string" && o.indexOf("://") > 0; }
// The EMPTY byte sequence, which is what every blocked path's body is. It is not the
// empty STRING: a caller that must tell "no bytes" from "bytes I cannot read" reads
// `ok`/`status`, and a body is one type on every path this function has.
function _NO_BYTES() { return new Uint8Array(0); }
/* ── WHAT A REFUSAL IS, GRADED BY THE ARM THAT MADE IT ───────────────────────────────────────────────────
   A HOST THAT TURNS A REFUSAL INTO Fetch §5.6 "Fetch methods"' NETWORK ERROR IS TELLING THE FLOW THE SERVER
   ANSWERED, AND FOR HALF THE ARMS BELOW THAT IS FALSE. §5.6 is explicit about what a network error becomes on
   the page: "If response is a network error, then reject p with a TypeError and abort these steps" — so the
   page's request RESUMES down its failure path, and every branch after that `catch` is then explored under a
   fact about the origin that no observation supports. That is the plausible-datum defect at the level of the
   solver's world model: nothing crashes, the arm is real code, and the report cannot tell it from an arm a
   real failure reached.
   THE DISCRIMINATOR IS NOT "PERMANENT VS TEMPORARY" AND IT IS NOT "PRE- VS POST-REQUEST". It is: WOULD A REAL
   BROWSER, PERFORMING THIS SAME REQUEST, ALSO PRODUCE A NETWORK ERROR? Where it would, §5.6's answer is the
   FIDELITY and not a fabrication — the browser half is spec-locked and a `file:` URL, a CORS failure and a
   CORB-blocked script load all fail exactly this way in Chrome. Where no browser makes the refusal at all,
   there is no fact to relay and the only honest thing this zone can say is nothing.
     "network"  — a real browser refuses this same request, and Fetch §5.6's network error IS the answer.
                  Fetch §4.3 "Scheme fetch" ends its switch with "Return a network error" (and says of `file:`
                  "When in doubt, return a network error"); §4.10 "CORS check" failing makes §4.4 "HTTP fetch"
                  "return a network error"; a private-network target is refused by the browser's own PNA and
                  by CORS besides. The flow resumes down its failure path CORRECTLY.
     "decline"  — THIS TOOL declined, and no browser is refusing anything. Forced execution builds requests no
                  real client makes, and whether to spend an act on one is a POLICY (CLAUDE.md
                  §A-REQUEST-CARRIES-THE-PROVENANCE), so a refusal here is this zone declining to ASK. There is
                  no observation to hand back, and §@S says what that state is: search-not-yet-solved, a PARKED
                  flow, never a verdict. The flow stays parked and fires the day the origin is widened.
   WHY THERE IS NO THIRD WORD, WHICH IS A CONCLUSION AND NOT AN OMISSION. Two of the declines below are
   PERMANENT — the destructive-path deny list has no widening, and no setting reopens it — so it is fair to
   ask whether parking a flow on one for ever is a leak that needs an answer of its own. It is not, and the
   arithmetic is the scheduler's: a parked flow burns no CPU and emits nothing, so the WFQ's reward term stops
   paying it, `frontierWeight` (emit-per-visit) sinks it below productive and unrun work, and the disk share
   sheds it as a strict suffix of the one value order. Nothing is truncated and nothing accumulates. What a
   permanent decline owes the READER is not a third channel but the SENTENCE, and it already has one: the
   reason travels in `statusText`, and `blocked-destructive:logout` and `blocked-signal:destination=value` say
   different things to the person reading the stall — the second naming the ROW of their own control that
   holds it, which is the whole of what a per-signal policy buys over a score. §Attacker-sources states the rest outright — a
   derived-and-unfired request "is not a gap in the report, it IS the report".
   WHAT A DECLINE DOES COST, NAMED HERE BECAUSE THIS IS WHERE IT IS DECIDED: the page's own `.catch(...)` arm
   goes unexplored. That is a real loss and it is NOT an argument for the lie — an arm reached by fabricating
   a network error is an arm explored under a false premise, which is worse than an unexplored one. The right
   answer to it is a FORK (whether this request succeeds is unknown, so both arms are feasible), which is a
   solver capability and not a thing a chokepoint may buy by mis-grading a refusal.
   THE GRADE IS STAMPED BY THE ARM, WHICH IS WHAT MAKES THE WRONG PAIRING IMPOSSIBLE RATHER THAN DISCOURAGED.
   `computedType` is on the record for the same reason — the zone that read the bytes is the one that may
   answer what they are — and this is the zone that applied the rule, so it is the only one that knows which
   rule fired without re-deriving it. A consumer that re-asked the policy would be writing a second copy of it,
   and a consumer that MATCHED `statusText` would be writing that copy in a format nothing checks. */
/* AND THE HEADER MAP IS STATED BY THE ARM RATHER THAN DEFAULTED HERE, because one arm legitimately has one:
   a CORB refusal happens after the reply arrived and the headers ARE part of what was learned, while a
   pre-request refusal has none because no response exists. `{}` at the pre-request arms is therefore the
   positive statement "no response, therefore no headers" and not a hole this builder filled in. */
var _REFUSAL_KINDS = ["network", "decline"];
function _refused(kind, reason, urlList, headers) {
  DCHECK(_REFUSAL_KINDS.indexOf(kind) >= 0,
         "a refusal was graded `" + kind + "`, which is neither of the two this file states: `network` (a real " +
         "browser refuses this same request, so Fetch §5.6's network error is the faithful answer) and " +
         "`decline` (only this tool refuses, so there is nothing to hand back and the flow parks). A third " +
         "word would be a grade no host has an arm for, and the arm an unknown grade falls to is whichever a " +
         "consumer wrote as its else");
  DCHECK(typeof reason === "string" && reason !== "",
         "a refusal carries no reason — `statusText` is the only account a page or a person ever gets of a " +
         "request this zone did not make, and the empty string is a legitimate reason phrase from any HTTP/2 " +
         "response, so an unwritten one is indistinguishable from a reply");
  DCHECK(Array.isArray(urlList),
         "a refusal carries no Fetch §2.2.6 \"Responses\" URL LIST — the request URL is a fact even when the " +
         "reply is not, and the engine reads `response.url` off this list's last item");
  DCHECK(headers !== null && typeof headers === "object" && !Array.isArray(headers),
         "a refusal carries no header map — every record this file returns has one, and a caller that read " +
         "`undefined` here would be reading it off a refusal instead of off the reply it thought it had");
  return { ok: false, status: 0, statusText: reason, headers: headers, body: _NO_BYTES(),
           urlList: urlList, computedType: "", refusal: { kind: kind, reason: reason } };
}
/* THE SAME REFUSAL VALUE FOR A CALLER WHOSE REQUEST NEVER REACHES THE FETCH, WHICH IS THE OTHER HALF OF THE
   FIRING QUESTION AND THE ONE THIS FILE ANSWERS BY ABSENCE. RFC 9110 §9.2.1 "Safe Methods" is half of whether
   an act is spent and `_firingRefusal` is the other; this file enforces the method half STRUCTURALLY — it
   hardcodes `method:"GET"` and reads neither `opts.method` nor `opts.body`, which is why a non-GET cannot be
   issued here by any route — and that structure is exactly why nothing downstream can OBSERVE the answer.
   SO IT WAS BEING RE-DERIVED IN BOTH HOSTS, which is the shape `_firingRefusal` was hoisted here to end. Each
   held its own `if (method !== 'GET')` and each answered it differently: `engine/trusted.mjs` DECLINED (the
   flow stays parked) and `bridge.js` returned Fetch §5.6's network error (the flow resumes down its failure
   path having been told the server was unreachable, for a request nobody sent). One question, two answers,
   neither of them the policy — and the grade is the policy's to give, so it is given here.
   IT IS A DECLINE AND NOT A NETWORK ERROR: no browser refuses a POST. The address is DERIVED IN FULL AND
   REPORTED, which §Attacker-sources says is not a gap in the report but IS the report, and the flow parks.
   IT ANSWERS THE REFUSAL VALUE AND NOT A BOOLEAN, so a host that must tell somebody WHY cannot re-derive the
   grade from a `false` — the same reason `_corbDeniesScript` and `_firingRefusal` answer grades. */
function safeFetchMethodRefusal(method) {
  CHECK(typeof method === "string" && method !== "",
        "safeFetchMethodRefusal was asked about " + JSON.stringify(method) + ", which is not a Fetch §2.2.1 " +
        "\"Methods\" method — the caller is deciding whether this zone can perform a request at all, and an " +
        "absent method would take the arm `GET` takes, which is the arm that spends the network");
  return method === "GET" ? null : { kind: "decline", reason: "blocked-method:" + method };
}
// HTML's JavaScript MIME TYPE list, and Chromium's CORB-PROTECTED set — the two
// tables the CORB rule is stated over.
function _jsMime(m) {
  return m === "text/javascript" || m === "application/javascript" ||
    m === "application/ecmascript" || m === "text/ecmascript" ||
    m === "application/x-javascript" || m === "text/x-javascript" ||
    m === "application/x-ecmascript" || m === "text/jscript" ||
    m === "application/node" || /^text\/javascript1\.[0-5]$/.test(m);
}
function _corbProtectedMime(m) {
  return m === "text/html" || m === "text/xml" || m === "application/xml" ||
    /\+xml$/.test(m) || m === "application/json" || /\+json$/.test(m) ||
    /^multipart\//.test(m);
}
// Fetch's DETERMINE NOSNIFF, over the `X-Content-Type-Options` value as "get a
// header" has already joined the list's duplicates: "let values be the result of
// getting, DECODING AND SPLITTING the header … if values[0] is an ASCII
// case-insensitive match for `nosniff`, return true."
// THIS WAS `indexOf("nosniff") >= 0` AND THAT IS A DIFFERENT ALGORITHM. A substring
// test sets the flag for `foo, nosniff`, where the standard splits on U+002C, strips
// HTTP whitespace and matches only the FIRST value — so that response was treated as
// unsniffable here and is sniffable under Fetch, and a cross-origin body served
// `text/plain` was refused to a code loader that a browser would have allowed. The
// split is the whole fix and it belongs at the one place that reads the header.
function _determineNosniff(v) {
  if (typeof v !== "string") return false;   // absent header = §"values is null" = false
  return v.split(",")[0].replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, "").toLowerCase() === "nosniff";
}
/* MIME Sniffing §6 "Matching a MIME type pattern", and the two of §7.1 "Identifying a resource with an unknown
   MIME type"'s tables whose answers any reader of `computedType` can act on.

   THIS IS A MIRROR OF `engine/host/browser/core/mime/mime_sniff.c` AND NOT A RIVAL TO IT, for the reason
   `engine/host/check.h` is mirrored by `extension/check.js`: that component is inside the WASM sandbox and
   this is the trusted zone, and no call crosses between them in either direction. CLAUDE.md §Architecture puts
   the sniff HERE by name — "TYPE SNIFFING STAYS IN JAVASCRIPT, in `safeFetch`, where SECURITY.md puts it" —
   while `mime_sniff.c` answers HTML §7.4.5's computed type for a NAVIGATION this engine is about to parse into
   its own document, which its own header states is a C algorithm no host can answer instead. Neither can serve
   the other, so the two carry the SAME spec citations and the SAME cell encoding: a reader repairing one can
   find the other by grepping the section number, which is the only thing that keeps two mirrors from drifting.

   ONE ARRAY CARRIES BOTH OF §6's COLUMNS — each cell is the pattern mask byte in bits 8-15 and the byte pattern
   byte in bits 0-7 — and that is `mime_sniff.c`'s encoding kept for its reason rather than copied for
   symmetry: §6's very first step is "Assert: pattern's length is equal to mask's length", and a pair of
   hand-written arrays is the shape that assert exists to catch. With one array the two lengths cannot
   disagree, so there is nothing here for an assert to check.

   §5.2 "Reading the resource header" bounds what §7 LOOKS AT: "the number of bytes in buffer is greater than or
   equal to 1445". It is not a truncation of the body — every byte still reaches the reader below and the
   record still carries the whole sequence. */
var _MS_RESOURCE_HEADER_MAX = 1445;

/* §6.1 "Matching an image type pattern" — the table in the standard's own row order, which is also the order
   `mime_sniff.c`'s IMAGE_TABLE states it in. */
var _MS_IMAGE = [
  { cells: [0xFF00, 0xFF00, 0xFF01, 0xFF00], type: "image/x-icon" },   // a Windows Icon signature
  { cells: [0xFF00, 0xFF00, 0xFF02, 0xFF00], type: "image/x-icon" },   // a Windows Cursor signature
  { cells: [0xFF42, 0xFF4D], type: "image/bmp" },                      // the string BM
  { cells: [0xFF47, 0xFF49, 0xFF46, 0xFF38, 0xFF37, 0xFF61], type: "image/gif" },   // GIF87a
  { cells: [0xFF47, 0xFF49, 0xFF46, 0xFF38, 0xFF39, 0xFF61], type: "image/gif" },   // GIF89a
  { cells: [0xFF52, 0xFF49, 0xFF46, 0xFF46, 0x0000, 0x0000, 0x0000, 0x0000,
            0xFF57, 0xFF45, 0xFF42, 0xFF50, 0xFF56, 0xFF50], type: "image/webp" },  // RIFF, four bytes, WEBPVP
  { cells: [0xFF89, 0xFF50, 0xFF4E, 0xFF47, 0xFF0D, 0xFF0A, 0xFF1A, 0xFF0A], type: "image/png" },
  { cells: [0xFFFF, 0xFFD8, 0xFFFF], type: "image/jpeg" }              // SOI, then another marker's indicator
];
/* §6.4 "Matching an archive type pattern". */
var _MS_ARCHIVE = [
  { cells: [0xFF1F, 0xFF8B, 0xFF08], type: "application/x-gzip" },
  { cells: [0xFF50, 0xFF4B, 0xFF03, 0xFF04], type: "application/zip" },              // PK, then ETX EOT
  { cells: [0xFF52, 0xFF61, 0xFF72, 0xFF21, 0xFF1A, 0xFF07, 0xFF00], type: "application/x-rar-compressed" }
];

/* §6's PATTERN MATCHING ALGORITHM, given a byte sequence and one row's packed pattern-and-mask.
   §6's STEPS 3 AND 4 ARE ABSENT AND THAT IS THE TABLES' OWN STATEMENT RATHER THAN A SHORTCUT: they advance past
   the `ignored` set, and every row of §6.1 and §6.4 prints that column as None — so `s` never moves ahead of
   `p` and the two indices are one. A table with a non-empty ignored column may not be walked by this. */
function _msPatternMatch(input, cells) {
  var p, masked;
  if (input.length < cells.length) return false;                        // step 2
  for (p = 0; p < cells.length; p++) {                                  // steps 5-6
    masked = input[p] & ((cells[p] >> 8) & 0xFF);
    if (masked !== (cells[p] & 0xFF)) return false;
  }
  return true;                                                          // step 7
}
/* "Execute the following steps for each row row in the following table ... If patternMatched is true, return
   the value in the fourth column of row" — the shape §6.1 and §6.4 share. `null` is the standard's undefined. */
function _msTable(rows, header) {
  for (var i = 0; i < rows.length; i++)
    if (_msPatternMatch(header, rows[i].cells)) return rows[i].type;
  return null;
}
/* §7.1's IMAGE STEP THEN ITS ARCHIVE STEP, in the standard's own order, and `null` where neither answers.

   NAMED RESIDUAL — WHAT IS NOT COVERED IS A PROPERTY AND NOT A LIST OF INPUTS: every row of §7.1 other than the
   two tables above. That is §6.2 "Matching an audio or video type pattern" with its three signature
   sub-algorithms, §7.1's sniff-scriptable table, its PostScript-and-BOM table, and its closing
   contains-no-binary-data-bytes fall-through; and, one level up, §7's steps 1, 4 and 5-6. Where neither table
   answers, `_computedType` returns the answer it returned before this existed, so the code is CORRECT for what
   it decides and NARROWER than §7 — never a different answer from it. The two tables may be walked alone
   because no row of §7.1 that precedes them can match a byte sequence either of them matches: the scriptable
   table's rows all open 0x3C, its other table's open 0x25, 0xFE, 0xFF 0xFE or 0xEF, and the only image row
   opening 0xFF is 0xFF 0xD8 0xFF.

   WHAT THE NEXT DIFF BUILDS is §6.2's table and its §6.2.1 MP4, §6.2.2 WebM and §6.2.3 MP3-without-ID3
   signature algorithms, mirrored from `mime_sniff.c` (GREPPED there as `sniff_av_pattern` and its helpers) —
   AND §7.1's PostScript-and-BOM table IN THE SAME DIFF, which is an ordering constraint rather than a
   preference: §6.2.3 finds an MP3 frame by a sync whose first two bytes are 0xFF and any byte with its top
   three bits set, and §7.1's UTF-16LE BOM row is 0xFF 0xFE, which satisfies that sync — so §6.2 landed without
   the BOM row in front of it computes `audio/mpeg` for a UTF-16LE text document. §7.1 orders the BOM table
   first for exactly this reason and the order is the algorithm.

   HOW ITS ABSENCE WOULD SHOW, as an OBSERVATION and never as an instance: an endpoint record on the @RESULT
   surface for an address whose reply carried no `Content-Type` at all, or one of §7 step 2's three unknown
   essences, and whose first bytes match a §6.2 audio-or-video pattern — the learned surface naming as an API
   a resource whose own bytes are a media stream, with the reply's header list beside it saying no server
   named it anything. */
function _msUnknownTypePattern(header) {
  var m = _msTable(_MS_IMAGE, header);
  if (m) return m;
  return _msTable(_MS_ARCHIVE, header);
}

// CORB's own sniff, over BYTES — the check decoding the evidence it judges, which is
// what Chrome's ORB does too (it attempts a JSON parse of the body). The head is
// decoded first and the whole body only in the branch that actually needs it, so a
// multi-megabyte JS chunk (which starts with none of `<`, `{`, `[`) costs one 4 KiB
// decode rather than a full one.
// IT ANSWERS WHICH SHAPE IT MATCHED RATHER THAN A BARE BOOLEAN, and that is the one
// change on top of what stood here. Both arms already KNEW: the `{`/`[` arm ran the
// real JSON parser and only returns having been told yes, so naming that answer
// `application/json` invents nothing. The markup arm names NO type — a leading `<` is
// markup and CORB protects it, but no standard says which markup a bare `<` is, and
// answering `text/html` for `<?xml` or `<svg` would be a stamp nobody could trust.
// `protected` is what the CORB rule reads and `type` is what the record below
// carries, out of ONE pass over the bytes.
// AND IT NOW RUNS FOR EVERY RESPONSE AND NOT ONLY A SCRIPT LOAD, because the record
// states a type for every response. The two JSON attempts are therefore ordered HEAD
// FIRST, which is free: the version that stood here parsed the WHOLE body and fell
// back to the head in its catch, so the ANSWER is "either parses" either way, and
// asking the head first means a body that fits in 4 KiB — which is most JSON an API
// returns — is never parsed twice and a large one costs exactly what it cost before.
// AND THE THIRD FIELD IS MIME Sniffing §7.1's, WHICH IS A THIRD QUESTION OVER THIS ONE PASS AND NOT A
// WIDENING OF EITHER ANSWER ABOVE. `protected` is CORB's — may these cross-origin bytes reach a code loader —
// and `type` is `did the bytes contradict a label this zone acts on`, which is why its only non-null value is
// `application/json` and why `_computedType` lets it OVERRIDE a declared essence. Neither is MIME Sniffing
// §7 "Determining the computed MIME type of a resource", and `unknownType` is: it is that algorithm's step 2
// arm, the one case where the standard lets the BYTES name a resource the server did not. Three questions,
// one fact, one pass — CLAUDE.md's own cure for a predicate answering two, which this file already applies at
// `_isScriptLike` / `_isSubresource` over the destination string.
// IT IS COMPUTED OVER §5.2 "Reading the resource header"'s BYTES AND NOT OVER THE DECODED HEAD `h`, because
// §6.1's and §6.4's rows all state their leading-bytes-to-be-ignored column as None: a whitespace skip here
// would match a pattern at an offset the standard does not look at.
function _sniff(bytes) {
  var dec = new TextDecoder("utf-8");   // strips a UTF-8 BOM, exactly as resp.text() did
  var hdr = bytes.subarray(0, _MS_RESOURCE_HEADER_MAX);
  var unknown = _msUnknownTypePattern(hdr);
  var h = dec.decode(bytes.subarray(0, 4096)).replace(/^﻿/, "").replace(/^\s+/, "");
  if (h.charAt(0) === "<") return { protected: true, type: null, unknownType: unknown };   // HTML/XML/SVG/markup
  if (h.charAt(0) === "{" || h.charAt(0) === "[") {
    try { JSON.parse(h); return { protected: true, type: "application/json", unknownType: unknown }; }
    catch (e) {
      try {
        JSON.parse(new TextDecoder("utf-8").decode(bytes));
        return { protected: true, type: "application/json", unknownType: unknown };
      }
      catch (_) {}
    }
  }
  return { protected: false, type: null, unknownType: unknown };
}
// WHAT THIS RESPONSE IS, AS ONE STATEMENT THE RENDERER IS TOLD. MIME Sniffing §4.2 "MIME type
// miscellaneous"'s ESSENCE — the type, a solidus, the subtype — of the server's
// `Content-Type` when it stated one, and what the bytes say when it did not or when
// they contradict it. The empty string is MIME Sniffing §5.1 "Interpreting the resource
// metadata"'s "the supplied MIME type is undefined" surviving the sniff: the
// server named nothing and the bytes named nothing either, which is a POSITIVE
// answer and not a hole (a reader that must distinguish it reads it as absent, the
// way `mime_type_extract` reads a null header).
// NOSNIFF IS FINAL. The server has said its label is the last word, so this returns
// the essence unchanged whatever the body looks like — the same sentence that stops
// the CORB rule below from sniffing past it.
//
// AND A DECLARED TYPE THE BYTES CONTRADICT IS THE SERVER'S TO NAME, WHICH IS THE ONE THING A READER OF THIS
// FUNCTION KEEPS RE-DERIVING BACKWARDS. MIME Sniffing §7 "Determining the computed MIME type of a resource"
// reaches the bytes at exactly three of its nine steps — step 2 (the supplied type is undefined or unknown),
// step 4 (the check-for-apache-bug flag), and steps 5-6 (the supplied type is already an image or an
// audio-or-video type the user agent renders, where the bytes only REFINE it within its own group) — and its
// last step is "The computed MIME type is the supplied MIME type." So a PNG served `text/plain` computes
// `text/plain` IN EVERY BROWSER, and a classifier that answers `image/png` for it is not a more faithful
// sniff, it is a different question. THAT IS WHY `classifyResponseAsset` IN `extension/lib/discovery.js` IS
// NOT ROUTED TO FROM HERE AND MUST NOT BE: its own header says magic bytes are authoritative and the header is
// a weaker cross-check, which is the correct rule for the question IT asks (has this captured body a schema
// worth extracting) and the inverse of §7 for the question this one answers. Its answer set says so on its
// face — it returns `opaque-cross-origin`, `binary-structured` and `font/woff2`, of which the first two are
// not MIME types at all and the third is one §7 CANNOT PRODUCE FROM BYTES, since §7.1 runs §6.1, §6.2 and
// §6.4 and never §6.3 "Matching a font type pattern", which only §8.7 "Sniffing in a font context" invokes.
// A browser never sniffs a font, so a font-typed record here can only ever come from a server that declared
// one, and that is faithful rather than a gap.
// RETIREMENT: this paragraph goes when the two classifiers cannot be confused — when the asset verdict the
// @H surface acts on is a field of its own beside `computedType` rather than `solver/reply_decode.c`'s
// `is_asset` asked of §7's answer, at which point one of them stops being a candidate to route to.
function _computedType(declared, nosniff, sniff) {
  var mime = String(declared == null ? "" : declared).split(";")[0].trim().toLowerCase();
  DCHECK(sniff !== null && typeof sniff === "object" &&
         (sniff.unknownType === null || typeof sniff.unknownType === "string"),
         "the sniff handed to `_computedType` carries no §7.1 answer — `_sniff` writes that field on every " +
         "arm, as `null` for a resource whose bytes match no pattern it walks, so an absent one is a SECOND " +
         "sniff from somewhere else and the arm below would silently take the answer that has no bytes " +
         "behind it");
  // §7 STEP 2, AND IT IS ASKED BEFORE STEP 3's NOSNIFF BECAUSE THE STANDARD ASKS IT THERE: "If the supplied
  // MIME type is undefined or if the supplied MIME type's essence is "unknown/unknown", "application/unknown",
  // or "*/*", execute the rules for identifying an unknown MIME type with the sniff-scriptable flag equal to
  // the inverse of the no-sniff flag and abort these steps." A server that sent `nosniff` and NO
  // `Content-Type` has not named a label for nosniff to make final, so the flag narrows WHICH of §7.1's tables
  // may run rather than stopping the algorithm — and neither of the two tables walked here is in the
  // sniff-scriptable half, so both run under either setting and the flag has nothing to gate yet. The day
  // §7.1's first table lands, that gate lands with it: the DCHECK below is what makes forgetting it loud.
  // THE QUOTATION IS IN A LINE COMMENT AND NOT IN THE BLOCK ONES EITHER SIDE OF IT BECAUSE OF WHAT IS IN IT:
  // the step's third essence is U+002A U+002F U+002A, which closes a block comment, so `mime_sniff.c` writes
  // that essence out in prose and says it cannot be written in a C comment. It can be written here, and a
  // quotation that PARAPHRASES the one token a reader would grep for is the mis-transcription CLAUDE.md
  // §Browser half rates as worse than no quotation — this one was caught by the citation audit reading it.
  if (!mime || mime === "unknown/unknown" || mime === "application/unknown" || mime === "*/*") {
    if (sniff.unknownType) {
      /* NOT A VACUOUS ASSERT: it cannot fail over the two tables above, and it is exactly constructible over
         the next diff those tables name. §7.1's sniff-scriptable table returns `text/html`, `text/xml` and
         `application/pdf`, every one of which is a CORB-protected or scriptable type, and §7.2's own note is
         that these rules must "never determine the computed MIME type to be a scriptable MIME type, as this
         could allow a privilege escalation attack". `computedType` is read by `solver/engine.c` to decide
         whether a reply's bytes are QUEUED AS A PROGRAM, so a row added here without §7.1's flag in front of
         it is that escalation with this engine's own compiler on the end of it. This fires there instead. */
      DCHECK(!_jsMime(sniff.unknownType) && !_corbProtectedMime(sniff.unknownType),
             "§7.1's unknown-type sniff answered `" + sniff.unknownType + "`, which is a JavaScript or a " +
             "CORB-protected type — the tables walked here are §6.1's and §6.4's and neither can produce one, " +
             "so a row was added from §7.1's SNIFF-SCRIPTABLE table without the flag that gates it, and a " +
             "reply is one `computedType` read away from being compiled as a program");
      return sniff.unknownType;
    }
  }
  if (nosniff) return mime;
  if (!mime) return sniff.type || "";
  // The bytes contradict the label: a JSON body under `text/plain` or under a
  // JavaScript type is JSON, and saying so is what lets the reply be LEARNED from
  // rather than skipped as whatever the server mislabelled it.
  if (sniff.type && sniff.type !== mime) return sniff.type;
  return mime;
}
// THE PRINCIPAL COMPARISON USED TO LIVE HERE, as `_corbSameOrigin(scriptUrl, pageOrigin)`,
// and it is GONE rather than moved: it re-parsed the landed address under a `catch` of its
// own to re-derive an origin the request path had already computed, which made it a THIRD
// answer to "is the resource same-origin with the page principal" beside the credentialed
// gate's. `_resourceSameOrigin`, computed once beside `_finalOrigin` and read by both
// gates, is that one answer — see the paragraph that computes it for why one is the whole
// point and for the reasoning this comment used to carry.
// CORB FOR A SCRIPT LOAD, over the facts already computed above so nothing here
// re-reads a header or re-decodes a body. Answers the RULE THAT REFUSED, or null for
// allowed — because "blocked" alone sends whoever reads the status message hunting
// for which of four rules fired, and the four are not interchangeable.
// IT WAS `_corbAllowsScript`, RETURNING A BOOLEAN, and the name is renamed with the
// return rather than kept over it: a function called "allows" that answers a deny
// reason reads correctly at exactly zero of its call sites. Every rule below is the
// one that stood in that function, in the order it stood in.
// IS THIS REQUEST'S DESTINATION SCRIPT-LIKE — Fetch §2.2.5 "Requests": "A request's
// destination is script-like if it is `audioworklet`, `paintworklet`, `script`,
// `serviceworker`, `sharedworker`, or `worker`." That predicate IS the CORB question
// this file asks, so it is asked in the spec's own words instead of being restated as
// a bespoke load-type keyword: the caller passes the destination the ENGINE put on
// the request (solver/engine.h), and this decides.
// `xslt` IS DELIBERATELY NOT IN IT, and the spec's own note is why rather than an
// oversight: it says algorithms using script-like "should also consider `xslt` as that
// too can cause script execution", and considering it here yields exclusion — the rule
// below is `the body must be JAVASCRIPT-TYPED or same-origin`, and an XSLT stylesheet
// is XML, so requiring a JS MIME of one would refuse every correct response. The day
// this engine loads an XSLT stylesheet it needs its own rule, not this one.
function _isScriptLike(d) {
  return d === "audioworklet" || d === "paintworklet" || d === "script" ||
    d === "serviceworker" || d === "sharedworker" || d === "worker";
}
// AND IS THIS REQUEST A SUBRESOURCE OF THE DOCUMENT — A SECOND QUESTION ASKED OF THE
// SAME FACT, which is CLAUDE.md's own cure for a predicate answering two questions and
// not a second spelling of the one above: the destination STRING is the fact, and each
// of these is a QUESTION asked of it, so the two cannot come to disagree about a chunk
// the way two answers to one question could. `_isScriptLike` answers CORB's — may this
// reply be ingested as CODE — and stays exactly the spec's list. This one answers the
// EGRESS question, which CLAUDE.md §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS states as
// whose ACT the request is rather than what its reply becomes.
//
// THE TWO AGREED WHILE A `<script src>` WAS THE ONLY SUBRESOURCE IN THE PERMITTED
// GROUP, AND A `<link rel=stylesheet>` IS WHAT MADE THEM COME APART. Read by the CORB
// predicate alone a stylesheet lands in the group that spends somebody else's server,
// and the page then renders with UA defaults only — which is not a narrower answer but
// a DIFFERENT DOCUMENT. The bit kept whatever the stricter question needed and the
// looser one was refused with nothing anywhere to say it had been asked.
//
// IT IS FETCH §2.2.5 "Requests"' OWN subresource request MINUS THE EMPTY STRING, AND
// THE MINUS IS THE DECISION RATHER THAN A TIDY-UP. §2.2.5 states the term verbatim: "A
// subresource request is a request whose destination is audio, audioworklet, font,
// image, json, manifest, paintworklet, script, style, text, track, video, xslt, or the
// empty string" — and the empty string is what a `fetch()` and an XHR carry, since
// §2.2.5 gives every request a destination and "unless stated otherwise it is the empty
// string". So the standard's term AS WRITTEN admits every data request at every origin,
// which is the whole per-origin opt-in deleted by one predicate: the same dangerous
// spelling the arm list below already names as `{signal:"destination",value:"value"}`,
// reached this time through a term the standard hands you rather than through a typo.
// The departure is therefore stated HERE rather than inherited silently. A NON-EMPTY
// subresource destination is one some element, stylesheet rule or DOM constructor in
// the document NAMED; the empty one is page code asking for bytes directly, which is
// the data fetch the default refuses.
//
// AND IT IS THE STANDARD'S POSITIVE LIST AND NEVER THE COMPLEMENT OF ITS OTHER ONE,
// because §2.2.5's two terms DO NOT PARTITION its own enumeration: `webidentity` is in
// neither, so a complement would have admitted a destination the standard declines to
// call a subresource — silently, and on the widening side.
//
// NAMED RESIDUAL — WHAT IS NOT COVERED IS A PROPERTY AND NOT A LIST: a subresource the
// document's own markup or its own running code names, whose §2.2.5 destination is the
// EMPTY STRING. At this chokepoint such a request is not separable from the data fetch
// the default refuses, because the destination is the only fact crossing the seam that
// says what a request is FOR, and §2.2.5 gives that value to a `fetch()` and to a
// page-declared subresource alike. The code above is CORRECT for what it decides and
// NARROWER than the owner's sentence, which reaches every subresource; it is not a gap
// to crash on, because the arm it feeds is a permission and the narrow answer is the
// refusing one.
// WHAT THE NEXT DIFF BUILDS is the PARK KIND on the pending line beside the
// destination — the engine already separates a browser algorithm's subresource fetch
// from a `fetch()`/XHR by kind, and that fact does not cross. GREPPED at
// engine/host/solver/engine.c's `engine_pending_fetches`, which joins METHOD,
// DESTINATION, INITIATOR, PROVENANCE, PINNED, CREDENTIALS and URL, and reads PEND_KIND
// at exactly one line in that function, inside a DCHECK. A kind-derived token there
// would say WHOSE ALGORITHM IS OWED THE REPLY, which is the egress question asked of a
// fact the engine already holds, and the empty destination would stop having to stand
// for two things. That phrasing is this file's own and carries no quotation marks on
// purpose: a run shown as a SPELLING under a spec anchor is judged as a quotation of
// that spec, which is how this very sentence was reported as a fabricated §2.2.5
// quotation on the run before this one.
// HOW ITS ABSENCE WOULD SHOW, as an OBSERVATION and never as an instance: a run whose
// log carries a `blocked-signal:destination=value` refusal for an address the analysed
// document's own markup declares — the refusal and the markup disagreeing about whose
// act the request is, which is the one disagreement this predicate exists to end.
function _isDocumentSubresource(d) {
  return d === "audio" || d === "audioworklet" || d === "font" || d === "image" ||
    d === "json" || d === "manifest" || d === "paintworklet" || d === "script" ||
    d === "style" || d === "text" || d === "track" || d === "video" || d === "xslt";
}
// FETCH §2.2.5 "Requests"' NAVIGATION REQUEST, AND IT IS HERE FOR THE REASON THE SUBRESOURCE
// PREDICATE ABOVE IS: this file is the one that decides from the destination, and the standard already has
// the term. §2.2.5 defines it as "A navigation request is a request whose destination is
// `document`, `embed`, `frame`, `iframe`, or `object`", and that POSITIVE LIST is what this is, never a
// complement — the argument the subresource predicate makes about `webidentity` being in neither of
// §2.2.5's other two sets holds here too.
//
// WHAT IT CLOSES IS AN ACCIDENT AND NOT A GAP, WHICH IS WHY IT CHANGES NO VERDICT. A navigation used to
// fall through both questions above and read `value`, so the ONLY thing keeping it out of the owner's data
// arm was that arm's `witness: unpinned` conjunct answering `unstated` — and `unstated` is there
// because a notice carries no witness mark, which is a fact about PLUMBING rather than a decision about
// navigations. CLAUDE.md §A-REAL-NAVIGABLE says in as many words that the mark SHOULD travel
// ("`engine_pinned_of_running_path()` exists; which arm it lands on is the owner's call"), so the day
// somebody lands that plumbing — correct, owed, and not a policy diff — every derived child
// navigable at every origin would START FIRING with nobody having decided it. That is the shape
// CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS names at the one boundary where the consumer is the
// PERSON: a permission whose reach moves when an unrelated field starts being stated.
//
// AND THE ROW'S OWN PROSE WAS ALREADY WRONG ABOUT IT, WHICH IS THE CHEAPEST EVIDENCE THAT THE VALUE WAS
// DOING TWO JOBS: the destination signal says a `value` request "is this tool spending somebody else's
// server", and a child navigable the analysed document's OWN MARKUP names is the page loading itself by
// exactly the sentence the two arms above are drawn from. One value, two meanings, and the refusal's
// stated reason was about the wrong question — §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS.
//
// WHERE IT IS ASKED CANNOT MATTER, AND THAT IS ASSERTED RATHER THAN ARGUED. §2.2.5's navigation set
// is DISJOINT from the script-like set and from the subresource set — no destination is both — so
// this question's position in the cascade cannot change any answer, which is what makes the two existing
// values byte-identical after this diff. `_signalRegistryCheck` walks `_DESTINATION_TYPES` and DCHECKs it,
// so the day a later edition of §2.2.5 moves a destination into two sets the host aborts at startup
// instead of this paragraph being quietly false. It is asked LAST of the three regardless, because the two
// above are the ones whose order IS load-bearing and the reason for theirs is written at the row.
//
// NO ARM NAMED `navigation` AND THAT WAS THE WHOLE OF THE VERDICT, AND IT IS REWRITTEN RATHER THAN DELETED
// BECAUSE THE ARGUMENT IS SOUND AND A READER WILL RE-DERIVE IT FROM THE VALUE SPACE. It read: a value no arm
// names is refused — the row above already relies on that property in the other direction — so an
// unconfigured origin refuses a navigation exactly as it did before, and names the destination when it does;
// whether a child navigable the page's own markup names SHOULD fire is the project owner's decision and is
// open. THAT DECISION IS MADE, IN THE AFFIRMATIVE, and the arm at the end of `_DEFAULT_ARMS` is where it is
// written: `destination=navigation` AND `actor=page` AND `provenance=derived`. What this predicate did was
// make it ONE ARM rather than a question about what an unrelated plumbing fix would do, and that is exactly
// what it bought.
// AND THE HAZARD IT WAS WRITTEN AGAINST IS STILL CLOSED, WHICH IS THE FIRST THING A READER OF THE PARAGRAPHS
// ABOVE WILL CHECK AND IS NOW TRUE FOR A SECOND REASON. The fear was that landing the witness mark for a
// navigation — a correct, owed plumbing diff — would silently start firing every derived child
// navigable with nobody having decided it. No arm a navigation can take reads the `witness` row at all: the
// one that admits it names destination, actor and provenance, and the `provenance`/`doc-reach` arm names
// neither. So the mark travelling changes NO firing outcome for any navigation at any setting; it changes
// only the per-origin row a person sees, which they permit separately.
// AND THE ARM THAT ALREADY FIRES A NAVIGATION IS UNTOUCHED, which is said here so nobody reads this as a
// narrowing: the `observed` arm reads `provenance` and `doc-reach` and no destination, so the ambient seed
// — which states both — still fires, and every document this tool opens still opens.
// HOW ITS ABSENCE WOULD SHOW: a drive log whose navigation refusals read `blocked-signal:destination=value`
// — the chokepoint naming a pinning question as its reason for refusing a document load.
// RETIREMENT — MET BY A CONSTRUCTION, AND RE-KEYED RATHER THAN DELETED FOR THE REASON THE FIRST
// PARAGRAPH GIVES. The condition read: this record goes when an arm names `navigation`, because the owner's
// decision is then IN the table and the hazard this closes has a deliberate answer in front of it rather
// than an absent field. An arm names it. What a reader re-derives from `_DESTINATION_TYPES` is the retired
// argument and not the arm, so the wording stays.
// RETIREMENT: this record goes when the DESTINATION VALUE a request computes is asserted against the arms
// that can admit it — so a value this cascade produces and no arm can ever name is a host-startup
// failure rather than a refusal a reader has to recognise — because the property this predicate relies
// on is then checked rather than argued, at the one place both halves are in hand.
function _isNavigation(d) {
  return d === "document" || d === "embed" || d === "frame" || d === "iframe" || d === "object";
}
// FETCH §2.2.5 "Requests"' DESTINATION TYPE, ENUMERATED — "A destination type is one
// of: the empty string, `audio`, `audioworklet`, `document`, `embed`, `font`, `frame`,
// `iframe`, `image`, `json`, `manifest`, `object`, `paintworklet`, `report`, `script`,
// `serviceworker`, `sharedworker`, `style`, `text`, `track`, `video`, `webidentity`,
// `worker`, or `xslt`." It lives HERE, beside the script-like predicate that is a
// SUBSET of it, because this file is the one that decides from the value; a second
// table in a zone that only relays the field would be a copy that goes stale.
var _DESTINATION_TYPES = ["", "audio", "audioworklet", "document", "embed", "font", "frame", "iframe",
                          "image", "json", "manifest", "object", "paintworklet", "report", "script",
                          "serviceworker", "sharedworker", "style", "text", "track", "video",
                          "webidentity", "worker", "xslt"];
// AND EVERY CALLER MUST STATE ONE, AND IT MUST BE ONE §2.2.5 DEFINES — a `CHECK`, and
// both halves of that are the same sentence rather than two rules. Fetch §2.2.5's
// default IS the empty string, so a caller CAN legitimately answer "" and mean data;
// what must not happen is a value this file cannot serve READING as though it had been
// answered, because `_isScriptLike` says false for everything it does not recognise —
// so an ABSENT destination and a MISSPELLED one take the identical arm, and it is the
// permissive one: the bytes it silently accepts are a cross-origin body a compiler is
// about to be handed. That is the shape this file already had once under another name,
// when the class rode a keyword a caller could forget.
//
// WHY IT IS A `CHECK` AND NOT A `DCHECK`, WHICH IS WHAT IT WAS. The discriminator is
// SECURITY.md's own, stated for `opts.pageUrl` one bullet over: a DCHECK is right where
// "release must still be able to PROCEED, and it can" — with the check compiled out an
// unparseable principal still classifies as public and private targets are still
// blocked, so the release behaviour is unchanged. This field FAILS that test in the
// opposite direction. With the check compiled out, `undefined` reaches `_isScriptLike`,
// answers false, and the CORB gate — which SECURITY.md assigns to this file by name —
// is skipped for a code load. Fail-open on a security boundary is what CLAUDE.md's
// `CHECK` is for ("a security/authorization boundary … we must not PROCEED even in
// production"), and it is what `_statedFacts` is a CHECK for one zone over.
//
// AND THE VALUE CROSSES FROM THE UNTRUSTED ZONE, WHICH IS THE OTHER HALF. The engine
// states the destination at each park and the engine is attacker-controlled
// (SECURITY.md §The QuickJS/WASM sandbox); its own join and splitter assert §2.2.5's
// enumeration with DCHECKs, which are compiled out on the far side of a mojo boundary
// in the zone this one does not trust. So `the producer refuses to emit a value outside
// the enumeration` is not a check this zone holds. `bridge.js` already CHECKs the
// INITIATOR and the PROVENANCE of that same line — the two fields nothing decides an
// ingestion from — and the field the ingestion IS decided from was the one taken on the
// producer's word. Refusing here closes it for BOTH hosts at once, which is the reason
// it belongs at the chokepoint rather than in either host's splitter.
/* ── DOES A GATE IN THIS FILE READ THIS REQUEST'S WHOLE BODY ─────────────────────────────────────────────
   A POSITIVE STATEMENT ABOUT THE REQUEST, ASKED BEFORE ANY BYTE MOVES, and the one thing that decides whether
   a reply may be handed over a piece at a time. It is DERIVED FROM `_isScriptLike` AND NEVER RESTATED: CORB is
   the only gate below that reads the body, its scope IS that predicate, and a second spelling of that scope
   here would be a copy that goes stale the day the predicate moves — which is the same reason `_isScriptLike`
   itself sits beside `_DESTINATION_TYPES` rather than in a zone that only relays the field.

   WHY THE ANSWER IS NOT "SNIFF THE FIRST CHUNK AND CARRY ON". Measured, with a control, over one 9 KB JSON
   body labelled `text/plain`, cross-origin, script-like: the WHOLE body sniffs `protected: true` and
   `_corbDeniesScript` answers `sniffed-data`, so the load is refused; the FIRST 4096 BYTES sniff
   `protected: false` and it answers null, so the same body is admitted to a code loader — and the record's
   type goes `application/json` to `text/plain` with it. That is not a weaker decision, it is the OPPOSITE
   decision, and it is structural rather than unlucky: `_sniff`'s second arm re-parses the whole body, so any
   JSON document longer than 4096 bytes fails the prefix parse BY CONSTRUCTION and cannot reach the arm that
   protects it. A first-chunk CORB answer therefore fails OPEN on the commonest shape there is.

   SO THE ANSWER IS A REFUSAL AND NOT A PARTIAL ANSWER. A script-like destination is not streamable, the
   caller is TOLD so before the request is made, and nothing is handed over. It is graded `decline` rather
   than `network` on this file's own discriminator: a real browser streams a script perfectly well, so no
   browser refuses this and there is nothing to relay — only this tool declines, which is what `decline`
   means and is why the flow parks rather than exploring a failure path no server produced. */
function _bodyGated(opts) {
  return _isScriptLike(_destinationOf(opts));
}
function _destinationOf(opts) {
  CHECK(typeof opts.destination === "string" && _DESTINATION_TYPES.indexOf(opts.destination) >= 0,
        "safeFetch was called with a DESTINATION that is not one Fetch §2.2.5 \"Requests\" enumerates: " +
        JSON.stringify(opts.destination) + " — §2.2.5 gives every request one (\"unless stated otherwise it " +
        "is the empty string\") and this file decides the CORB class from it by asking §2.2.5's own " +
        "script-like predicate. A value that predicate does not recognise — absent, misspelled, or invented " +
        "by a compromised renderer — takes the `not script-like` arm, which is how a code load gets fetched " +
        "as data and a cross-origin HTML or JSON body reaches a compiler; state \"\" and mean it, or state " +
        "what the request is for");
  return opts.destination;
}
// ── WHAT A REPLY WOULD BE EVIDENCE OF, AND WHETHER THIS ZONE SPENDS A REQUEST ON IT ──
// CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's three names, spelled here because this
// is the file that DECIDES from them. The engine states the word (solver/engine.h's
// PENDING_PROVENANCE_*, composed at the park from HTML §4.12.1.1 "Processing model"'s
// parser-inserted — a `script` whose parser document is non-null, NOT §4.10.18.3
// "Association of controls and forms"' parser inserted flag, which is a different
// thing on a different element — and the parking flow's `path_forced`); a zone that
// originated an act states it for itself. Neither may decide, and this file may not
// re-derive it — nothing in an ADDRESS distinguishes a page a person's own session
// would have loaded from one that exists because a gate was forced, which is the
// whole reason the field travels.
var _PROVENANCE_TYPES = ["observed", "derived", "forced"];
// AND EVERY CALLER MUST STATE ONE — a `CHECK`, by exactly the argument `_destinationOf`
// carries one function up and for the same failure shape. The arm an unstated value
// falls to is whichever the firing test below happens to be written as its else, and
// with the test written the way it must be ("refuse unless the origin is widened") an
// ABSENT provenance and an INVENTED one both read as `not forced` — the PERMISSIVE arm.
// A fail-open on a decision about whether to spend someone else's server, under the
// person's own session, is what CLAUDE.md's `CHECK` is for; and the value crosses from
// the untrusted engine, whose own splitter asserts the vocabulary with DCHECKs that are
// compiled out on the far side of a mojo boundary. Asserting it HERE closes it for both
// hosts at once, which is why it is not in either host's splitter.
// THERE IS NO `unknown` GRADE. CLAUDE.md §Attacker-sources makes a request whose
// provenance is not established a CRASH AT THE DECISION rather than a load, because
// there is no partition and no interception behind this line: a document load one call
// below carries the person's cookies.
function _provenanceOf(opts) {
  CHECK(typeof opts.provenance === "string" && _PROVENANCE_TYPES.indexOf(opts.provenance) >= 0,
        "safeFetch was called with a PROVENANCE that is none of the three CLAUDE.md " +
        "§A-REQUEST-CARRIES-THE-PROVENANCE declares (solver/engine.h's PENDING_PROVENANCE_*): " +
        JSON.stringify(opts.provenance) + " — this file decides whether to FIRE the request from it, and " +
        "an absent or invented value takes the same arm `derived` does, which is the arm that spends the " +
        "network. State `observed`, `derived` or `forced` and mean it");
  return opts.provenance;
}
/* ── AND THE SAME QUESTION ASKED ONE LEVEL OUT: HOW WAS THE DOCUMENT HOLDING THIS REQUEST ITSELF REACHED ──
   `provenance` is a fact about THIS request. This is a fact about the DOCUMENT it was made from, and the two
   are INDEPENDENT — which is the whole reason this row exists. A page reached by a route only its own bundle
   names goes on making its own `fetch()`es, and the engine grades every one of them `observed`, CORRECTLY,
   because the page really did make them. Nothing in the request says that the document it was made from is
   one nobody observed being reached.
   IT IS A SECOND ROW AND NOT A JOIN INTO `provenance`, and the difference is the person's to see. One word
   carrying both would answer two questions with one bit and settle it by the stricter — CLAUDE.md
   §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS exactly — and the loss lands where it is least visible: a person
   reading a refusal could no longer tell "this tool forced this request" from "the page made this request,
   in a document this tool chose to open". Those are different decisions and a control that cannot spell the
   difference has taken one of them on the person's behalf.
   IT IS THE ZONE'S WORD AND NOT THE ENGINE'S, which is why it is an option rather than something read off
   the pending line. The engine grades a PARK; how the document holding that park was reached is a fact about
   a load THIS ZONE performed, before any instance existed — so the zone that performed it is the only party
   that holds it, exactly as it is the only party that holds the browser-stated principal.
   A `CHECK` FOR `_provenanceOf`'s REASON WORD FOR WORD, and the arm an unstated value falls to is the same
   one: the default arm below is a CONJUNCTION over this signal and `provenance`, so an absent `docReach`
   would make that conjunction unsatisfiable and the request would be refused — which sounds conservative and
   is the wrong failure, because the refusal would name a signal the caller never meant to answer and a
   person would be shown a row holding a request nobody asked about. State the word. */
function _docReachOf(opts) {
  CHECK(typeof opts.docReach === "string" && _PROVENANCE_TYPES.indexOf(opts.docReach) >= 0,
        "safeFetch was called without stating how the DOCUMENT this request was made from was itself " +
        "reached: " + JSON.stringify(opts.docReach) + " — it is none of the three CLAUDE.md " +
        "§A-REQUEST-CARRIES-THE-PROVENANCE declares. This file decides whether to FIRE from it beside the " +
        "request's own grade, and the two are independent: a page this tool chose to open makes its own " +
        "`fetch()`es, which the engine grades `observed` because the page really made them. A caller that " +
        "states only the request's grade is describing half the act");
  return opts.docReach;
}
/* ── AND HOW TWO OF THOSE WORDS COMBINE, DECLARED ONCE BECAUSE BOTH HOSTS COMPOSE ONE ─────────────────────
   A document reached from inside another document is reached under BOTH assumptions: a child navigable of a
   page this tool chose to open is a page this tool chose to open, however the engine graded the navigation
   that created it. So the composition is the WEAKER of the two grades, and it is declared here — in the file
   that declares the vocabulary — rather than in each host, for the reason every other pair in this project
   is one function: two right answers to one question is the shape that drifts, and a host that composed its
   own would be the second copy.
   THE ORDER IS DECLARED AND NOT READ OFF `_PROVENANCE_TYPES`' POSITIONS. An array index names a member only
   while the array is fixed, and this one is a vocabulary somebody may extend — so a rank taken from a
   position would silently re-order the day a fourth word is added between two existing ones. The map is
   explicit and the assert below is what makes an unranked word a crash rather than a `undefined` that
   compares false against every `Math.max`. */
var _REACH_RANK = { observed: 0, derived: 1, forced: 2 };
function safeFetchReachJoin(outer, own) {
  CHECK(_PROVENANCE_TYPES.indexOf(outer) >= 0 && _PROVENANCE_TYPES.indexOf(own) >= 0,
        "two reach grades were composed and one of them is not a word this file declares (" +
        JSON.stringify(outer) + ", " + JSON.stringify(own) + ") — the composition is what stops a child " +
        "document of a page this tool chose to open reading as a page the person navigated to, and a word " +
        "with no rank would compose to `undefined` and be refused by whichever caller stated it");
  CHECK(typeof _REACH_RANK[outer] === "number" && typeof _REACH_RANK[own] === "number",
        "a reach grade this file declares carries no rank — the vocabulary and the ordering are two " +
        "statements and this is where they are made to agree, because a word added to one and not the " +
        "other composes to nothing while every caller reads it as a grade");
  return _REACH_RANK[outer] >= _REACH_RANK[own] ? outer : own;
}
/* …AND THE NARROWER FACT THE ENGINE STATES BESIDE IT — solver/engine.h's `pinned`/`unpinned`, composed from
   solver/flow.h's `path_pinned` at the PARK for a request that parks and at the ACT for one the page's code
   made without parking (`engine_pinned_of_running_path`). It says whether the flow had DETERMINED some
   source's value on an arm nothing observed before it built this address, so that everything the page computed
   afterwards may carry a witness THE ENGINE picked rather than one the document or the server supplied.
   TWO COMPOSITION POINTS AND ONE FACT, WHICH IS WHAT KEEPS THEM FROM BEING TWO RULES: both read
   `flow_path_pinned` of the flow whose path the address was built on, and the only thing that differs is WHEN
   that flow is asked — at the park for a work item, at the act for a request composed and handed over on one
   turn. §scheduler's "an operation that becomes a work item takes its inputs with it" is why neither may be
   re-asked later, and both sites say so at their own line.
   A `CHECK` FOR `_provenanceOf`'s REASON EXACTLY, and it is the sharper of the two: an ABSENT mark and an
   INVENTED one both read as `not pinned`, which is this function's PERMISSIVE arm, and the arm it feeds is the
   one that SPENDS AN ACT. A provenance read wrongly mislabels a reply; this read wrongly fires a request.
   IT IS A MAY-REST-ON AND NEVER A DOES-REST-ON, and a reader deciding from it is entitled to know which:
   concretize-on-pin answers a pinned source's read with a BARE primitive (solver/concolic.c's `pin_mint`), so
   the composed address has forgotten where its bytes came from and NOTHING can ask it. The engine records the
   fact at the one door those bytes enter through instead. So `pinned` means "this path had chosen a witness
   before this request existed", and `unpinned` is the positive statement that it had not — under which every
   byte of the address came from the document, the server, or the bundle's own text. */
/* THE THIRD WORD IS THIS ZONE'S AND NOT THE ENGINE'S, AND IT IS A STATEMENT ABOUT THE ACT RATHER THAN A HOLE.
   `pinned` and `unpinned` are solver/engine.h's; `unstated` is what a reader of an act carrying NO MARK
   composes for itself. Writing `unpinned` there would be a FALSE claim (a flow that pinned a witness has
   `path_forced` set by the nesting, so exactly those acts can be `forced` and carry our bytes) and writing
   `pinned` would be a wrong sentence in the other direction. So the honest third answer is that the act does
   not carry the fact.
   WHICH ACTS CARRY ONE IS A FACT ABOUT THE ENGINE AND MOVES, AND THIS PARAGRAPH USED TO NAME IT AS A FACT
   ABOUT THE PARK LINE — kept in its own words because the reasoning is sound and a reader will re-derive it.
   It read: "`pinned` and `unpinned` are solver/engine.h's and ride the PENDING-FETCH line. `unstated` is what
   an act that does not come off that line says: the engine composes a witness mark at the PARK, and an XHR
   record or a navigation notice is not a park — those carry `engine_provenance_of_running_path`'s word and no
   mark beside it." The PARK half is still true and the "does not come off that line" half is not: an
   `xhr.send` record carries a mark now, composed at the act by `engine_pinned_of_running_path`, which is the
   same fact about the path that is STANDING rather than the one that is parking. So the question is not which
   LINE an act rides but whether the RECORD IT ARRIVES ON CARRIES A MARK, and the residual below names the two
   notices that still do not.
   AND THE PARK TEST IN THE SENTENCE ABOVE IS THE INTUITIVE READING AND IS NOT THE ONE THE CALLERS OBEY,
   WHICH IS WRITTEN DOWN HERE BECAUSE IT HAS NOW COST A READING. Read literally, `an act that does not come
   off that line says unstated` condemns THREE of this file's own callers: `trusted.mjs`'s seed, the peer
   gate's probe and the automatic discovery sweep all compose their request in the TRUSTED ZONE, park
   nothing, and state `unpinned`. They are RIGHT and this paragraph's headline was imprecise, and its own
   parenthesis one line up already gives the real discriminator without ever stating it as the rule:
   CAN THIS ACT'S PROVENANCE BE `forced`. The witness mark is strictly nested inside `path_forced`, so an act
   whose provenance is a LITERAL `observed` or `derived` cannot be carrying a witness this engine chose, and
   `unpinned` is then a true positive statement rather than a claim about a line the act never rode; an act
   whose provenance is a VARIABLE, or the literal `forced`, may be exactly such an act, and its zone cannot
   say. `bridge.js`'s navigation relay states that rule in its own words at its own site — `unstated`
   BECAUSE THIS PROVENANCE IS A VARIABLE — which is the consumer being right where the declaration was
   loose. MEASURED over every hand-stated mark in this tree, with no exception: every site that states
   `unpinned` states a literal non-forced provenance beside it, and every one that states `unstated` states a
   provenance that is a variable or is `forced` outright.
   THE DERIVATION AND NOT THE FIGURES, BECAUSE THE FIGURES THAT STOOD HERE WENT WRONG IN BOTH WAYS A COUNT
   CAN. `git grep -nE "pinned[[:space:]]*:[[:space:]]*['\"]" -- .` is the population — hand-stated marks, both
   quote styles, tree-wide — and a mark this file RELAYS off a record is not one of them, which is why the
   count falls every time a seam stops guessing. It read "ten sites ... the four that state `unpinned`": the
   ten was exact and the four was THREE, a subtotal contradicting its own list in a sentence whose author had
   both halves in front of them, which is CLAUDE.md §AND-WHERE-A-SENTENCE-CARRIES-BOTH-A-COUNT-AND-THE-LIST
   with the enumeration one command away. And the ten went stale by the very landing this paragraph records,
   since the XHR relay left the population the day it began relaying. A derivation cannot do either.
   THE READING IT COSTS IS THE EXPENSIVE ONE AND IS WHY THIS IS A CORRECTION RATHER THAN A TIDY-UP: a reader
   who takes the park test concludes the discovery sweep's `unpinned` is a FALSE claim to be repaired, and
   repairing it would move a true statement to the word for an act that does not carry the fact — after
   which the sweep and the analysed page's own `fetch()`, which are identical on every gating signal today,
   would appear to be separable by the `witness` row and an egress arm could be built on the difference.
   IT IS NOT A DEFAULT AND MUST NOT BECOME ONE: an ABSENT `opts.pinned` still aborts. `unstated` has to be
   TYPED by a caller that means it, which is what makes it greppable, countable, and retirable.
   NAMED RESIDUAL — TWO SEAMS STILL STATE IT, AND THEY ARE NOT THE TWO THIS RESIDUAL WAS WRITTEN ABOUT. The
   XHR half is BUILT: `engine_pinned_of_running_path` (solver/engine.c) stands beside
   `engine_prov_of_running_path`, `xml_http_request.c`'s request op takes both on one line and writes a
   `pinned` key, and `bridge.js`'s `xhr.send` relay passes what it finds there. What is not covered: the
   NAVIGATION load (core/frame/navigable.c's three notices) and the ROUTE DECLARATION (solver/route_seed.c)
   reach this function with a provenance that may be `forced` and no witness mark, so what the policy is told
   about those two acts is still that the fact does not travel. What the next diff builds: the same mark
   written into those four notices and relayed by their readers here — `bridge.js`'s navigation relay and its
   route-declaration hypothetical, which are the two sites left stating a literal `unstated` off an engine
   word. How its absence would show, as an OBSERVATION and never as an instance: a person auditing what they
   permitted at an origin finds `witness=unstated` on the DOCUMENT rows of a run whose `fetch()` and XHR rows
   at the same host carry `pinned` or `unpinned`, so the two seams that could state the fact are the two that
   do not.
   AND THE CLAUSE THIS RESIDUAL USED TO CARRY IS RETIRED BY THE DIFF THAT MADE THE WITNESS A SIGNAL, WHICH IS
   WHY IT IS REWRITTEN HERE RATHER THAN LEFT STANDING. It read "It changes no firing outcome at any setting of
   this table — both words refuse and the same widening reopens both". That was true while one per-origin
   switch reopened every arm at once. It is FALSE now: `witness` is a gating signal, so `unstated` and
   `unpinned` are two rows a person permits separately, and an origin permitted for one and not the other
   fires the discovery sweep (which states `unpinned`) while refusing the XHR relay (which states `unstated`)
   — a difference in OUTCOME and not only in the sentence. The residual is therefore larger than it was, not
   smaller, and the direction is the safe one: an act that cannot state the fact needs its own permission.
   AND THAT PARAGRAPH'S OWN WORKED EXAMPLE IS THE THING THIS LANDING SPENT, WHICH IS SAID HERE RATHER THAN BY
   EDITING IT BECAUSE ITS ARGUMENT IS UNTOUCHED AND ONLY ITS INSTANCE MOVED. "Refusing the XHR relay (which
   states `unstated`)" was the difference-in-outcome it named, and the XHR relay states a real mark now — so
   the example is spent while the rule it illustrates stands exactly: `unstated` and `unpinned` are still two
   rows a person permits separately, and an origin permitted for one and not the other still fires the
   discovery sweep while refusing everything that cannot state the fact. What is left to illustrate it is the
   two seams named in the residual above, and they are DOCUMENT acts — which is why the paragraph below prices
   the remainder as a sentence rather than as an outcome, and why "larger than it was" was true of the
   residual as that diff left it and is no longer true of the residual as this one does.
   AND THAT PRICE IS REFUTED BY ASKING THIS FILE ITSELF, WHICH IS WHY THE CLAUSE IS REWRITTEN RATHER THAN
   DELETED: ITS PREMISE IS ONE A READER RE-DERIVES FROM THE WORD `document` AND IT IS FALSE. It read: "AND THE
   TWO REMAINING SEAMS ARE A SPECIFICITY RESIDUAL AND NOT A FIRING ONE … Both are `document` acts, and
   `document` is not in this row's `value` bucket at all, so no setting of this table fires either one on the
   strength of a witness mark — what they cost is the sentence a person reads". `document` IS in the `value`
   bucket: `_isScriptLike` answers false for it and `_isDocumentSubresource` does not list it, so the
   `destination` row above computes `value` — which is the first conjunct of the OWNER'S arm at the end of
   `_DEFAULT_ARMS`, and a CHILD NAVIGABLE already satisfies its third: `bridge.js`'s loader takes the actor
   word from its caller, and the two callers that are the page's own doing — a child navigable and a
   route an application declared of itself — state `page`, while the ambient seed states `tool`. The
   witness mark is the ONLY conjunct refusing the child navigable.
   MEASURED THROUGH THIS FILE'S OWN WALK, AS A COMMAND AND NOT A FIGURE, because a row here moves with the
   table: `node testing/egress_arm_probe.mjs` loads this file verbatim into a realm, states the EMPTY table a
   host states, and asks `safeFetchFiringRefusal` with the navigation relay's own vector — which answers
   REFUSED on `witness=unstated` and FIRES with that one field changed to `unpinned`, everything else held,
   with a `actor: tool` control refusing and a program-load control firing in the same run.
   SO THE REMAINDER IS A FIRING RESIDUAL AND THE UNDER-CLAIM WAS THE WHOLE OF ITS COST. CLAUDE.md
   §AN-UNDER-CLAIM-IS-NOT-FOUND-BY-ACTING-ON-IT is exact about why it stood: a clause saying a seam cannot
   change an outcome tells its reader there is nothing here to look at, so nobody looks, and the lever stays
   unbuilt with no later moment at which the claim is contradicted. The fact is COMPUTABLE today —
   `engine_pinned_of_running_path()` stands beside the provenance function the three notices already call.
   WHAT IT IS NOT IS A DIFF ANYBODY MAY LAND ON THIS ARGUMENT ALONE, AND THE DECISION IT WAS WAITING ON IS
   MADE — WHICH MOVES THE PRICE BACK TO A SENTENCE, BY A ROUTE THAT IS NOT THE ONE THIS PARAGRAPH RETIRED
   TWO CLAUSES UP. It read: the arm's own enumeration names the navigation as NOT admitted, by name, so
   writing the true mark would reach past that stated population, and what is owed is a DECISION about
   whether a child navigable the page's own markup names is the page loading itself for the purposes of
   that arm — which CLAUDE.md §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS answers YES for a SUBRESOURCE
   and is silent about for a NAVIGATION. Every clause of that was true and the silence is gone: the project
   owner answered YES for a NAVIGATION too, and `_DEFAULT_ARMS`' last arm is where it is written.
   AND IT IS A DIFFERENT ARM, WHICH IS THE WHOLE OF WHY THE WITNESS MARK IS NO LONGER A FIRING QUESTION. The
   decision did NOT widen the owner's data arm to admit a document; it added an arm keyed on
   `destination=navigation` AND `actor=page` AND `provenance=derived`, which names no witness row — so
   the child navigable this residual is about is admitted by facts that already travel, and the mark landing
   changes no outcome for it at any setting. The earlier clause that priced this as a sentence was REFUTED
   (`document` IS in the `value` bucket, so the data arm really was one field away); this one reaches the
   same price by the opposite route, and the distinction matters because a reader who re-derives the old
   reason will re-derive the old refutation with it.
   SO WHAT IS LEFT IS SPECIFICITY AND IT IS STILL WORTH BUILDING: a person auditing an origin sees
   `witness=unstated` on the DOCUMENT rows of a run whose `fetch()` and XHR rows at the same host carry a
   real mark, and `unstated` is a row they permit SEPARATELY from `unpinned` — so the two seams that
   cannot state the fact are the two whose permission cannot be expressed in the same terms as everything
   else at that origin. The direction is still the safe one; what changed is that the work is a sentence
   again, and this time because no arm reads the row rather than because `document` was thought to be
   outside the bucket. */
var _PINNED_MARKS = ["pinned", "unpinned", "unstated"];
function _pinnedOf(opts) {
  CHECK(_PINNED_MARKS.indexOf(opts.pinned) >= 0,
        "safeFetch was called with a PINNED mark that is none of the three this file declares: " +
        JSON.stringify(opts.pinned) + " — this file decides whether to SPEND AN ACT on an address from it, " +
        "and an absent or invented value would take the permissive arm exactly as a missing provenance " +
        "would. State `pinned` or `unpinned` from the engine's pending line, or `unstated` if this act does " +
        "not carry one");
  return opts.pinned;
}
/* ── WHOSE ACT THIS REQUEST IS: THE ANALYSED PAGE'S, OR THIS TOOL'S ──────────────────────────────────────
   EVERY OTHER ROW IN THE REGISTRY DESCRIBES THE REQUEST AND THIS ONE DESCRIBES THE CALLER, WHICH IS WHY NO
   COMPOSITION OF THE OTHERS CAN STAND IN FOR IT. Measured, and it is what this fact exists for: the analysed
   page's own `fetch()` and this tool's AUTOMATIC discovery sweep carry the SAME `destination`, the SAME
   `provenance` (both `derived`), the SAME `doc-reach` (both `observed`), the SAME `witness` (both `unpinned`)
   and the SAME `cookies`. They are identical on every signal that describes the request, because the thing
   that differs about them is not a property of the request at all — one was composed by the page's code and
   the other by `lib/discovery-probe.js`. CLAUDE.md §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS's discriminator is
   WHOSE ACT THE REQUEST IS, and until this row existed nothing here could ask it.
   IT IS A STACK PARAMETER AND NEVER AN INFERENCE, WHICH IS CLAUDE.md §AND-AN-EXEMPTION-SCOPED-BY-WHO-ACTED
   VERBATIM: a fact about who acted is a VALUE stated by the site that knows, carried down every frame and
   asserted where it is relied on, never re-derived and never inferred from a sentence about who the callers
   are. That rule is written from an incident in this project where a privilege was scoped by exactly such a
   sentence and was false of three callers for as long as it stood. So the word is TYPED at each asker and
   `CHECK`ed here, and an ABSENT one ABORTS rather than taking an arm — forgetting may not be a way to be
   exempted, and there is no third word for "this act does not carry the fact" because every asker knows.
   THE VALUES ARE THIS SIGNAL'S OWN AND ARE DELIBERATELY NOT `lib/schema.js`'s `PAGE_CONTEXT_*`. That pair
   answers HUMAN-or-TOOL and gates the operator relay; this one answers ANALYSED-PAGE-or-THIS-TOOL. A page's
   own `fetch()` is NEITHER of those words — no human initiated it and this tool did not compose it — so
   reusing them would be one vocabulary serving two questions, which is the defect
   §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS names and which this row exists because of. The two axes are also
   independent in both directions: the operator's Fetch-Discovery button is `user-initiated` AND this tool's
   act, and the automatic sweep is `tool-initiated` AND this tool's act.
   IT IS THE ONE GATING SIGNAL THE UNTRUSTED ZONE CANNOT REACH, which is why its grade is `certain` where
   `provenance`, `doc-reach` and `witness` are only `stated`. Those three are words the ENGINE composes and
   this zone relays, and the engine is attacker-controlled (SECURITY.md §The QuickJS/WASM sandbox); this one
   is a literal in trusted-zone source at every site, so no bundle and no compromised renderer can move it.
   A REQUEST THIS TOOL COMPOSED AT A PERSON'S DIRECTION IS STILL `tool`, AND THAT IS A DECISION RATHER THAN
   AN OVERSIGHT. `trusted.mjs`'s seed is an address a person typed on a command line and `bridge.js`'s
   frontier-residue re-fetch is this tool re-opening a document it parked — neither is composed by any
   analysed page's code, so `page` would be a false statement about both. What their human authorization is
   worth is a REAL fact and it is carried on the axis that asks it, not folded into this one; and neither
   site's outcome moves today, so the value is chosen for CORRECTNESS rather than for effect, which is
   exactly the kind that gets defaulted because nothing tests it. */
var _ACTOR_WORDS = ["page", "tool"];
function _actorOf(opts) {
  CHECK(_ACTOR_WORDS.indexOf(opts.actor) >= 0,
        "safeFetch was called with an ACTOR that is neither word this file declares: " +
        JSON.stringify(opts.actor) + " — it says whether the ANALYSED PAGE'S own code composed this request " +
        "or THIS TOOL did, which is the fact that separates a page's own `fetch()` from a probe this tool " +
        "built, and no other signal here can: those two arrive identical on destination, provenance, " +
        "doc-reach, witness and cookies. Every caller knows which it is by construction — a relay answering " +
        "the engine's own record states `page`, and a composer that built the address itself states `tool` " +
        "— so there is no third word and an absent one is a site that never answered the question rather " +
        "than an act that cannot");
  return opts.actor;
}
/* ── AND WHAT THIS FUNCTION DOES *NOT* READ, WHICH IS THE HALF NO ASSERT WAS MAKING ──────────────────────
   The two rules above refuse a VALUE this file cannot serve. This one refuses a FIELD it will not read, and
   the failure it closes is the opposite shape: not a bad answer to a question this file asks, but a caller's
   answer to a question this file never asks — DROPPED IN SILENCE, and read by the caller as carried.
   THE OPTION THAT MATTERS IS `method`, AND THE REASON IT MATTERS IS REWRITTEN HERE RATHER THAN DELETED
   BECAUSE IT IS THE REASON A READER RE-DERIVES. It used to read: `method` "IS THE MIDDLE CONJUNCT OF THE ONE
   COMBINATION CLAUDE.md SAYS IS NEVER A SETTING: credentialed AND state-mutating AND forced", and this rule
   made that conjunct false at every setting of the widening table. The project owner has RETIRED that
   absolute (CLAUDE.md §AND-THAT-ABSOLUTE-IS-RETIRED-BY-THE-PROJECT-OWNER); nothing is refused at every
   setting now, and what replaces it is the per-signal control below.
   WHAT SURVIVES IS SMALLER AND IS STILL THE REASON THIS RULE EXISTS: this file can issue ONE VERB — `init`
   hardcodes `method: "GET"` and nothing below reads `opts.method` or `opts.body` — so the `method` SIGNAL
   has exactly one value here, and a caller that states another is describing a request this transport cannot
   perform. That is a FACT ABOUT THE TRANSPORT and not a combination no setting may reach, and the difference
   is checkable: delete the whole widening table and the GET literal is still here, because it selects
   against nothing. What was missing is that the argument was enforced by a GREP, and
   SECURITY.md said so in as many words ("GET only, enforced by ABSENCE … grep: no occurrence"). Absence
   stops a method being SENT; it says NOTHING to the caller that believes it sent one. So a call site that
   passed a verb was not refused, it was IGNORED, and the reply to a GET came back attributed to its POST.
   THAT SUBSTITUTION IS NOT HYPOTHETICAL HERE — this project has paid for it once, on the XHR path, where
   `xhr.open("POST", u)` was answered with the reply to a GET of `u`, and every @H example value and every
   @S verdict downstream was derived from a response the server never gave for that request. A wrong answer
   is worse than an absent one, and this is the shape that produces one without anybody writing a bug.
   IT SUPERSEDES THE `as` DCHECK RATHER THAN STANDING BESIDE IT. That assert was this same rule written for
   ONE dead keyword — a load-type word whose silent drop fetched a code load as data — and a rule that has to
   be re-written per option has a hole for every option nobody thought of. `as` is not in the set below, so
   the general rule refuses it and the special case is deleted rather than kept as a second copy.
   A CLOSED SET IS A RESTATEMENT OF WHAT THE BODY READS, AND THE DRIFT DIRECTION IS WHY THAT IS SAFE HERE:
   an option added to the body and forgotten in this list aborts on its AUTHOR's own first call, at the line
   they just wrote — while the failure it closes is silent and belongs to somebody else, later. */
var _SAFEFETCH_OPTIONS = ["pageUrl", "pageOrigin", "destination", "provenance", "pinned", "docReach",
                          "actor", "credentialed", "credentials", "headers", "signal", "onChunk"];
/* FETCH §2.2.5 "Requests"' CREDENTIALS MODE — "which is `omit`, `same-origin`, or `include`" — and these are
   the same three words `core/fetch/fetch.h`'s `fetch_credentials_token` puts on the pending line, which is
   the only place they are spelled on the engine side. Written here rather than derived because this zone is
   the other party to that contract and a contract is checked by both. */
var _CREDENTIALS_MODES = ["omit", "same-origin", "include"];
/* DCHECK AND NOT CHECK, ON THE DISCRIMINATOR THIS FILE ALREADY STATES FOR `opts.pageUrl`: release must still
   be able to PROCEED, and it can. With this compiled out every unread option is dropped exactly as it is
   dropped today, and every one of them lands on the safe side — a dropped `method` fires the GET this file
   was always going to fire, a dropped `credentials` fires uncredentialed, a dropped `pageOrigin` is
   same-origin with nothing and takes strict CORB. Nothing here is a boundary failing open; what is lost is
   the caller's belief about what it sent, which is an epistemic harm no release build could fix. The two
   options whose bad VALUES do fail open — `destination` and `provenance` — are `CHECK`s of their own above.
   AN ASSERT STANDS HERE AT ALL BECAUSE THE KEYS ARE OURS AND ARE NEVER ON THE WIRE. Every call site composes
   an object LITERAL in trusted-zone source; the untrusted engine supplies VALUES — `headers` on the XHR path,
   and the destination and provenance tokens — and never a KEY, so no bundle and no compromised renderer can
   reach this abort. That is the same line SECURITY.md draws when it refuses to DCHECK a page-suggested
   address: the discriminator is who determines the value, and here it is this zone in every frame. */
function _refuseUnreadOptions(opts) {
  for (var k in opts) {
    if (!Object.prototype.hasOwnProperty.call(opts, k)) continue;
    DCHECK(_SAFEFETCH_OPTIONS.indexOf(k) >= 0,
           "safeFetch was passed the option `" + k + "`, which it does not read — so it would be DROPPED IN " +
           "SILENCE and the request would not carry what its caller said about it. `method` and `body` are " +
           "the ones that matter: this file hardcodes `method:\"GET\"`, which is RFC 9110 §9.2.1 \"Safe " +
           "Methods\"' safe set enforced structurally — this transport performs ONE verb, which is what " +
           "the `method` signal of the egress policy states — so a caller that states a verb gets the reply to a GET " +
           "attributed to its own, which is the substitution this project already paid for once on the XHR " +
           "path. `as` is refused by this same rule and no longer by one of its own: the CORB class is the " +
           "request's DESTINATION now (Fetch §2.2.5 \"Requests\"). State only what this file reads (" +
           _SAFEFETCH_OPTIONS.join(", ") + "); if the verb is not GET the request is refused AT THE CALL " +
           "SITE, and `safeFetchMethodRefusal` is that answer in this file's own refusal vocabulary");
  }
}
/* ── WHETHER THE PERSON'S SESSION PAYS FOR THIS REQUEST — AND THE ONE OPTION THAT MAY NOT RIDE ALONG ──────
   `credentialed` is this zone's own literal at every call site; nothing on the wire states it, and the
   engine cannot ask for it. What it decides is whether the browser attaches the person's cookies, so it is
   the conjunct every other rule in this file is scoped BY: the destructive-path deny list runs only when it
   is true, and the credentialed SOP below exists only for the replies it produces.
   `opts.headers` IS THE ONE OPTION THIS FILE READS WHOSE VALUES COME FROM THE UNTRUSTED ZONE. The XHR path
   forwards the analysed BUNDLE's own header list; a browser strips the forbidden names, and on an
   uncredentialed GET to a public host that is within the model — the bundle is choosing headers on a request
   it already gets to make for itself, which confers nothing.
   THE TWO TOGETHER ARE A DIFFERENT QUESTION, AND IT WAS ANSWERED BY A SENTENCE ABOUT WHO THE CALLERS ARE.
   SECURITY.md §Network stated the scope in as many words — the header path "stays within it only because
   credentialed mode is off: a bundle-chosen header list on a cookie-bearing request is a different question
   and must be re-decided when that lands" — and that premise had stopped being true: the same file records
   that credentialed mode now has a caller (the document load). That bullet carries the correction rather
   than the claim now, so this comment is quoting what it RETIRED. What actually kept the combination from
   arising was that the callers passing a header list and the callers asking for cookies are disjoint
   SETS OF FUNCTIONS, which is a fact no site can see and no diff has to preserve. CLAUDE.md names that shape
   exactly — an invariant whose subject is absent from the site that relies on it — and says the cure is
   always the same: the fact is asserted where it is relied on rather than described somewhere else.
   WHY IT IS WORTH CLOSING RATHER THAN RESTATING. RFC 9110 §9.2.1 "Safe Methods" is enforced here
   STRUCTURALLY — the verb is a literal and `_refuseUnreadOptions` makes a caller-stated one impossible to
   write — which is what makes the `method` signal of the egress policy a fact with one value rather than a
   row a person could change. A header list is the OTHER route to a
   verb: `X-Http-Method-Override` is a convention this project's own code sends (`lib/discovery.js` calls it
   "the documented trick"), and a server that honours it reads a GET as whatever the header names. So a
   bundle-chosen header list on a cookie-bearing request puts a verb back one layer up, past the
   one place this file closed it. That is not a bug anybody has written; it is a bug this file would not
   refuse if somebody did.
   CHECK AND NOT DCHECK, on this file's own discriminator: release must be able to PROCEED correctly with the
   assert compiled out, and here it cannot — the combination would go out, with cookies, carrying header
   values from the zone this one does not trust. That is the fail-open direction on a security boundary,
   which is what `_destinationOf` and `_provenanceOf` are CHECKs for one and two functions up.
   AND IT MAY ASSERT AT ALL FOR THE REASON `_refuseUnreadOptions` GIVES: the KEYS are composed in trusted-zone
   source at every call site — the untrusted engine supplies header VALUES and never the decision to ask for
   cookies — so no bundle and no compromised renderer can reach this abort.
   WHAT THE NEXT DIFF BUILDS IF THIS REFUSES SOMEBODY. Not an exemption for the caller: a statement of WHOSE
   header list it is. The bundle's and this zone's own analyzer-probe list are two populations that arrive
   through one parameter, and the credentialed question is answerable for one of them and not the other —
   so what is missing is that distinction, carried from the site that knows, exactly as `provenance` and the
   page-context relay's initiator grade already are. Its absence shows as this abort and nowhere else.
   AND THE OPENING SENTENCE OF THIS BLOCK IS ABOUT `credentialed`, NOT ABOUT `credentials`, WHICH THE WIRE
   DOES STATE — the two are one letter apart on purpose because they are one decision made of two facts.
   `credentials` is Fetch §2.2.5 "Requests"' credentials mode, named by the algorithm that CREATED the
   request and carried across the seam on the pending line beside its method and its destination. It can
   only ever NARROW what this zone was already willing to do; the derivation of the pair is below. */
function _credentialedOf(opts) {
  /* THE FLAG IS STATED AND NOT COERCED, AND THIS IS A `DCHECK` FOR THIS FILE'S OWN DISCRIMINATOR: WHICH ARM
     A COMPILED-OUT ASSERT LEAVES. `!!undefined` is `false`, so the arm left here is the UNCREDENTIALED one —
     release proceeds correctly, nobody's session is spent, and what is lost is only the caller's belief
     about what it sent. That is `_refuseUnreadOptions`' position one function up, where a dropped
     `credentials` fires uncredentialed and is a `DCHECK` for the same reason. The two `CHECK`s below are
     fatal on the opposite ground: the arm THEY leave SENDS the cookies.
     WHY ASSERT AT ALL WHEN THE DEFAULT IS THE SAFE ONE. An absent key and a stated `false` were ONE fact at
     this door and TWO facts at the composing site, and the composing site is where the question is asked —
     so the sites that omitted it were exactly the sites that state a HEADER list (`bridge.js`'s
     `fetchedXhr`, `discovery-probe.js`'s `_chokepointGetFn`). That is the disjointness SECURITY.md already
     records as "a fact no site can see, that no diff has to preserve", holding by coincidence of which
     functions exist. Stating it makes the `credentialed && headers` CHECK below a test of what a caller
     SAYS rather than of which callers happen to have been written.
     IT MAY ASSERT AT ALL FOR `_refuseUnreadOptions`' REASON: the KEY is composed in trusted-zone source at
     every call site this door has — `bridge.js`, `engine/trusted.mjs`, `engine/peergate.mjs`,
     `lib/discovery-probe.js` — and the untrusted engine supplies only VALUES, never a key, so no bundle and
     no compromised renderer can reach this abort. */
  DCHECK(typeof opts.credentialed === "boolean",
         "safeFetch was called without stating whether the PERSON'S SESSION pays for this request. The " +
         "coercion below reads an absent flag as `false`, which is the safe arm and is why this is not " +
         "fatal — but absent and `false` are two different facts AT THE CALL SITE, and only one of them is " +
         "a decision somebody took. State `credentialed: false` (this zone is unwilling, or supplies no " +
         "`pageOrigin` for the credentialed SOP to match), or `credentialed: true` beside a " +
         "Fetch §2.2.5 \"Requests\" credentials mode. A caller that states neither hands the egress " +
         "surface a `cookies=no` row with no fact under it, which is the reading a person widens an " +
         "origin on");
  var credentialed = !!opts.credentialed;
  var mode = opts.credentials;
  CHECK(!(credentialed && opts.headers),
        "safeFetch was asked for a CREDENTIALED request that also states a header list — these are two " +
        "populations arriving through one parameter and only one of them has been decided. The header list " +
        "on the XHR path is the analysed BUNDLE's own, which is within the model while the request is " +
        "uncredentialed (a header on a request the page can already make itself confers nothing) and is a " +
        "different question the moment the person's cookies pay for it: this file enforces RFC 9110 §9.2.1 " +
        "\"Safe Methods\" by making the verb a literal, and a header list is the other route to a verb — " +
        "`X-Http-Method-Override` is a convention this project's own code sends, and a server honouring it " +
        "reads this GET as whatever the header names, which puts a state-mutating verb back on a " +
        "cookie-bearing request past the place this file closed it. State " +
        "whose header list it is at the site that knows, the way `provenance` is stated, or send it " +
        "uncredentialed");
  /* AND THE SECOND INPUT, WHICH IS THE REQUEST'S OWN AND NOT THIS ZONE'S. `credentialed` is a decision — this
     zone's willingness to spend the person's session on this fetch. `credentials` is a STATEMENT — Fetch
     §2.2.5 "Requests"' credentials mode, named by the algorithm that CREATED the request and carried across
     the seam on the pending line. They are two questions and CLAUDE.md forbids one bit answering both: the
     zone cannot know what §2.5.1 or §2.5.4 says an `<img>`'s or a `<script src>`'s mode is, and the engine
     cannot know whether this person wants their session spent here. So both are stated and the answer is
     their conjunction.
     THE STATEMENT CAN ONLY EVER NARROW, WHICH IS WHAT MAKES ADDING IT SAFE. There is no arm below on which a
     stated mode turns credentials ON where this zone had not already said yes — an engine that could do that
     would be holding network policy, which SECURITY.md puts here and nowhere else. What it CAN do is refuse:
     a park whose own algorithm said `omit` is never credentialed however willing this zone is.
     A MODE IS VALIDATED WHENEVER IT IS STATED AND IS REQUIRED WHENEVER THE SESSION PAYS, which are two rules
     because they close two different holes. An unknown word is a producer drift and is fatal at any setting —
     `_isScriptLike`'s lesson one field over, where an invented token and an absent one took the identical
     permissive arm. An ABSENT one is a caller that named no creating algorithm, which is honest for a request
     this zone composed itself (a probe, a gate's own load) and is exactly what may not be true of a
     cookie-bearing one: this zone may not spend somebody's session on a request whose creating algorithm never
     said whether it should.
     WHAT IS NOT COVERED: `same-origin` when this zone is willing. §2.2.5 makes that a CONDITIONAL answer —
     credentials only where the request is same-origin — and this file already answers that comparison, but
     AFTER the wire, over the post-redirect origin (`_resourceSameOrigin`), which is the wrong side of the
     fetch to decide from. WHAT THE NEXT DIFF BUILDS: the same-origin test lifted to the request's own parsed
     URL against `pageOrigin` and read here, with the post-fetch gate keeping the redirect case it already
     owns. HOW ITS ABSENCE SHOWS: it cannot show today and that is why it crashes rather than guessing — every
     caller in this tree that is willing is a §7.4 navigation stating `include`, so this arm is unreachable
     by construction and the day a caller reaches it is the day the answer must be built rather than defaulted
     to the logged-out body in silence. */
  CHECK(mode === undefined || _CREDENTIALS_MODES.indexOf(mode) >= 0,
        "safeFetch was called with a CREDENTIALS MODE that is none of Fetch §2.2.5 \"Requests\"' three: " +
        JSON.stringify(mode) + " — the engine spells them in exactly one place " +
        "(core/fetch/fetch.h's `fetch_credentials_token`, which is what writes this field onto the pending " +
        "line), so a fourth word is that spelling and this one having parted, and it would be answered here " +
        "by whichever arm happens to be last");
  CHECK(!credentialed || mode !== undefined,
        "safeFetch was asked for a CREDENTIALED request that states no Fetch §2.2.5 credentials mode — this " +
        "zone owns whether the person's session is spent and the algorithm that CREATED the request owns " +
        "what the request IS, and neither answers for the other. Spending a session on a request whose " +
        "creating algorithm never said whether it should is the credential question decided by silence, " +
        "which is the whole reason the mode crosses the seam. State it (`include` is HTML §7.4.5 " +
        "\"Populating a session history entry\"'s answer for a navigation), or send it uncredentialed");
  if (!credentialed) return false;
  /* §2.2.5's own arms, in its own order. `omit` is the narrowing this whole parameter exists to be able to
     make; `include` is the one every willing caller in this tree states. */
  if (mode === "omit") return false;
  if (mode === "include") return true;
  CHECK_FAIL("safeFetch was asked for a CREDENTIALED request whose Fetch §2.2.5 credentials mode is " +
             "`same-origin`, and that is a CONDITIONAL answer this file does not yet resolve BEFORE the " +
             "wire — see the paragraph above. It is fatal rather than narrowed to `omit` because a silent " +
             "narrowing here hands a same-origin document load the LOGGED-OUT body with nothing anywhere " +
             "saying so, which is the moat this tool exists to cross. Build the pre-request same-origin " +
             "test against `pageOrigin`; every willing caller today is a §7.4 navigation stating `include`");
}
/* ─── THE SIGNALS THE EGRESS POLICY SURFACES, AND THE PERSON WHO DECIDES FROM THEM ───────────────────────
   CLAUDE.md §AND-THAT-ABSOLUTE-IS-RETIRED-BY-THE-PROJECT-OWNER: "METHOD, CREDENTIAL STATE, PROVENANCE,
   URL-CARRIED AUTHORITY, CREDENTIAL LINEAGE, DESTINATION AND INTENDED INVALIDITY ARE FACTS THE POLICY
   SURFACES, AND THE PERSON DECIDES WHICH COMBINATIONS THEIR ORIGIN ALLOWS." The control is PER-SIGNAL and
   PER-ORIGIN and not a score, "because a collapsed number is the several-states-behind-one-answer shape
   arriving where a person has to act on it — a wrong weighting inside a score is invisible, and a named
   signal a person allowed is a decision they can revisit".
   THE RULE THIS REPLACED, REWRITTEN RATHER THAN DELETED BECAUSE IT IS THE ONE A READER RE-DERIVES. It was a
   CONJUNCTION — credentialed AND state-mutating AND forced is never a setting — from which it followed that
   STRIPPING THE CREDENTIAL ended it. The arithmetic was sound; the clause it carried was false, and no
   amount of care about the method could have reached the reason: STRIPPING THE COOKIE DOES NOT MAKE A
   REQUEST UNCORRELATED WITH THE PERSON. A CAPABILITY URL carries its authority in the path or the query — a
   presigned object-store address, a password-reset or invite token, a signed webhook — so an uncredentialed
   request to one mutates state perfectly well and there is no cookie to strip. An address DERIVED from a
   credentialed reply names that person's account whatever headers it carries, because the identifiers came
   out of their session. And the virtual document may itself have been created WITH cookies, so everything
   computed downstream of it is downstream of that session. Two halves of the same framing go with it and
   both are spec-true: GET DOES NOT MEAN SAFE, since RFC 9110 §9.2.1 "Safe Methods" defines safety by what
   the CLIENT requests and expects and says in its own text that an implementation MAY cause side effects —
   so method safety is a signal about INTENT and never a guarantee about CONSEQUENCE; and POST DOES NOT MEAN
   UNSAFE WHERE THE REQUEST IS DELIBERATELY INVALID, since a probe built to fail validation never reaches
   business logic, which is the whole technique.
   NOTHING IS REFUSED AT EVERY SETTING, WHICH MOVES THE HARD PART RATHER THAN REMOVING IT. With no
   combination refused, the only thing between a person and a combination they did not intend is whether the
   control STATED THE CONSEQUENCE — so a signal is surfaced only where this file can COMPUTE it, and one it
   cannot determine renders as a POSITIVE STATED UNKNOWN rather than as an absence a default fills. That is
   §A-FIELD-A-CONSUMER-DEFAULTS arriving at the one boundary where the CONSUMER IS THE PERSON, and it is the
   worst place for it: a plausible datum there is not a wrong number in a report, it is a decision made on a
   fact nobody established.
   WHAT THIS FILE DOES NOT DO AND MUST NOT BE READ AS DOING. It still cannot issue a verb: `init` hardcodes
   `method:"GET"` and `_refuseUnreadOptions` makes a caller-stated one impossible to write. That is NOT the
   retired carve-out re-introduced — it is a statement about what this transport IS, and the retired rule was
   a statement about what no SETTING may permit. The difference is checkable: delete the widening table
   entirely and the GET literal is still there, because it is not selecting against anything. A caller whose
   request is not a GET is told so (`safeFetchMethodRefusal`) rather than having its verb dropped, and
   teaching this chokepoint a verb is a subproblem with its own diff, not a value of this table.
   IT LIVES IN THIS FILE BECAUSE THE DECISION DOES. A registry in a host is a registry the OTHER host does
   not read, and two zones answering one question is the shape this whole parameter exists to end.
   `engine/trusted.mjs` loads this file verbatim into a realm of its own and its `--explore <origin>` writes
   HERE; the offscreen loads it in `ast-worker.html`, restores what the person granted in an earlier session
   from its own store, and reads the same table.
   WHAT WIDENING OBLIGES, STATED WHERE IT IS GRANTED. §@H makes the reply to a forced request evidence about
   what a server says to a request NO CLIENT MAKES, so its values must be carried as FORCED and never merged
   into the observed pool — a 401 body parses as JSON and yields fields that exist nowhere, and one invented
   field is the example that shapes the next endpoint. That carrying is the engine's and it is the subproblem
   AFTER this one; until it exists, a permission buys the request and the person takes on the reply. */
/* HOW RELIABLE EACH SIGNAL IS, DECLARED BESIDE IT, BECAUSE GRADING FACTS OF DIFFERENT RELIABILITY AS ONE
   LIST IS §THE-MIRROR-OF-THAT-IS-A-LIST-OF-EXAMPLES PERFORMED ON A CONTROL SURFACE. A person ticking a row
   is entitled to know whether they are permitting something this zone KNOWS, something another zone SAID, or
   something nobody has established — and the three take different amounts of trust.
     "certain"      — THIS zone determines the value out of its own literals and its own code. It cannot be
                      wrong unless this file is.
     "stated"       — ANOTHER zone determined it and this file asserts only the VOCABULARY. The `CHECK`s
                      above catch a word outside the enumeration and can never catch a wrong word inside it,
                      so a `stated` signal is a CLAIM this zone relays and not a fact it holds.
     "partial"      — this file computes a LOWER BOUND. One value is a fact when it appears; the OTHER value
                      is a stated unknown and is NEVER the negative of the first. Reading it as the negative
                      is the inverse reading §Attacker-sources forbids of the deny list, one signal over.
     "undetermined" — nothing in this zone can compute it at all, so its only value is `unknown`. It is
                      surfaced anyway, because a person permitting a request whose lineage NOBODY established
                      has made a different decision from one permitting a request known to have none, and a
                      row that is absent says the second while meaning the first.
     "intent"       — an intent the ENGINE holds and cannot VERIFY. A probe can be BUILT to fail validation
                      and cannot be KNOWN to be refused before business logic, since a server may log,
                      rate-limit, lock an account or act on malformed input. */
var _SIGNAL_CERTAINTY = ["certain", "stated", "partial", "undetermined", "intent"];
/* ── URL-CARRIED AUTHORITY: A MATCH, IN THE DENY DIRECTION ONLY, AND NEVER READABLE AS ITS NEGATIVE ──────
   §RUN-DON'T-MATCH bans matching because a matched NAME would be ASSERTED as a value; a match that only ever
   REFUSES asserts nothing, which is the argument `_destructiveToken` already stands on in this same file and
   the reason it is "the one place this project matches on a name, matching only to refuse". This is the
   second, on the same terms and with the same ceiling: a hit is a fact, and a MISS IS NOT A FINDING. The
   value for a miss is `unknown`, never `none`, and a person permitting `unknown` is permitting a stated
   unknown rather than an established absence — which the surface must say in those words.
   WHAT IS IN THE LIST IS WHAT AUTHORIZES A STRANGER WITHOUT A SESSION. A presigned object-store address, a
   reset or invite token and a signed webhook all carry the authority in the address, which is precisely what
   refuted the retired rule: an uncredentialed request to one is not uncorrelated with the person.
   WHAT IS DELIBERATELY NOT IN IT, STATED SO IT IS NOT RE-ADDED. `key` and `api_key` are excluded: an API key
   is the APPLICATION's authority and not the person's, it is what a published discovery document is fetched
   with, and admitting it would make `present` the common answer — which does not merely add noise, it
   retrains the reader of this signal to ignore it, so the day a real capability URL appears the row it
   appears in is the row nobody looks at. A bare opaque token in a PATH SEGMENT is also outside it, because
   nothing distinguishes one from an object id; the JWS prefix below is the one path-borne form that is
   self-identifying, and the residual is stated rather than guessed at.
   NAMED RESIDUAL. WHAT IS NOT COVERED: authority carried in a path segment that is not a JWS — an opaque
   reset token spelled `/invite/<40 hex>` reads `unknown` and is permitted by whoever permitted `unknown`.
   WHAT THE NEXT DIFF BUILDS: the signal read off the ENGINE instead of off the address — the park already
   knows which source each segment of an address came from, so a segment that came from a credentialed reply
   is the lineage question and answers this one on the way past. HOW ITS ABSENCE WOULD SHOW: a person
   inspecting a widened origin's permitted values sees `url-authority=unknown` doing all the work while the
   `present` row is never exercised, on an app whose addresses carry capability tokens. */
var _URL_AUTHORITY_PARAMS = ["x-amz-signature", "x-goog-signature", "x-ms-signature", "sig", "signature",
                             "hmac", "token", "access_token", "auth_token", "reset_token", "invite_token"];
function _urlAuthorityMarker(u) {
  var found = null;
  u.searchParams.forEach(function (v, n) {
    if (found === null && _URL_AUTHORITY_PARAMS.indexOf(String(n).toLowerCase()) >= 0) found = String(n).toLowerCase();
  });
  if (found !== null) return found;
  /* A JWS COMPACT SERIALIZATION ANYWHERE IN THE ADDRESS. RFC 7515 §3.1 "JWS Compact Serialization Overview"
     makes the first component `BASE64URL(UTF8(JWS Protected Header))`, and §4 "JOSE Header" makes that header
     a JSON OBJECT — so its first three bytes are `{`, `"` and the first character of the first member's
     name, and base64url of those is `eyJ` for every name beginning with an ASCII LETTER or `_`. Measured
     rather than asserted, because the two-byte reading is a different string: `base64url("{\"")` alone is
     `eyI`, and the `J` comes from the THIRD byte — `{"a` is `eyJh`, `{"k` is `eyJr`, `{"0` is `eyIw`. Every
     header name the JOSE registry defines (`alg`, `typ`, `kid`, `enc`, `crit`) begins with a letter.
     IT IS A LOWER BOUND LIKE EVERY OTHER ENTRY HERE AND MAY NOT BE READ AS MORE: a header whose first member
     name began with a digit would not match, which is the `unknown` arm and is not a statement that there is
     no authority. It is asked of the PATH and the QUERY together because a JWS is authority in the address
     whatever component it sits in and whatever the parameter is called. */
  if ((u.pathname + u.search).indexOf("eyJ") >= 0) return "jws";
  return null;
}
/* ── THE REGISTRY. ONE DECLARATION, READ BY THE FIRING WALK AND BY THE SURFACE ───────────────────────────
   THE ORDER IS THE REFUSAL'S ORDER, which is the whole of what a per-signal control buys over a score: the
   refusal names the FIRST gating signal whose value the origin does not permit, so a person reading a
   frontier that will not drain is told WHICH FACT held it rather than that something did. `destination` is
   first deliberately — at an origin nobody has permitted anything at, the sentence a person most needs is
   "this origin does not permit data requests", and every other row is a refinement of it.
   `gates` IS NOT A STYLE FLAG AND IS ASSERTED. A signal that does not gate has exactly ONE possible value at
   this chokepoint, so a control over it would be a control with one setting — §AN-ASSERT-WHOSE-TWO-SIDES-
   CANNOT-DISAGREE's shape arriving in a permission surface. The day the value space grows the signal MUST
   start gating, and `_signalRegistryCheck` is what makes that impossible to forget rather than a thing to
   remember: a second value on a non-gating signal aborts at the host's own startup.
   A NON-GATING SIGNAL IS STILL SURFACED. It is a fact the person is entitled to see before they permit
   anything — "this transport only ever sends GET" is the sentence that makes the method rows of every other
   security discussion legible — and it is where the row already is on the day it starts gating. */
var _SIGNALS = [
  /* FETCH §2.2.5 "Requests"' DESTINATION, READ AS THE TWO DISTINCTIONS THE POLICY TURNS ON — each by its
     own predicate over the ONE string, which is why `_isDocumentSubresource` sits beside `_isScriptLike`
     rather than widening it.
     THE ARGUMENT FOR ONE PREDICATE IS REWRITTEN HERE RATHER THAN DELETED, BECAUSE A READER WILL RE-DERIVE
     IT. It read: the predicate is `_isScriptLike` itself rather than a second spelling of it, since two
     answers to "is this code" would be two rules free to disagree about a chunk, and CORB decides on that
     same line one function up. Every clause of that is still TRUE. What was wrong is the premise underneath
     it — that the egress question WAS "is this code" — and the paragraph's own next sentence gave it away:
     A PROGRAM LOAD IS A BROWSER LOADING A PAGE is a question about WHOSE ACT the request is, and a
     stylesheet answers that one the way a chunk does while answering CORB's the other way. So there is
     still exactly one answer to "is this code", and it is still `_isScriptLike`; what this row gained is a
     SECOND QUESTION, not a second answer.
     THE CASCADE IS ORDERED AND THE ORDER IS LOAD-BEARING. §2.2.5's sets OVERLAP and do not partition its
     enumeration: `script`, `audioworklet` and `paintworklet` are script-like AND subresource requests,
     while `serviceworker`, `sharedworker` and `worker` are script-like and NON-subresource requests.
     Asking the CORB set first keeps all six reading `program`, which is what they are and what the first
     arm below already permits; asking the other way round would have left those last three in a value no
     arm names, refusing three code loads at every origin.
     A PROGRAM LOAD AND A SUBRESOURCE LOAD ARE BOTH THE PAGE LOADING ITSELF — the bytes are the app's own,
     served identically to every visitor, revealing nothing about this person — and a VALUE request is this
     tool spending somebody else's server. That line is where the defaults below are drawn, and it is
     CLAUDE.md §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS's rather than this file's. */
  { name: "destination", gates: true, certainty: "stated",
    values: ["program", "subresource", "navigation", "value"],
    of: function (f) {
      if (_isScriptLike(f.destination)) return "program";
      if (_isDocumentSubresource(f.destination)) return "subresource";
      return _isNavigation(f.destination) ? "navigation" : "value";
    } },
  /* WHOSE ACT THIS REQUEST IS — see `_actorOf`, which carries why no other row can answer it and why the
     word is TYPED at each asker rather than inferred from who the callers are.
     IT SITS SECOND, AND THE POSITION IS THE REFUSAL'S SENTENCE RATHER THAN A RANKING OF HARM. The walk names
     the FIRST gating signal an origin does not permit, so after "this origin does not permit data requests"
     the sentence a person most needs is "and this one is a request THIS TOOL composed rather than one your
     page made" — which is coarser than any grade about the request's PATH and is what every row below it
     refines. `provenance` and `doc-reach` follow it for the reason they already give each other.
     `certain` AND NOT `stated`, WHICH IS THE ONE PLACE THIS REGISTRY CAN SAY THAT ABOUT A ROW THE POLICY
     TURNS ON: the three grades nearest it are words the ENGINE composes and this zone relays, and the engine
     is attacker-controlled; this one is a literal in trusted-zone source at every asker. */
  { name: "actor", gates: true, certainty: "certain", values: _ACTOR_WORDS,
    of: function (f) { return f.actor; } },
  /* CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's three names. `stated` and not `certain`: the engine
     composes the word at the park out of the park's own kind and the parking flow's `path_forced`, and the
     `CHECK` in `_provenanceOf` catches a word outside the three and nothing else. */
  { name: "provenance", gates: true, certainty: "stated", values: _PROVENANCE_TYPES,
    of: function (f) { return f.provenance; } },
  /* AND HOW THE DOCUMENT THIS REQUEST WAS MADE FROM WAS ITSELF REACHED — see `_docReachOf`. It is the same
     three words about a DIFFERENT ACT, and the pair is what stops one permission answering for two
     populations: the row above says whose act this request is, and this one says whose act the document
     holding it was. A page this tool chose to open goes on making its own `fetch()`es, and every one of them
     is `provenance=observed` because the page really made them.
     IT SITS DIRECTLY AFTER `provenance` BECAUSE THE REFUSAL'S ORDER IS THIS LIST'S ORDER, and a person told
     "this origin does not permit requests of this grade" is owed the one-level-out sentence next rather than
     after four rows about the address.
     `stated` AND NOT `certain`: the word is composed by whichever zone performed the document load and
     relayed to this one, and `_docReachOf`'s CHECK catches a word outside the three and nothing else — the
     same grade `provenance` carries, for the same reason. */
  { name: "doc-reach", gates: true, certainty: "stated", values: _PROVENANCE_TYPES,
    of: function (f) { return f.docReach; } },
  /* WHETHER THE PERSON'S COOKIE JAR PAYS, AND IT MEANS COOKIES AND NOTHING WIDER. `credentials:"omit"`
     strips the jar; it does not strip an `Authorization` header, it does not strip authority carried in the
     address, and it says nothing about lineage — which is why those are their own rows rather than readings
     of this one. `certain` because `_credentialedOf` composes it out of this zone's own literal and a mode
     whose vocabulary it asserts, and because it is the flag that decides what `init.credentials` says. */
  { name: "cookies", gates: true, certainty: "certain", values: ["yes", "no"],
    of: function (f) { return f.credentialed ? "yes" : "no"; } },
  /* AND WHETHER ANY OTHER AUTHORITY RIDES IN A HEADER, WHICH IS A QUESTION ABOUT WHOSE LIST IT IS AND NOT
     ABOUT WHAT IS IN IT. `opts.headers` on the XHR path is the analysed BUNDLE's own list, which may carry
     the bearer token that person's session minted — and nothing states whose list it is, which is the
     residual `_credentialedOf` already names and the reason a credentialed request may not state one at all.
     SO `none` IS A FACT AND `unknown` IS A STATED UNKNOWN: with no list, this zone adds no authority beyond
     what `init.credentials` says, and that is certain; with a list, the owner is unstated and so is the
     answer. It is NOT `partial`, because the informative value here is the ABSENCE — the reverse of
     `url-authority`, where the informative value is the presence. */
  { name: "header-authority", gates: true, certainty: "certain", values: ["none", "unknown"],
    of: function (f) { return f.headers ? "unknown" : "none"; } },
  /* solver/engine.h's WITNESS MARK — whether the flow had DETERMINED some source's value on an arm nothing
     observed before it built this address, so the address may carry a witness THE ENGINE picked. Composed at
     the PARK off the parking flow's `flow_path_pinned`, or at the ACT off the standing one's
     (`engine_pinned_of_running_path`) for a request the page's code made without parking.
     `unstated` is this zone's third word for an act whose record carries NO MARK — a navigation notice or a
     route declaration today, and an artifact older than the key on any seam; see `_pinnedOf` for why writing
     `unpinned` there would be false exactly where it matters. */
  { name: "witness", gates: true, certainty: "stated", values: _PINNED_MARKS,
    of: function (f) { return f.pinned; } },
  /* AUTHORITY IN THE ADDRESS — see `_urlAuthorityMarker`. `partial`, and the grade is the whole warning:
     `present` is a fact and `unknown` is NOT the statement that there is none. */
  { name: "url-authority", gates: true, certainty: "partial", values: ["present", "unknown"],
    of: function (f) { return _urlAuthorityMarker(f.url) !== null ? "present" : "unknown"; } },
  /* CREDENTIAL LINEAGE — DO THIS ADDRESS'S BYTES DERIVE FROM A RESPONSE THAT WAS FETCHED CREDENTIALED. THE
     CHOKEPOINT CANNOT SEE IT AND THIS ROW IS THAT SENTENCE, WRITTEN WHERE THE DECISION IS MADE RATHER THAN
     IN A DOCUMENT. A chokepoint reads the credential state of the request IN FRONT OF IT and nothing
     whatever about where the address came from; an address the bundle computed out of a credentialed reply
     (`/api/orgs/{theirOrgId}/members/{theirUserId}`) is about that person's account whatever headers it
     carries, and no property of the address distinguishes it from one the bundle spelled as a constant.
     IT IS SURFACED AS `unknown` RATHER THAN OMITTED, AND THAT IS THE POINT OF THE ROW. Omitting it would
     make every permission read as covering a request whose lineage had been established; stating it makes a
     person's permission say, in their own control, that they are permitting requests nobody has established
     the lineage of. That is the difference between a stated unknown and an absence a default fills.
     NAMED RESIDUAL. WHAT IS NOT COVERED: every request, at every setting — this signal has exactly one value
     today and it is the unknown one. WHAT THE NEXT DIFF BUILDS: a `credentialed` bit on the RESPONSE the
     engine ingests, carried into the concolic value's source identity so that a value derived from that
     reply carries it, and composed at the park beside the provenance word onto the pending line — the same
     KIND of question the engine already answers with `path_forced`, one hop further out. HOW ITS ABSENCE
     WOULD SHOW: a person auditing what they permitted at an origin finds every request there reading
     `lineage=unknown`, including the ones whose address the bundle demonstrably built out of a logged-in
     reply, so the row cannot separate the population it exists to separate. */
  /* AND IT IS NOT THE ROW `doc-reach` ANSWERS, WHICH IS SAID HERE SO NEITHER IS RETIRED AS COVERED BY THE
     OTHER. `doc-reach` is about an ACT this zone performed — how the document holding this request was
     reached — and it is fully determined today. This row is about the BYTES of the address: whether the
     values the bundle composed it out of came back from a response fetched with the person's session. A
     document reached `observed` routinely computes addresses out of credentialed replies, and a document
     reached `forced` routinely composes them from its own literals, so neither row bounds the other in
     either direction. */
  { name: "lineage", gates: true, certainty: "undetermined", values: ["unknown"],
    of: function () { return "unknown"; } },
  /* THE METHOD, WHICH IS A FACT ABOUT THIS TRANSPORT AND NOT A CONTROL. `init` hardcodes `method:"GET"` and
     `_refuseUnreadOptions` refuses a caller that states one, so the value space here has exactly one member
     and a permission row over it would be a checkbox that cannot be unticked. It is SURFACED because a
     person deciding what to permit is entitled to know that this transport sends one verb, and because the
     day it can send another the row is already here and `_signalRegistryCheck` forces it to start gating. */
  { name: "method", gates: false, certainty: "certain", values: ["GET"],
    of: function () { return "GET"; } },
  /* INTENDED INVALIDITY — AN INTENT THIS ENGINE HOLDS AND CANNOT VERIFY, AND AT THIS CHOKEPOINT IT HAS NO
     POPULATION AT ALL. A probe built to fail validation never reaches business logic, which is why POST does
     not mean unsafe and is the whole `req2proto` technique — but that request is a POST WITH A BODY, and
     this file can compose neither: `_SAFEFETCH_OPTIONS` is a closed set containing no `method` and no
     `body`, so there is no way for a caller to state such an intent and no request here for it to be about.
     `unstated` IS THEREFORE A STRUCTURAL FACT AND NOT A DEFAULT. Nothing declares it because nothing CAN,
     and the closed option set is what makes that checkable rather than a claim about who the callers are.
     WHERE THE POPULATION LIVES: `pageContextFetch`, the operator-initiated relay, which is a different
     transport with a different authorization — a human at a surface that shows them the bytes — and whose
     verb is stated at its own call site. A signal surfaced HERE about requests made THERE would be a control
     over something this file never sees, which is the second policy point SECURITY.md puts in one place.
     RETIREMENT: this row starts gating the day a caller here can state an invalidity intent, which requires
     a verb and a body to exist in `_SAFEFETCH_OPTIONS` first; until then `_signalRegistryCheck` asserts it
     has one value, which is the same sentence enforced rather than described. */
  { name: "invalidity", gates: false, certainty: "intent", values: ["unstated"],
    of: function () { return "unstated"; } }
];
/* THE REGISTRY IS CHECKED ONCE, AT THE ONE DOOR EVERY HOST GOES THROUGH, AND THE THREE THINGS IT CHECKS ARE
   THE THREE A GRANT IS KEYED ON. A duplicate name would make one row's permission silently answer for
   another's; a certainty outside the vocabulary would render as a blank where a person reads how much to
   trust a row; and a NON-GATING signal with more than one value is a control that was dropped — the value
   varies, nothing in the table can express it, and every grant silently permits both. That last one is the
   only assert here that can catch a future diff rather than a typo, and it is why `gates` is data. */
function _signalRegistryCheck() {
  var seen = Object.create(null), i, s;
  for (i = 0; i < _SIGNALS.length; i++) {
    s = _SIGNALS[i];
    CHECK(!seen[s.name],
          "the egress signal `" + s.name + "` is declared twice — a grant is keyed on the NAME, so the " +
          "second declaration's permitted values would answer for the first's and a person's permission " +
          "would be about a row they never saw");
    seen[s.name] = true;
    CHECK(_SIGNAL_CERTAINTY.indexOf(s.certainty) >= 0,
          "the egress signal `" + s.name + "` declares the certainty " + JSON.stringify(s.certainty) +
          ", which is none this file states — the grade is what tells a person whether they are permitting " +
          "something this zone KNOWS, something another zone SAID, or something nobody established, and an " +
          "unrecognised one renders as a blank beside a checkbox they are about to tick");
    CHECK(Array.isArray(s.values) && s.values.length >= 1,
          "the egress signal `" + s.name + "` declares no value space — the surface renders one row per " +
          "value and a grant permits values, so a signal with none is a row a person can neither see nor " +
          "permit while the firing walk still asks it");
    CHECK(s.gates || s.values.length === 1,
          "the egress signal `" + s.name + "` does not gate and has more than one possible value — a signal " +
          "whose value VARIES and which no permission can express is a control that was silently dropped: " +
          "every grant permits every value of it and nothing anywhere says so. A signal is surfaced without " +
          "gating only where this chokepoint can produce exactly one answer, which is a structural fact " +
          "(`method` is a literal, `invalidity` cannot be stated through a closed option set) and stops " +
          "being true the moment that structure changes. Make it gate");
  }
  /* AND THE DESTINATION CASCADE'S ONE STRUCTURAL ASSUMPTION IS ASSERTED HERE RATHER THAN WRITTEN DOWN AT
     IT. `_isNavigation`'s banner argues that its position in that cascade cannot change any answer, and the
     whole of that argument is that §2.2.5's navigation set is DISJOINT from the other two — which is
     a property of a standard that RENUMBERS AND REWORDS, so a paragraph claiming it is a paragraph that can
     go quietly false. The operands are every one of them this file's own literals, which is what makes a
     DCHECK the right macro: no page, no engine and no remote party can reach it, so it is this codebase
     asserting its own logic and never an abort switch somebody else holds.
     WHAT IT WOULD CATCH IS AN ORDERING BUG WITH NO SYMPTOM. If a later edition puts a destination in two
     sets, the cascade silently answers whichever question is asked FIRST and the row a person reads moves
     with it — no crash, no diagnostic, and a permission about a different population than its own
     prose claims. In dev it aborts at the host's own startup instead. */
  for (i = 0; i < _DESTINATION_TYPES.length; i++)
    DCHECK(!_isNavigation(_DESTINATION_TYPES[i]) ||
           (!_isScriptLike(_DESTINATION_TYPES[i]) && !_isDocumentSubresource(_DESTINATION_TYPES[i])),
           "the destination " + JSON.stringify(_DESTINATION_TYPES[i]) + " answers BOTH §2.2.5's " +
           "navigation question and one of the two above it, so which value the cascade computes is decided " +
           "by the order the questions are asked in — and `_isNavigation`'s banner says that order cannot " +
           "matter. One of the two is now wrong, and the arm a person permitted is about a population they " +
           "did not see");
  /* AND THE DEFAULT ARMS ARE CHECKED AGAINST THE SAME REGISTRY, HERE, BECAUSE AN ARM IS A PERMISSION AND A
     PERMISSION THAT CANNOT MATCH IS A PERMISSION THAT WAS SILENTLY REVOKED. An arm naming a signal this file
     does not declare compares `undefined` against its value and is false for every request for ever — the
     page's own `fetch()` would stop being relayed, which is the exact silence §Learning-from-replies has
     already been measured costing a whole session's parks, with no crash and an empty report. An arm naming
     a VALUE outside a declared space is the same failure one field over.
     IT IS A `CHECK` AND NOT A `DCHECK` BECAUSE RELEASE CANNOT PROCEED CORRECTLY THROUGH IT. Every other
     assert in this function guards a control a person reads; this one guards whether the engine can answer
     the document it is analysing at all, and the failing arm is one this project's own build shipped. */
  for (i = 0; i < _DEFAULT_ARMS.length; i++) {
    CHECK(Array.isArray(_DEFAULT_ARMS[i].when) && _DEFAULT_ARMS[i].when.length >= 1,
          "a default egress arm states no condition — an arm is a permission matched on the conjunction it " +
          "names, and one naming none would fire for EVERY request at every origin, which is this file's " +
          "whole decision deleted by an empty array");
    _DEFAULT_ARMS[i].when.forEach(function (c) {
      var why = safeFetchSignalUsable(c.signal, c.value);
      CHECK(why === null,
            "a default egress arm names `" + c.signal + "=" + c.value + "`, which this file's registry " +
            "answers `" + why + "` — an arm is matched by comparing the signal's computed value, so an " +
            "unknown name compares against `undefined` and an unknown value against nothing: the arm is " +
            "false for every request for ever, and what it was permitting is refused with no crash, no " +
            "diagnostic and a report that reads as a page with nothing behind it");
    });
  }
}
/* ── THE FACTS EVERY SIGNAL IS COMPUTED FROM, REQUIRED AT THE ONE DERIVATION BOTH READERS SHARE ──────────
   THIS USED TO STAND INSIDE `_firingRefusal` AND IS MOVED RATHER THAN COPIED, BECAUSE THE SENTENCE BELOW
   CLAIMED A PROPERTY ONLY ONE OF THE TWO READERS WAS ENFORCING. `_signalVector`'s own banner said its facts
   were ones "this file has already validated" — true of the firing walk, which CHECKed all seven, and FALSE
   of the surface, which reached the identical derivation through `safeFetchSignalVector` having checked only
   that a URL parses. One derivation, two gates, and the weaker one decided what a PERSON was shown.
   THE THREE THAT FAILED SILENTLY ARE THE POINT, AND THEY ARE THE ONES NO ASSERT ANYWHERE COULD CATCH. An
   unstated fact whose value lands OUTSIDE its signal's declared space is caught by the `DCHECK` below —
   `provenance`, `doc-reach`, `witness` and `actor` compute `undefined` and fire it in dev. The other three
   compute a value INSIDE the space, from nothing: an absent `destination` takes neither `_isScriptLike` nor
   `_isDocumentSubresource` and reads `value`; an absent `credentialed` is `!!undefined` and reads `no`; an
   absent `headers` is falsy and reads `none`. MEASURED: the vector for a request stating none of the three is
   BYTE-IDENTICAL to one stating all of them, in dev and in release alike, with nothing raised anywhere.
   AND THE DIRECTION IS THE REASSURING ONE, WHICH IS WHY IT IS A `CHECK`. `cookies` and `header-authority` are
   the two rows this registry grades `certain` — "this tool computed it", the strongest thing the surface can
   say — so a person about to widen an origin was shown `cookies=no` and `header-authority=none`, marked as
   THIS REQUEST, for a request whose credential state and header list nobody had stated. That is
   CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS at the one boundary where the CONSUMER IS THE PERSON, which
   §NOTHING-IS-REFUSED-AT-EVERY-SETTING makes the whole of the safety: with no combination refused, the only
   thing between somebody and a combination they did not intend is whether the control stated the
   consequence. A plausible datum here is not a wrong number in a report, it is a decision made on a fact
   nobody established — so release cannot PROCEED correctly through it and the grade is the firing walk's own.
   IT MAY ASSERT AT ALL FOR `_refuseUnreadOptions`' REASON, WHICH IS THE ONE THAT DECIDES IT: every field here
   is composed in TRUSTED-ZONE SOURCE. The surface's only caller builds an object LITERAL whose seven facts
   are literals, and whose `url` is a subject `safeFetchWidenable` has already answered for; the untrusted
   engine supplies header VALUES and grade WORDS through `safeFetch`'s own options, every one of which is
   CHECKed at its read one function per fact above. So no bundle and no compromised renderer can reach this
   abort — CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE's discriminator is who DETERMINES the value, and here it is
   this zone in every frame. A stranger's bytes are asserted about nowhere in this function. */
function _requireFacts(facts) {
  CHECK(facts !== null && typeof facts === "object",
        "the signal vector was asked for with no facts — every signal is computed from them and a caller " +
        "that passed none would be answered by whichever arm each signal's predicate happens to reach first");
  CHECK(typeof facts.destination === "string" && _PROVENANCE_TYPES.indexOf(facts.provenance) >= 0 &&
        _PROVENANCE_TYPES.indexOf(facts.docReach) >= 0 &&
        _PINNED_MARKS.indexOf(facts.pinned) >= 0 && _ACTOR_WORDS.indexOf(facts.actor) >= 0 &&
        typeof facts.credentialed === "boolean" &&
        facts.url !== null && typeof facts.url === "object" && typeof facts.url.origin === "string",
        "a signal vector was asked for without the facts that decide it — Fetch §2.2.5 \"Requests\"' " +
        "DESTINATION says " +
        "whether this reply becomes a PROGRAM or a VALUE, the actor says whether the ANALYSED PAGE composed " +
        "this request or THIS TOOL did, the provenance says what its path is evidence of, the reach " +
        "grade says whose act the DOCUMENT it was made from was, the witness " +
        "mark says whether the address may rest on a value this engine chose, the credential flag says " +
        "whether the person's session pays, and the parsed URL is what the address-borne authority is read " +
        "off. A caller that omitted any of them would be answered by the FIRING walk's permissive arm, " +
        "which is the one that spends an act, and shown on the SURFACE as a value with no fact under it: " +
        "an absent destination reads `value`, an absent credential flag reads `cookies=no` and an absent " +
        "header list reads `header-authority=none`, both of those on rows this registry grades `certain`. " +
        "Every one of them is a fact the CALLER holds and this file cannot " +
        "re-derive: forgetting to state one may never be a way to be exempted");
  /* AND THE HEADER LIST'S SHAPE, WHICH NOTHING WAS ASKING — THE `header-authority` ROW IS THE ONE PLACE A
     PRIMITIVE READS AS AN ESTABLISHED ABSENCE. That row's own declaration says `none` IS A FACT and grades
     it `certain`: with no list this zone adds no authority beyond what `init.credentials` says. It computes
     that fact by TRUTHINESS, so a caller that passed `""`, `0` or `false` — none of which is a header list
     and every one of which is a bug at the call site — is told, on the strongest grade this registry has,
     that the question was asked and answered. A list is an OBJECT; `null` and absent are the honest
     spellings of "this caller composed none", which is what every call site in this tree states today.
     IT IS THE SHAPE AND NEVER THE CONTENTS, which is the line §WHOSE-BYTES-STATE-THE-VALUE draws: the VALUES
     in that list are the analysed BUNDLE's on the XHR path, so this file may assert that a list IS a list
     and may never assert anything about what a stranger put in it. A name or a value inside it is input. */
  CHECK(facts.headers === null || facts.headers === undefined || typeof facts.headers === "object",
        "a signal vector was asked for with a header list that is not one: " +
        JSON.stringify(facts.headers) + " — the `header-authority` row reads this by TRUTHINESS and grades " +
        "the answer `certain`, so a primitive here is rendered to a person as the established fact that no " +
        "authority rides in a header, on a request whose header list was never stated. `null` is how a " +
        "caller with no list says so and is a fact; anything that is not an object is a call site that " +
        "meant something else. This asserts the SHAPE and never the CONTENTS — the names and values are " +
        "the analysed bundle's on the XHR path and are input this zone may not assert about");
}
/* THE VECTOR FOR ONE REQUEST — every signal's value, computed from the facts REQUIRED DIRECTLY ABOVE. It is
   what the firing walk asks and what the surface renders, so there is one derivation and not two: a control
   that showed a person a value the walk did not decide from would be a permission surface about a different
   request. The gate is HERE, on the derivation, rather than at each of the two entries — an entry is a list
   somebody maintains and this is every asker by construction, which is the same argument
   `safeFetchEgressStating` makes one door over. */
function _signalVector(facts) {
  var v = Object.create(null), i, s, val;
  _requireFacts(facts);
  for (i = 0; i < _SIGNALS.length; i++) {
    s = _SIGNALS[i];
    val = s.of(facts);
    /* THE COMPUTATION AND THE DECLARED VALUE SPACE ARE TWO STATEMENTS AND THIS IS WHERE THEY ARE MADE TO
       AGREE. A grant permits values out of the DECLARED space, so a computed value outside it is a value no
       permission can ever name — a request that can never fire however much a person permits, refused with
       a sentence naming a value they cannot find in their own control. DCHECK and not CHECK on this file's
       own discriminator: release still PROCEEDS correctly, because an unpermittable value is refused, which
       is the conservative arm. */
    DCHECK(s.values.indexOf(val) >= 0,
           "the egress signal `" + s.name + "` computed " + JSON.stringify(val) + ", which is not in the " +
           "value space it declares (" + s.values.join(", ") + ") — a grant names values out of that space, " +
           "so this request can never be permitted by any setting and the person's control has no row for " +
           "the answer it is being refused with");
    v[s.name] = val;
  }
  return v;
}
/* ── WHAT FIRES WITHOUT ANYBODY SAYING SO, AND IT IS DATA SO THE SURFACE CAN SHOW IT ─────────────────────
   CLAUDE.md: "THE DEFAULT FOR AN UNCONFIGURED ORIGIN IS PROGRAM LOADS ONLY — a script, a module import, a
   lazy chunk, same-origin and credentialed exactly as a browser is, WHICH IS THE PAGE LOADING ITSELF AND IS
   A REQUEST THE PERSON'S OWN BROWSER WOULD HAVE MADE. No data fetch, no probe, no discovery, until the
   origin is deliberately widened."
   THE SECOND ARM IS A READING OF THAT SENTENCE'S OWN JUSTIFYING CLAUSE AND IT IS NAMED AS ONE, because the
   DEFINITION ("program loads only") and the PURPOSE ("a request the person's own browser would have made")
   disagree on exactly one population and this file has to answer for it. An OBSERVED request is
   solver/engine.h's "a real load of this document makes exactly this request", so it satisfies the PURPOSE
   clause verbatim while not being a program load. CLAUDE.md's own rule for that disagreement is
   §WHERE-A-RULE'S-DEFINITION-AND-ITS-STATED-PURPOSE-DISAGREE: the purpose is the half that was checked
   against reality, because a purpose is written while looking at the hazard and a definition afterwards, to
   be tidy.
   AND THE TWO MISTAKES ARE NOT THE SAME SIZE, WHICH IS THE OTHER HALF OF WHY IT IS HERE. Getting this wrong
   in the REFUSING direction stops this engine answering the page's own `fetch()` — §Learning-from-replies
   calls learning from replies "the POINT", and this project has already measured what that costs once, when
   the reply path was unreachable in the shipped extension and every flow a page's `fetch()` parked stayed
   parked for ever. The failure is SILENT: parked flows, no crash, an empty report. Getting it wrong in the
   PERMITTING direction relays a request the page itself made, which the person's own browser made anyway.
   IT IS ONE LINE TO OVERTURN AND THE LINE IS DATA. If the intended reading is the literal one, delete the
   `provenance`/`observed` entry: the arms are a list, the surface renders them, and the refusal that follows
   names the signal. What must NOT happen is the reading being changed in prose while this list stays.
   AND `PROGRAM LOADS ONLY` IS THE QUOTATION AND NO LONGER THE TABLE, WHICH IS SAID HERE SO THE BANNER AND
   THE LIST CANNOT BE READ AS DISAGREEING. The project owner has since widened the unconfigured default
   TWICE on the same discriminator — WHOSE ACT THE REQUEST IS — first to the rest of what a document
   loads in order to be itself, then to a child navigable the page's own markup named or its own code
   computed. Each widening is ONE ARM below, each cites the CLAUDE.md paragraph that carries the decision,
   and the quotation above is kept verbatim because it is what the arms were drawn FROM and because a reader
   who re-derives the literal reading will re-derive it in those words. What is unchanged is the other half
   of that sentence: no data fetch this tool composed, no probe and no discovery, until the origin is
   deliberately widened.
   THESE ARE PERMISSIONS AND NOT REFUSALS, so no value of the table below can be narrowed by them and
   §NOTHING-IS-REFUSED-AT-EVERY-SETTING is untouched: an arm here only ever makes something fire. */
var _DEFAULT_ARMS = [
  /* AND THIS ARM IS THE ONE WITH A MEASUREMENT UNDER IT, CARRIED FORWARD FROM THE SINGLE-SWITCH CONTROL IT
     REPLACES BECAUSE IT IS EVIDENCE AND NOT AN ARGUMENT. Driven against a mirrored production SPA, ONE run
     refused 467 requests over 85 distinct addresses and every single refusal was this gate's — not one
     refusal of any other kind — and the addresses were ORDINARY LAZY CHUNKS OF THE PAGE'S OWN BUNDLE at its
     own origin: `IconDot-<hash>.js`, `Pill-<hash>.js`, `agent-ids-<hash>.js`, and the vendor chunk holding
     the descriptor decoder. §the-symbolic/trust-boundary names exactly that case as the one that must fire
     ("a lazy chunk reveals real endpoints — the headline moat surface"), and it was what was refused. The
     command is quoted rather than the figure, because the figure is one run's:
     `grep -o "blocked-[a-z-]*:[a-z=-]*" <a drive log> | sort | uniq -c`, run with a negative control.
     A FORCED SEGMENT IN A PROGRAM'S ADDRESS DOES NOT MOVE IT ACROSS THIS LINE, which is the case the split
     most invites getting wrong: `import("/chunks/" + region + ".js")` with `region` pinned by forcing a gate
     reads as the hard case and is not one — the chunk is still the app's own code, still served to anyone
     who asks, and it is PRECISELY the gated surface this product exists to reach. That is why this arm reads
     the destination ALONE: the `witness` row cannot narrow it, because an arm here is a permission and the
     table below only ever widens. */
  { when: [{ signal: "destination", value: "program" }],
    why: "a script, a module import or a lazy chunk is the page loading itself — the app's own code, served " +
         "byte-identically to every visitor, revealing nothing about this person" },
  /* AND THE SAME SENTENCE ABOUT THE REST OF WHAT A DOCUMENT LOADS IN ORDER TO BE ITSELF — WHICH IS A
     DECISION THE PROJECT OWNER MADE AND NOT ONE THIS FILE INFERRED, so it is cited rather than argued.
     CLAUDE.md §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS carries it: the discriminator is WHOSE ACT THE REQUEST
     IS and not what the reply becomes, so a subresource the page's own markup names or its own running code
     computes is the page loading itself and is fetched exactly as a browser fetches it — same-origin,
     credentialed, before any widening. Read by the destination's CORB question alone a `<link
     rel=stylesheet>` is refused and the document renders with UA defaults only, which is not a narrower
     answer but a different document.
     IT IS DESTINATION-KEYED ALONE, SYMMETRICALLY WITH THE ARM ABOVE AND FOR THAT ARM'S OWN REASON. A forced
     segment in the address does not carry a request across this line: a stylesheet or a font whose path
     segment a forced equality pinned is still the app's own asset, served byte-identically to every
     visitor, and it is precisely the gated surface this product exists to reach. The pinned-or-contradicted
     bit refines the VALUE side and buys nothing here, which is why the `witness` row below cannot narrow
     this arm any more than it can narrow the one above.
     AND A CONJUNCTION WITH `provenance=observed` WOULD BE INERT RATHER THAN CAREFUL, which is the reading
     this arm most invites and is measured in the paragraph after the next one: no subresource park in this
     engine can be graded `observed`, so such an arm would sit here reading as a permission and match
     nothing for ever.
     WHAT IT RE-GRADES IS ENUMERATED RATHER THAN LEFT TO BE FOUND, because a permission whose reach the
     person cannot state is not a control. Every destination §2.2.5 calls a subresource request except the
     empty string now fires at every origin, which is ten words of that list; and of those this engine can
     STATE six today, through `<link rel=stylesheet>`, through `<img>`, and through the four an `as=` on a
     `<link rel=preload>` or a `modulepreload` translates to. The one that needs saying out loud is `<img>`,
     because the paragraph below refused it at length: that refusal is retired by the OWNER'S DECISION and
     not by anybody disagreeing with its reasoning, and it is rewritten rather than deleted there. */
  { when: [{ signal: "destination", value: "subresource" }],
    why: "a stylesheet, an image, a font or any other subresource the document's own markup or its own " +
         "running code names is the page loading itself — the same request the person's own browser would " +
         "have made, for bytes served byte-identically to every visitor" },
  /* AND THIS ARM IS A CONJUNCTION, WHICH IS WHERE THE SOUNDNESS OF ITS OWN `why` IS NOW STATED RATHER THAN
     ASSERTED ABOUT THE LIST. It used to name `provenance=observed` alone, and that was sound only while
     every document had itself been reached observably — a property of the OTHER arms, which this function
     could not ask of the request in front of it and therefore checked over the arm list instead.
     WHAT THAT MISSED IS ONE LINE AND READS AS A SMALL WIDENING. Fetch §2.2.5 "Requests"' destination for a
     navigation is `document`, `_isScriptLike` answers false for it, so the `destination: program` arm cannot
     fire a navigation and in the default configuration a document was loaded through the `observed` arm
     alone. Add a `provenance: derived` arm — the obvious way to let the tool follow a route only the bundle
     names, which §A-REAL-NAVIGABLE licenses — and derived NAVIGATIONS begin firing; after which every
     request those documents make is graded `observed` by the engine, CORRECTLY, and this arm relayed that
     too. One arm added, TWO populations permitted, and the person's surface showed one.
     SO THE SECOND POPULATION HAS ITS OWN ROW AND THIS ARM NAMES BOTH. At every setting reachable today the
     outcome is BYTE-IDENTICAL, because every document is reached `observed` and the second conjunct is
     satisfied by every request that satisfied the first — which is the property that makes this auditable:
     the only requests that behave differently are the ones the old arm was permitting without being asked. */
  { when: [{ signal: "provenance", value: "observed" }, { signal: "doc-reach", value: "observed" }],
    why: "the page made exactly this request, in a document this browser actually navigated to — so " +
         "relaying it is this browser being a browser" },
  /* THE ARM ABOVE USED NOT TO EXIST, AND THE PARAGRAPH THAT SAID SO IS REWRITTEN HERE RATHER THAN DELETED
     — BECAUSE ITS REASONING IS SOUND, ONLY ITS PREMISE WAS OVERRULED, AND A READER WHO RE-DERIVES THE
     REASONING WILL RE-DERIVE THE REFUSAL WITH IT. It read: there is no third arm for a document's own
     `<img>`, and that is a decision rather than an omission; the arm that would carry one is the `observed`
     arm below, and what holds it is a fact the ENGINE states rather than a permission this file withholds.
     A markup `<img src>` satisfies the purpose clause on every word — the person's own browser makes that
     request, the bytes are served identically to every visitor, nothing about this person is revealed —
     and it was refused, which read as a line this file drew. It was not: the `observed` arm already named
     exactly that request and did not fire, because the request arrives graded `derived`.
     IT THEN NAMED TWO WIDENINGS AS THE APPARENT FIX AND REFUSED BOTH, AND EXACTLY ONE OF THE TWO REFUSALS
     SURVIVES. (1) IT SAID A DESTINATION-KEYED ARM IS UNSOUND AT EVERY ENGINE STATE, because the destination
     signal cannot separate the page loading itself from a code-composed data request: `<img src=…>` in
     markup, `new Image().src = …` and `document.createElement("img").src = …` all arrive with the same
     value of every signal this file reads, and an image address is a first-class exfiltration channel,
     `new Image().src = "/api/users/" + id`. THAT OBSERVATION IS EXACTLY RIGHT AND ITS PREMISE IS GONE. The
     premise was that a CODE-COMPOSED subresource must be gated, and
     CLAUDE.md §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS records the project owner overruling it in as many
     words — a subresource the page's own markup names, OR ITS OWN RUNNING CODE COMPUTES, is the page
     loading itself.
     So the three spellings arriving alike is no longer a defect in the signal; it is the answer, and the
     signal now says it. What the refusal was PROTECTING is protected by a different row anyway: a request
     on an arm nothing observed is graded `forced`, and this file's own `program` arm has permitted exactly
     that shape for a `<script src>` since it was written — so refusing `image` while permitting `script`
     closed one spelling of a channel it left open in another, which is furniture rather than a gate.
     WHAT THAT PARAGRAPH ALSO NAMED IS STILL LIVE AND IS THE REASON THE ARM ABOVE READS `subresource` AND
     NOT A DESTINATION WORD: spelled `{signal:"destination", value:"image"}` an arm does not survive
     `_signalRegistryCheck`, because this signal's value space is the three words it declares — and THAT
     REFUSAL POINTS AT THE DANGEROUS SPELLING, since `{signal:"destination", value:"value"}` passes every
     assert in this file and permits every data request at every origin, which is the whole opt-in deleted
     by one arm. An arm may name only a value the registry declares, which is what keeps the widening this
     file makes exactly as wide as the predicate it is derived from.
     (2) IT SAID A CONJUNCTION ARM `{destination:"image"} ∧ {provenance:"observed"}` IS SOUND AND INERT, and
     THAT IS STILL TRUE AND IS WHY THE ARM ABOVE IS KEYED ON THE DESTINATION ALONE: no request this engine
     composes for a subresource can be graded `observed`, so such an arm would read as a permission and
     match nothing for ever — `safeFetchWidenable`'s own recorded failure shape, one field over.
     THE ENGINE-SIDE CLAIM IS A CLAIM ABOUT ANOTHER FILE AND TRAVELS WITH ITS DERIVATION RATHER THAN AS A
     SENTENCE, because that is the half a reader must check before acting on any of this:
       git grep -n "parser_inserted" engine/host/solver/pending.c engine/host/solver/engine.c
     `pending_prov_compose` answers `PROV_OBSERVED` only for a park whose `parser_inserted` is set, and its
     own DCHECK confines that flag to the two `<script>` kinds, citing HTML §4.12.1.1 "Processing model",
     which gives a `parser document` to `script` elements and to nothing else. An `<img>` and a `<link>` park
     through `engine_pending_resource_url`, which passes the flag as a literal 0. So the grade is not wrong
     about §4.12.1.1; it is NARROWER than the definition `observed` states for itself, which is CLAUDE.md
     §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS with the one bit answering "is §4.12.1.1's parser document
     non-null" (read by `script_block_schedule` for scheduling, and script-only by the standard) and "does a
     real load of this document make exactly this request" (read here).
     AND THAT FILE'S OLD RETIREMENT CLAUSE NAMED A NEXT DIFF THAT IS NOW REFUTED, WHICH IS RECORDED AT THE
     PLACE THE CLAUSE WAS WRITTEN BECAUSE IT WOULD OTHERWISE GO ON INSTRUCTING EVERY LATER READER. It said
     this record goes when a request for a subresource the parser put in the document's own MARKUP reaches
     this file graded `observed`. Building that means a new engine-side fact carried to
     `pending_prov_compose` — `parser_inserted` itself cannot be widened, because the DCHECKF above aborts
     for any kind but the two `<script>` ones and §4.12.1.1 is right that it is script-only. The reason it
     is REFUTED rather than merely expensive is that a markup bit cannot express the OTHER HALF of the
     owner's sentence: a `<link>` or an `<img>` that page code CREATED is not parser-inserted at any
     definition, and "computed and fetched at runtime" is the half that names it. A grade keyed on markup
     would have permitted a document's own declared stylesheet and refused the one its router loads, which
     is the population this product is most for. The destination row is what can state both, and stating it
     there is what CLAUDE.md's own retirement clause for that paragraph names.
     WHAT THE ARGUMENT ABOVE ALSO RETIRED STAYS RETIRED AND IS KEPT HERE FOR THE SAME REASON. The `observed`
     arm's `why` used to end "refusing it would leave the engine unable to answer the document's own fetch",
     and the comment above this list named an OBSERVED request as "the page's own parked `fetch()` or XHR".
     The arm cannot reach either. The `fetch()`/XHR park passes the same literal 0 — its own comment reads
     "a `fetch()` or an XHR — page code composed it" — so `observed` is unreachable for that kind BY THE
     DCHECK above, not by accident. Measured on one document with a `<script src>`, a `<link rel=preload>`
     of each kind, an `<img src>`, a `fetch()` and two JS-composed images, reading the run's own
     `fetchCallSites`: the ONLY row graded `observed` was the markup `<script src>`.
     AND THE CONSEQUENCE FOR THIS LIST IS THAT THE `observed` ARM IS SUBSUMED BY THE FIRST ONE TODAY, which
     is said here so that nobody deletes it as dead: `observed` implies one of the two `<script>` kinds,
     both of which stamp `PENDING_DESTINATION_SCRIPT`, which `_isScriptLike` answers true for, which is the
     `program` arm. It permits nothing the first arm does not — and it is the arm that becomes load-bearing
     the instant the engine can state a true grade for a NAVIGATION, which the destination row cannot carry
     because `document` is not a subresource destination in §2.2.5 or in any reading of the owner's
     sentence. Deleting it would refuse every document this tool opens.
     RETIREMENT: this record goes when no reader can re-derive the destination-keyed refusal above from the
     signals this file declares — which is to say when the `subresource` value and the arm it feeds are no
     longer separable from the `program` one, and the paragraph has nothing left to be wrong about. */
  /* THE ARM BELOW DID NOT EXIST AND A RESIDUAL HERE SAID WHY, AND THAT RESIDUAL IS RETIRED BY THE DIFF
     THAT BUILT IT. What is kept is the part that stays true once the arm exists, which is a METHOD finding
     rather than a description of the tree.
     WHAT IT ESTABLISHED IS NOW `_actorOf`'s BANNER AND IS NOT REPEATED HERE: the analysed page's own
     `fetch()` and this tool's automatic discovery sweep are identical on every signal that describes the
     REQUEST, so the fact separating them had to be about the CALLER, and until the `actor` row existed the
     owner's two clauses could not both be spelled by any arm.
     WHAT IS KEPT IS THE CLAUSE THAT WAS WRONG, BECAUSE THE METHOD IS THE FINDING AND A READER WILL
     RE-DERIVE IT. That residual's own WHAT-THE-NEXT-DIFF-BUILDS first read `whether a flow in the analysed
     document is PARKED on this reply`, and an `xhr.send` record IS NOT A PARK — bridge.js says exactly that
     at its own site — so a signal keyed on parking would have ADMITTED the page's `fetch()` and REFUSED the
     page's XHR, which is the same backwards sort the clause existed to end. It was written an hour after
     the paragraph recording that sort, by its author, in the paragraph directly beneath it. CLAUDE.md rates
     this the worst failure a next-diff clause has, because a clause is read ONCE, by somebody who has
     already decided to do the work: a wrong one is not caught, it is EXECUTED. The park is where the ENGINE
     composes a witness; it was never the question of whose act a request is, and reaching for it was
     reasoning from the mechanism that happened to be in hand rather than from the population to be split.
     AND THE ROW THAT LOOKS LIKE A CHEAPER ANSWER MAY NOT BE USED, WHICH IS SAID HERE SO THAT NOBODY REACHES
     FOR IT ON THE DAY THE `actor` OPTION READS AS PLUMBING. `header-authority` does differ between the
     sweep and the page's `fetch()` — the sweep passes `headers || {}` and `{}` is truthy — and that is a
     property of ONE call site's argument-passing rather than of whether a request is a probe. It is wrong
     in outcome too: the XHR relay is a genuine page request that states a list, so the conjunct would
     refuse every XHR the analysed page makes.
     RETIREMENT: this record goes when no reader can re-derive a parking-keyed discriminator from the
     paragraphs around it — which is to say when whose-act is asked somewhere a park is not, and the two
     stop being confusable at a glance. */
  /* AND THE ANALYSED PAGE'S OWN DATA REQUESTS, WHICH IS A DECISION THE PROJECT OWNER MADE AND NOT ONE THIS
     FILE INFERRED, so it is cited rather than argued: permit unpinned value fetches — a data fetch whose
     address carries no value this path pinned fires at every origin — and anything whose address a fork
     pinned, and every derived probe, is still refused. Recorded under CLAUDE.md
     §AND-THAT-ABSOLUTE-IS-RETIRED-BY-THE-PROJECT-OWNER, whose model is signals a person decides per origin.
     THE ARM IS THE OWNER'S TWO CLAUSES AND EACH CONJUNCT CARRIES EXACTLY ONE OF THEM, which is what stops
     either being left to the other: `witness: unpinned` is what refuses an address a fork pinned, and
     `actor: page` is what refuses the probe. Neither is decoration and removing either one admits a
     population the other clause names.
     IT IS KEYED ON `pinned` AND NOT ON `forced`, WHICH IS THE OWNER'S LITERAL WORD AND IS NOT THE SAME SET.
     A page `fetch()` on a FORCED-but-unpinned path fires here: the flow stood past an arm nothing observed,
     but no source's value was DETERMINED, so the address holds no witness this engine chose — which is
     precisely what "carries no value this path pinned" says. Keying it on `forced` instead would refuse the
     boot of any SPA whose boot flow forks at all, which is the population the decision is about.
     AND `provenance` IS UNTOUCHED BY IT. §A-REQUEST-CARRIES-THE-PROVENANCE is explicit that what a
     permission changes is which ACT may be spent and never what a reply is WORTH: a forced reply is still
     carried as forced, still never merged into the observed pool, and every consumer downstream still sees
     that word. An arm makes something FIRE; it re-grades nothing.
     WHAT IT RE-GRADES IS ENUMERATED RATHER THAN LEFT TO BE FOUND, because a permission whose reach the
     person cannot state is not a control — the same obligation the subresource arm above discharges. Of the
     destinations §2.2.5 leaves in this row's `value` bucket, the only ones any caller can reach with a
     witness mark of `unpinned` AND an actor of `page` are the EMPTY STRING's, and there are TWO of them: the
     analysed page's own `fetch()`, relayed off the engine's pending line, and the analysed page's own XHR,
     relayed off the `xhr.send` record. A navigation never reaches this row at all, because its
     destination is one of §2.2.5's NAVIGATION set and the destination signal now says so — and THIS
     SENTENCE USED TO READ `a navigation is `document` and states `unstated``, which is kept because it was
     TRUE and a reader will re-derive it: the mark really is absent, so the enumeration really did hold. What
     was wrong is that it held BY AN ACCIDENT OF PLUMBING. CLAUDE.md §A-REAL-NAVIGABLE says the witness
     mark should travel and `engine_pinned_of_running_path()` already computes it, so the day that lands —
     a plumbing diff, correct and owed — this arm would have begun firing every derived child navigable at
     every origin, and this enumeration would have become false in the same commit with nothing anywhere
     saying so. `_isNavigation`'s banner carries the whole argument. The seed and the
     residue re-fetch state `tool`; the sweep and the peer gate state `tool`. So what this arm admits today is
     exactly the analysed document's own data requests — through either door — on a path that pinned nothing,
     and nothing else.
     THE SECOND OF THE TWO IS NEW AND IS A WIDENING A PERSON IS OWED THE WORD FOR. This enumeration read "the
     EMPTY STRING's: the analysed page's own `fetch()`" and named the XHR one clause further down as a seam
     that could not reach the arm, which was true while the record carried no mark; the mark is written now, so
     a page's `XMLHttpRequest` to an origin permitted for this row FIRES where it was refused on
     `witness=unstated` before. It is the SAME POPULATION by the owner's own rule — both are requests the
     analysed page's code made — which is the argument the residual below was built on, and the outcome is now
     what that argument always said it should be. What it does NOT widen: an XHR on a path that pinned a
     witness states `pinned` and is refused here exactly as a `fetch()` on such a path is.
     THE RESIDUAL THAT STOOD HERE IS BUILT, AND IT IS REWRITTEN RATHER THAN DELETED BECAUSE ITS ARGUMENT IS
     WHAT THE ENUMERATION ABOVE NOW RESTS ON. It read: "WHAT IS NOT COVERED: the analysed page's own XHR, which
     is the SAME population as the `fetch()` above by the owner's rule and does not fire, because
     `bridge.js`'s `xhr.send` relay states `witness: unstated` — correctly, since that record's provenance is a
     variable that can be `forced` and the zone cannot say whether the address holds this engine's bytes. WHAT
     THE NEXT DIFF BUILDS: `engine_pinned_of_running_path()` written into the XHR request op". That function
     exists, the op writes the mark, and the relay passes it — so the clause is spent and the population it
     named is in the enumeration above rather than in a residual beneath it.
     ITS ABSENCE-CLAUSE IS THE PART TO KEEP, INVERTED, BECAUSE IT IS THE WITNESS THAT THIS LANDED: the
     observation it named was "a run whose log carries a `blocked-signal:witness=unstated` refusal for an
     address the analysed document reached through `XMLHttpRequest`, on a document whose `fetch()`es to the
     same host are answered on the lines around it". That refusal is now unreachable for an XHR from a build
     that carries the key, so the same log line is what a WRONGLY-BUILT relay would still produce — an
     `unstated` on an XHR row means the artifact predates the `pinned` key, which is a fact about the BUILD,
     and `bridge.js`'s `in` test is what makes it that rather than a firing.
     AND WHAT FIRES HERE IS UNCREDENTIALED, WHICH IS STATED SO THAT NOBODY READS THIS ARM AS THE WHOLE OF THE
     OWNER'S SENTENCE. Their words were "same-origin and credentialed exactly as a browser is" (CLAUDE.md
     §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS; this quoted them as "like a browser", which is a PARAPHRASE IN
     QUOTATION MARKS — the marks carry an authority the words never earned, and the citation auditor reported
     it as a fetch §2.2.5 quotation because the nearest preceding anchor is a spec rather than the owner),
     and the relay that
     reaches this arm passes `credentialed: false` — so the reply is the LOGGED-OUT one, and an app whose
     boot data differs by session boots on the wrong payload. That is a decision in another file and is NOT
     this arm's to make; it is named here because a person reading what they permitted is entitled to know
     that the row they ticked does not spend their session. */
  { when: [{ signal: "destination", value: "value" },
           { signal: "witness", value: "unpinned" },
           { signal: "actor", value: "page" }],
    why: "the analysed page's own code made this request and composed its address out of nothing this " +
         "engine pinned — so what fires is the app asking for its own state, not a probe this tool built" },
  /* AND A CHILD NAVIGABLE THE PAGE'S OWN MARKUP NAMED OR ITS OWN CODE COMPUTED, WHICH IS A DECISION THE
     PROJECT OWNER MADE AND NOT ONE THIS FILE INFERRED, so it is cited rather than argued. CLAUDE.md
     §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS's discriminator is WHOSE ACT THE REQUEST IS and not what the
     reply becomes — "a subresource the page's own markup names, or its own running code computes, is THE
     PAGE LOADING ITSELF and is fetched exactly as a browser fetches it" — and a nested document the page's
     own `<iframe src>` names is that same fact ONE LEVEL UP: the person's own browser loaded it when they
     visited the page. The DECISION OWED that `_pinnedOf`'s residual and `_isNavigation`'s banner both named
     is answered in the AFFIRMATIVE and this arm is where it is answered.
     WHAT REFUSING IT COST IS THE SHAPE §NO-STUBS CALLS THE WORST ONE: no crash, a completed run, and EVERY
     FRAMED DOCUMENT IN EVERY APP never analysed — a feature-detected absence degrading to silence, one
     level of nesting up from the 467 refusals over 85 lazy chunks the `program` arm above carries as its
     own measurement. The refusal is a PARK, so it is not even a failure path a reader could find.
     IT IS NOT DESTINATION-KEYED AND MUST NOT BECOME SO, WHICH IS THE ASYMMETRY WITH THE TWO ARMS ABOVE AND
     IS THE ONE THING THE NEXT READER WILL TRY TO TIDY. Those two are destination-keyed ALONE and their own
     banners argue at length that a FORCED SEGMENT in a program's address does not carry it across the line.
     THAT ARGUMENT DOES NOT TRANSFER HERE, and the reason is not caution: a chunk whose path a forced
     equality pinned is still the app's own code, SERVED BYTE-IDENTICALLY TO EVERY VISITOR, revealing
     nothing whatever about this person — while a navigation to a route that exists only past a forced gate
     is a DOCUMENT ONLY THIS ENGINE EVER ASKED FOR, and a credentialed reply to it is the plausible
     fabrication CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE is entirely about. The bytes of a program are
     the same for everyone; the bytes of a document are not. So the forced path stays REFUSED and the
     per-origin widening is where it belongs.
     THE METHOD HALF IS ANSWERED HERE RATHER THAN LEFT TO BE RE-DERIVED. `init` below hardcodes
     `method: "GET"` and `_refuseUnreadOptions` refuses a caller that states one, and RFC 9110 §9.2.1 "Safe
     Methods" puts GET in its safe set — "the GET, HEAD, OPTIONS, and TRACE methods are defined to be safe".
     AND THAT IS A SIGNAL ABOUT INTENT AND NEVER A GUARANTEE ABOUT CONSEQUENCE, which §9.2.1 says in its own
     words: the definition "does not prevent an implementation from including behavior that is potentially
     harmful, that is not entirely read-only, or that causes side effects while invoking a safe method".
     That is exactly why the DESTRUCTIVE DENY LIST still stands BEHIND this arm rather than being satisfied
     by it — and this arm REACHES it rather than bypassing it: the list is scoped `credentialed && provenance
     !== "observed"`, which a same-origin `derived` child navigable satisfies, so a population that was
     refused upstream of that gate now passes THROUGH it. The deny list's reach grows with this diff; it
     does not shrink.
     THERE IS NO `observed` TWIN AND WRITING ONE WOULD BE INERT, WHICH IS THE TRAP CLAUDE.md
     §AND-A-CONJUNCTION-WITH-`provenance=observed`-WOULD-BE-INERT NAMES AND THE `_signalRegistryCheck`
     ABOVE CANNOT CATCH — it refuses an unknown NAME and an undeclared VALUE, and `observed` is a declared
     value of a declared signal, so an arm naming it would sit in this list reading as a permission and
     match nothing for ever. The derivation, because it is a claim about another file:
       git grep -n 'engine_prov_of_running_path' engine/host/solver/engine.c
     answers `return f != NULL && flow_path_forced(f) ? PROV_FORCED : PROV_DERIVED;` — TWO of the three
     words, never `PROV_OBSERVED` — and every `actor: page` navigation this tree has takes its word from
     there (`core/frame/navigable.c`'s three notices through `bridge.js`'s `fetchedDocument`, and
     `solver/route_seed.c`, whose own comment says "a real load of this document makes no pushState, so
     `observed` is unreachable here"). The one act that DOES state `observed` for a navigation is the
     AMBIENT SEED, and it states `actor: tool` — so the conjunction below could not admit it either, and it
     does not need to: the `provenance`/`doc-reach` arm above already fires it, which is why this tool can
     analyse anything at all.
     AND THERE IS NO `witness` CONJUNCT, FOR TWO INDEPENDENT REASONS AND EITHER WOULD BE ENOUGH. It would be
     INERT TODAY: `bridge.js`'s navigation relay types `pinned: "unstated"` because the provenance arrives on
     a NOTICE and a notice is not a park, so an arm naming `unpinned` matches nothing for ever — the same
     trap one field over. And it would be REDUNDANT THE DAY THE MARK TRAVELS: `_firingRefusal` ASSERTS
     `facts.provenance === "forced" || facts.pinned !== "pinned"`, because solver/flow.h declares
     `path_pinned` strictly inside `path_forced` — so `provenance: derived` ALREADY excludes a pinned
     address, by an invariant this file checks rather than one it hopes for.
     THE CONSEQUENCE IS THAT THE HAZARD `_isNavigation` WAS WRITTEN AGAINST STAYS CLOSED, which is said here
     because it is the first thing a reader of that banner will worry about. No arm a navigation can take
     reads the `witness` row at all, so landing the plumbing CLAUDE.md §A-REAL-NAVIGABLE calls owed changes
     NO firing outcome for any navigation at any setting — it changes only the per-origin ROW a person sees,
     which they permit separately. The decision is in the table and no unrelated field can move it.
     AND THERE IS NO `doc-reach` CONJUNCT, WHICH IS A DECISION AND NOT AN OMISSION. At the default table a
     document's reach grade can only be `observed` (the ambient seed) or `derived` (this arm), because the
     only other word is `forced` and this arm refuses it — so a `forced`-reach document exists only at an
     origin a person DELIBERATELY WIDENED, and refusing its child navigables would be re-litigating their
     decision one level down. It is also what makes frames NEST: a direct child of the seed reads
     `doc-reach=observed` and a grandchild reads `derived`, so any single conjunct would refuse one of the
     two. The arm above needed its `doc-reach` row because it names no destination and would otherwise
     re-grade a second population; this one names `navigation` and admits navigations and nothing else.
     AND THERE IS NO SAME-ORIGIN CONJUNCT, WHICH IS CORRECT AND NOT FOR THE REASON A READER WILL REACH FOR —
     RECORDED BECAUSE THE WRONG REASON IS A GATE THAT IS STRUCTURALLY SILENT ABOUT THIS ARM'S OWN
     CROSS-ORIGIN POPULATION. The tempting account is that this file's SOP/CORS check enforces it anyway, so
     an arm needs no conjunct and adding one would be a second copy. The CONCLUSION holds and that account
     is NOT what carries it: the credentialed SOP/CORS gate below runs AFTER THE WIRE and only
     `if (credentialed)`, and the cross-origin navigations this arm admits are UNCREDENTIALED, so that gate
     never runs for them at all. What actually carries it is one line at the CALLER — `bridge.js`'s
     `navigationCarriesSession` is `_isRealOrigin(principalOrigin) && originOf(absUrl) === principalOrigin`,
     a SAME-ORIGIN TEST computed per call, so a cross-origin child navigable asks for no cookies in the
     first place. A reader who took the other account would be resting a policy on a gate that cannot see
     the population, which is CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE one level out.
     SO WHAT FIRES CROSS-ORIGIN HERE IS AN UNCREDENTIALED GET, WHICH IS STRICTLY NARROWER THAN A BROWSER —
     a real browser DOES send cookies to a cross-origin `<iframe src>` — and it is exactly what the two
     destination-keyed arms above already do for a CDN chunk or a third-party font. It still passes the
     scheme allowlist, the userinfo refusal, the private-network gate on the initial AND post-redirect URL,
     and this file's own post-redirect re-ask of this very walk.
     WHAT IT RE-GRADES IS ENUMERATED RATHER THAN LEFT TO BE FOUND, because a permission whose reach the
     person cannot state is not a control — the same obligation the two arms above discharge. §2.2.5's
     NAVIGATION set is five destinations (`document`, `embed`, `frame`, `iframe`, `object`) and every one of
     them now fires at every origin for an `actor: page` request on a path that forced nothing; of those
     this engine can STATE exactly ONE today, `document`, which is what HTML §7.4.5 "Populating a session
     history entry" gives the navigate algorithm's own fetch and what `bridge.js`'s navigation relay types.
     The other four are destinations no caller in this tree composes, so this arm's reach is one value wider
     than its population and the four are named rather than discovered.
     AND `provenance` IS UNTOUCHED BY IT, which is the arm above's sentence and is owed here too:
     §A-REQUEST-CARRIES-THE-PROVENANCE is explicit that a permission changes which ACT may be spent and
     never what a reply is WORTH, so a `derived` document's reply is still carried as derived, is still
     never merged into the observed pool, and every consumer downstream still sees that word. An arm makes
     something FIRE; it re-grades nothing.
     RETIREMENT: this record goes when `_signalRegistryCheck` REFUSES a default arm that names a value no
     caller in this tree can state — an arm-reachability check over the vectors the trusted zones type —
     because the two INERT-conjunct arguments above are then enforced by the host's own startup instead of
     by this paragraph, and an arm that permits nothing for ever is a build failure rather than a sentence
     somebody has to read.
     MEASURED ABSENT WITH THE COMMAND, SO THIS CONDITION IS NOT BORN MET — AND THE FIRST SPELLING OF THAT
     MEASUREMENT WAS WRONG IN A WAY THAT IS GUARANTEED RATHER THAN UNLUCKY, WHICH IS WHY THE COMMAND BELOW
     MATCHES A CONSTRUCT AND NOT A NAME. CLAUDE.md §AND-THE-THIRD-BAD-CONDITION requires the mechanism a
     retirement condition names to be GREPPED at the moment the condition is written, so that a condition
     cannot be born met; CLAUDE.md §AND-A-COUNT-OVER-SOURCE-TEXT says a count of a NAME counts every
     MENTION, prose included. Those two collide HERE BY CONSTRUCTION and not by accident: a condition must
     NAME its mechanism in order to be a condition at all, so the prose that discharges the first rule is
     itself the hit that defeats the second — `grep -c armReachable` over this file answered 0 before this
     paragraph existed and answers 1 afterwards, and the 1 is this sentence. A reader who takes that for a
     presence concludes the check is built and retires a live record; a reader who takes it for an absence
     is right by luck.
     SO THE COMMAND MATCHES THE DECLARATION, WHICH PROSE DOES NOT WRITE, AND IT IS READ AT A REVISION WHERE
     THIS PARAGRAPH DOES NOT EXIST — because matching the construct closes the SUBJECT and leaves the
     CONTROL open, which the first repair of this clause demonstrated on itself: naming the control inside
     the printed command moved that control from 1 to 2, one level in from the defect the sentence above
     records. The only reading neither half can contaminate is one taken where the condition is not written.
       git show origin/main:extension/lib/safe-fetch.js > /tmp/sf-main.js
       grep -cE 'function (armReachable|_armReachability|safeFetchArmReachable)' /tmp/sf-main.js   # 0
       grep -cE 'function _signalRegistryCheck' /tmp/sf-main.js                                   # 1, armed
     SO THE GENERAL RULE A CONDITION OWES ITSELF IS THAT ITS GREP NAMES A CONSTRUCT AND ITS REVISION IS NOT
     THE ONE IT IS LANDING IN — and the tell is free and needs no second command: your condition's own text
     contains the string your condition greps for. */
  { when: [{ signal: "destination", value: "navigation" },
           { signal: "actor", value: "page" },
           { signal: "provenance", value: "derived" }],
    why: "the analysed page's own markup named this nested document or its own code computed the address — " +
         "so the person's own browser loaded it when they visited the page, and what fires is the page " +
         "loading itself one level down rather than a route only this engine ever asked for" }
];
var _EXPLORED = Object.create(null);
/* `_EXPLORED` is the per-origin egress table, origin → signal → value → true. Whether a host has stated it
   is a separate field, because an unstated table is empty and an empty one reads as policy. */
var _EXPLORED_STATED = false;
/* A host's promise to state the table. A host whose grants are in IndexedDB cannot state them until a read
   returns, and a request in that window may be answered neither from the empty table nor by stating one
   (which the restore would then replace), so `safeFetch` awaits this promise. The promise is the host's:
   this file reads no store and invents no default. `null` means no promise was made; the native host states
   synchronously, and a host that does neither still reaches the DCHECK in `_firingRefusal`. */
var _EXPLORED_STATING = null;
/* Whether an origin can be permitted at all, answering the reason it cannot or null. Two callers, two
   outcomes: `safeFetchPermit` aborts, because a caller inside this project passing a bad origin is broken;
   a surface with a person in front of it refuses with the reason, because an opaque origin is an ordinary
   state of the web and a fatal there would give any sandboxed page an abort switch on the trusted zone.
   The value must be its own serialized origin (parsed, re-serialized, compared), because `_isRealOrigin`
   only tests for "://" and a full address would be a key no request's `.origin` ever equals: a grant that
   reads as granted and matches nothing. `ws:`, `wss:` and `ftp:` origins serialize back to themselves, so
   the http(s) test is what refuses them; this file refuses those schemes before the wire anyway. `null` does
   not parse, so an opaque origin is refused by the parse. */
function safeFetchWidenable(origin) {
  var normalized = null;
  try { normalized = new URL(String(origin)).origin; } catch (e) { RETHROW_FATAL(e); normalized = null; }
  if (!_isRealOrigin(origin) || normalized !== origin) return "not-an-origin";
  if (origin.indexOf("http://") !== 0 && origin.indexOf("https://") !== 0) return "not-http";
  return null;
}
/* Whether a signal name and value are ones this file knows, as a reason or null, split for the same two
   callers as `safeFetchWidenable`. */
function safeFetchSignalUsable(signal, value) {
  var i;
  for (i = 0; i < _SIGNALS.length; i++) {
    if (_SIGNALS[i].name !== signal) continue;
    if (!_SIGNALS[i].gates) return "does-not-gate";
    return _SIGNALS[i].values.indexOf(value) >= 0 ? null : "not-a-value";
  }
  return "not-a-signal";
}
/* The registry for a surface that renders one row per signal: name, grade and value space, copied. The `of`
   functions stay here, so a surface cannot compute a value the walk did not decide from. */
function safeFetchSignals() {
  return _SIGNALS.map(function (s) {
    return { name: s.name, gates: s.gates, certainty: s.certainty, values: s.values.slice() };
  });
}
/* A host's promise that it will state the table (see `_EXPLORED_STATING`), registered once at the host's
   load. `safeFetch` awaits it before the firing question, so the guarantee belongs to the chokepoint rather
   than to whichever door a request came through. A host that already holds its answer calls
   `safeFetchEgressStated` instead; this hands over the read itself, not a placeholder. */
function safeFetchEgressStating(promise) {
  CHECK(promise !== null && typeof promise === "object" && typeof promise.then === "function",
        "a host registered something other than a thenable as its promise to state the per-origin egress " +
        "table — every request this zone makes waits on it before the firing question is asked, so a value " +
        "with no `then` would be awaited once, settle immediately, and hand the walk the empty table this " +
        "promise exists to keep it away from");
  CHECK(!_EXPLORED_STATED,
        "a host promised to state its per-origin egress table after it had already stated one — the promise " +
        "is what a request waits on BEFORE the table exists, and one registered afterwards can only be a " +
        "second statement in flight, which REPLACES rather than adds and would revoke every grant a surface " +
        "made in between");
  CHECK(_EXPLORED_STATING === null,
        "a host promised twice to state its per-origin egress table — a waiting request awaits ONE promise, " +
        "so a second would leave which table that request is answered from decided by which registration " +
        "happened to win");
  _EXPLORED_STATING = promise;
}
/* The default arms, for a surface explaining why an app's scripts load before anything is permitted; a
   surface that explained it in its own words would be a second copy of the list. */
function safeFetchDefaultArms() {
  return _DEFAULT_ARMS.map(function (a) {
    return { when: a.when.map(function (c) { return { signal: c.signal, value: c.value }; }), why: a.why };
  });
}
/* The host states the table once, before anything may ask it: restored grants, or the empty table as the
   positive "nobody has permitted anything" (the native host, whose command line is a sentence for one run).
   A second call aborts, since it would replace grants a surface added since; later grants go through
   `safeFetchPermit`.
   The shape is `{ origin: { signal: [values] } }`. An origin whose entry is not an object is a legacy
   single-switch grant: carrying it forward would permit every value of every signal, including signals that
   did not exist when it was granted, so it is dropped and named in the answer. An origin with a signal whose
   values are not an array is named too; the signals walked before it stay permitted and the rest are
   skipped. A signal a stored grant does not name is not permitted, so adding a signal narrows every existing
   grant rather than silently widening it. */
function safeFetchEgressStated(table) {
  var origin, signal, values, why, i, legacy = [];
  CHECK(!_EXPLORED_STATED,
        "a host stated its per-origin egress table a second time — the table is the person's standing " +
        "sentence and a re-statement REPLACES it, so a grant made from a surface between the two calls is " +
        "revoked with nothing anywhere saying so. State it ONCE — synchronously before this zone can run, or " +
        "through the read registered with safeFetchEgressStating, which every request awaits — and add to it " +
        "with safeFetchPermit");
  _signalRegistryCheck();
  CHECK(table !== null && typeof table === "object" && !Array.isArray(table),
        "a host stated its per-origin egress table as " + JSON.stringify(table) + " rather than as an " +
        "origin-keyed object — an ARRAY is the shape the previous single-switch control persisted and it " +
        "cannot be read as this one, because a bare origin meant `permit everything` including the signals " +
        "that did not exist when it was written. This is the whole of what the chokepoint knows about what " +
        "a person has permitted, and a value it cannot walk would leave the table empty while the host " +
        "believed it had spoken");
  for (origin in table) {
    if (!Object.prototype.hasOwnProperty.call(table, origin)) continue;
    why = safeFetchWidenable(origin);
    CHECK(why === null,
          "a host stated " + JSON.stringify(origin) + " as a permitted origin and it is not one (" + why +
          ") — the entry would sit in this table matching no request's `.origin` for ever, which is a " +
          "permission that was granted, reads as granted, and refuses nothing. A host is the only writer of " +
          "its own store, so this is that store corrupted rather than a person mistyping");
    if (typeof table[origin] !== "object" || table[origin] === null || Array.isArray(table[origin])) {
      legacy.push(origin);
      continue;
    }
    for (signal in table[origin]) {
      if (!Object.prototype.hasOwnProperty.call(table[origin], signal)) continue;
      values = table[origin][signal];
      if (!Array.isArray(values)) { legacy.push(origin); break; }
      for (i = 0; i < values.length; i++) {
        /* A stored value this build no longer knows is dropped, not asserted: a renamed signal or a retired
           value is an upgrade, an unknown value permits nothing, and aborting would brick every profile that
           ever granted anything. This is the one place this file reads its own store as input. */
        if (safeFetchSignalUsable(signal, values[i]) !== null) continue;
        if (!_EXPLORED[origin]) _EXPLORED[origin] = Object.create(null);
        if (!_EXPLORED[origin][signal]) _EXPLORED[origin][signal] = Object.create(null);
        _EXPLORED[origin][signal][values[i]] = true;
      }
    }
  }
  _EXPLORED_STATED = true;
  return legacy;
}
/* One signal, one value, at one origin: the door a per-signal surface writes through, deliberately not a
   batch, so what a person reads back is the table in force. `allow` false removes the value, because a
   permission with no withdrawal is a one-way door. */
function safeFetchPermit(origin, signal, value, allow) {
  var why;
  CHECK(_EXPLORED_STATED,
        "an egress permission was changed before this host stated its table — the statement is what restores " +
        "the person's earlier grants, so a grant that ran first would be REPLACED by that restore and the " +
        "person would watch a permission they had just made disappear");
  why = safeFetchWidenable(origin);
  CHECK(why === null,
        "an egress permission was asked for at " + JSON.stringify(origin) + ", which cannot be permitted (" +
        why + ") — the table is compared against a request URL's own `origin`, so a full address, an opaque " +
        "`null`, an explicit default port, a non-http(s) scheme or an empty string would sit here matching " +
        "nothing while reading as a permission somebody granted. A surface with a person in front of it asks " +
        "`safeFetchWidenable` and tells them why; a caller inside this project passing one is broken");
  why = safeFetchSignalUsable(signal, value);
  CHECK(why === null,
        "an egress permission was asked for the signal " + JSON.stringify(signal) + " at the value " +
        JSON.stringify(value) + ", which this file cannot use (" + why + ") — a permission naming a signal " +
        "or a value the firing walk never asks about is one that reads as granted and refuses nothing, and " +
        "a surface asks `safeFetchSignalUsable` before it offers a person the row");
  if (allow) {
    if (!_EXPLORED[origin]) _EXPLORED[origin] = Object.create(null);
    if (!_EXPLORED[origin][signal]) _EXPLORED[origin][signal] = Object.create(null);
    _EXPLORED[origin][signal][value] = true;
    return;
  }
  if (_EXPLORED[origin] && _EXPLORED[origin][signal]) delete _EXPLORED[origin][signal][value];
}
/* Permit every value of every gating signal at one origin: the coarse sentence, meant by
   `engine/trusted.mjs`'s `--explore <origin>` and by the popup's one-click allow. It is derived from the
   registry, so a signal added later is included. That is the opposite of the stored-grant rule above, and
   deliberately: a grant made now through a control that says "everything" does mean the new signal. */
function safeFetchWiden(origin) {
  var i, j;
  for (i = 0; i < _SIGNALS.length; i++) {
    if (!_SIGNALS[i].gates) continue;
    for (j = 0; j < _SIGNALS[i].values.length; j++)
      safeFetchPermit(origin, _SIGNALS[i].name, _SIGNALS[i].values[j], true);
  }
}
/* The person takes a whole origin back. Answers whether anything was there, so a surface reports what it
   did rather than a revocation of nothing. */
function safeFetchUnwiden(origin) {
  var had = _EXPLORED[origin] !== undefined;
  CHECK(_EXPLORED_STATED,
        "an egress permission was revoked before this host stated its table — the revocation would be " +
        "undone by the restore that follows it, and the person would watch a permission they had just " +
        "withdrawn come back");
  delete _EXPLORED[origin];
  return had;
}
/* The permitted origins, for a caller that asserts their absence where its own reasoning relies on it. */
function safeFetchWidenedOrigins() {
  CHECK(_EXPLORED_STATED,
        "the permitted-origin list was read before this host stated its table — an empty answer would be " +
        "read as `nobody has permitted anything`, which is a statement this host has not yet made, and a " +
        "surface rendering it would tell a person their standing grants are gone");
  return Object.keys(_EXPLORED);
}
/* The table in the shape `safeFetchEgressStated` takes, so a host persists what the chokepoint uses; the
   round trip is the contract. */
function safeFetchEgressTable() {
  var out = Object.create(null), origin, signal;
  CHECK(_EXPLORED_STATED,
        "the egress table was read before this host stated it — an empty answer would be persisted as `this " +
        "person has permitted nothing`, which would write over every standing grant with a statement this " +
        "host had not yet made");
  for (origin in _EXPLORED) {
    if (!Object.prototype.hasOwnProperty.call(_EXPLORED, origin)) continue;
    out[origin] = Object.create(null);
    for (signal in _EXPLORED[origin]) {
      if (!Object.prototype.hasOwnProperty.call(_EXPLORED[origin], signal)) continue;
      out[origin][signal] = Object.keys(_EXPLORED[origin][signal]);
    }
  }
  return out;
}
/* One origin's permissions row by row, with `false` for an unpermitted value rather than an absent key,
   because a checkbox rendered from a missing key looks like a denial and is a different fact. */
function safeFetchPermitted(origin) {
  var out = [], i, j, row;
  CHECK(_EXPLORED_STATED,
        "one origin's egress permissions were read before this host stated its table — every row would " +
        "answer `not permitted`, which is a sentence this host has not yet earned the right to say");
  for (i = 0; i < _SIGNALS.length; i++) {
    row = { name: _SIGNALS[i].name, gates: _SIGNALS[i].gates, certainty: _SIGNALS[i].certainty, values: [] };
    for (j = 0; j < _SIGNALS[i].values.length; j++)
      row.values.push({ value: _SIGNALS[i].values[j],
                        permitted: !!(_EXPLORED[origin] && _EXPLORED[origin][_SIGNALS[i].name] &&
                                      _EXPLORED[origin][_SIGNALS[i].name][_SIGNALS[i].values[j]] === true) });
    out.push(row);
  }
  return out;
}
/* The firing decision, answering "signal=value" for the first gating signal this origin does not permit, or
   null to fire. A default arm whose every condition matches fires first; otherwise every gating signal's
   value must be permitted at the request URL's origin.
   It answers a signal and value, never a score, so a person reading a frontier that will not drain is told
   which fact held it; the order is the registry's, not a ranking of harm. A permission changes which act may
   be spent, never what a reply is worth: `forced` still marks a reply as evidence about a request no client
   makes. Wherever this refuses, the address is still derived in full and reported. */
function _firingRefusal(facts) {
  var v, i, s;
  /* `_signalVector` requires the facts, so the vector is computed first and the nesting CHECK below prints
     validated operands. */
  v = _signalVector(facts);
  /* The nesting, asserted at the consumer: solver/flow.h declares `path_pinned` strictly inside
     `path_forced`, so a request that is not `forced` cannot carry a witness this engine chose
     (`pending_pinned_compose` asserts the other end). */
  CHECK(facts.provenance === "forced" || facts.pinned !== "pinned",
        "a request states that its address may rest on a witness this engine DETERMINED, while stating a " +
        "provenance of `" + facts.provenance + "` — solver/flow.h declares the witness mark strictly nested " +
        "inside the forced-path bit, so this pair cannot both be true and one of the two producers is wrong");
  /* No pairing of `provenance` with `doc-reach` is asserted. `observed` with a `forced` reach is the tempting
     one to forbid, and it is exactly a page this tool chose to open making its own `fetch()`, which the
     engine correctly grades `observed`. All nine pairs are reachable and distinct: the request's grade is the
     engine's, the reach grade is this zone's, and neither bounds the other. */
  /* Defaults first: they are permissions and the table only widens, so the order changes no answer. */
  for (i = 0; i < _DEFAULT_ARMS.length; i++)
    if (_DEFAULT_ARMS[i].when.every(function (c) { return v[c.signal] === c.value; })) return null;
  /* An unstated table is empty, so the walk would refuse a permitted origin in the policy's own voice. That
     is conservative, hence a DCHECK, and unreadable, hence asserted at all. `safeFetch` awaits
     `_EXPLORED_STATING` first, so this checks that construction; a host that neither states nor promises
     still reaches it. The table is read for every request no default arm admits, including plain data GETs
     the offscreen composes with no engine, so a statement placed at a per-document door could not close
     this window. */
  DCHECK(_EXPLORED_STATED,
         "the firing question was asked before this host STATED its per-origin egress table — the table is " +
         "empty until a host speaks, so this refusal would tell a person their own standing permission does " +
         "not exist, in the policy's own voice and indistinguishable from a policy they set. A host either " +
         "STATES the table before this zone can run (the native host, before it reads `--explore`) or " +
         "REGISTERS the read that will state it (`safeFetchEgressStating`, which every request awaits), so " +
         "reaching here unstated is a host that did NEITHER — not a door that was never told");
  for (i = 0; i < _SIGNALS.length; i++) {
    s = _SIGNALS[i];
    if (!s.gates) continue;
    if (_EXPLORED[facts.url.origin] && _EXPLORED[facts.url.origin][s.name] &&
        _EXPLORED[facts.url.origin][s.name][v[s.name]] === true) continue;
    return s.name + "=" + v[s.name];
  }
  return null;
}
/* The same answer for a caller whose act is not a fetch: `bridge.js`'s route-declaration arm, which records a
   work item loaded rounds later, and `engine/trusted.mjs`'s `navigate`, which must decline rather than
   produce the empty Document a network refusal would. It answers the signal that refused, so a host can say
   why without re-deriving the policy; the sentence around it is the host's own. It takes an absolute URL
   (one that will not parse throws: that is a caller's serializer disagreeing with a URL parser) and a facts
   object rather than positional arguments, so a new signal is a key and not a shifted operand. Every fact is
   required, because a hypothetical answered from fewer facts is about a different request. */
function safeFetchFiringRefusal(facts) {
  CHECK(facts !== null && typeof facts === "object" && typeof facts.url === "string",
        "safeFetchFiringRefusal was asked without an absolute URL — the origin comparison is this file's to " +
        "make and a caller that parsed one for itself would be the second copy of that rule");
  return _firingRefusal({ url: new URL(String(facts.url)), destination: facts.destination,
                          provenance: facts.provenance, pinned: facts.pinned, docReach: facts.docReach,
                          actor: facts.actor,
                          credentialed: facts.credentialed, headers: facts.headers });
}
/* The vector for a hypothetical request, for a surface that shows a person what they are deciding about. It
   is the same derivation the firing walk reads, so the rows a person ticks are about the request the policy
   sees. */
function safeFetchSignalVector(facts) {
  CHECK(facts !== null && typeof facts === "object" && typeof facts.url === "string",
        "the signal vector was asked for without an absolute URL — `url-authority` is read off the parsed " +
        "address, so a caller with none would be shown a row about nothing");
  /* Every fact is forwarded, `docReach` included; a field dropped here would compute outside its row's value
     space and render a row about nothing. */
  return _signalVector({ url: new URL(String(facts.url)), destination: facts.destination,
                         provenance: facts.provenance, pinned: facts.pinned, docReach: facts.docReach,
                         actor: facts.actor,
                         credentialed: facts.credentialed, headers: facts.headers });
}
function _corbDeniesScript(mime, nosniff, sniff, sameOrigin) {
  /* CORB for a script-like load, over facts already computed, answering the rule that refused or null, so
     the status message names which of four rules fired. Same-origin: the page's own data is its to read, and
     only a protected non-JavaScript type reaching a code loader is refused. */
  if (sameOrigin)
    return _corbProtectedMime(mime) && !_jsMime(mime) ? "same-origin-protected" : null;
  if (_corbProtectedMime(mime)) return "protected-type";      // CORB-protected type
  if (nosniff && !_jsMime(mime)) return "nosniff-not-js";      // browser blocks too
  if (sniff.protected) return "sniffed-data";                  // mislabeled data
  return null;
}

// Private, loopback and link-local classification for the origin-relative SSRF rule: RFC 1918, 127/8,
// 169.254/16, `0.0.0.0`, `::`, `::1`, IPv6 unique-local (fc00::/7) and `fe80:` link-local, `localhost`,
// `.local` and `.localhost`. A request is refused only when the target is private and the page principal is
// not; private to private and anything to public are allowed, as on the web.
//
// Every host here comes from the WHATWG URL parser, so it is canonical: IPv4 is dotted-quad (the parser folds
// decimal, octal and hex spellings, so `http://2130706433/` is `127.0.0.1`), and IPv6 is the serializer's
// shortest lowercase hex with no dotted tail (`[::ffff:127.0.0.1]` arrives as `[::ffff:7f00:1]`). RFC 4291
// §2.5.5.2's IPv4-mapped and §2.5.5.1's IPv4-compatible forms denote an IPv4 address, so `_v4OfIPv6`
// converts them and the IPv4 rules classify them once.
function _v4OfIPv6(h) {
  var m = /^::(?:ffff:)?([0-9a-f]{1,4}):([0-9a-f]{1,4})$/.exec(h);
  if (!m) return null;
  var hi = parseInt(m[1], 16), lo = parseInt(m[2], 16);
  return ((hi >> 8) & 255) + "." + (hi & 255) + "." + ((lo >> 8) & 255) + "." + (lo & 255);
}
function _isPrivateHost(host) {
  if (!host) return false;
  host = String(host).toLowerCase().replace(/^\[|\]$/g, "");
  /* The canonical form is the contract: a literal with both a colon and a dot is the dotted IPv4-in-IPv6
     text no URL parser emits, so its caller passed a raw string instead of `URL.hostname`. */
  DCHECK(!(host.indexOf(":") >= 0 && host.indexOf(".") >= 0),
         "a host reached the SSRF classifier as an IPv4-in-IPv6 literal with a dotted tail (" + host + ") — " +
         "the WHATWG IPv6 serializer emits hex pieces only, so this host did not come from URL.hostname and " +
         "is about to be classified by rules written for the form that one produces");
  var v4 = _v4OfIPv6(host);
  if (v4) host = v4;
  return host === "localhost" || host === "0.0.0.0" || host === "::" || host === "::1" ||
    host.endsWith(".local") || host.endsWith(".localhost") ||
    /^127\./.test(host) || /^169\.254\./.test(host) || /^10\./.test(host) ||
    /^192\.168\./.test(host) || /^172\.(1[6-9]|2\d|3[01])\./.test(host) ||
    /^(fe80:|fc[0-9a-f][0-9a-f]:|fd[0-9a-f][0-9a-f]:)/.test(host);
}
/* The destructive-path deny list: the one place this project matches on a name, and it matches only to
   refuse. A wrong deny costs one unfired request that forced execution still derives and reports, while a
   wrong assert would fabricate a finding that propagates, so over-broad is its cheap direction.
   Forced execution builds requests no real client makes, and RFC 9110 §9.2.1 "Safe Methods" leaves keeping
   GET side-effect-free to the resource owner while naming the failure: "unfortunate side effects when
   automated processes perform a GET on every URI reference". A GET that ends the person's session
   mid-analysis is a CSRF this tool commits against its own user, so the list applies where the session is
   spent (credentialed, and not `observed`; see `safeFetch`). A path that does not match is not thereby safe:
   the list is a floor under the egress policy and under this tool's own autonomy, never a substitute.
   Matching is by whole token: each path segment and query token is tested raw and with `-`, `_` and `.`
   removed (`log-out` reaches `logout`; `/deleted-items` does not reach `delete`). Tokens are taken raw and
   after each percent-decoding pass, because a URL parser only encodes and a server treats `%6F` as `o` (RFC
   3986 §6.2.2.2 "Percent-Encoding Normalization"). Decoding reserved octets such as `%2F` goes beyond
   §6.2.2.2's normalization, which is safe here because an extra token can only refuse one more request. */
var _DESTRUCTIVE = [
  /* ending a session or revoking an authorization */
  "logout", "logoff", "signout", "signoff", "deauth", "deauthorize", "revoke",
  "endsession", "destroysession", "invalidate", "unsubscribe", "optout",
  /* destroying or disabling a resource or an account */
  "delete", "destroy", "remove", "purge", "erase", "wipe", "truncate",
  "deactivate", "disable", "terminate", "cancel", "reset",
  "unlink", "unfollow", "unfriend", "deleteaccount", "closeaccount"
];
/* An entry that could never match is a silent hole, so its shape is asserted: the matcher compares
   lowercased, separator-stripped tokens, and an entry with an uppercase letter or a separator would match
   nothing. */
var _DESTRUCTIVE_SET = (function () {
  var m = Object.create(null);
  for (var i = 0; i < _DESTRUCTIVE.length; i++) {
    DCHECK(/^[a-z0-9]+$/.test(_DESTRUCTIVE[i]),
           "a destructive-path token is compared against lowercased separator-stripped " +
           "tokens, so one carrying an uppercase letter or a separator can never match — " +
           "it would be a gate entry that looks protective and refuses nothing: " +
           _DESTRUCTIVE[i]);
    m[_DESTRUCTIVE[i]] = true;
  }
  return m;
})();
/* URL Standard §1.3 "Percent-encoded bytes"' percent-decode, one pass. It is that algorithm rather than
   `decodeURIComponent` because it cannot fail: a `%` not followed by two hex digits is data, where
   `decodeURIComponent` throws, and a catch in front of this gate would read "could not evaluate" as "no
   token matched". It stops at bytes: each triplet becomes one code unit with that byte's value, which is
   enough because the token class is ASCII and any other byte is a separator. Both hex cases are accepted,
   as RFC 3986 §2.1 "Percent-Encoding" makes them equivalent. */
function _percentDecode(s) {
  var out = "";
  for (var i = 0; i < s.length; i++) {
    var c = s.charAt(i);
    if (c !== "%" || i + 2 >= s.length || !/^[0-9a-fA-F]{2}$/.test(s.substr(i + 1, 2))) { out += c; continue; }
    out += String.fromCharCode(parseInt(s.substr(i + 1, 2), 16));
    i += 2;
  }
  return out;
}
/* One form's tokens, so the raw and decoded forms share one matcher. */
function _destructiveIn(form) {
  var parts = form.toLowerCase().split(/[^a-z0-9._-]+/);
  for (var i = 0; i < parts.length; i++) {
    var t = parts[i];
    if (!t) continue;
    if (_DESTRUCTIVE_SET[t]) return t;
    var squashed = t.replace(/[-._]/g, "");
    if (squashed && _DESTRUCTIVE_SET[squashed]) return squashed;
  }
  return "";
}
/* Answers the token that refused this URL, or "" as the positive statement that nothing matched. The token
   travels in the status message so a reviewer can dispute this entry rather than the list. The raw form is
   asked first, then each successive decoding, so the gate need not know how many times a server decodes.
   The argument is CHECKed rather than guarded: a non-URL composes a form that matches nothing and would
   permit a credentialed request in silence, so release must not proceed through it. URL Standard §6.1 "URL
   class" gives `pathname` and `search` no failure step, so there is nothing to catch. */
function _destructiveToken(u) {
  CHECK(!!u && typeof u.pathname === "string" && typeof u.search === "string",
        "the destructive-path deny list was handed something that is not a URL — this gate is the last thing " +
        "between a credentialed GET and a path that ends the person's session, it reads `pathname` and " +
        "`search` off a URL this zone itself parsed, and anything else composes a form that matches no token " +
        "and permits the request in silence");
  var form = u.pathname + "&" + u.search;
  for (;;) {
    var t = _destructiveIn(form);
    if (t) return t;
    var next = _percentDecode(form);
    /* Termination is structural, not a cap: a pass that changed anything replaced a three-unit triplet with
       one unit and is strictly shorter, and a pass that changed nothing returns the same string. Asserted
       because the exit rests on it. */
    DCHECK(next === form || next.length < form.length,
           "a percent-decoding pass returned a string that differs from its input yet is no shorter — URL " +
           "Standard §1.3 \"Percent-encoded bytes\" replaces a three-code-unit triplet with one byte and " +
           "copies every other code unit unchanged, so a change that does not shorten is this decoder no " +
           "longer implementing it, and the destructive-path loop uses that length to know it is done");
    if (next.length >= form.length) return "";
    form = next;
  }
}

/* Fetch §2.2.6 "Responses"' URL list, as far as script may see it: Fetch §5.5 "Response class" exposes only
   the first and last URL, so the list is [requested] or [requested, landed]. It is built from the landed
   href the caller already computed, so the address `bridge.js` keys a document's agent cluster on is the
   string every gate here judged. `resp.redirected` decides the length rather than `finalHref !== requested`,
   because a 3xx that lands back on its own address still grew the list. */
function _urlList(requested, finalHref, redirected) {
  return redirected ? [requested, finalHref] : [requested];
}

/* Fetch §5.3 "Body mixin"'s body, read chunk by chunk. With no sink this is consume body run to completion
   and the assembled bytes are identical; with `opts.onChunk` each chunk is also released as it arrives.
   Where this runs relative to the gates decides what a released chunk has passed. Before it: the scheme
   allowlist, the userinfo refusal, both private-host checks, both destructive-path checks and both firing
   refusals. After it: CORB, the one gate that reads the body, and the credentialed SOP/CORS check, which
   reads only headers but is placed after the read. So a released chunk has passed neither. No caller states
   `onChunk` today; a credentialed one would release bytes the SOP check may then refuse.
   A script-like destination is therefore not streamable: `_sniff` classifies most JSON only from the whole
   body (a prefix of a document longer than 4096 bytes does not parse), so a first-chunk answer would be
   CORB's allow arm. The entry refuses `blocked-stream-body-gated:` instead. `computedType` is derived from
   the body, so a head delivered before the body cannot carry one.
   Chunks may come from another realm (`engine/trusted.mjs` runs this file in a vm context with the outer
   `fetch`), so they are never `instanceof`-tested; `%TypedArray%.prototype.set` reads internal slots and
   accumulates them whatever their realm. */
async function _readBody(resp, sink, gated) {
  /* The entry refuses a body-gated request that asked for chunks, so this cannot fire through it; it is here
     so a later route that skips that refusal aborts at the release point instead of streaming. */
  DCHECK(!(sink !== undefined && gated),
         "a body-gated request reached the chunk release point — CORB is the one gate in this file that reads " +
         "the BODY, a first-chunk sniff answers it the OPPOSITE way for any JSON document over 4096 bytes, and " +
         "a chunk handed over here has not passed it. The entry refuses this with " +
         "`blocked-stream-body-gated:` before the request is made; reaching this line means that refusal was " +
         "removed and the reply is about to be streamed past the only gate that needed all of it");
  DCHECK(sink === undefined || typeof sink === "function",
         "safeFetch was asked for incremental delivery with an `onChunk` that is not callable — the sink is a " +
         "value this zone composes at its own call site, never one the untrusted engine states, so a " +
         "non-function is this zone's contract broken and would throw mid-body with the request already spent");
  /* No body is not an empty body. Fetch §5.3 "Body mixin"'s body getter returns null where there is none,
     which a §2.2.3 "Statuses" null body status (101, 103, 204, 205, 304) produces. The answer is the empty
     byte sequence, as `arrayBuffer()` gives, and a sink is called zero times; a sink that must tell "no body"
     from "not yet" reads the returned record. */
  if (resp.body === null) return _NO_BYTES();
  var reader = resp.body.getReader();
  var parts = [], total = 0, step, chunk, i, out, off;
  /* No bound: no chunk cap, size cap, read count or timeout. A cap truncates a reply, and a truncated reply
     is a wrong answer about the server rather than a smaller one; the floor is the platform's memory.
     The wait is unbounded too, so a body the server never ends keeps this call pending for as long as the
     server talks. A door that must stay responsive does not await it inside a scheduling round: `bridge.js`
     issues parks through an in-flight table (`engineIssue`), delivers them on a later round, and so never
     re-issues a request the engine re-lists while it is in flight. A sink gives a consumer the bytes in
     time; it does not give it the head, since `computedType` needs the whole body. */
  for (;;) {
    step = await reader.read();
    if (step.done) break;
    chunk = step.value;
    /* The bytes are a stranger's and are never asserted on; that the reader yields a byte view is this host's
       contract. A non-view would corrupt silently (`set` reads a string as zeroes). No input reaches this,
       since a server states bytes and never their carrier. `ArrayBuffer.isView` for the cross-realm reason
       above. */
    DCHECK(ArrayBuffer.isView(chunk),
           "the response body reader yielded a chunk that is not a byte view — the bytes themselves are a " +
           "stranger's and are never asserted on, but the SHAPE of a chunk is this host's contract, and a " +
           "non-view accumulates as zeroes into the body that every gate below and the engine read as the reply");
    /* Released before it is accumulated, because timeliness is what a sink is for. Accumulation continues
       either way, so every gate below reads the same record whether or not a sink was stated. */
    if (sink !== undefined) sink(chunk);
    parts.push(chunk);
    total += chunk.byteLength;
  }
  out = new Uint8Array(total);
  off = 0;
  for (i = 0; i < parts.length; i++) { out.set(parts[i], off); off += parts[i].byteLength; }
  return out;
}
/* @security-contract  ENFORCEMENT POINT (the single network chokepoint)
   Guarantees: cookies omitted unless `opts.credentialed`, and then the reply passes this file's own SOP/CORS
   (same-origin with `opts.pageOrigin`, else exact-origin ACAO and ACAC `true`); GET only; http(s) only; no
   userinfo; origin-relative SSRF on the initial and the landed URL; the destructive-path deny list on
   credentialed non-`observed` requests, before the wire and after a redirect; the egress policy's firing
   walk before the wire and on the landed URL; CORB for script-like destinations. Every post-fetch gate
   judges the landed URL.
   Principals, per call and never a global: `opts.pageUrl` (the document's `MessageSender.url`) classifies
   the SSRF target; `opts.pageOrigin` (`MessageSender.origin`, opaque-unique) is the credentialed SOP's
   principal and is never parsed from a URL. An opaque principal is same-origin with nothing.
   Credentialed callers: `bridge.js`'s `navigationLoad` and `frontierRederive`, only where the address is
   same-origin with the browser-stated principal (`navigationCarriesSession`). The learned-GET replay
   (`fetched`) states `credentialed: false` and no `pageOrigin`. The facts the policy decides from
   (`destination`, `provenance`, `pinned`, `docReach`, `actor`, `credentialed`, `credentials`) are stated by
   every caller and asserted at the entry. */
async function safeFetch(url, opts) {
  opts = opts || {};
  /* The option set this file reads is closed, so a caller's statement it will not read is refused at the
     entry rather than dropped after the bytes are back. */
  _refuseUnreadOptions(opts);
  /* Every fact the firing walk decides from is read once, here, and passed down. The request is judged
     twice (before the wire and after a redirect), and a fact re-read at each gate is one the two can
     disagree about. Reading them at the entry also makes each CHECK fire on every path, not only on paths
     that reach the firing question. */
  var destination = _destinationOf(opts);
  var provenance = _provenanceOf(opts);
  var pinnedMark = _pinnedOf(opts);
  var docReach = _docReachOf(opts);
  var actor = _actorOf(opts);
  /* Derived once, above every gate it scopes; `_credentialedOf` also refuses a header list on a
     cookie-bearing request. */
  var credentialed = _credentialedOf(opts);
  var parsed;
  try { parsed = new URL(String(url)); }
  /* Network: a string the URL Standard §6.1 "URL class" constructor refuses never becomes a request in a
     browser either. There is no URL, so the URL list is empty and the engine's `response.url` is "". */
  catch (e) { return _refused("network", "bad-url", [], {}); }
  /* Network: Fetch §4.3 "Scheme fetch" ends its switch with "Return a network error", so a page fetching
     `file:` gets §5.6's TypeError in a browser too. This allowlist keeps a crafted URL off local and extension
     resources. */
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return _refused("network", "blocked-scheme:" + parsed.protocol, [parsed.href], {});
  /* Network: URL Standard §4.2 "URL miscellaneous" — "A URL includes credentials if its username or password
     is not the empty string" — and Fetch §5.4 "Request class" throws a TypeError for one, which `fetch()`
     rejects with (§5.6 "Fetch methods"). A browser refuses this request identically and no widening reopens
     it. Without this line the `fetch` below would throw and the address would leave no record at all.
     It is a refusal and not an egress signal: a signal is something a person may permit, and this cannot be
     sent at any setting. A redirect to a userinfo URL is refused by the browser itself (Fetch §4.5
     "HTTP-redirect fetch": cors mode, credentials in locationURL, cross-origin), so it is not re-asked.
     The reason names no ground because the ground is the secret; the URL list carries the href the caller
     already holds. */
  if (parsed.username !== "" || parsed.password !== "")
    return _refused("network", "blocked-url-credentials", [parsed.href], {});
  /* Decline: a script-like destination cannot be streamed (see `_bodyGated`), and no browser refuses a
     streamed script, so only this tool declines. Serving it whole to a caller that asked for chunks would
     leave that caller believing it streamed past the one gate that needs the whole body. */
  if (opts.onChunk !== undefined && _bodyGated(opts))
    return _refused("decline", "blocked-stream-body-gated:" + _destinationOf(opts), [parsed.href], {});
  /* Origin-relative SSRF. The principal is the analysed document's own browser-stated address, passed per
     call as `opts.pageUrl` (its `MessageSender.url`, never `sender.tab.url`, so a sub-frame of a tab whose top
     is localhost does not inherit a private classification). It is per call, not a global, because the
     trusted zone interleaves many renderers. A cross-origin script acts with the origin of the page it is
     loaded into, never its own host. A private target is refused only when the principal is not private.
     `opts.pageUrl` is never on the wire: the trusted zone composes it (in the extension, from
     `_browserFacts`' `url`, minted from `MessageSender` and DCHECKed non-empty there). So an absent or
     unparseable principal is this zone's bug and is DCHECKed; with the DCHECKs compiled out it classifies
     public, which still refuses every private target.
     A principal that parses and names no host (`about:blank`, `data:`, `blob:`, reachable through the content
     script's `match_about_blank` and `match_origin_as_fallback`) is input; it names no server, so it is not a
     private-network principal. */
  DCHECK(typeof opts.pageUrl === "string" && opts.pageUrl !== "",
         "the network chokepoint was asked to classify an SSRF target with no page principal (`" +
         opts.pageUrl + "`) — the principal is the analysed document's OWN browser-stated address, " +
         "minted by _browserFacts and DCHECKed non-empty there, so its absence at this call is a " +
         "caller that stopped passing one and never a document that has no address");
  var _pageUrl = null;
  try { _pageUrl = new URL(String(opts.pageUrl)); }
  catch (e) { RETHROW_FATAL(e); _pageUrl = null; }
  DCHECK(_pageUrl !== null,
         "the page principal handed to the SSRF classifier is not a parseable URL (`" + opts.pageUrl +
         "`) — the browser states this address and this zone only carries it, so a string a URL parser " +
         "refuses is our copy of it corrupted rather than anything the analysed page did");
  /* `!!_pageUrl &&` is the release arm under the DCHECKs above: an unparseable principal classifies public,
     which is this gate's fail-closed answer. */
  var _pagePrivate = !!_pageUrl && _isPrivateHost(_pageUrl.hostname);
  /* Network: host permissions bypass the browser's own answer, and this reinstates it. Private Network
     Access, and Fetch §4.10 "CORS check" besides, refuse a public page a private host. */
  if (_isPrivateHost(parsed.hostname) && !_pagePrivate)
    return _refused("network", "blocked-private-from-public", [parsed.href], {});
  /* The person's stated table is awaited, not assumed (see `_EXPLORED_STATING`); waiting at the chokepoint
     covers every door this zone has. It comes after the gates that refuse a request on facts about itself,
     which owe the policy nothing. Once the table is stated the test is false and costs no microtask, and
     `_EXPLORED_STATED` never returns to false. */
  if (!_EXPLORED_STATED && _EXPLORED_STATING !== null) await _EXPLORED_STATING;
  /* The firing decision, before the request exists (see `_firingRefusal`). Decline: no browser refuses this;
     this tool declines to spend the act. The flow parks rather than running its failure path, so it fires
     the day the origin is widened, and the address is still reported. The reason names the signal and value
     that refused. */
  var _ptok = _firingRefusal({ url: parsed, destination: destination, provenance: provenance,
                               pinned: pinnedMark, docReach: docReach, actor: actor,
                               credentialed: credentialed, headers: opts.headers });
  if (_ptok)
    return _refused("decline", "blocked-signal:" + _ptok, [parsed.href], {});
  /* The destructive-path deny list, before the request exists (see `_destructiveToken`). It is scoped to
     credentialed requests, because without cookies a logout path ends no session, and to provenance other
     than `observed`. An `observed` credentialed request is the ambient seed: the person's own browser just
     made this exact request in this profile, so refusing to repeat it prevents nothing. A credentialed
     `derived` or `forced` request is refused on a destructive path here and again after a redirect.
     Decline, and permanent: no widening reopens this list. A browser would send this request; this tool will
     not send it with the person's session. */
  if (credentialed && provenance !== "observed") {
    var _dtok = _destructiveToken(parsed);
    if (_dtok)
      return _refused("decline", "blocked-destructive:" + _dtok, [parsed.href], {});
  }
  var init = { method: "GET", credentials: credentialed ? "include" : "omit", redirect: "follow" };
  /* The caller's header list. On the XHR path it is the analysed bundle's own list, which is within the model
     for the request this file issues (an uncredentialed GET whose forbidden header names the browser strips)
     and is refused on a cookie-bearing one by `_credentialedOf`. This file never adds auth headers; cookies,
     in credentialed mode, are the browser's own and are gated by the SOP/CORS check below. */
  if (opts.headers) init.headers = opts.headers;
  if (opts.signal) init.signal = opts.signal;
  var resp = await fetch(parsed.href, init);
  /* The URL the bytes came from, parsed once above every gate that judges it. Fetch §2.2.5 "Requests": "A
     request has an associated current URL. It is a pointer to the last URL in request's URL list", and §4.1
     "Main fetch" bases readability on that URL's origin, not on the address requested. With `redirect:
     "follow"` the chain is already walked, so every post-fetch gate reads `_finalUrl`.
     An empty `resp.url` is §5.5 "Response class"' "the empty string if this's response's URL is null", so the
     landed URL is the requested one, already judged; `parsed` is that answer, not a default.
     A landed URL this zone cannot parse is refused rather than skipped, so no gate is silently disabled. It
     is a refusal and not an assert because the server's redirect chain determines it, and an assert on a
     value the other side picks would hand a server an abort of the trusted zone. */
  var _finalUrl = parsed;
  if (resp.url) {
    var _fu = null;
    /* URL Standard §6.1 "URL class": "If parsedURL is failure, then throw a TypeError", so this catch has a
       job; it rethrows an invariant abort first. */
    try { _fu = new URL(resp.url); }
    catch (e) { RETHROW_FATAL(e); _fu = null; }
    /* Network: the reply arrived and cannot be read; no policy of ours declined anything. */
    if (!_fu)
      return _refused("network", "blocked-final-url-unparseable", [parsed.href], {});
    _finalUrl = _fu;
  }
  /* The scheme allowlist reads only the requested URL, and that is complete after a redirect: Fetch §4.5
     "HTTP-redirect fetch" step 6, "If locationURL's scheme is not an HTTP(S) scheme, then return a network
     error", has run at every hop. A DCHECK because the browser, not the server, decides the landed scheme; a
     firing means that guarantee no longer holds and the allowlist owes a post-redirect arm. */
  DCHECK(_finalUrl.protocol === "https:" || _finalUrl.protocol === "http:",
         "the landed URL names a scheme the entry allowlist refuses, which " +
         "Fetch §4.5 \"HTTP-redirect fetch\" step 6 makes unreachable as a redirect target — that gate " +
         "runs on the initial URL " +
         "ONLY because the browser refuses a non-HTTP(S) locationURL, so a firing here means that is no " +
         "longer true and the allowlist owes a post-redirect arm: " + _finalUrl.protocol);
  /* An `https` request landing on `http` is mixed content, which is not this zone's question. The engine asks
     Mixed Content §4.4 "Should fetching request be blocked as mixed content?" before the wire; the
     response-side §4.5 "Should response to request be blocked as mixed content?" is a named residual at
     core/fetch/mixed_content.h. A scheme test here would be wrong both ways: §4.5 depends on §4.3 "Does
     settings prohibit mixed security contexts?" over the document's settings and ancestors, which this zone
     does not hold; Secure Contexts §3.1 "Is origin potentially trustworthy?" treats loopback as trustworthy;
     and a top-level navigation is exempt. What this zone owes that residual is `urlList`, whose last item is
     the response URL §4.5 judges. */
  /* The href that leaves this function comes off the same record, so the URL list's last item (which
     `bridge.js` decides an agent cluster from) is the string the gates judged. Fetch §5.5 "Response class"
     serializes the URL with fragments excluded, so re-serializing it is the identity. */
  var _finalHref = _finalUrl.href;
  var _finalOrigin = _finalUrl.origin;
  /* Is the landed resource same-origin with the page principal: one answer, read by both CORB's exemption and
     the credentialed SOP, so the two cannot disagree. `pageOrigin` is the browser-stated
     `MessageSender.origin` and is never parsed from a URL: a sandboxed frame has an ordinary address and an
     opaque origin, and parsing the address would fabricate a tuple origin with same-origin access to its
     embedder. An opaque or absent principal is same-origin with nothing, so it gets strict CORB and a
     credentialed read that needs CORS, which it can never be granted (ACAO cannot equal it). */
  var _pageOrigin = opts.pageOrigin || "";
  var _resourceSameOrigin = _isRealOrigin(_pageOrigin) && _isRealOrigin(_finalOrigin) &&
                            _finalOrigin === _pageOrigin;
  /* Network: SSRF via redirect. The initial check cannot see a 3xx into the intranet, so the landed host is
     re-judged before the body is read, and a public page's request that landed on a private host never feeds
     internal data into the analysis. PNA and CORS both refuse this in a browser. Without a redirect
     `_finalUrl` is `parsed` and this re-asks the same question. */
  if (_isPrivateHost(_finalUrl.hostname) && !_pagePrivate)
    return _refused("network", "blocked-private-redirect", [parsed.href], {});
  /* The deny list again, on the landed URL. The redirect has already been followed, so this cannot unsend
     anything; it refuses to ingest. Following a redirect the server chose is the server's behaviour, and the
     act this list guards is initiating the request, which the pre-request check decided. */
  if (credentialed && provenance !== "observed") {
    /* Decline even though the request went out: the server answered, and a network error would say it did
       not. Refusing to ingest is a decline on either side of the wire. */
    var _rtok = _destructiveToken(_finalUrl);
    if (_rtok)
      return _refused("decline", "blocked-destructive-redirect:" + _rtok,
                      _urlList(parsed.href, _finalHref, resp.redirected), {});
  }
  /* The firing decision again, on the landed URL, with the same limit: it refuses to ingest. A widening is a
     statement about one host, and a reply from another host is about a server nobody permitted. The landed
     URL is also what `url-authority` must read: a 3xx onto a presigned or token-bearing address is the case
     that row exists for. Decline, so a person who widens the landed origin makes this load fire. The facts
     travel as one object, so a new signal is a key a caller omits and a CHECK names, never a shifted
     positional argument. */
  var _rptok = _firingRefusal({ url: _finalUrl, destination: destination, provenance: provenance,
                                pinned: pinnedMark, docReach: docReach, actor: actor,
                                credentialed: credentialed, headers: opts.headers });
  if (_rptok)
    return _refused("decline", "blocked-signal-redirect:" + _rptok,
                    _urlList(parsed.href, _finalHref, resp.redirected), {});
  /* The headers, unwrapped. Fetch §5.5 "Response class"' headers getter returns this's headers, whose
     iteration throws only what the callback throws; an invented empty map would have CORB judge an unlabelled
     body and the SOP read an absent `Access-Control-Allow-Origin`. */
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  /* The body, as bytes, after both SSRF checks, so nothing internal is ingested before the target is judged.
     There is no decode: the engine decodes with the response's charset and BOM (see the header), so it
     receives exactly what the server sent. CORB, the one gate reading the body, and the credentialed SOP run
     below (see `_readBody`). */
  var body = await _readBody(resp, opts.onChunk, _bodyGated(opts));
  /* The one sniff and the one type: CORB reads `protected` and the record reads `type`, so no second answer
     exists to disagree. A missing `Content-Type` is MIME Sniffing §5.1 "Interpreting the resource metadata"'s
     "the supplied MIME type is undefined", and is passed as absent. */
  var _nosniff = _determineNosniff(headers["x-content-type-options"]);
  var _sn = _sniff(body);
  var _computed = _computedType(headers["content-type"], _nosniff, _sn);
  /* CORB by the request's destination (Fetch §2.2.5). A script-like destination is bytes that will run as
     code under QuickJS, so a cross-origin body must be JavaScript-typed; every other destination ("" for a
     `fetch()`, "image", "font", "style" …) is data and exempt. Whether the bytes later reach QuickJS is the
     caller's contract; this returns bytes.
     The gate applies to every reply, whatever the status. Fetch §2.10 "Should response to request be blocked
     due to its MIME type?" reads the header list, the destination and the essence, never a status, and an
     authenticated error page is exactly the cross-origin data CORB keeps out of the renderer's process. The
     engine's separate refusal of a non-ok script (HTML §8.1.4.2 "Fetching scripts", `script_fetch_status_ok`)
     keeps a refused response from becoming a program; neither rule is the other's guard. */
  if (_isScriptLike(_destinationOf(opts))) {
    // The declared essence is what CORB's tables are stated over; the sniff confirms a body whose label lied.
    var _declared = String(headers["content-type"] == null ? "" : headers["content-type"])
      .split(";")[0].trim().toLowerCase();
    var _deny = _corbDeniesScript(_declared, _nosniff, _sn, _resourceSameOrigin);
    /* The deciding rule rides the status message with the computed type, as every refusal names its ground.
       Network: Chromium's CORB, and ORB after it, refuse this load too, and an ORB-blocked script load reaches
       the page as Fetch §5.6's network error; this zone applies the rule only because a host-permission fetch
       bypasses the browser's copy.
       Named residual. Not covered: `same-origin-protected` refuses a page its own HTML or JSON as script
       text, which a browser hands over and then fails at compile. Next diff: return same-origin bytes and let
       the engine's compile fail them. Its absence shows as a same-origin script load ending in an `error`
       event where Chrome reports a SyntaxError. */
    if (_deny)
      return _refused("network", "blocked-corb:" + _deny + ":" + _computed,
                      _urlList(parsed.href, _finalHref, resp.redirected), headers);
  }
  /* Own SOP/CORS for a credentialed reply. The browser does not apply the same-origin policy to an extension
     fetch with host permissions, so when cookies are attached this zone enforces it on the bytes before
     returning them; otherwise a bundle could record a cross-origin endpoint and read the person's
     authenticated data from any site they are signed into.
       SOP:  a resource same-origin with the page principal (`opts.pageOrigin`) is readable.
       CORS: a cross-origin credentialed read needs `Access-Control-Allow-Origin` equal to that exact origin
             (never `*`) and `Access-Control-Allow-Credentials: true`.
     A refused read returns no body; the request was a GET, so what is refused is the bytes. */
  if (credentialed) {
    /* `_pageOrigin` and `_resourceSameOrigin` are read, not recomputed: they are over `_finalOrigin`, so a
       same-origin request that redirected cross-origin is cross-origin here and at CORB alike. An opaque or
       absent principal is same-origin with nothing and can never match ACAO, so it fails closed. */
    if (!_resourceSameOrigin) {
      var _acao = headers["access-control-allow-origin"] || "";
      var _acac = (headers["access-control-allow-credentials"] || "").toLowerCase();
      // Fetch §4.10 "CORS check": ACAO must byte-match the request's own origin (`*` is
      // refused once credentials mode is "include") and ACAC must be `true`.
      if (!_isRealOrigin(_pageOrigin) || _acao !== _pageOrigin || _acac !== "true")
        /* Network: Fetch §4.10 "CORS check" failing makes §4.4 "HTTP fetch" "return a network error". The
           reason names the landed origin that failed. Headers are `{}` here, unlike CORB's refusal: this
           refuses a cross-origin read the server never granted, and its headers are part of that read. */
        return _refused("network", "blocked-cors-credentialed:" + _finalOrigin,
                        _urlList(parsed.href, _finalHref, resp.redirected), {});
    }
  }
  /* The type this zone computed travels with the bytes, so the renderer is handed an answer rather than
     evidence (`core/fetch/fetch.c` asserts its presence on the reply record). `refusal: null` is the positive
     statement that the request reached the wire and this is the reply, including an HTTP error status.
     Status 0 is what every refusal answers and no HTTP response has; `mode` is unset, so Fetch's default
     `cors` applies and no opaque filtered response can answer 0, which is what the DCHECK below binds. */
  DCHECK(resp.status !== 0,
         "the wire answered with status 0, which is the one status no HTTP response has and which every " +
         "refusal in this file answers with — `mode` is unset so Fetch's default `cors` applies and an " +
         "opaque filtered response cannot arise, so a 0 here is a reply and a refusal wearing one number");
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: body,
           urlList: _urlList(parsed.href, _finalHref, resp.redirected), computedType: _computed,
           refusal: null };
}
/* The chokepoint and the policy that decides whether it fires, installed together so no host holds half the
   contract. `engine/trusted.mjs` runs this file in a vm context and reads them off it; `ast-worker.html`
   loads it into the offscreen document before `bridge.js`.
   Writers: `safeFetchPermit` (one signal, one value, one origin), `safeFetchWiden` and `safeFetchUnwiden` (a
   whole origin), `safeFetchEgressStated` (the host's one restore, answering the entries it dropped) and
   `safeFetchEgressStating` (the restore's read, which every request awaits). `safeFetchEgressTable` is what a
   host persists, in the shape the restore takes. For a surface: `safeFetchWidenable` and
   `safeFetchSignalUsable` (the tests `safeFetchPermit` aborts on, so a surface can refuse with the reason),
   `safeFetchSignals`, `safeFetchPermitted`, `safeFetchSignalVector`, `safeFetchDefaultArms` and
   `safeFetchWidenedOrigins`. `safeFetchFiringRefusal` and `safeFetchMethodRefusal` answer for an act that is
   not a fetch, in the reply record's refusal vocabulary. Every one resolves to `_firingRefusal`,
   `_signalVector` or a predicate they compare against, so none is a second policy.
   There is no exported destructive-path check: the deny list is a floor under requests this file composes
   and fires on its own, and never overrules an operator's explicit act through the page-context relay. */
if (typeof self !== "undefined") {
  self.safeFetch = safeFetch;
  self.safeFetchMethodRefusal = safeFetchMethodRefusal;
  self.safeFetchWiden = safeFetchWiden;
  self.safeFetchUnwiden = safeFetchUnwiden;
  self.safeFetchPermit = safeFetchPermit;
  self.safeFetchEgressStated = safeFetchEgressStated;
  self.safeFetchEgressStating = safeFetchEgressStating;
  self.safeFetchEgressTable = safeFetchEgressTable;
  self.safeFetchWidenable = safeFetchWidenable;
  self.safeFetchSignalUsable = safeFetchSignalUsable;
  self.safeFetchSignals = safeFetchSignals;
  self.safeFetchPermitted = safeFetchPermitted;
  self.safeFetchSignalVector = safeFetchSignalVector;
  self.safeFetchDefaultArms = safeFetchDefaultArms;
  self.safeFetchFiringRefusal = safeFetchFiringRefusal;
  self.safeFetchWidenedOrigins = safeFetchWidenedOrigins;
  /* The reach-grade ordering, exported so both hosts compose a child document's reach with one answer. */
  self.safeFetchReachJoin = safeFetchReachJoin;
}
