/* safe-fetch.js — the single external-fetch entry point for the analyzer.
   Every request the analyzer makes off the network (documents, lazy chunks, source maps, discovery probes)
   goes through `safeFetch`, so its security invariants live in one auditable place:
     • cookies omitted by default; in credentialed mode the reply passes this file's own SOP/CORS, because a
       host-permission fetch bypasses the browser's;
     • GET only, by a literal, with a caller-stated verb refused (`_refuseUnreadOptions`);
     • a destructive-path deny list on credentialed requests not graded `observed` (`_destructiveToken`);
     • http(s) only and no userinfo, so a crafted URL cannot read local or extension resources;
     • origin-relative SSRF on the initial and the landed URL: a public page may not reach the person's private
       network through the extension's host permissions, while a private page may reach its own;
     • CORB by the request's destination, for every code loader at once;
     • the per-origin egress policy (`_firingRefusal`), which decides whether an act may be spent at all.
   It runs in the offscreen document (`ast-worker.html`, after `check.js`, which defines the asserts) and
   verbatim in `engine/trusted.mjs`'s vm realm. It fetches directly: COEP requires CORP only of no-cors
   subresources, so only an over-restrictive `connect-src` could block this CORS fetch. */
/* Returns a plain record, not a Response, so both hosts read one shape: { ok, status, statusText, headers
   (lowercased), body, urlList, computedType, refusal }.
   The body is bytes. A decode is a semantic the engine owns: HTML §8.1.4.2's "fetch a classic script" decodes
   "using encoding as the fallback encoding", with the `Content-Type` charset and a BOM deciding, so the engine
   must receive what the server sent. No security decision reads a decoded body; CORB's sniff decodes only for
   its own comparison.
   `urlList` is Fetch §2.2.6's response URL list, which only this zone can report because the redirect chain
   exists only here; it is [requested] or [requested, landed] (see `_urlList`).
   Type sniffing lives here and nowhere else in the extension. What a flow needs mid-execution belongs in the
   engine; this is a decision the trusted zone makes once about a reply it fetched, and `computedType` states
   it to the renderer, so no zone downstream sniffs or re-parses a raw header for a type of its own. */
/* A tuple origin (scheme://host[:port]) is usable for same-origin and CORS. An opaque origin reports "null",
   or this zone's minted "null:<uuid>" token, and is unique, so it is same-origin with nothing, not even
   another "null". Only a tuple origin contains "://", so this one test separates them. */
function _isRealOrigin(o) { return typeof o === "string" && o.indexOf("://") > 0; }
/* The empty byte sequence every refusal's body is, so a body is one type on every path; a caller telling "no
   bytes" from "bytes I cannot read" reads `ok` and `status`. */
function _NO_BYTES() { return new Uint8Array(0); }
/* What a refusal is, graded by the arm that made it. A host that turns a refusal into Fetch §5.6 "Fetch
   methods"' network error ("reject p with a TypeError") resumes the page down its failure path under a fact
   about the origin, so the grade answers one question: would a real browser performing this request also
   produce a network error?
     "network" — yes, and the network error is the faithful answer: Fetch §4.3 "Scheme fetch" ends "Return a
                 network error", a failed §4.10 "CORS check" makes §4.4 "HTTP fetch" return one, and Local
                 Network Access and CORB refuse their cases. The flow resumes down its failure path correctly.
     "decline" — only this tool refuses, because spending an act on a request no client makes is a policy
                 question. There is no observation to hand back, so the flow parks, which is a search not yet
                 solved and never a verdict, and fires the day the origin is widened.
   A permanent decline (the deny list) needs no third word: a parked flow emits nothing, so the scheduler sinks
   it, and the reason in `statusText` tells the reader which rule holds it. A decline costs the page's own
   `.catch` arm, and the answer to that is a fork, never a fabricated network error. The grade is stamped by
   the arm, so a consumer never re-derives the policy or matches `statusText`. */
/* The header map is stated by the arm: a CORB refusal happens after the reply arrived and its headers are
   part of what was learned, while a pre-request refusal has no response, so `{}` there is a statement. */
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
/* The method half of the firing question, for a host whose request never reaches the fetch. This file
   enforces RFC 9110 §9.2.1 "Safe Methods" structurally (it can only send GET), so nothing downstream can
   observe the answer, and each host would otherwise re-derive it with its own grade. It is a decline: no
   browser refuses a POST, the address is derived in full and reported, and the flow parks. It answers the
   refusal value rather than a boolean, so a host can say why. */
function safeFetchMethodRefusal(method) {
  CHECK(typeof method === "string" && method !== "",
        "safeFetchMethodRefusal was asked about " + JSON.stringify(method) + ", which is not a Fetch §2.2.1 " +
        "\"Methods\" method — the caller is deciding whether this zone can perform a request at all, and an " +
        "absent method would take the arm `GET` takes, which is the arm that spends the network");
  return method === "GET" ? null : { kind: "decline", reason: "blocked-method:" + method };
}
// HTML's JavaScript MIME type list and Chromium's CORB-protected set: the two tables the CORB rule is stated
// over.
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
// Fetch's determine nosniff, over the `X-Content-Type-Options` value as "get a header" has already joined its
// duplicates: "let values be the result of getting, decoding and splitting the header … if values[0] is an
// ASCII case-insensitive match for `nosniff`, return true." Only the first comma-separated value counts, so
// `foo, nosniff` is sniffable, as it is in a browser.
function _determineNosniff(v) {
  if (typeof v !== "string") return false;   // absent header = §"values is null" = false
  return v.split(",")[0].replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, "").toLowerCase() === "nosniff";
}
/* MIME Sniffing §6 "Matching a MIME type pattern", and the two tables of §7.1 "Identifying a resource with an
   unknown MIME type" whose answers a reader of `computedType` can act on.
   This mirrors `engine/host/browser/core/mime/mime_sniff.c` and is not a rival to it: that component is in
   the WASM sandbox and answers HTML §7.4.5's computed type for a navigation the engine parses itself, this is
   the trusted zone, and no call crosses between them. The two carry the same citations and the same cell
   encoding, so a reader repairing one finds the other by section number.
   One array carries both of §6's columns (mask byte in bits 8-15, pattern byte in bits 0-7), so §6's first
   step, "Assert: pattern's length is equal to mask's length", holds by construction.
   §5.2 "Reading the resource header" bounds what §7 looks at to 1445 bytes; it does not truncate the body. */
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

/* §6's pattern matching algorithm over one row's packed cells. Steps 3 and 4 advance past the `ignored` set,
   and every row of §6.1 and §6.4 states that column as None, so the two indices are one; a table with an
   ignored column may not be walked by this. */
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
/* §7.1's image step then its archive step, in the standard's order, and null where neither answers.
   Named residual. Not covered: every other row of §7.1 — §6.2 "Matching an audio or video type pattern" with
   its signature algorithms, the sniff-scriptable table, the PostScript-and-BOM table and the
   no-binary-data fall-through — and §7's steps 1, 4 and 5-6. The two tables may be walked alone because no
   earlier row of §7.1 can match what they match (the scriptable rows open 0x3C, the other table's 0x25, 0xFE,
   0xFF 0xFE or 0xEF, and the only image row opening 0xFF is 0xFF 0xD8 0xFF). Next diff: §6.2's table and its
   §6.2.1 MP4, §6.2.2 WebM and §6.2.3 MP3-without-ID3 algorithms, mirrored from `mime_sniff.c`'s
   `sniff_av_pattern`, together with §7.1's PostScript-and-BOM table, because §6.2.3's MP3 sync matches the
   UTF-16LE BOM 0xFF 0xFE and §7.1 orders the BOM table first. Its absence shows as an endpoint record whose
   reply carried no `Content-Type` (or an unknown essence) and whose bytes are a media stream. */
function _msUnknownTypePattern(header) {
  var m = _msTable(_MS_IMAGE, header);
  if (m) return m;
  return _msTable(_MS_ARCHIVE, header);
}

// CORB's own sniff, over bytes: a check decoding the evidence it judges, as Chrome's ORB does (it attempts a
// JSON parse). The head is decoded first and the whole body only where the head starts like JSON and does not
// parse, so a large JS chunk costs one 4 KiB decode and most API JSON is parsed once.
// It answers three questions from one pass. `protected` is CORB's: may these cross-origin bytes reach a code
// loader (markup and JSON may not). `type` is whether the bytes contradict a label this zone acts on, so its
// only non-null value is `application/json`, which the real parser confirmed; a leading `<` names no type,
// since no standard says which markup it is. `unknownType` is MIME Sniffing §7 "Determining the computed MIME
// type of a resource"'s step 2 arm, the one case where the bytes may name what the server did not; it is
// computed over §5.2 "Reading the resource header"'s raw bytes, because §6.1's and §6.4's rows ignore no
// leading bytes.
function _sniff(bytes) {
  var dec = new TextDecoder("utf-8");   // strips a leading UTF-8 BOM
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
// What this response is, as one statement the renderer is told: MIME Sniffing §4.2 "MIME type
// miscellaneous"'s essence of the server's `Content-Type` when it stated one, and what the bytes say when it
// did not or when they contradict it with JSON. The empty string is MIME Sniffing §5.1 "Interpreting the
// resource metadata"'s "the supplied MIME type is undefined" surviving the sniff: a positive answer that
// neither the server nor the bytes named a type. Nosniff is final: the server's label is returned unchanged.
//
// A declared type the bytes contradict is otherwise the server's to name. MIME Sniffing §7 reaches the bytes
// only at step 2 (undefined or unknown type), step 4 (the apache-bug flag) and steps 5-6 (refining an image or
// audio-or-video type), and its last step is "The computed MIME type is the supplied MIME type", so a PNG
// served `text/plain` computes `text/plain` in every browser. That is why `extension/lib/discovery.js`'s
// `classifyResponseAsset`, where magic bytes are authoritative for a different question (is this body worth
// extracting a schema from), must not be routed to from here. §7 never sniffs a font: §6.3 "Matching a font
// type pattern" is invoked only by §8.7 "Sniffing in a font context".
function _computedType(declared, nosniff, sniff) {
  var mime = String(declared == null ? "" : declared).split(";")[0].trim().toLowerCase();
  DCHECK(sniff !== null && typeof sniff === "object" &&
         (sniff.unknownType === null || typeof sniff.unknownType === "string"),
         "the sniff handed to `_computedType` carries no §7.1 answer — `_sniff` writes that field on every " +
         "arm, as `null` for a resource whose bytes match no pattern it walks, so an absent one is a SECOND " +
         "sniff from somewhere else and the arm below would silently take the answer that has no bytes " +
         "behind it");
  // MIME Sniffing §7 step 2, asked before step 3's nosniff because the standard asks it there: "If the supplied
  // MIME type is undefined or if the supplied MIME type's essence is "unknown/unknown", "application/unknown",
  // or "*/*", execute the rules for identifying an unknown MIME type with the sniff-scriptable flag equal to
  // the inverse of the no-sniff flag and abort these steps." So nosniff with no `Content-Type` narrows which of
  // §7.1's tables may run rather than stopping the algorithm; neither table walked here is in the
  // sniff-scriptable half, so the flag gates nothing yet, and the DCHECK below makes that loud the day one is.
  // This quotation is in line comments because its third essence would close a block comment.
  if (!mime || mime === "unknown/unknown" || mime === "application/unknown" || mime === "*/*") {
    if (sniff.unknownType) {
      /* It cannot fail over the two tables above and can over the next ones: §7.1's sniff-scriptable table
         returns `text/html`, `text/xml` and `application/pdf`, and §7.2's note says these rules must "never
         determine the computed MIME type to be a scriptable MIME type, as this could allow a privilege
         escalation attack". `solver/engine.c` reads `computedType` to decide whether reply bytes are queued
         as a program, so a row added without §7.1's flag would be that escalation; this fires instead. */
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
  // The bytes contradict the label: a JSON body under `text/plain` or a JavaScript type is JSON, and saying so
  // lets the reply be learned from rather than skipped as whatever the server mislabelled it.
  if (sniff.type && sniff.type !== mime) return sniff.type;
  return mime;
}
// Is this request's destination script-like, in Fetch §2.2.5 "Requests"' own words: "A request's destination
// is script-like if it is `audioworklet`, `paintworklet`, `script`, `serviceworker`, `sharedworker`, or
// `worker`." That predicate is the CORB question, asked of the destination the engine put on the request.
// `xslt` is deliberately outside it: the spec's note says such algorithms "should also consider `xslt` as that
// too can cause script execution", and considering it here yields exclusion, because this rule demands a
// JavaScript MIME type and an XSLT stylesheet is XML. Loading XSLT needs its own rule.
function _isScriptLike(d) {
  return d === "audioworklet" || d === "paintworklet" || d === "script" ||
    d === "serviceworker" || d === "sharedworker" || d === "worker";
}
// Is this request a subresource of the document: a second question asked of the same destination string, for
// the egress policy (whose act the request is) rather than for CORB (may the reply run as code). The two part
// at a stylesheet, which is CORB data and yet the page loading itself.
// It is Fetch §2.2.5's subresource request — "a request whose destination is audio, audioworklet, font,
// image, json, manifest, paintworklet, script, style, text, track, video, xslt, or the empty string" — minus
// the empty string, and the minus is the decision: the empty string is what a `fetch()` and an XHR carry, so
// the term as written would admit every data request at every origin. It is the positive list, never the
// complement of another set, because §2.2.5's sets do not partition its enumeration (`webidentity` is in
// neither).
// Named residual. Not covered: a subresource the page's markup or code names whose destination is the empty
// string, which this chokepoint cannot separate from a data fetch. Next diff: the park kind on the pending
// line beside the destination (`engine_pending_fetches` joins method, destination, initiator, provenance,
// pinned, credentials and URL), saying whose algorithm is owed the reply. Its absence shows as a
// `blocked-signal:destination=value` refusal for an address the analysed document's own markup declares.
function _isDocumentSubresource(d) {
  return d === "audio" || d === "audioworklet" || d === "font" || d === "image" ||
    d === "json" || d === "manifest" || d === "paintworklet" || d === "script" ||
    d === "style" || d === "text" || d === "track" || d === "video" || d === "xslt";
}
// Fetch §2.2.5 "Requests"' destination type, enumerated: "A destination type is one of: the empty string,
// `audio`, `audioworklet`, `document`, `embed`, `font`, `frame`, `iframe`, `image`, `json`, `manifest`,
// `object`, `paintworklet`, `report`, `script`, `serviceworker`, `sharedworker`, `style`, `text`, `track`,
// `video`, `webidentity`, `worker`, or `xslt`." It lives beside the predicates over it because this file is
// the one that decides from the value.
var _DESTINATION_TYPES = ["", "audio", "audioworklet", "document", "embed", "font", "frame", "iframe",
                          "image", "json", "manifest", "object", "paintworklet", "report", "script",
                          "serviceworker", "sharedworker", "style", "text", "track", "video",
                          "webidentity", "worker", "xslt"];
/* Every caller must state a destination §2.2.5 defines (`_destinationOf`, a CHECK). "" is legitimate and
   means data; what must not happen is an absent or misspelled value reading as an answer, because
   `_isScriptLike` is false for anything it does not recognise, so both take the arm that skips CORB for a
   code load. It is a CHECK and not a DCHECK because with it compiled out that fail-open arm is what release
   runs, on a security boundary. The value also crosses from the untrusted engine, whose own DCHECKs on the
   enumeration are compiled out on the far side of a mojo boundary; asserting it at the chokepoint covers the
   extension and `engine/trusted.mjs` at once. */
/* Does a gate in this file read this request's whole body. Derived from `_isScriptLike`, never restated,
   because CORB is the only gate that reads the body and its scope is that predicate. A first-chunk sniff is
   not a weaker answer but the opposite one: `_sniff` falls back to parsing the whole body, so a JSON document
   longer than 4096 bytes fails the prefix parse and a first-chunk CORB would admit it to a code loader. So a
   script-like destination is refused streaming before the request is made, graded `decline` because a
   browser streams a script and only this tool declines. */
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
/* Fetch §2.2.5 "Requests"' mode — "A request has an associated mode, which is `same-origin`, `cors`, `no-cors`,
   `navigate`, `websocket`, or `webtransport`." Stated by the algorithm that created the request (the engine
   states it on the pending line and the XHR record; a §7.4 navigation is `navigate`) and never inferred from
   the address. Fetch §4.1 "Main fetch" decides from it what the page may read of the reply: a `cors` reply
   passes §4.10 "CORS check", a cross-origin `no-cors` reply becomes an opaque filtered response, and a
   `same-origin` request to another origin is a network error. A CHECK, because the arm an absent or invented
   mode would take is the one that hands the page another origin's bytes. `websocket` and `webtransport` are
   not requests this chokepoint performs. */
var _MODE_TYPES = ["same-origin", "cors", "no-cors", "navigate", "websocket", "webtransport"];
function _modeOf(opts) {
  CHECK(typeof opts.mode === "string" && _MODE_TYPES.indexOf(opts.mode) >= 0,
        "safeFetch was called with a MODE that is not one Fetch §2.2.5 \"Requests\" enumerates: " +
        JSON.stringify(opts.mode) + " — the mode decides whether the reply is CORS-checked, opaque, or " +
        "refused for another origin, so a request that does not state the one its creating algorithm named " +
        "would be answered by whichever arm reads it as the default");
  CHECK(opts.mode !== "websocket" && opts.mode !== "webtransport",
        "safeFetch was called with the mode `" + opts.mode + "`, which is a connection this chokepoint does " +
        "not open — a WebSocket or WebTransport session is the page's own (intercept.js), never a GET here");
  return opts.mode;
}
// What a reply is evidence of: `observed`, `derived` or `forced`. The engine states the word (solver/engine.h's
// PENDING_PROVENANCE_*, composed at the park from the parking flow's `path_forced` and from HTML §4.12.1.1
// "Processing model"'s parser-inserted script, a `script` whose parser document is non-null); a zone that
// originated an act states it for itself. This file decides from it and may not re-derive it: nothing in an
// address distinguishes a request a session would have made from one that exists because a gate was forced.
var _PROVENANCE_TYPES = ["observed", "derived", "forced"];
// Every caller must state one, and it is a CHECK: the firing decision and the deny list's scope rest on it,
// and a missing or invented word must not be answered by whichever arm it happens to fall to. The value
// crosses from the untrusted engine, and asserting it at the chokepoint covers both hosts. There is no
// `unknown` grade: a request whose provenance is not established crashes at the decision rather than loading.
function _provenanceOf(opts) {
  CHECK(typeof opts.provenance === "string" && _PROVENANCE_TYPES.indexOf(opts.provenance) >= 0,
        "safeFetch was called with a PROVENANCE that is none of the three CLAUDE.md " +
        "§A-REQUEST-CARRIES-THE-PROVENANCE declares (solver/engine.h's PENDING_PROVENANCE_*): " +
        JSON.stringify(opts.provenance) + " — this file decides whether to FIRE the request from it, and " +
        "an absent or invented value takes the same arm `derived` does, which is the arm that spends the " +
        "network. State `observed`, `derived` or `forced` and mean it");
  return opts.provenance;
}
/* How the document holding this request was itself reached. `provenance` is a fact about this request and
   this is a fact about the document it was made from, and the two are independent: a page reached by a route
   only its bundle names makes its own `fetch()`es, which the engine correctly grades `observed`.
   It is a second row, not a join into `provenance`, so a person can tell "this tool forced this request" from
   "the page made this request in a document this tool chose to open". It is this zone's word, not the
   engine's, because this zone performed the load before any instance existed. A CHECK for the provenance
   reason; an absent value would make the observed arm's conjunction unsatisfiable, and the refusal would name
   a row the caller never meant to answer. */
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
/* How two reach grades compose, declared once because both hosts compose one: a child navigable of a page
   this tool chose to open is itself such a page, so the composition is the weaker grade. The order is an
   explicit map, not array positions, so a word added to the vocabulary cannot silently re-rank the others;
   the CHECKs make an unranked word a crash rather than an `undefined` that compares false. */
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
/* The witness mark the engine states beside the provenance: solver/engine.h's `pinned`/`unpinned`, from
   solver/flow.h's `path_pinned`, composed at the park for a request that parks and at the act
   (`engine_pinned_of_running_path`) for one the page's code made without parking. It says whether the flow
   had determined some source's value on an arm nothing observed before it built this address. It is a
   may-rest-on: concretize-on-pin answers a pinned read with a bare primitive, so the address cannot be asked
   where its bytes came from, and the engine records the fact where they are chosen instead. `unpinned`
   positively states that every byte came from the document, the server or the bundle's own text. A CHECK
   for the provenance reason. */
/* `unstated` is this zone's third word, for an act whose record carries no mark; writing `unpinned` there
   would be false exactly where it matters. A caller states `unpinned` only for an address no engine path
   composed, or from its record's mark; any other act states `unstated`. The mark is independent of the
   provenance, so a `derived` act may rest on a witness. An absent mark still aborts, so `unstated` is always
   typed by a caller that means it, and `unstated` and `unpinned` are separate rows a person permits separately.
   The navigation records (core/frame/navigable.c's load job and create notice, solver/route_seed.c's
   declaration) carry the engine's mark like the pending line does. */
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
/* Whose act this request is: the analysed page's or this tool's. Every other row describes the request and
   this one describes the caller: the page's own `fetch()` and this tool's automatic discovery sweep are
   identical on destination, provenance, doc-reach, witness and cookies, and differ only in who composed
   them. `header-authority` differs between them only because the sweep passes `headers || {}`, and the
   page's XHR states a list, so it cannot stand in for this row.
   It is a value typed at each asker and CHECKed here, never inferred from who the callers are, and an absent
   one aborts. The words are deliberately not `lib/schema.js`'s `PAGE_CONTEXT_*`, which answer human or tool
   for an operator send (`safeFetchOperator`); a page's `fetch()` is neither. It is the one gating fact the untrusted zone cannot
   reach, a literal in trusted-zone source, hence `certain`. A request this tool composed at a person's
   direction (`trusted.mjs`'s seed, `bridge.js`'s residue re-fetch) is still `tool`; its human authorization
   belongs on another axis. */
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
/* The engine's drive mark (solver/engine.h's PENDING_DRIVEN_*): `driven` when the request was minted inside the
   engine's own drive of a function the page never called, `undriven` otherwise. It is a separate fact from the
   provenance (a drive that forced no arm is `derived`), and the `actor` row reads it alone: a `page` request
   that is `driven` is this tool's act. `unstated` is this zone's word for an act no engine record describes,
   and only a `tool` asker may state it, so every relay of an engine record carries the mark it was given. */
var _DRIVEN_MARKS = ["driven", "undriven", "unstated"];
function _drivenOf(opts) {
  CHECK(_DRIVEN_MARKS.indexOf(opts.driven) >= 0,
        "safeFetch was called with a DRIVE MARK that is none of the three this file declares: " +
        JSON.stringify(opts.driven) + " — the `actor` row reads it, so an absent one could fire a drive's " +
        "request as the page's own. Relay `driven`/`undriven` from the engine's record, or state `unstated` " +
        "for an act this tool composed");
  CHECK(opts.actor !== "page" || opts.driven !== "unstated",
        "safeFetch was called with actor `page` and an unstated drive mark — a `page` asker relays an engine " +
        "record, and every such record carries the mark, so this caller dropped the field that decides whose " +
        "act the request is");
  return opts.driven;
}
/* What this function does not read: a field outside this set is refused rather than dropped in silence and
   read by its caller as carried. The option that matters is `method`: `init` hardcodes `method: "GET"` and
   nothing reads `opts.method` or `opts.body`, so a caller stating a verb would otherwise get the reply to a
   GET attributed to its own request, a wrong answer worse than an absent one. That makes the `method` signal
   a fact about the transport with one value, which deleting the whole widening table would not change. The
   rule also refuses `as`, since the CORB class is the request's destination. A closed set restates what the
   body reads, and the drift direction is safe: an option added to the body and not here aborts on its
   author's first call. */
var _SAFEFETCH_OPTIONS = ["pageUrl", "pageOrigin", "destination", "mode", "provenance", "pinned", "driven",
                          "docReach", "actor", "credentialed", "credentials", "headers", "useCorsPreflight",
                          "identity", "signal", "onChunk"];
/* Fetch §2.2.5 "Requests"' credentials mode — "which is `omit`, `same-origin`, or `include`" — the same three
   words `fetch_credentials_token` writes on the pending line. Written here because this zone is the other
   party to that contract. */
var _CREDENTIALS_MODES = ["omit", "same-origin", "include"];
/* A DCHECK: release can proceed, because every unread option lands on the safe side (a dropped `method` or
   `body` fires the GET this file always fires); what is lost is the caller's belief about what it sent. The
   facts the firing walk decides from are CHECKed at their own reads. It may assert at all because the keys
   are trusted-zone literals at every call site and the untrusted engine supplies only values, so no bundle or
   renderer can reach it. */
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
/* Whether the person's session pays for this request, and the one option that may not ride along with it.
   `credentialed` is this zone's own literal; nothing on the wire states it. It scopes the deny list and the
   credentialed SOP. `opts.headers` is the one option whose values come from the untrusted zone: the bundle's
   own list on the XHR path, which on an uncredentialed GET confers nothing the page lacks.
   Together they are a different question. RFC 9110 §9.2.1 "Safe Methods" is enforced here by making the verb
   a literal, and a header list is another route to a verb: with `X-Http-Method-Override` (a convention
   `lib/discovery.js` uses) a server reads this GET as the header's verb. So a credentialed request may not
   state a header list, a CHECK because with it compiled out that request goes out with cookies and untrusted
   header values. It may assert because the decision to ask for cookies is a trusted-zone literal at every
   call site. If it refuses someone, what is owed is a statement of whose header list it is, carried from the
   site that knows, not an exemption.
   `credentials` is a separate fact: Fetch §2.2.5's credentials mode, named by the algorithm that created the
   request and carried on the pending line. It can only narrow what this zone was already willing to do. */
function _credentialedOf(opts) {
  /* The flag is stated, not coerced. A DCHECK because the arm a compiled-out assert leaves is the
     uncredentialed one, so nobody's session is spent. It is asserted at all because an absent key and a
     stated `false` are one fact here and two at the call site, and stating it makes the header CHECK below
     test what a caller says. The key is a trusted-zone literal at every call site, so no bundle or renderer
     can reach the abort. The two CHECKs below are fatal because the arm they leave sends the cookies. */
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
  /* The second input is the request's own: `credentialed` is this zone's willingness to spend the session,
     `credentials` is the mode the creating algorithm stated, and the answer is their conjunction, since the
     zone cannot know an element's mode and the engine cannot know whether this person wants their session
     spent. A stated mode can only narrow: an engine that could turn cookies on would hold network policy. An
     unknown word is producer drift and fatal at any setting; an absent one is honest for a request this zone
     composed itself and may not stand on a cookie-bearing request.
     Named residual. Not covered: `same-origin` when this zone is willing; the only same-origin comparison is
     after the wire, over the landed origin (`_resourceSameOrigin`). Next diff: the same-origin test on the
     request's own URL against `pageOrigin`, read here, with the post-fetch gate keeping the redirect case.
     Its absence shows as the CHECK_FAIL below firing; every willing caller today is a navigation stating
     `include`. */
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
  /* §2.2.5's own arms in its own order: `omit` is the narrowing this parameter exists to make, and `include`
     is what every willing caller states. */
  if (mode === "omit") return false;
  if (mode === "include") return true;
  CHECK_FAIL("safeFetch was asked for a CREDENTIALED request whose Fetch §2.2.5 credentials mode is " +
             "`same-origin`, and that is a CONDITIONAL answer this file does not yet resolve BEFORE the " +
             "wire — see the paragraph above. It is fatal rather than narrowed to `omit` because a silent " +
             "narrowing here hands a same-origin document load the LOGGED-OUT body with nothing anywhere " +
             "saying so, which is the moat this tool exists to cross. Build the pre-request same-origin " +
             "test against `pageOrigin`; every willing caller today is a §7.4 navigation stating `include`");
}
/* The signals the egress policy surfaces, and the person who decides from them. Method, credential state,
   provenance, URL-carried authority, credential lineage, destination and intended invalidity are facts the
   policy shows, and the person decides per origin which values it allows. The control is per-signal rather
   than a score: a wrong weighting inside a score is invisible, while a named signal a person allowed is a
   decision they can revisit.
   No combination is refused at every setting, so the only thing between a person and a combination they did
   not intend is whether the control stated the consequence. A signal is shown only where this file can
   compute it, and one it cannot determine is shown as a stated unknown, never as an absence a default fills.
   They are separate rows because stripping cookies does not make a request uncorrelated with the person
   (capability URLs, lineage), GET is a signal about intent rather than a guarantee (RFC 9110 §9.2.1 "Safe
   Methods"), and a request built to fail validation is not thereby unsafe.
   The registry lives here because the decision does: `engine/trusted.mjs` loads this file verbatim and its
   `--explore <origin>` writes here, and the offscreen restores a person's grants into the same table. A
   permission buys the request; carrying the reply to a forced request as forced, never merged into the
   observed pool, is the engine's obligation. */
/* How reliable each signal is, declared beside it, so a person ticking a row knows whether they permit
   something this zone knows, something another zone said, or something nobody established.
     "certain"      — this zone computes the value from its own literals and code.
     "stated"       — another zone determined it; the CHECKs assert only the vocabulary, so a wrong word
                      inside it is a claim this zone relays, not a fact it holds.
     "partial"      — a lower bound: one value is a fact when it appears, and the other is a stated unknown,
                      never the first one's negative.
     "undetermined" — nothing in this zone can compute it, so its only value is `unknown`; it is shown anyway,
                      because permitting a request whose lineage nobody established differs from permitting
                      one known to have none.
     "intent"       — an intent the engine holds and cannot verify: a probe built to fail validation cannot be
                      known to be refused before business logic. */
var _SIGNAL_CERTAINTY = ["certain", "stated", "partial", "undetermined", "intent"];
/* URL-carried authority: a match in the deny direction only. Matching is banned because a matched name
   would be asserted as a value; a match that only refuses asserts nothing. A hit is a fact and a miss is not
   a finding: a miss reads `unknown`, never `none`.
   The list is what authorizes a stranger without a session: presigned object-store signatures, reset and
   invite tokens, signed webhooks. `key` and `api_key` are excluded: an API key is the application's
   authority, not the person's, and admitting it would make `present` the common answer and train readers to
   ignore the row. A bare opaque token in a path segment is indistinguishable from an object id; the JWS
   prefix below is the one self-identifying path-borne form.
   Named residual. Not covered: authority in a path segment that is not a JWS (an opaque `/invite/<40 hex>`
   reads `unknown`). Next diff: read the signal off the engine, whose park knows which source each segment
   came from, so a segment from a credentialed reply answers this and lineage together. Its absence shows as
   `url-authority=unknown` doing all the work at a widened origin whose addresses carry capability tokens,
   while the `present` row is never exercised. */
var _URL_AUTHORITY_PARAMS = ["x-amz-signature", "x-goog-signature", "x-ms-signature", "sig", "signature",
                             "hmac", "token", "access_token", "auth_token", "reset_token", "invite_token"];
function _urlAuthorityMarker(u) {
  var found = null;
  u.searchParams.forEach(function (v, n) {
    if (found === null && _URL_AUTHORITY_PARAMS.indexOf(String(n).toLowerCase()) >= 0) found = String(n).toLowerCase();
  });
  if (found !== null) return found;
  /* A JWS compact serialization anywhere in the path or query. RFC 7515 §3.1 "JWS Compact Serialization
     Overview" makes the first component `BASE64URL(UTF8(JWS Protected Header))` and §4 "JOSE Header" makes
     that a JSON object, so it begins `{"` and a member name; base64url of those three bytes is `eyJ` whenever
     the name begins with an ASCII letter or `_` (`{"a` is `eyJh`, while `{"0` is `eyIw`). Every registered
     JOSE header name begins with a letter. A lower bound like every other entry: a miss is `unknown`. */
  if ((u.pathname + u.search).indexOf("eyJ") >= 0) return "jws";
  return null;
}
/* The registry: one declaration, read by the firing walk and by the surface. The order is the refusal's
   order: the refusal names the first gating signal whose value the origin does not permit, so `destination`
   comes first and the coarsest sentence ("this origin does not permit data requests") is the one a person
   reads. `gates` is asserted rather than styled: a non-gating signal has exactly one value at this
   chokepoint, and `_signalRegistryCheck` aborts host startup if one grows a second. A non-gating signal is
   still shown, because it is a fact a person is entitled to see before permitting anything. */
var _SIGNALS = [
  /* Fetch §2.2.5 "Requests"' destination, read as the distinctions the policy turns on, each by its own
     predicate over the one string: `_isScriptLike` answers CORB's "is this code", and the others answer whose
     act the request is. A program load and a subresource load are the page loading itself (the app's own
     bytes, served identically to every visitor). Every other request is `value`, a navigation included: by
     the project owner's decision a new navigable, like a postMessage, is judged by the same rows as a
     `fetch()`, since each reaches another document's state.
     The cascade order is load-bearing because §2.2.5's sets overlap: `script`, `audioworklet` and
     `paintworklet` are script-like and subresources, while `serviceworker`, `sharedworker` and `worker` are
     script-like only. Asking the CORB set first keeps all six reading `program`; the other order would leave
     the last three in no arm. */
  { name: "destination", gates: true, certainty: "stated",
    values: ["program", "subresource", "value"],
    of: function (f) {
      if (_isScriptLike(f.destination)) return "program";
      return _isDocumentSubresource(f.destination) ? "subresource" : "value";
    } },
  /* Whose act this request is (see `_actorOf`). Second, so after "this origin does not permit data requests"
     a person reads "and this is a request this tool composed rather than one your page made". `certain`, the
     one gating row that can say so: it is a literal in trusted-zone source at every asker, while the grades
     beside it are words the attacker-controlled engine composes. A `page` request the engine states it minted
     inside a flow this tool drives (`driven`: a drive of uncalled code or a candidate run, see
     `_drivenOf`) reads `tool`: the engine's word can only
     narrow the row, never widen it. */
  { name: "actor", gates: true, certainty: "certain", values: _ACTOR_WORDS,
    of: function (f) { return f.actor === "page" && f.driven === "driven" ? "tool" : f.actor; } },
  /* The request's provenance (see `_provenanceOf`). `stated`: the engine composes it at the park from the
     park's kind and the flow's `path_forced`, and the CHECK catches only a word outside the three. */
  { name: "provenance", gates: true, certainty: "stated", values: _PROVENANCE_TYPES,
    of: function (f) { return f.provenance; } },
  /* How the document holding this request was reached (see `_docReachOf`): the same three words about a
     different act. A page this tool chose to open makes its own `fetch()`es, each `provenance=observed`, so
     this row is what stops one permission answering for both. It follows `provenance` so the one-level-out
     sentence comes next. `stated`: the zone that performed the load composes it, and the CHECK asserts only
     the vocabulary. */
  { name: "doc-reach", gates: true, certainty: "stated", values: _PROVENANCE_TYPES,
    of: function (f) { return f.docReach; } },
  /* Whether the person's cookie jar pays, and it means cookies and nothing wider: `credentials:"omit"` does
     not strip an `Authorization` header, authority in the address, or lineage, which is why those are their
     own rows. `certain`: `_credentialedOf` composes it from this zone's literal and an asserted mode, and it
     decides what `init.credentials` says. */
  { name: "cookies", gates: true, certainty: "certain", values: ["yes", "no"],
    of: function (f) { return f.credentialed ? "yes" : "no"; } },
  /* Whether other authority rides in a header, which is a question about whose list it is. On the XHR path
     `opts.headers` is the bundle's own list, which may carry a bearer token the person's session minted, and
     nothing states its owner. So `none` is a fact (with no list this zone adds no authority) and `unknown` is
     a stated unknown. Not `partial`, because here the informative value is the absence. */
  { name: "header-authority", gates: true, certainty: "certain", values: ["none", "unknown"],
    of: function (f) { return f.headers ? "unknown" : "none"; } },
  /* solver/engine.h's witness mark: whether the flow had determined some source's value on an arm nothing
     observed before it built this address, so the address may carry a witness the engine picked. Composed at
     the park from `flow_path_pinned`, or at the act (`engine_pinned_of_running_path`) for a request the
     page's code made without parking. `unstated` is this zone's word for an act whose record carries no mark
     (see `_pinnedOf`). */
  { name: "witness", gates: true, certainty: "stated", values: _PINNED_MARKS,
    of: function (f) { return f.pinned; } },
  /* Authority in the address (see `_urlAuthorityMarker`). `partial`: `present` is a fact, and `unknown` is not
     the statement that there is none. */
  { name: "url-authority", gates: true, certainty: "partial", values: ["present", "unknown"],
    of: function (f) { return _urlAuthorityMarker(f.url) !== null ? "present" : "unknown"; } },
  /* Credential lineage: do this address's bytes derive from a response fetched credentialed. The chokepoint
     cannot see it: it reads the credential state of the request in front of it, and an address the bundle
     computed from a credentialed reply (`/api/orgs/{orgId}/members/{userId}`) is about that person's account
     whatever headers it carries. It is shown as `unknown` rather than omitted, so a person's permission says
     they are permitting requests whose lineage nobody established. It is not what `doc-reach` answers: that
     row is about the act that reached the document, and neither bounds the other.
     Named residual. Not covered: every request; the only value is `unknown`. Next diff: a `credentialed` bit
     on the response the engine ingests, carried into the concolic value's source identity and composed at the
     park beside the provenance word. Its absence shows as every request at an origin reading
     `lineage=unknown`, including addresses the bundle demonstrably built from a logged-in reply. */
  { name: "lineage", gates: true, certainty: "undetermined", values: ["unknown"],
    of: function () { return "unknown"; } },
  /* The method, a fact about this transport and not a control: `init` hardcodes `method:"GET"` and
     `_refuseUnreadOptions` refuses a caller that states one, so the value space has one member. It is shown
     because a person deciding is entitled to know this transport sends one verb; the day it can send another,
     `_signalRegistryCheck` forces the row to gate. */
  { name: "method", gates: false, certainty: "certain", values: ["GET"],
    of: function () { return "GET"; } },
  /* Intended invalidity: an intent the engine holds and cannot verify, with no population at this chokepoint.
     A probe built to fail validation is a POST with a body, and the closed option set has neither, so no
     caller can state such an intent; `unstated` is a structural fact, not a default. The population lives on
     `safeFetchOperator`, whose verb is stated at its own call site and which has no firing decision. The
     row starts gating the day a verb and a body exist in `_SAFEFETCH_OPTIONS`; until then `_signalRegistryCheck` asserts it has
     one value. */
  { name: "invalidity", gates: false, certainty: "intent", values: ["unstated"],
    of: function () { return "unstated"; } }
];
/* The registry is checked once, at the door every host goes through (`safeFetchEgressStated`), on what a
   grant is keyed by: a duplicate name would let one row's permission answer for another's; an unknown
   certainty renders as a blank where a person reads how far to trust a row; and a non-gating signal with
   more than one value is a dropped control, since every grant would silently permit both. */
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
  /* The default arms against the same registry. An arm naming an undeclared signal or value compares against
     `undefined` and is false for ever, silently revoking what it permitted (for the page's own `fetch()`,
     every waiting flow stays parked with no crash). A CHECK, because release cannot proceed correctly through
     an arm this build shipped broken. */
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
/* The facts every signal is computed from, required at the one derivation the firing walk and the surface
   share, so neither reader can be weaker than the other. Four unstated facts (`provenance`, `doc-reach`,
   `witness`, `actor`) compute outside their value space and the DCHECK in `_signalVector` would catch them;
   three compute a value inside it from nothing: an absent `destination` reads `value`, an absent
   `credentialed` reads `cookies=no`, an absent `headers` reads `header-authority=none`. The last two are
   `certain` rows, so a person would be shown an established fact nobody stated; release cannot proceed
   correctly through that, hence a CHECK.
   It may assert because every field is composed in trusted-zone source: the surface's caller builds a
   literal whose `url` `safeFetchWidenable` has answered for, and the engine-supplied values reaching
   `safeFetch` are CHECKed at their read. No page or renderer can reach this abort. */
function _requireFacts(facts) {
  CHECK(facts !== null && typeof facts === "object",
        "the signal vector was asked for with no facts — every signal is computed from them and a caller " +
        "that passed none would be answered by whichever arm each signal's predicate happens to reach first");
  CHECK(typeof facts.destination === "string" && _PROVENANCE_TYPES.indexOf(facts.provenance) >= 0 &&
        _PROVENANCE_TYPES.indexOf(facts.docReach) >= 0 &&
        _PINNED_MARKS.indexOf(facts.pinned) >= 0 && _ACTOR_WORDS.indexOf(facts.actor) >= 0 &&
        _DRIVEN_MARKS.indexOf(facts.driven) >= 0 &&
        typeof facts.credentialed === "boolean" &&
        facts.url !== null && typeof facts.url === "object" && typeof facts.url.origin === "string",
        "a signal vector was asked for without the facts that decide it — Fetch §2.2.5 \"Requests\"' " +
        "DESTINATION says " +
        "whether this reply becomes a PROGRAM or a VALUE, the actor says whether the ANALYSED PAGE composed " +
        "this request or THIS TOOL did, the drive mark says whether the engine's own drive minted it, the " +
        "provenance says what its path is evidence of, the reach " +
        "grade says whose act the DOCUMENT it was made from was, the witness " +
        "mark says whether the address may rest on a value this engine chose, the credential flag says " +
        "whether the person's session pays, and the parsed URL is what the address-borne authority is read " +
        "off. A caller that omitted any of them would be answered by the FIRING walk's permissive arm, " +
        "which is the one that spends an act, and shown on the SURFACE as a value with no fact under it: " +
        "an absent destination reads `value`, an absent credential flag reads `cookies=no` and an absent " +
        "header list reads `header-authority=none`, both of those on rows this registry grades `certain`. " +
        "Every one of them is a fact the CALLER holds and this file cannot " +
        "re-derive: forgetting to state one may never be a way to be exempted");
  /* The header list's shape: `header-authority` reads it by truthiness and grades `none` as `certain`, so a
     primitive (`""`, `0`, `false`) would be shown as an established absence. `null` and absent are how a
     caller with no list says so. Shape only, never contents: on the XHR path the names and values are the
     bundle's and are input. */
  CHECK(facts.headers === null || facts.headers === undefined || typeof facts.headers === "object",
        "a signal vector was asked for with a header list that is not one: " +
        JSON.stringify(facts.headers) + " — the `header-authority` row reads this by TRUTHINESS and grades " +
        "the answer `certain`, so a primitive here is rendered to a person as the established fact that no " +
        "authority rides in a header, on a request whose header list was never stated. `null` is how a " +
        "caller with no list says so and is a fact; anything that is not an object is a call site that " +
        "meant something else. This asserts the SHAPE and never the CONTENTS — the names and values are " +
        "the analysed bundle's on the XHR path and are input this zone may not assert about");
}
/* The vector for one request: every signal's value, computed from the facts required above. The firing walk
   and the surface both read it, so a person is never shown a value the walk did not decide from. */
function _signalVector(facts) {
  var v = Object.create(null), i, s, val;
  _requireFacts(facts);
  for (i = 0; i < _SIGNALS.length; i++) {
    s = _SIGNALS[i];
    val = s.of(facts);
    /* The computation and the declared value space must agree: a grant names declared values, so a value
       outside the space can never be permitted. A DCHECK, because release refuses it, the conservative arm. */
    DCHECK(s.values.indexOf(val) >= 0,
           "the egress signal `" + s.name + "` computed " + JSON.stringify(val) + ", which is not in the " +
           "value space it declares (" + s.values.join(", ") + ") — a grant names values out of that space, " +
           "so this request can never be permitted by any setting and the person's control has no row for " +
           "the answer it is being refused with");
    v[s.name] = val;
  }
  return v;
}
/* What fires without anybody saying so, as data the surface shows. Every arm is the page loading itself, as a
   browser would: its program loads (scripts, module imports, lazy chunks), its other subresources, the
   requests of a document reached by observation, the page's own unpinned data requests, and child navigables
   its markup or code names. No probe, no discovery and no tool-composed data fetch fires until the origin is
   deliberately widened. The discriminator for every arm is whose act the request is, not what its reply
   becomes.
   The two mistakes are not the same size. Refusing the page's own request silently parks every flow waiting
   on its reply, with no crash and an empty report; permitting it relays a request the person's own browser
   made anyway. Each arm is one entry to delete if the intended reading is narrower, and the refusal that
   follows names the signal. These are permissions only, so nothing here narrows the table. */
var _DEFAULT_ARMS = [
  /* Program loads are destination-keyed alone. A forced segment in a program's address does not move it
     across the line: `import("/chunks/" + region + ".js")` with `region` pinned by forcing is still the app's
     own code, served to anyone who asks, and is the gated surface this product exists to reach. So the
     `witness` row cannot narrow this arm. */
  { when: [{ signal: "destination", value: "program" }],
    why: "a script, a module import or a lazy chunk is the page loading itself — the app's own code, served " +
         "byte-identically to every visitor, revealing nothing about this person" },
  /* The rest of what a document loads in order to be itself: a subresource the page's markup names or its
     running code computes is fetched as a browser fetches it. Read by CORB's question alone a stylesheet would
     be refused and the page would render with UA defaults, which is a different document. Destination-keyed
     alone for the program arm's reason. A conjunction with `provenance=observed` would be inert, since no
     subresource park is graded `observed` (`pending_prov_compose` grades only a parser-inserted `<script>`
     so). Reach: every §2.2.5 subresource destination outside the script-like set, except the empty string. */
  { when: [{ signal: "destination", value: "subresource" }],
    why: "a stylesheet, an image, a font or any other subresource the document's own markup or its own " +
         "running code names is the page loading itself — the same request the person's own browser would " +
         "have made, for bytes served byte-identically to every visitor" },
  /* The requests a real load of an observed document makes. It is a conjunction because `provenance=observed`
     alone would also admit every request made inside a document this tool reached by a derived or forced
     route, which the engine correctly grades `observed`; `doc-reach` is that second population's row. Today
     only a parser-inserted `<script>` request is graded `observed`, so for page requests this arm overlaps
     the program arm. It is also what fires the ambient seed's document load, which states `observed` for
     both; deleting it would refuse every document this tool opens. */
  { when: [{ signal: "provenance", value: "observed" }, { signal: "doc-reach", value: "observed" }],
    why: "the page made exactly this request, in a document this browser actually navigated to — so " +
         "relaying it is this browser being a browser" },
  /* The analysed page's own requests, by the project owner's decision: a `fetch()`, an XHR or a navigation
     whose address carries no value this path pinned fires at every origin, while anything a fork pinned, and
     every probe, is refused. A navigation is not special-cased: a new navigable, like a postMessage, reaches
     another document's state exactly as a data request reaches a server's. Each conjunct carries one clause:
     `witness: unpinned` refuses an address a fork pinned and `actor: page` refuses the probe; removing either
     admits a population the other names. It is keyed on `pinned`, not `forced`: a forced-but-unpinned path
     determined no value, so its address holds no witness the engine chose, and keying on `forced` would
     refuse the boot of any SPA whose boot flow forks.
     Reach: the empty-string destination (the page's own `fetch()` off the pending line and its XHR off the
     `xhr.send` record) and §2.2.5's navigation destinations, of which the engine states only `document`
     (HTML §7.4.5 "Populating a session history entry"). A data request is uncredentialed; a navigation
     carries the session only same-origin (`navigationCarriesSession`), and the deny list applies to it.
     Never write `destination=value` alone: it passes every assert in this file and permits every such
     request at every origin. */
  { when: [{ signal: "destination", value: "value" },
           { signal: "witness", value: "unpinned" },
           { signal: "actor", value: "page" }],
    why: "the analysed page's own code made this request or navigation and composed its address out of " +
         "nothing this engine pinned — so what fires is the app asking for its own state, not a probe this " +
         "tool built" }
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
   did not exist when it was granted, so it is dropped and named in the answer. An origin with any signal
   whose values are not an array is dropped and named the same way, whole, so what the answer names is what
   is not permitted. A signal a stored grant does not name is not permitted, so adding a signal narrows every
   existing grant rather than silently widening it. */
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
    if (typeof table[origin] !== "object" || table[origin] === null || Array.isArray(table[origin]) ||
        Object.keys(table[origin]).some(function (k) { return !Array.isArray(table[origin][k]); })) {
      legacy.push(origin);
      continue;
    }
    for (signal in table[origin]) {
      if (!Object.prototype.hasOwnProperty.call(table[origin], signal)) continue;
      values = table[origin][signal];
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
  v = _signalVector(facts);
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
                          actor: facts.actor, driven: facts.driven,
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
                         actor: facts.actor, driven: facts.driven,
                         credentialed: facts.credentialed, headers: facts.headers });
}
/* Fetch §4.10 "CORS check", over the reply's own headers: "Let origin be the result of getting
   `Access-Control-Allow-Origin` from response's header list. If origin is null, then return failure. Null is not
   `null`. If request's credentials mode is not "include" and origin is `*`, then return success. If the result of
   byte-serializing a request origin with request is not origin, then return failure. If request's credentials
   mode is not "include", then return success. Let credentials be the result of getting
   `Access-Control-Allow-Credentials` from response's header list. If credentials is `true`, then return
   success. Return failure." Answers the failing step's name, or null for success. `headers` is this file's
   lowercased map, whose duplicate values are already combined, so a list of two origins matches neither. */
function _corsCheck(headers, serializedOrigin, credentialsMode) {
  var origin = headers["access-control-allow-origin"];
  if (origin === undefined) return "no-allow-origin";
  if (credentialsMode !== "include" && origin === "*") return null;
  if (origin !== serializedOrigin) return "allow-origin-mismatch";
  if (credentialsMode !== "include") return null;
  if (headers["access-control-allow-credentials"] === "true") return null;
  return "no-allow-credentials";
}
/* Fetch §2.2.6 "Responses"' CORS filtered response: "a filtered response whose type is "cors" and header list
   excludes any headers in internal response's header list whose name is not a CORS-safelisted response-header
   name, given internal response's CORS-exposed header-name list". §2.2.2 "Headers" defines the safelist
   ("`Cache-Control`, `Content-Language`, `Content-Length`, `Content-Type`, `Expires`, `Last-Modified`,
   `Pragma`, Any item in list that is not a forbidden response-header name") and the forbidden names
   (`Set-Cookie`, `Set-Cookie2`). The exposed list is `Access-Control-Expose-Headers`, whose `*` counts as a
   wildcard only for a request whose credentials mode is not "include". The filter runs here, in the zone that
   read the headers, so what the page may not read never reaches the engine. */
var _CORS_SAFELISTED_RESPONSE = ["cache-control", "content-language", "content-length", "content-type",
                                 "expires", "last-modified", "pragma"];
var _FORBIDDEN_RESPONSE = ["set-cookie", "set-cookie2"];
function _corsFilteredHeaders(headers, credentialsMode) {
  var exposed = String(headers["access-control-expose-headers"] === undefined ? ""
                       : headers["access-control-expose-headers"])
    .split(",").map(function (n) { return n.trim().toLowerCase(); }).filter(function (n) { return n !== ""; });
  var all = credentialsMode !== "include" && exposed.indexOf("*") >= 0;
  var out = {}, k;
  for (k in headers) {
    if (!Object.prototype.hasOwnProperty.call(headers, k)) continue;
    if (_CORS_SAFELISTED_RESPONSE.indexOf(k) >= 0 ||
        (_FORBIDDEN_RESPONSE.indexOf(k) < 0 && (all || exposed.indexOf(k) >= 0)))
      out[k] = headers[k];
  }
  return out;
}
/* Fetch §2.2.1 "Methods": "A CORS-safelisted method is a method that is `GET`, `HEAD`, or `POST`." */
function _corsSafelistedMethod(m) { return m === "GET" || m === "HEAD" || m === "POST"; }
/* Fetch §2.2.2 "Headers"' CORS-unsafe request-header byte: below 0x20 except HT, or one of `"():<>?@[\]{}`
   and DEL. A header value here is a byte string, one code point per byte. */
function _hasCorsUnsafeRequestHeaderByte(v) {
  for (var i = 0; i < v.length; i++) {
    var c = v.charCodeAt(i);
    if ((c < 0x20 && c !== 0x09) || '"():<>?@[\\]{}\x7f'.indexOf(v[i]) >= 0) return true;
  }
  return false;
}
/* MIME Sniffing §4.4 "Parsing a MIME type", as far as the essence: a parameter never makes that algorithm fail,
   so the essence (or null for failure) is decided by its steps before the parameters. */
var _HTTP_TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
function _mimeEssence(input) {
  var s = input.replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, "");
  var slash = s.indexOf("/");
  if (slash < 0) return null;
  var rest = s.slice(slash + 1), semi = rest.indexOf(";");
  var type = s.slice(0, slash), sub = (semi < 0 ? rest : rest.slice(0, semi)).replace(/[\t\n\r ]+$/, "");
  if (!_HTTP_TOKEN.test(type) || !_HTTP_TOKEN.test(sub)) return null;
  return (type + "/" + sub).toLowerCase();
}
/* Fetch §2.2.2's "parse a single range header value" with allowWhitespace false: [start, end] with null for an
   absent bound, or null for failure. */
function _singleRangeValue(v) {
  var m = /^bytes=([0-9]*)-([0-9]*)$/.exec(v);
  if (!m) return null;
  var start = m[1] === "" ? null : Number(m[1]), end = m[2] === "" ? null : Number(m[2]);
  if (start === null && end === null) return null;
  if (start !== null && end !== null && start > end) return null;
  return [start, end];
}
/* Fetch §2.2.2's CORS-safelisted request-header, given a name and a value. */
function _corsSafelistedRequestHeader(name, value) {
  if (value.length > 128) return false;
  switch (name.toLowerCase()) {
  case "accept":
    return !_hasCorsUnsafeRequestHeaderByte(value);
  case "accept-language":
  case "content-language":
    return /^[0-9A-Za-z *,\-.;=]*$/.test(value);
  case "content-type": {
    if (_hasCorsUnsafeRequestHeaderByte(value)) return false;
    var essence = _mimeEssence(value);
    return essence === "application/x-www-form-urlencoded" || essence === "multipart/form-data" ||
           essence === "text/plain";
  }
  case "range": {
    var r = _singleRangeValue(value);
    return r !== null && r[0] !== null;
  }
  default:
    return false;
  }
}
/* Fetch §2.2.2's CORS-unsafe request-header names of a header list (`[name, value]` pairs), as the
   sorted-lowercase set the spec returns: every unsafelisted name, and every name once the safelisted values
   exceed 1024 bytes together. */
function _corsUnsafeRequestHeaderNames(list) {
  var unsafe = [], potentially = [], size = 0;
  list.forEach(function (h) {
    if (!_corsSafelistedRequestHeader(h[0], h[1])) unsafe.push(h[0]);
    else { potentially.push(h[0]); size += h[1].length; }
  });
  if (size > 1024) unsafe = unsafe.concat(potentially);
  var set = [];
  unsafe.forEach(function (n) { n = n.toLowerCase(); if (set.indexOf(n) < 0) set.push(n); });
  return set.sort();
}
/* Fetch §2.2.2's "extract header list values" for a `#method` or `#field-name` header (Fetch §3.3.4 "HTTP
   new-header syntax"): undefined where the header is absent (the spec's null), null where its value is not
   that list (the spec's failure), else the elements. `headers` is this file's lowercased map, whose repeated
   fields are already joined with ", ", which is the same list. */
function _extractTokenList(headers, name) {
  if (headers[name] === undefined) return undefined;
  var out = [], parts = String(headers[name]).split(",");
  for (var i = 0; i < parts.length; i++) {
    var e = parts[i].replace(/^[\t ]+|[\t ]+$/g, "");
    if (e === "") continue;
    if (!_HTTP_TOKEN.test(e)) return null;
    out.push(e);
  }
  return out;
}
/* Fetch §4.8 "CORS-preflight fetch" for a request (`method`, `href`, `headerList`, credentials mode, use-CORS-
   preflight flag), CORS-checked against `origin`, the Origin the request carries. Answers null on success or
   the name of the step that failed, which is a network error. The preflight is uncredentialed and does not
   follow a redirect: a 3xx is not an ok status. This zone provides no CORS-preflight cache, so the step "If
   the user agent does not provide for a cache, then return response" ends it and `Access-Control-Max-Age` is
   not read. */
async function _corsPreflight(href, method, headerList, credentialsMode, usePreflightFlag, origin, signal) {
  var unsafe = _corsUnsafeRequestHeaderNames(headerList);
  /* `Access-Control-Request-Method` and `-Headers` are forbidden request-headers (§2.2.2), which a script's
     fetch cannot set; the OPTIONS rule speakAsPreflightHeaders composes (lib/speak-as.js) supplies both. */
  var init = { method: "OPTIONS", headers: [["Accept", "*/*"]], credentials: "omit", redirect: "manual" };
  if (signal) init.signal = signal;
  var resp = await fetch(href, init);
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  if (resp.body) {
    try { await resp.body.cancel(); } catch (e) { RETHROW_FATAL(e); }
  }
  var cors = _corsCheck(headers, origin, credentialsMode);
  if (cors) return "cors-check:" + cors;
  if (!(resp.status >= 200 && resp.status <= 299)) return "status:" + resp.status;
  var methods = _extractTokenList(headers, "access-control-allow-methods");
  var headerNames = _extractTokenList(headers, "access-control-allow-headers");
  if (methods === null) return "allow-methods-unparseable";
  if (headerNames === null) return "allow-headers-unparseable";
  if (methods === undefined && usePreflightFlag) methods = [method];
  methods = methods || [];
  headerNames = (headerNames || []).map(function (n) { return n.toLowerCase(); });
  if (methods.indexOf(method) < 0 && !_corsSafelistedMethod(method) &&
      (credentialsMode === "include" || methods.indexOf("*") < 0))
    return "method-not-allowed:" + method;
  for (var i = 0; i < headerList.length; i++)
    if (headerList[i][0].toLowerCase() === "authorization" && headerNames.indexOf("authorization") < 0)
      return "header-not-allowed:authorization";
  for (var j = 0; j < unsafe.length; j++)
    if (headerNames.indexOf(unsafe[j]) < 0 && (credentialsMode === "include" || headerNames.indexOf("*") < 0))
      return "header-not-allowed:" + unsafe[j];
  return null;
}
/* A request that speaks as an identity (lib/request-identity.js's identityFor answer) runs `send` under
   lib/speak-as.js's rules: one for its own method and, when `preflightNames` is a list, one for its OPTIONS
   request. Its cookies are the identity's (cookieHeaderFor's Cookie) only where this zone is willing
   (`credentialed`); otherwise it carries none. The zone's own fetch omits credentials, so this extension's
   jar adds nothing and a reply's Set-Cookie is not stored. `stated` is null, or the operator tier's
   Send-panel record (safeFetchStatedShape), whose Origin and Referer replace the identity's on every rule of
   the send. A request with no identity runs `send` as this extension, uncredentialed.
   Named residual. Not covered: a credentialed send's reply Set-Cookie, which the identity's own request would
   store in its jar and this send stores nowhere. Next diff stores it there through a route that reads this
   extension's own reply headers; webRequest reports none of the offscreen document's requests. Its absence
   shows as a server that rotates a token on a tool-sent credentialed request leaving the person's next own
   request presenting the retired value. */
async function _speakAs(identity, href, method, mode, destination, credentialed, preflightNames, stated, init, send) {
  if (!identity) return send();
  CHECK(typeof withSpeakAsRules === "function",
        "a request states an identity on a host that loads no lib/speak-as.js — the identity's headers would " +
        "be dropped and the request would go out as this extension");
  var facts = credentialed ? identity : Object.assign({}, identity, { credentials: "omit" });
  var cookie = await cookieHeaderFor(facts, href);
  init.credentials = "omit";
  var rules = [{ method: method, headers: speakAsHeaders(facts, method, mode, destination, href, cookie) }];
  if (preflightNames !== null)
    rules.push({ method: "OPTIONS", headers: speakAsPreflightHeaders(facts, method, destination, href, preflightNames) });
  if (stated !== null)
    rules.forEach(function (r) {
      if (stated.origin !== null) r.headers["Origin"] = stated.origin.header;
      if (stated.referer !== null) r.headers["Referer"] = stated.referer;
    });
  /* The worker admits an Origin or Referer outside the identity only as a value the person stated; removing
     a header needs no admission. */
  var admitted = stated === null ? null
    : { origin: stated.origin === null ? null : stated.origin.header, referer: stated.referer };
  return withSpeakAsRules(identity.origin, href, rules, admitted, send);
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

/* The IP address space of a host, for the origin-relative SSRF rule: Local Network Access §2.1 "IP Address
   Space"'s "Non-public IP address blocks" table, row for row in `_v4Space` and `_ipAddressSpace`, including
   ::ffff:0:0/96's "see mapped IPv4 address"; every other address is `public`. Two names are classified by the
   standards that define them: `localhost` and `*.localhost` are loopback (RFC 6761 §6.3 "Domain Name
   Reservation Considerations for "localhost."": they "will always resolve to the respective IP loopback
   address"), and `*.local` is local (Local Network Access §2.5 "Mixed Content" names a `.local` domain as a
   request known to be a local network request). Any other name is `public` here; what it resolves to is not
   visible to this zone, which is the private-network-by-name residual in SECURITY.md.
   Every host comes from the WHATWG URL parser, so it is canonical: IPv4 is dotted-quad (the parser folds
   decimal, octal and hex spellings, so `http://2130706433/` is `127.0.0.1`), and IPv6 is the serializer's
   lowercase hex pieces with at most one `::` and no dotted tail. */
function _v4Octets(host) {
  var m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
  return m ? [+m[1], +m[2], +m[3], +m[4]] : null;
}
function _v6Pieces(host) {
  if (host.indexOf(":") < 0) return null;
  var halves = host.split("::");
  DCHECK(halves.length <= 2, "an IPv6 host carries two `::` — the WHATWG IPv6 serializer compresses one run");
  var head = halves[0] === "" ? [] : halves[0].split(":");
  var tail = halves.length === 2 ? (halves[1] === "" ? [] : halves[1].split(":")) : [];
  var fill = halves.length === 2 ? 8 - head.length - tail.length : 0;
  var parts = head.concat(new Array(fill).fill("0"), tail);
  DCHECK(parts.length === 8 && parts.every(function (p) { return /^[0-9a-f]{1,4}$/.test(p); }),
         "an IPv6 host is not eight hex pieces once expanded (" + host + ") — the URL parser serializes every " +
         "IPv6 address as hex pieces, so this host did not come from URL.hostname");
  return parts.map(function (p) { return parseInt(p, 16); });
}
function _v4Space(o) {
  if (o[0] === 127) return "loopback";
  if (o[0] === 10) return "local";
  if (o[0] === 100 && o[1] >= 64 && o[1] <= 127) return "local";
  if (o[0] === 172 && o[1] >= 16 && o[1] <= 31) return "local";
  if (o[0] === 192 && o[1] === 168) return "local";
  if (o[0] === 198 && (o[1] === 18 || o[1] === 19)) return "loopback";
  if (o[0] === 169 && o[1] === 254) return "local";
  if (o[0] === 0 && o[1] === 0 && o[2] === 0 && o[3] === 0) return "loopback";
  if (o[0] === 0) return "local";
  return "public";
}
function _ipAddressSpace(host) {
  if (!host) return "public";
  host = String(host).toLowerCase().replace(/^\[|\]$/g, "");
  /* The canonical form is the contract: a literal with both a colon and a dot is the dotted IPv4-in-IPv6
     text no URL parser emits, so its caller passed a raw string instead of `URL.hostname`. */
  DCHECK(!(host.indexOf(":") >= 0 && host.indexOf(".") >= 0),
         "a host reached the SSRF classifier as an IPv4-in-IPv6 literal with a dotted tail (" + host + ") — " +
         "the WHATWG IPv6 serializer emits hex pieces only, so this host did not come from URL.hostname and " +
         "is about to be classified by rules written for the form that one produces");
  if (host === "localhost" || host.endsWith(".localhost")) return "loopback";
  if (host.endsWith(".local")) return "local";
  var o = _v4Octets(host);
  if (o) return _v4Space(o);
  var p = _v6Pieces(host);
  if (!p) return "public";
  var zero5 = p[0] === 0 && p[1] === 0 && p[2] === 0 && p[3] === 0 && p[4] === 0;
  if (zero5 && p[5] === 0xffff)
    return _v4Space([p[6] >> 8, p[6] & 255, p[7] >> 8, p[7] & 255]);
  if (zero5 && p[5] === 0 && p[6] === 0 && (p[7] === 0 || p[7] === 1)) return "loopback";
  if ((p[0] & 0xfe00) === 0xfc00) return "local";
  if ((p[0] & 0xffc0) === 0xfe80) return "local";
  if ((p[0] & 0xffc0) === 0xfec0) return "local";
  if (p[0] === 0x2001 && p[1] === 0x0db8) return "local";
  if (p[0] === 0x3fff && (p[1] & 0xf000) === 0) return "local";
  return "public";
}
/* Local Network Access §2.1's "less public than": loopback is less public than local and public, and local is
   less public than public. A request is refused when its target is less public than the page principal, which
   is §2.2 "Local Network Request"'s definition of the request a browser would gate. */
var _SPACE_RANK = { loopback: 0, local: 1, public: 2 };
function _lessPublicThan(lhs, rhs) { return _SPACE_RANK[lhs] < _SPACE_RANK[rhs]; }
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
   allowlist, the userinfo refusal, both private-host checks, both destructive-path checks, both firing
   refusals and the credentialed SOP/CORS check. After it: CORB, the one gate that reads the body.
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
   Guarantees: GET only; http(s) only; no userinfo; cookies omitted unless `opts.credentialed`, and then the
   reply passes SOP/CORS (same-origin with `opts.pageOrigin`, else exact-origin ACAO and ACAC `true`); the
   origin-relative SSRF rule; the destructive-path deny list on credentialed non-`observed` requests; the
   egress policy's firing walk; CORB for script-like destinations; for a page's request, Fetch §4.1 "Main
   fetch"' response tainting from its mode (`cors` passes §4.10 "CORS check" and is CORS-filtered,
   cross-origin `no-cors` is opaque, cross-origin `same-origin` is a network error), stated as the record's
   `type`. Every post-fetch gate judges the landed URL.
   Principals, per call: `opts.pageUrl` (`MessageSender.url`) classifies the SSRF target; `opts.pageOrigin`
   (`MessageSender.origin` or the host-assigned cluster origin, never parsed from a URL or taken from the
   engine) is the SOP principal and every page request's origin. An opaque principal is same-origin with
   nothing. Credentialed callers are `bridge.js`'s `navigationLoad` and `frontierRederive`, only where
   `navigationCarriesSession` holds; the page's own requests (`fetched`, `fetchedXhr`) are uncredentialed.
   Every caller states `destination`, `mode`, `provenance`, `pinned`, `docReach`, `actor`, `credentialed`
   and `credentials`, asserted at the entry. */
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
  var driven = _drivenOf(opts);
  /* Derived once, above every gate it scopes; `_credentialedOf` also refuses a header list on a
     cookie-bearing request. */
  var credentialed = _credentialedOf(opts);
  /* A credentialed request speaks as a document: `_speakAs` sets that document's Cookie and the zone's fetch
     omits credentials. One with no identity would go out with this extension's own jar, whose partition is
     the extension's, and its reply's Set-Cookie would be stored there and in the person's jar. */
  CHECK(!credentialed || (!!opts.identity && typeof opts.identity === "object"),
        "safeFetch was asked for a credentialed request with no identity to speak as — it would send this " +
        "extension's jar and store the reply's cookies where no page of the person's set them");
  var mode = _modeOf(opts);
  /* The request's origin for everything the analysed page's own code asks for: the browser-stated
     `MessageSender.origin` of the document (or the agent-cluster origin this zone assigned an instance), passed
     by the host from its own record and never taken from the engine. Fetch §4.1 "Main fetch" compares the
     request's current URL with it to choose the response tainting. `""` is a principal nobody recorded and is
     treated as an opaque origin, same-origin with nothing. A request this tool composed has this extension's
     origin, whose `<all_urls>` host permission is the browser's answer to the CORS question, so no page origin
     is asked of it. */
  CHECK(actor !== "page" || mode === "navigate" || typeof opts.pageOrigin === "string",
        "safeFetch was asked for a request the analysed page made without the page's origin — Fetch §4.1 " +
        "\"Main fetch\" decides from it whether the page may read the reply (§4.10 \"CORS check\" against it, " +
        "or an opaque response), so a page request with none would be read as same-origin with whatever it " +
        "named. State the origin this zone holds for the requesting document");
  CHECK(actor !== "page" || mode !== "cors" || _CREDENTIALS_MODES.indexOf(opts.credentials) >= 0,
        "safeFetch was asked for a `cors` request the analysed page made without its credentials mode — " +
        "Fetch §4.10 \"CORS check\" reads the request's credentials mode to decide whether `*` passes, so the " +
        "check would be decided by silence");
  CHECK(actor !== "page" || mode !== "cors" || typeof opts.useCorsPreflight === "boolean",
        "safeFetch was asked for a `cors` request the analysed page made without its use-CORS-preflight flag " +
        "(Fetch §2.2.5 \"Requests\") — §4.1 \"Main fetch\" preflights a request with the flag set whatever " +
        "its method and headers, so an unstated flag would decide by silence whether the server is asked first");
  CHECK(opts.headers === undefined || (Array.isArray(opts.headers) && opts.headers.every(function (h) {
          return Array.isArray(h) && h.length === 2 && typeof h[0] === "string" && typeof h[1] === "string";
        })),
        "safeFetch was handed a header list that is not `[name, value]` string pairs — the CORS-unsafe " +
        "request-header names are computed over it, and a list this zone cannot read would be sent unjudged");
  var _pageOrigin = typeof opts.pageOrigin === "string" ? opts.pageOrigin : "";
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
  /* Network: Fetch §4.1 "Main fetch"' switch — "request's mode is "same-origin": Return a network error" —
     for a request that did not match the arm before it ("request's current URL's origin is same origin with
     request's origin"). Asked before the wire and again on the landed URL. */
  if (actor === "page" && mode === "same-origin" &&
      !(_isRealOrigin(_pageOrigin) && parsed.origin === _pageOrigin))
    return _refused("network", "blocked-mode-same-origin:" + parsed.origin, [parsed.href], {});
  /* Decline: a script-like destination cannot be streamed (see `_bodyGated`), and no browser refuses a
     streamed script, so only this tool declines. Serving it whole to a caller that asked for chunks would
     leave that caller believing it streamed past the one gate that needs the whole body. */
  if (opts.onChunk !== undefined && _bodyGated(opts))
    return _refused("decline", "blocked-stream-body-gated:" + _destinationOf(opts), [parsed.href], {});
  /* Origin-relative SSRF. The principal is the analysed document's own browser-stated address, passed per
     call as `opts.pageUrl` (its `MessageSender.url`, never `sender.tab.url`, so a sub-frame of a tab whose top
     is localhost does not inherit a private classification). It is per call, not a global, because the
     trusted zone interleaves many renderers. A cross-origin script acts with the origin of the page it is
     loaded into, never its own host. A target is refused when its IP address space is less public than the
     principal's (`_ipAddressSpace`, Local Network Access §2.1): a public page reaches neither the person's
     network nor their machine, and a page on the local network does not reach the machine's loopback.
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
  /* `_pageUrl ?` is the release arm under the DCHECKs above: an unparseable principal classifies public,
     which is this gate's fail-closed answer. */
  var _pageSpace = _pageUrl ? _ipAddressSpace(_pageUrl.hostname) : "public";
  /* Network: host permissions bypass the browser's own answer, and this reinstates it for the addresses this
     zone can classify. Local Network Access §2.2 "Local Network Request" is the request a browser gates. */
  var _targetSpace = _ipAddressSpace(parsed.hostname);
  if (_lessPublicThan(_targetSpace, _pageSpace))
    return _refused("network", "blocked-local-network:" + _targetSpace + "-from-" + _pageSpace, [parsed.href], {});
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
                               pinned: pinnedMark, docReach: docReach, actor: actor, driven: driven,
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
  /* The one verb this transport sends; the preflight below and the request are built from it. */
  var _method = "GET";
  /* Fetch §4.1 "Main fetch": a `cors` request that is not same-origin with its origin, whose use-CORS-preflight
     flag is set, or whose method is not CORS-safelisted, or whose header list has CORS-unsafe names, runs §4.4
     "HTTP fetch" with makeCORSPreflight, and with no CORS-preflight cache §4.4 always runs §4.8 "CORS-preflight
     fetch" first. Its failure is a network error, and the request itself is never sent. The request's origin
     is `identity.origin`, the trusted zone's identity for this request, which must agree with `pageOrigin`. */
  // A page's `cors` request carries a header list of pairs (asserted at the entry); no other request is preflighted.
  var _unsafeNames = actor === "page" && mode === "cors" ? _corsUnsafeRequestHeaderNames(opts.headers || []) : [];
  var _preflight = actor === "page" && mode === "cors" &&
                   !(_isRealOrigin(_pageOrigin) && parsed.origin === _pageOrigin) &&
                   (opts.useCorsPreflight || !_corsSafelistedMethod(_method) || _unsafeNames.length > 0);
  CHECK(!_preflight || (opts.identity && typeof opts.identity === "object" && typeof opts.identity.origin === "string"),
        "safeFetch must CORS-preflight this request and was handed no request identity — Fetch §4.8's " +
        "preflight carries the request's origin and is CORS-checked against it, and that origin is the " +
        "trusted zone's `identityFor(request)`, which the caller passes as `opts.identity`");
  /* Two records: `opts.identity` is who the request speaks as (its client, Fetch §2.2.5 "Requests"), and
     `pageOrigin` the document that reads the reply. They are one document for every request but a
     navigation's, whose client is the document that started it while its reader is the one it creates. */
  CHECK(!opts.identity || mode === "navigate" ||
        opts.identity.origin === (_isRealOrigin(_pageOrigin) ? _pageOrigin : "null"),
        "a " + mode + " request speaks as " + (opts.identity && opts.identity.origin) + " while its reply is read " +
        "by " + _pageOrigin + " — only a navigation's client differs from its reader, and the preflight and " +
        "the reply would be CORS-checked as two different requesters");
  /* Named residual. Not covered: a redirect hop of a send that speaks as an identity, which Fetch §4.5
     "HTTP-redirect fetch" and HTML §7.4.5 send with the request's destination, mode and referrer policy, and a
     Referer and Cookie recomputed for the new URL. This document cannot read a Location before following it
     (`redirect: "manual"` answers an opaqueredirect with no headers) and a speak-as rule must name its URL, so
     the hop goes out as this extension, uncredentialed. The next diff re-speaks the request at the landed URL
     when the reply was redirected. Its absence shows as a redirected document loaded logged out, its hop
     reaching the server with `Sec-Fetch-Mode: cors` and no Referer. */
  var init = { method: _method, credentials: "omit", redirect: "follow" };
  /* The caller's header list. On the XHR path it is the analysed bundle's own list, which is within the model
     for the request this file issues (an uncredentialed GET whose forbidden header names the browser strips)
     and is refused on a cookie-bearing one by `_credentialedOf`. This file never adds auth headers; cookies,
     in credentialed mode, are the identity's (`_speakAs`) and are gated by the SOP/CORS check below. */
  if (opts.headers) init.headers = opts.headers;
  if (opts.signal) init.signal = opts.signal;
  /* Fetch §4.1 "Main fetch": the preflight (§4.8) and then the request, as one unit. A request with an
     identity speaks as it (`_speakAs`); its failure is a network error and the request is never sent. */
  var _sent = await _speakAs(opts.identity, parsed.href, _method, mode, destination, credentialed,
                             _preflight ? _unsafeNames : null, null, init, async function () {
    if (_preflight) {
      var _pf = await _corsPreflight(parsed.href, _method, opts.headers || [], opts.credentials,
                                     opts.useCorsPreflight, opts.identity.origin, opts.signal);
      if (_pf) return { refusal: _refused("network", "blocked-cors-preflight:" + _pf + ":" + parsed.origin,
                                          [parsed.href], {}) };
    }
    return { resp: await fetch(parsed.href, init) };
  });
  if (_sent.refusal) return _sent.refusal;
  var resp = _sent.resp;
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
  var _resourceSameOrigin = _isRealOrigin(_pageOrigin) && _isRealOrigin(_finalOrigin) &&
                            _finalOrigin === _pageOrigin;
  /* Network: SSRF via redirect. The initial check cannot see a 3xx into the intranet, so the landed host is
     re-judged before the body is read, and a request that landed on a host less public than its page never
     feeds internal data into the analysis. Local Network Access and CORS both refuse this in a browser. Without
     a redirect `_finalUrl` is `parsed` and this re-asks the same question. */
  var _landedSpace = _ipAddressSpace(_finalUrl.hostname);
  if (_lessPublicThan(_landedSpace, _pageSpace))
    return _refused("network", "blocked-local-network-redirect:" + _landedSpace + "-from-" + _pageSpace,
                    [parsed.href], {});
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
                                pinned: pinnedMark, docReach: docReach, actor: actor, driven: driven,
                                credentialed: credentialed, headers: opts.headers });
  if (_rptok)
    return _refused("decline", "blocked-signal-redirect:" + _rptok,
                    _urlList(parsed.href, _finalHref, resp.redirected), {});
  /* The headers, unwrapped. Fetch §5.5 "Response class"' headers getter returns this's headers, whose
     iteration throws only what the callback throws; an invented empty map would have CORB judge an unlabelled
     body and the SOP read an absent `Access-Control-Allow-Origin`. */
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
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
  /* Fetch §4.1 "Main fetch"' response tainting, for a request the analysed page made. It is `basic` only where
     the requested and the landed URL are both same-origin with the page; a redirect hop between them that left
     the origin is not visible here (only the first and last URL are), which is a named residual in
     SECURITY.md. Otherwise:
       `same-origin` mode — a network error, the main-fetch switch's arm for that mode.
       `cors` mode — §4.10 "CORS check" on the reply, before its body is read, with the request's origin
         serialized as "null" where a redirect left an origin the request was not made from (§2.2.5's redirect
         taint); a failure is a network error, and a pass is §2.2.6's CORS filtered response.
       `no-cors` mode — §2.2.6's opaque filtered response. A `fetch()` (destination "") is the one consumer that
         reads that response itself, so its body is not read and no bytes reach the engine; an element (a classic
         script, an image, a stylesheet) consumes the internal response, so its bytes are delivered beside the
         `opaque` type and the engine keeps them from page script.
     A navigation is `navigate` and is judged by the instance that becomes its reader. */
  var _type = "basic";
  if (actor === "page" && mode !== "navigate" &&
      !(_isRealOrigin(_pageOrigin) && parsed.origin === _pageOrigin && _resourceSameOrigin)) {
    if (mode === "same-origin")
      return _refused("network", "blocked-mode-same-origin:" + _finalOrigin,
                      _urlList(parsed.href, _finalHref, resp.redirected), {});
    if (mode === "cors") {
      var _tainted = resp.redirected && parsed.origin !== _finalOrigin && parsed.origin !== _pageOrigin;
      var _cors = _corsCheck(headers, _tainted || !_isRealOrigin(_pageOrigin) ? "null" : _pageOrigin,
                             opts.credentials);
      if (_cors)
        return _refused("network", "blocked-cors:" + _cors + ":" + _finalOrigin,
                        _urlList(parsed.href, _finalHref, resp.redirected), {});
      headers = _corsFilteredHeaders(headers, opts.credentials);
      _type = "cors";
    } else {
      _type = "opaque";
      if (destination === "") {
        /* The body is given back unread: nothing may read an opaque response's body. */
        if (resp.body) {
          try { await resp.body.cancel(); } catch (e) { RETHROW_FATAL(e); }
        }
        return { ok: resp.ok, status: resp.status, statusText: "", headers: {}, body: _NO_BYTES(),
                 urlList: _urlList(parsed.href, _finalHref, resp.redirected), computedType: "",
                 refusal: null, type: _type };
      }
    }
  }
  /* The body, as bytes, after both SSRF checks, so nothing internal is ingested before the target is judged.
     There is no decode: the engine decodes with the response's charset and BOM (see the header), so it
     receives exactly what the server sent. It is read after the credentialed SOP/CORS check above, which needs
     only headers (Fetch §4.4 "HTTP fetch" runs the CORS check on the response before its body is delivered),
     and before CORB, the one gate that reads the body (see `_readBody`). */
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
           refusal: null, type: _type };
}

/* Fetch §2.2.2 "Headers": a forbidden request-header is one "forbidden so the user agent remains in full
   control over them". A script's value for one is ignored, so a request this zone sends refuses it by name
   rather than letting the person believe it was sent. Returns the refused name, or null. */
var _FORBIDDEN_REQUEST_HEADER_NAMES = [
  "accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method",
  "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin",
  "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via",
];
var _METHOD_OVERRIDE_HEADER_NAMES = ["x-http-method", "x-http-method-override", "x-method-override"];
function fetchForbiddenRequestHeader(pairs) {
  for (var i = 0; i < pairs.length; i++) {
    var lower = pairs[i][0].toLowerCase();
    if (_FORBIDDEN_REQUEST_HEADER_NAMES.indexOf(lower) >= 0 || lower.startsWith("proxy-") || lower.startsWith("sec-"))
      return pairs[i][0];
    // "getting, decoding, and splitting" the value, then §2.2.1 "Methods"' forbidden methods.
    if (_METHOD_OVERRIDE_HEADER_NAMES.indexOf(lower) >= 0 && pairs[i][1].split(",").some(function (m) {
          return ["CONNECT", "TRACE", "TRACK"].indexOf(m.trim().toUpperCase()) >= 0; }))
      return pairs[i][0];
  }
  return null;
}

/* The Send panel's stated record, {origin, referer, jar}, each null for the frame's own:
   - `origin` {header}: the Origin sent, a serialized origin or "null", or null for none at all;
   - `referer`: the Referer sent;
   - `jar` {tabId, documentId}: the open frame whose cookie partition the send speaks with, picked from
     lib/send.js's operatorJarDomain and resolved there through chrome.cookies.getPartitionKey, so no
     partition key is ever typed. */
function safeFetchStatedShape(stated) {
  return !!stated && typeof stated === "object" && Object.keys(stated).length === 3 &&
         (stated.origin === null || (!!stated.origin && Object.keys(stated.origin).length === 1 &&
           (stated.origin.header === null || typeof stated.origin.header === "string"))) &&
         (stated.referer === null || typeof stated.referer === "string") &&
         (stated.jar === null || (!!stated.jar && Number.isInteger(stated.jar.tabId) &&
           typeof stated.jar.documentId === "string" && stated.jar.documentId !== ""));
}

/* The person's typing is input: a stated value that is not what its header carries is refused, never sent.
   Answers the refusal reason, or null. The operator tier and lib/send.js's identity preview both ask it. A
   jar is never typed: it names a frame, and lib/send.js refuses one whose frame is gone. */
function safeFetchStatedRefusal(stated) {
  var o = stated.origin === null ? null : stated.origin.header;
  if (o !== null && o !== "null" &&
      !(URL.canParse(o) && /^https?:$/.test(new URL(o).protocol) && new URL(o).origin === o))
    return "stated-origin-not-an-origin:" + o;
  if (stated.referer !== null &&
      !(URL.canParse(stated.referer) && /^https?:$/.test(new URL(stated.referer).protocol) &&
        new URL(stated.referer).href === stated.referer && !stated.referer.includes("#")))
    return "stated-referer-not-a-url:" + stated.referer;
  return null;
}

/* The operator tier: a request the person composed in the popup, sent as the identity of a frame they
   picked (`opts.identity`, identityFor's answer over lib/request-identity.js's frameIdentity). Any method and
   a body. No firing decision and no destructive deny list: a person authorized these bytes at a surface that
   showed them, and a token list there would veto their act. What a browser refuses still applies: the
   scheme, credentials in the URL, a forbidden request-header, a same-origin-mode request to another origin,
   and Fetch §4.8's preflight. The reply is returned whole, whether or not Fetch §4.10's CORS check would let
   the frame's own script read it.
   `opts.stated` is the Send panel's record (safeFetchStatedShape), for testing a server's CORS or CSRF
   checks; its jar is already in `opts.identity`. This is the one tier that passes an Origin or Referer to the
   speak-as rule; the preflight is then CORS-checked against the stated Origin, which is what the server was
   sent, or the identity's when the person stated that none is sent. `Sec-Fetch-*` stays the frame's. */
async function safeFetchOperator(url, opts) {
  CHECK(typeof senderIsExtensionDocument === "function" && senderIsExtensionDocument(opts.sender),
        "an operator send arrived from " + JSON.stringify(opts.sender && opts.sender.origin) + " — only an " +
        "extension document (the router's own predicate) carries a person's act, and this tier spends the " +
        "person's cookies with no deny list");
  CHECK(!!opts.identity && typeof opts.identity.origin === "string",
        "an operator send names no identity — it speaks as a frame the person picked, never as this extension");
  CHECK(typeof opts.method === "string" && opts.method !== "" && Array.isArray(opts.headers) &&
        (opts.body === null || typeof opts.body === "string" || opts.body instanceof Uint8Array) &&
        ["cors", "no-cors", "same-origin"].indexOf(opts.mode) >= 0,
        "an operator send is missing its method, header pairs, body statement or mode");
  CHECK(safeFetchStatedShape(opts.stated),
        "an operator send states no {origin, referer, jar} record — lib/send.js passes the Send panel's, or " +
        "PAGE_CONTEXT_STATED_NONE for a probe");
  var stated = opts.stated;
  var statedRefusal = safeFetchStatedRefusal(stated);
  if (statedRefusal !== null) return _refused("network", statedRefusal, [], {});
  var parsed;
  try { parsed = new URL(String(url)); } catch (e) { return _refused("network", "bad-url", [], {}); }
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return _refused("network", "blocked-scheme:" + parsed.protocol, [parsed.href], {});
  if (parsed.username !== "" || parsed.password !== "")
    return _refused("network", "blocked-url-credentials", [parsed.href], {});
  var forbidden = fetchForbiddenRequestHeader(opts.headers);
  if (forbidden !== null)
    return _refused("network", "forbidden-request-header:" + forbidden, [parsed.href], {});
  var sameOrigin = parsed.origin === opts.identity.origin;
  if (opts.mode === "same-origin" && !sameOrigin)
    return _refused("network", "blocked-mode-same-origin:" + parsed.origin, [parsed.href], {});
  var unsafe = _corsUnsafeRequestHeaderNames(opts.headers);
  var preflight = opts.mode === "cors" && !sameOrigin && (!_corsSafelistedMethod(opts.method) || unsafe.length > 0);
  var init = { method: opts.method, headers: opts.headers, redirect: "follow" };
  if (opts.body !== null) init.body = opts.body;
  var sent = await _speakAs(opts.identity, parsed.href, opts.method, opts.mode, "", true, preflight ? unsafe : null,
                            stated, init, async function () {
    /* A fetch that rejects is Fetch §5.6's network error, an answer the person is shown; an invariant abort
       travels on. */
    try {
      if (preflight) {
        var pf = await _corsPreflight(parsed.href, opts.method, opts.headers, opts.identity.credentials, false,
                                      stated.origin !== null && stated.origin.header !== null
                                        ? stated.origin.header : opts.identity.origin, undefined);
        if (pf) return { refusal: _refused("network", "blocked-cors-preflight:" + pf + ":" + parsed.origin,
                                           [parsed.href], {}) };
      }
      return { resp: await fetch(parsed.href, init) };
    } catch (e) {
      RETHROW_FATAL(e);
      return { refusal: _refused("network", "network-error:" + ((e && e.message) || String(e)), [parsed.href], {}) };
    }
  });
  if (sent.refusal) return sent.refusal;
  var resp = sent.resp;
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  var body = new Uint8Array(await resp.arrayBuffer());
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: body,
           urlList: _urlList(parsed.href, resp.url || parsed.href, resp.redirected), computedType: "",
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
   and fires on its own, and never overrules an operator's explicit act (`safeFetchOperator`). */
if (typeof self !== "undefined") {
  self.safeFetch = safeFetch;
  self.safeFetchOperator = safeFetchOperator;
  self.safeFetchStatedRefusal = safeFetchStatedRefusal;
  self.safeFetchStatedShape = safeFetchStatedShape;
  self.fetchForbiddenRequestHeader = fetchForbiddenRequestHeader;
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
