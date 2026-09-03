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
// `response.url` as its LAST item and `response.redirected` as "its size is greater
// than 1", so an engine that never receives it cannot compute either — which is why
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
   reason travels in `statusText`, and `blocked-destructive:logout` and `blocked-provenance:forced` say
   different things to the person reading the stall. §Attacker-sources states the rest outright — a
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
function _sniff(bytes) {
  var dec = new TextDecoder("utf-8");   // strips a UTF-8 BOM, exactly as resp.text() did
  var h = dec.decode(bytes.subarray(0, 4096)).replace(/^﻿/, "").replace(/^\s+/, "");
  if (h.charAt(0) === "<") return { protected: true, type: null };   // HTML/XML/SVG/markup
  if (h.charAt(0) === "{" || h.charAt(0) === "[") {
    try { JSON.parse(h); return { protected: true, type: "application/json" }; }
    catch (e) {
      try { JSON.parse(new TextDecoder("utf-8").decode(bytes)); return { protected: true, type: "application/json" }; }
      catch (_) {}
    }
  }
  return { protected: false, type: null };
}
// WHAT THIS RESPONSE IS, AS ONE STATEMENT THE RENDERER IS TOLD. §4.2's ESSENCE — the
// type, a solidus, the subtype — of the server's `Content-Type` when it stated one,
// and what the bytes say when it did not or when they contradict it. The empty
// string is §5.1's "the supplied MIME type is undefined" surviving the sniff: the
// server named nothing and the bytes named nothing either, which is a POSITIVE
// answer and not a hole (a reader that must distinguish it reads it as absent, the
// way `mime_type_extract` reads a null header).
// NOSNIFF IS FINAL. The server has said its label is the last word, so this returns
// the essence unchanged whatever the body looks like — the same sentence that stops
// the CORB rule below from sniffing past it.
function _computedType(declared, nosniff, sniff) {
  var mime = String(declared == null ? "" : declared).split(";")[0].trim().toLowerCase();
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
// below is "the body must be JAVASCRIPT-TYPED or same-origin", and an XSLT stylesheet
// is XML, so requiring a JS MIME of one would refuse every correct response. The day
// this engine loads an XSLT stylesheet it needs its own rule, not this one.
function _isScriptLike(d) {
  return d === "audioworklet" || d === "paintworklet" || d === "script" ||
    d === "serviceworker" || d === "sharedworker" || d === "worker";
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
// in the zone this one does not trust. So "the producer refuses to emit a value outside
// the enumeration" is not a check this zone holds. `bridge.js` already CHECKs the
// INITIATOR and the PROVENANCE of that same line — the two fields nothing decides an
// ingestion from — and the field the ingestion IS decided from was the one taken on the
// producer's word. Refusing here closes it for BOTH hosts at once, which is the reason
// it belongs at the chokepoint rather than in either host's splitter.
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
/* ── AND WHAT THIS FUNCTION DOES *NOT* READ, WHICH IS THE HALF NO ASSERT WAS MAKING ──────────────────────
   The two rules above refuse a VALUE this file cannot serve. This one refuses a FIELD it will not read, and
   the failure it closes is the opposite shape: not a bad answer to a question this file asks, but a caller's
   answer to a question this file never asks — DROPPED IN SILENCE, and read by the caller as carried.
   THE OPTION THAT MATTERS IS `method`, AND IT IS THE MIDDLE CONJUNCT OF THE ONE COMBINATION CLAUDE.md
   §A-REQUEST-CARRIES-THE-PROVENANCE SAYS IS NEVER A SETTING: credentialed AND state-mutating AND forced.
   `_firingRefusal` argues that conjunct is false at every setting of the widening table "because this file
   cannot issue a non-GET at all", and that is TRUE — `init` hardcodes `method: "GET"` and nothing below
   reads `opts.method` or `opts.body`. What was missing is that the argument was enforced by a GREP, and
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
var _SAFEFETCH_OPTIONS = ["pageUrl", "pageOrigin", "destination", "provenance", "credentialed",
                          "headers", "signal"];
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
           "Methods\"' safe set enforced structurally and is what makes the state-mutating conjunct of " +
           "CLAUDE.md's never-a-setting triple false — so a caller that states a verb gets the reply to a GET " +
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
   write — and that is what makes the state-mutating conjunct of CLAUDE.md's never-a-setting triple
   (credentialed AND state-mutating AND forced) false by construction. A header list is the OTHER route to a
   verb: `X-Http-Method-Override` is a convention this project's own code sends (`lib/discovery.js` calls it
   "the documented trick"), and a server that honours it reads a GET as whatever the header names. So a
   bundle-chosen header list on a cookie-bearing request is the triple reconstituted one layer up, past the
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
   page-context relay's initiator grade already are. Its absence shows as this abort and nowhere else. */
function _credentialedOf(opts) {
  var credentialed = !!opts.credentialed;
  CHECK(!(credentialed && opts.headers),
        "safeFetch was asked for a CREDENTIALED request that also states a header list — these are two " +
        "populations arriving through one parameter and only one of them has been decided. The header list " +
        "on the XHR path is the analysed BUNDLE's own, which is within the model while the request is " +
        "uncredentialed (a header on a request the page can already make itself confers nothing) and is a " +
        "different question the moment the person's cookies pay for it: this file enforces RFC 9110 §9.2.1 " +
        "\"Safe Methods\" by making the verb a literal, and a header list is the other route to a verb — " +
        "`X-Http-Method-Override` is a convention this project's own code sends, and a server honouring it " +
        "reads this GET as whatever the header names, which is CLAUDE.md's never-a-setting triple " +
        "(credentialed AND state-mutating AND forced) rebuilt past the place this file closed it. State " +
        "whose header list it is at the site that knows, the way `provenance` is stated, or send it " +
        "uncredentialed");
  return credentialed;
}
/* THE PER-ORIGIN EXPLORATION WIDENING — A PERSON'S SENTENCE, NEVER AN INFERENCE.
   CLAUDE.md §Attacker-sources: firing what a bundle only reaches past a forced gate is
   "CONFIGURABLE AND PER-ORIGIN, BECAUSE EXPERIMENTATION IS NOT ALWAYS WRONG AND A SINGLE
   SWITCH CANNOT SAY SO … Default conservative, widened deliberately per origin, never
   inferred from a site looking like a test." Firing at an app you own is the point of the
   tool; doing it at a stranger's production account is not; and NO PROPERTY OF THE ADDRESS
   distinguishes them, which is precisely why the answer is a person's.
   IT LIVES IN THIS FILE BECAUSE THE DECISION DOES. A registry in a host is a registry the
   OTHER host does not read, and two zones answering one question is the shape this whole
   parameter exists to end. `engine/trusted.mjs` loads this file verbatim into a realm of
   its own and its `--explore <origin>` writes HERE; the offscreen loads it in
   `ast-worker.html` and reads the same table. An EMPTY table is the positive statement
   "nobody has widened anything", which is the conservative default rather than an absence.
   WHAT WIDENING OBLIGES, STATED WHERE IT IS GRANTED. §@H makes the reply to a forced
   request evidence about what a server says to a request NO CLIENT MAKES, so its values
   must be carried as FORCED and never merged into the observed pool — a 401 body parses as
   JSON and yields fields that exist nowhere, and one invented field is the example that
   shapes the next endpoint. That carrying is the engine's and it is the subproblem AFTER
   this one; until it exists, widening buys the request and the person takes on the reply. */
var _EXPLORED = Object.create(null);
/* THE VALUE MUST BE THE ORIGIN A URL PARSER WOULD PRODUCE, AND `_isRealOrigin` IS NOT THAT
   TEST. It asks only whether the string contains "://", which the whole of
   `https://a.test/some/path` does — so a person who typed an address rather than an origin
   would have widened a key no request's `.origin` can ever equal: a permission that was
   granted, is in the table, reads as granted, and matches nothing for ever. That is the
   destructive deny list's own failure shape (an entry that looks protective and refuses
   nothing) pointed the other way, and it is worse here because the silence looks like the
   conservative default working. So the value is required to be its OWN serialized origin —
   parsed, re-serialized, compared — which is exactly the comparison `_firingRefusal` makes.
   AN OPAQUE ORIGIN IS REFUSED BY THE SAME LINE and needs no clause of its own: `null` does
   not parse as a URL, and every opaque origin is same-origin with nothing (itself included),
   so there is no address one could ever widen. */
function safeFetchWiden(origin) {
  var normalized = null;
  try { normalized = new URL(String(origin)).origin; } catch (e) { RETHROW_FATAL(e); normalized = null; }
  CHECK(_isRealOrigin(origin) && normalized === origin,
        "an exploration widening was asked for " + JSON.stringify(origin) + ", which is not the serialized " +
        "TUPLE ORIGIN a URL parser produces (" + JSON.stringify(normalized) + ") — the widening is compared " +
        "against a request URL's own `origin`, so a full address, an opaque `null`, an explicit default port " +
        "or an empty string would sit in this table matching nothing while reading as a permission somebody " +
        "granted. Pass the origin, not the address");
  _EXPLORED[origin] = true;
}
/* WHAT HAS BEEN WIDENED, FOR A CALLER THAT MUST ASSERT THE ABSENCE OF ANY. Not a report —
   a PREMISE READER. A zone whose own reasoning rests on "no origin is widened HERE" holds
   that premise where it is relied on rather than in a comment that outlives it. */
function safeFetchWidenedOrigins() { return Object.keys(_EXPLORED); }
/* THE FIRING DECISION, IN ONE FUNCTION, ANSWERING THE RULE THAT REFUSED OR `null` FOR FIRE.
   THE METHOD HALF IS ALREADY ANSWERED AND IS NOT ASKED HERE: this file is GET-ONLY BY
   ABSENCE (it hardcodes `method:"GET"` and reads neither `opts.method` nor `opts.body`), and
   GET is in RFC 9110 §9.2.1 "Safe Methods"' safe set — "the client does not request, and
   does not expect, any state change on the origin server". So the entire remaining question
   is PROVENANCE, which is what this reads.
   `observed` FIRES: a real load of this document makes exactly this request, so the request
   is one the person's own client makes and firing it discovers nothing about a world that
   would not have happened anyway.
   `derived` FIRES: the page's own code computed the address from real inputs, so it is a
   FACT ABOUT THE APP even where no session sent it — and CLAUDE.md §Attacker-sources calls
   firing it REQUIRED ("Active discovery is REQUIRED … passive learning is too thin"), with
   §the-symbolic/trust-boundary naming the headline case in as many words: "a fetch whose
   body is JAVASCRIPT is ALWAYS fetched + EXECUTED (a lazy chunk reveals real endpoints —
   the headline moat surface)". A default that refused it would turn the tool's central
   capability off and call the silence caution.
   `forced` IS THE WIDENING, AND ITS REFUSAL IS THIS POLICY'S ANSWER RATHER THAN A GAP IN
   IT. A value in the request exists only because a gate was forced, so a reply to it is
   evidence about what a server says to a request no client makes. The address is DERIVED IN
   FULL and REPORTED, which §Attacker-sources says is not a gap in the report but IS the
   report — "that surface is what forced execution finds and a sniffer cannot".
   THE ONE COMBINATION THAT IS NEVER A SETTING IS UNREACHABLE HERE BY CONSTRUCTION, which is
   what makes this a whole answer rather than a hole with a flag over it: credentialed AND
   state-mutating AND forced. The middle conjunct is false at every setting of this table,
   because this file cannot issue a non-GET at all.
   AND THAT IS NOW ASSERTED RATHER THAN GREPPED, which is the difference between a request
   carrying a verb being IGNORED and one being IMPOSSIBLE TO WRITE. `_refuseUnreadOptions`
   at the entry refuses a caller that states a `method` or a `body`, so the conjunct is false
   because no such request can be composed — not merely because nothing here reads one.
   What is left of that sentence's concern
   — that §9.2.1's contract is the RESOURCE OWNER's to honour and not ours to verify — is
   the destructive-path deny list below, which is a FLOOR under this policy and never a
   substitute for it. */
function _firingRefusal(provenance, origin) {
  if (provenance === "observed" || provenance === "derived") return null;
  if (_EXPLORED[origin] === true) return null;
  return provenance;
}
/* THE SAME ANSWER, ASKED BY A CALLER WHOSE ACT IS NOT A FETCH. One function read twice is
   not two policies — it is the opposite, and it is why this is exported rather than
   restated. Two callers need it and each needs a different SHAPE of outcome, which is
   precisely what they may decide for themselves and the answer is not: `bridge.js`'s
   route-declaration arm records a WORK ITEM whose load happens rounds later, and enqueuing
   one this file will refuse would pay an admission slot every round to be told no;
   `engine/trusted.mjs`'s `navigate` must answer its channel with a DECLINE (no instance is
   provisioned) rather than with the empty Document a network refusal produces.
   IT ANSWERS THE RULE THAT REFUSED AND NOT A BOOLEAN, for the reason `_corbDeniesScript`
   answers one: a caller that must tell somebody WHY cannot re-derive the grade from a `false`,
   and re-deriving it is how the second copy of a policy gets written. The refusal is the
   GRADE; the SENTENCE a host wraps around it is that host's own, because how a person
   expresses a widening is per-host (`--explore <origin>` is a command line, and the offscreen
   has no command line) while what a widening MEANS is not.
   IT TAKES AN ABSOLUTE URL because the caller holds one and the origin comparison is this
   file's to make; an address that will not parse is not a refusal but a caller's serializer
   disagreeing with a URL parser, so it THROWS rather than answering a permission question
   about nothing. */
function safeFetchFiringRefusal(provenance, url) {
  CHECK(_PROVENANCE_TYPES.indexOf(provenance) >= 0,
        "safeFetchFiringRefusal was asked about the provenance " + JSON.stringify(provenance) + ", which is " +
        "none of the three — the caller is deciding whether to spend an act on this address and would read " +
        "an unknown value as the permissive arm, exactly as the chokepoint would");
  return _firingRefusal(provenance, new URL(String(url)).origin);
}
function _corbDeniesScript(mime, nosniff, sniff, sameOrigin) {
  // same-origin: the page's own data is its to read, and the only thing refused is
  // that data reaching a CODE loader — a load that could not have executed anyway.
  if (sameOrigin)
    return _corbProtectedMime(mime) && !_jsMime(mime) ? "same-origin-protected" : null;
  if (_corbProtectedMime(mime)) return "protected-type";      // CORB-protected type
  if (nosniff && !_jsMime(mime)) return "nosniff-not-js";      // browser blocks too
  if (sniff.protected) return "sniffed-data";                  // mislabeled data
  return null;
}

// Private/loopback/link-local classification (RFC1918 + loopback + IPv6 ULA/LL)
// for the origin-relative SSRF rule: a request is blocked ONLY when the TARGET is
// private but the PAGE origin is not — a public page reaching the intranet.
// localhost->localhost and any->public are allowed (normal web rules).
//
// AN IPv4-MAPPED ADDRESS *IS* AN IPv4 ADDRESS, SO IT IS UNMAPPED BEFORE IT IS CLASSIFIED — never matched a
// second time as text. Every host that reaches here comes out of the WHATWG URL parser (`parsed.hostname`,
// `new URL(resp.url).hostname`), so it is already CANONICAL: an IPv4 literal is dotted-quad (the URL
// Standard's IPv4 parser folds the decimal/octal/hex spellings, which is why `http://2130706433/` cannot
// slip past `/^127\./`), and an IPv6 literal is the IPv6 SERIALIZER's output — each piece "represented as
// the shortest possible lowercase hexadecimal number", with no dotted tail anywhere in it.
// This function carried an `::ffff:(127|10|192\.168|169\.254)` alternative, and that is a spelling the
// serializer CANNOT produce: `http://[::ffff:127.0.0.1]:8080/` arrives as `[::ffff:7f00:1]`. So the branch
// was dead, and the address it was written to stop was classified PUBLIC — a public page's bundle naming
// that URL walked straight through the origin-relative SSRF guard into the user's loopback, and safeFetch
// handed the response bytes back to the untrusted engine. SECURITY.md's attack table calls that case
// mitigated; it was not. RFC 4291 §2.5.5.2's IPv4-mapped form and §2.5.5.1's deprecated IPv4-compatible
// form both DENOTE an IPv4 address and are routed to it by the stack, so both are converted to that address
// and classified ONCE, by the v4 rules — rather than a growing list of ways to spell the same host.
function _v4OfIPv6(h) {
  var m = /^::(?:ffff:)?([0-9a-f]{1,4}):([0-9a-f]{1,4})$/.exec(h);
  if (!m) return null;
  var hi = parseInt(m[1], 16), lo = parseInt(m[2], 16);
  return ((hi >> 8) & 255) + "." + (hi & 255) + "." + ((lo >> 8) & 255) + "." + (lo & 255);
}
function _isPrivateHost(host) {
  if (!host) return false;
  host = String(host).toLowerCase().replace(/^\[|\]$/g, "");
  /* THE CANONICAL FORM IS THE CONTRACT, so a host that is not in it is an unclassifiable input rather than a
     public one. A literal holding BOTH a colon and a dot is the dotted IPv4-in-IPv6 text form, which no URL
     parser ever emits — its arrival means a caller handed this chokepoint a raw string instead of
     `URL.hostname`, and every rule below is written for the form the parser produces. */
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

// @security-contract  ENFORCEMENT POINT (the single network chokepoint)
//   guarantees: cookies omitted by default — or, in opts.credentialed mode, a GET
//               whose REPLY is gated by safeFetch's OWN SOP/CORS (same-origin
//               to the principal, else exact-origin ACAO + ACAC == true), since a
//               host-permission fetch bypasses the browser's same-origin policy;
//               method GET; http(s) only; origin-relative SSRF (a PRIVATE target is
//               blocked unless the page principal is itself private) on BOTH the
//               initial URL and the post-redirect final URL.
//               EVERY POST-FETCH GATE JUDGES THE POST-REDIRECT URL — the private-host
//               re-check, the destructive-path re-check, CORB's same-origin exemption
//               and the credentialed SOP. Fetch §2.2.5 "Requests"' CURRENT URL is the
//               last in the URL list, and §4.1 "Main fetch" reads a response's
//               readability off that one.
//   callers of opts.credentialed: bridge.js's `navigationLoad` and `frontierRederive`,
//               and ONLY where the address is same-origin with the browser-stated
//               principal of the document being loaded (`navigationCarriesSession`).
//               The learned-GET replay path (`fetched`) passes no pageOrigin and
//               states `credentialed: false` — turning that on is a separate
//               deliberate decision about the CORB class of a same-origin chunk, not
//               a side effect of the provenance below.
//               THAT LINE USED TO SAY IT WAS "still uncredentialed", ASSERTING A
//               PROPERTY OF ANOTHER FILE THAT HAD STOPPED BEING TRUE. `fetched` read
//               `!!(msg && msg.credentialed)`, and the seed's AST_ANALYZE writes that
//               field from `navigationCarriesSession` — true for the ordinary
//               same-origin seeded page. So cookies WERE attached, with no pageOrigin
//               beside them, and every such reply was refused
//               `blocked-cors-credentialed:` after the request had gone out. It is a
//               literal now. A claim here about who calls this file and how is a claim
//               to re-grep before it is repeated.
//   opts.provenance:
//               CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's OBSERVED / DERIVED /
//               FORCED, verbatim from the engine (solver/engine.h's
//               PENDING_PROVENANCE_*) or stated by the zone that originated the act.
//               It is what a reply is EVIDENCE OF, and it is half of the firing
//               decision — the other half being the METHOD, which RFC 9110 §9.2.1
//               "Safe Methods" answers and this file enforces by ABSENCE. See
//               `_firingRefusal`: the decision is HERE because CLAUDE.md puts it
//               here ("the engine holds no network policy by construction, so
//               `safeFetch` decides, from the provenance the request declares beside
//               its method and credential state"), and because a policy in either
//               host would be a second copy of it that drifts — which is exactly the
//               state this parameter ends: `engine/trusted.mjs` declined every
//               DERIVED and FORCED park while `bridge.js` fired every one of them,
//               two answers to one question, neither of them the policy.
//   opts.headers:
//               THE ONE OPTION THIS FILE READS WHOSE VALUES CROSS FROM THE UNTRUSTED
//               ZONE — on the XHR path they are the analysed BUNDLE's own list.
//               Within the model on the request this file issues (an uncredentialed
//               GET, forbidden header names stripped by the browser, to a host the
//               page can already reach itself); REFUSED on a credentialed one, by a
//               CHECK in `_credentialedOf` rather than by the callers happening to be
//               disjoint. A header list is the other route to a verb
//               (`X-Http-Method-Override`), so the combination is the never-a-setting
//               triple rebuilt past the place the literal `method:"GET"` closed it.
//   opts.destination:
//               Fetch §2.2.5 "Requests"' DESTINATION, verbatim from the request the
//               engine parked (solver/engine.h puts it on the pending line). A
//               SCRIPT-LIKE one ("script", "worker", …) -> + CORB (cross-origin must
//               be JS-typed); every other value, "" included, is data and takes no
//               CORB. Absent is NOT a synonym for data, and neither is UNRECOGNISED:
//               both take the `not script-like` arm silently, so `_destinationOf`
//               refuses anything outside §2.2.5's enumeration with a CHECK — fatal in
//               release too, because the arm a bad value falls through to is the
//               permissive one and the value crosses from the untrusted engine.
//   answers:    computedType — the ONE type decision made about this response, the
//               same one CORB was decided from, stamped on the record for the
//               renderer so no downstream zone repeats it.
//   principal:  opts.pageUrl PER CALL — the analysed DOCUMENT's OWN browser-stated
//               address (`MessageSender.url`), NEVER `sender.tab.url` — classifies
//               the SSRF host; opts.pageOrigin (the BROWSER-provided
//               MessageSender.origin, opaque-unique) is the SAME-ORIGIN principal for
//               the credentialed SOP — NOT re-parsed from a URL (a sandboxed frame
//               has a normal URL but an opaque origin). No shared global (concurrent
//               grinds would contaminate it); unknown principal -> public + opaque
//               -> private targets and credentialed cross-origin reads blocked.
// Fetch §2.2.6's URL list as far as script may ever see it: the FIRST item (the URL
// we requested, which §4.1 clones from the request) and the LAST (resp.url, after
// redirect:"follow" walked the chain). Both authorities are the browser's own and
// each answers one question: resp.redirected says whether the list has more than one
// item, resp.url says what the last one is. Deriving "did it redirect" from
// resp.url !== requested instead would report a 3xx that lands back on its own
// address as no redirect at all, which is a chain the list DID grow along.
/* THE DESTRUCTIVE-PATH DENY LIST — the ONE place this project matches on a name, and
   it matches only to REFUSE.
   CLAUDE.md's §RUN-DON'T-MATCH bans matching because a matched name would be ASSERTED
   as a value, and a name is meaningless in minified code. A deny list asserts NOTHING.
   It refuses, and the two directions fail in ways that are not comparable: a wrong DENY
   costs exactly one unfired request, which forced execution still derives and still
   reports in full, while a wrong ASSERT fabricates a finding that PROPAGATES, since one
   invented field is the example that shapes the next endpoint. So this list is sound
   PRECISELY BECAUSE IT IS ALLOWED TO BE WRONG, and being over-broad is its cheap
   direction rather than its dangerous one.
   WHAT IT IS FOR. Forced execution builds requests no real client makes, and RFC 9110
   §9.2.1 Safe Methods says a GET must not be relied on to change state — but it says so
   by placing the duty on the RESOURCE OWNER ("it is the resource owner's responsibility
   to ensure that the action is consistent with the request method semantics"), and names
   the failure it expects: "unfortunate side effects when automated processes perform a
   GET on every URI reference". This tool is that automated process. A GET that ends the
   person's session mid-analysis is a CSRF we committed against our own user, and no
   amount of §9.2.1 correctness makes that acceptable to discover afterwards.
   WHY ONLY WHEN CREDENTIALED. The harm needs the session: an uncredentialed GET to a
   logout path destroys nothing, and denying it would cost real learning for no safety.
   So the gate is scoped to the exact condition under which the harm exists, rather than
   applied everywhere and called caution.
   WHAT IT MAY NEVER BE READ AS. A path that does NOT match is not thereby established
   safe. This list is a FLOOR under the policy and never a substitute for it — the method
   and the value provenance still decide, and no absence of a match licenses firing
   anything they refuse.
   MATCHING IS BY WHOLE TOKEN, NEVER BY SUBSTRING, because substring matching is how a
   deny list becomes useless: `/catalogue` contains no token `logout`, and `/deleted-items`
   yields `deleted`, which is not `delete`. Each path segment and query token is tested
   raw AND with `-`, `_` and `.` removed, so `log-out` and `log_out` reach `logout`
   without the list having to enumerate spellings.
   AND IT IS MATCHED PERCENT-DECODED AS WELL AS RAW, because the address this gate is
   handed is the address a URL PARSER produced and a parser only ever ENCODES. URL
   Standard §1.3 "Percent-encoded bytes" gives the basic URL parser a percent-ENCODE set
   per component and no decode step anywhere, so `URL.pathname` hands back whatever
   triplets its input carried — while RFC 3986 §6.2.2.2 "Percent-Encoding Normalization"
   says two URIs differing only in those triplets are EQUIVALENT ("normalized by decoding
   any percent-encoded octet that corresponds to an unreserved character"), which is what
   the server on the other end acts on. Tokenising only the raw form therefore let the
   whole list be walked past by spelling one letter as its own hexadecimal: measured, at
   the revision this paragraph was written, `/log%6Fut` and `/%64elete/account` both
   answered `""` — no token, request permitted, cookies attached. `%2F` is worse than an
   escaped letter because it also DELETES a segment boundary the tokeniser splits on, so
   `/api%2Flogout` was one token `api2flogout` and matched nothing either.
   THIS IS DELIBERATELY NOT §6.2.2.2's NORMALIZATION, and the difference is the direction.
   §6.2.2.2 licenses decoding UNRESERVED octets only, because decoding a reserved one
   (§2.2's `/`) changes what the URI MEANS — so a normalizer must not, and this gate is not
   normalizing. It is building a SECOND set of tokens to test, and a token that only exists
   under an over-eager decode can do exactly one thing: refuse one more request. That is the
   cheap direction this whole list is built on. */
var _DESTRUCTIVE = [
  /* ending a session or revoking an authorization */
  "logout", "logoff", "signout", "signoff", "deauth", "deauthorize", "revoke",
  "endsession", "destroysession", "invalidate", "unsubscribe", "optout",
  /* destroying or disabling a resource or an account */
  "delete", "destroy", "remove", "purge", "erase", "wipe", "truncate",
  "deactivate", "disable", "terminate", "cancel", "reset",
  "unlink", "unfollow", "unfriend", "deleteaccount", "closeaccount"
];
/* A TOKEN THAT COULD NEVER MATCH IS A SILENT HOLE IN A SECURITY GATE, so the shape the
   matcher requires is asserted rather than assumed: the comparison is against lowercased,
   separator-stripped tokens, so an entry carrying an uppercase letter or a `-` would sit
   in this list looking protective and match nothing, for ever. */
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
/* URL STANDARD §1.3 "Percent-encoded bytes"' PERCENT-DECODE, ONE PASS — and it is that
   algorithm rather than `decodeURIComponent` for the one property a gate needs: IT CANNOT
   FAIL. §1.3 says of each byte "if byte is 0x25 (%) and the next two bytes after byte in
   input are not in the ranges 0x30 (0) to 0x39 (9), 0x41 (A) to 0x46 (F), and 0x61 (a) to
   0x66 (f), all inclusive, append byte to output" — a bare `%` is DATA, not an error.
   `decodeURIComponent("%")` throws a URIError instead, and so does every ill-formed UTF-8
   sequence, so reaching for it here would put a `try`/`catch` in front of a security gate
   whose catch arm is "no token matched" — an absent answer read as a permit, which is the
   defaulted-read defect standing exactly where a refusal belongs.
   IT STOPS AT BYTES AND NEVER DECODES THEM TO TEXT. §1.3's output is a BYTE SEQUENCE and
   nothing here needs characters out of it: the token class below is `[a-z0-9._-]`, so a byte
   outside ASCII is a SEPARATOR whatever text it would have become, and a UTF-8 decode could
   only ever turn one separator into another. Each triplet therefore becomes ONE code unit
   whose value is that byte, which is also what makes the caller's termination argument hold.
   §2.1's "the uppercase hexadecimal digits 'A' through 'F' are equivalent to the lowercase
   digits" is why both cases are accepted here rather than lowercased first. */
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
/* ONE FORM'S TOKENS, so the two forms cannot drift into two matchers. */
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
/* Returns the token that refused this URL, or "" — a POSITIVE statement that nothing in
   the list matched, never a boolean whose false could also mean "not asked". The token
   travels into the status message so a refusal is attributable and a reviewer can
   disagree with THIS entry rather than with the list.
   THE RAW FORM IS ASKED FIRST so an address that already matched names the SAME token it
   named before, and only then each successive decoding — a server that decodes once sees
   the first, one that decodes what its own framework already decoded sees a later one, and
   this gate does not have to know which kind it is talking to. */
/* AND THE ARGUMENT IS ASSERTED RATHER THAN GUARDED, BECAUSE THE GUARD'S ARM WAS A PERMIT.
   This read stood inside `try { … } catch (e) { return ""; }`, and `""` is this function's
   POSITIVE statement that nothing in the list matched — so the one thing the catch could
   ever do was turn "the gate could not be evaluated" into "the gate said yes", which is the
   defaulted-read defect standing exactly where a refusal belongs. It could not even do that
   much: `u` is a `URL` at both call sites, and URL Standard §6.1 "URL class" gives those two
   getters no failure step at all — "The pathname getter steps are to return the result of URL
   path serializing this's URL", and the `search` getter returns either the empty string or
   `?` followed by the query — so the arm was UNREACHABLE, hiding an assumption not stating it.
   WHAT IS REACHABLE IS THE SHAPE, AND IT FAILS OPEN IN SILENCE. Hand this a plain object and
   no exception happens at all: `String(undefined)` is the six characters `undefined`, which
   match no token, and a credentialed request goes out with the deny list never evaluated.
   THEREFORE `CHECK` AND NOT `DCHECK`, on the one discriminator that separates them here.
   §Offensive programming names "a security/authorization boundary" among the invariants that
   must hold in production, and the tiebreak "when unsure it is a DCHECK" does not apply
   because the release behaviour is not unknown: with the assert compiled out this composes
   `"undefined&undefined"` and PERMITS. A gate that fails open in release is what CHECK is
   for. It is `u && …` so a null argument reaches the assert rather than a TypeError. */
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
    /* TERMINATION IS STRUCTURAL AND IS NOT A §NO BOUNDS CAP — there is no counter here and
       nothing decides that work will not happen. A pass that changed anything replaced at
       least one three-code-unit triplet with one code unit, so it is strictly SHORTER; a
       pass that changed nothing returns the identical string. A strictly shrinking string
       over a finite input cannot loop, and "no shorter" is therefore the positive statement
       "there is nothing left to decode" rather than a guard against a bad state.
       ASSERTED BECAUSE THE EXIT RESTS ON IT: a decoder that stopped implementing §1.3 could
       make those two facts disagree, and the loop is inside the one chokepoint every byte of
       this extension goes through. */
    DCHECK(next === form || next.length < form.length,
           "a percent-decoding pass returned a string that differs from its input yet is no shorter — URL " +
           "Standard §1.3 \"Percent-encoded bytes\" replaces a three-code-unit triplet with one byte and " +
           "copies every other code unit unchanged, so a change that does not shorten is this decoder no " +
           "longer implementing it, and the destructive-path loop uses that length to know it is done");
    if (next.length >= form.length) return "";
    form = next;
  }
}

/* §2.2.6's URL LIST, OVER THE FINAL HREF ITS CALLER ALREADY COMPUTED — not over `resp`, and
   that is the same "one question, one answer" rule the fetch below is built on rather than a
   tidying. This took the Response and re-read `resp.url` for itself inside two swallowing
   catches, so the address the ENGINE decides a document's origin from (`bridge.js` reads this
   list's last item and keys an agent cluster on it) was a SECOND reading of the same fact,
   arrived at separately from the one every gate in this file judges. Two strings for one
   question is how they come to disagree, and the swallows meant a disagreement would have
   read as "nothing redirected". Both catches were unreachable besides — Fetch §5.5 "Response
   class" gives neither getter a failure step: "The redirected getter steps are to return true
   if this's response's URL list's size is greater than 1; otherwise false." */
function _urlList(requested, finalHref, redirected) {
  return redirected ? [requested, finalHref] : [requested];
}

async function safeFetch(url, opts) {
  opts = opts || {};
  /* THE REQUEST'S SHAPE IS CHECKED BEFORE THE REQUEST IS MADE, which is the difference between an abort and a
     network round trip followed by an abort.
     THIS SENTENCE USED TO READ "both of these are about the CORB class", and the pair it named was a
     `DCHECK(!("as" in opts))` beside `_destinationOf` — one dead keyword refused by name, which is a rule
     with a hole for every option nobody happened to think of. What stands here now is the general form of
     it: the fields this file READS are a closed set, so a caller's statement it will not read is REFUSED
     rather than dropped. The CORB class is one thing that goes wrong that way; the never-a-setting triple's
     state-mutating conjunct is the other. Both are asked at the entry rather than beside the rule they feed,
     which runs only after the bytes are already back. */
  _refuseUnreadOptions(opts);
  _destinationOf(opts);
  /* AND WHAT THE REQUEST IS EVIDENCE OF, ASKED AT THE SAME DOOR AND FOR THE SAME REASON: it is a field this
     zone DECIDES from, its absent and invented values take the permissive arm, and the decision it feeds
     happens before any byte moves. Read ONCE here and passed down, never re-read at the two gates below — a
     field consulted twice is a field two gates can disagree about. */
  var provenance = _provenanceOf(opts);
  /* AND WHETHER THE PERSON'S SESSION PAYS FOR IT, DERIVED ONCE AT THE SAME DOOR — see `_credentialedOf`,
     which is also where the one option this file reads from the UNTRUSTED zone is refused a place on a
     cookie-bearing request. It was derived below, beside the deny list; one derivation is what stops the
     credential mode and the invariant scoped BY the credential mode from being two reads that can differ. */
  var credentialed = _credentialedOf(opts);
  var parsed;
  try { parsed = new URL(String(url)); }
  // No URL at all, so there is no URL list either — « » is the honest report, and
  // the engine's `response.url` is then the empty string the spec names for it.
  /* NETWORK, because a browser agrees: the URL constructor throwing is URL Standard §6.1 "URL class"' own
     failure, and a request whose URL cannot be parsed never becomes a request at all. */
  catch (e) { return _refused("network", "bad-url", [], {}); }
  /* NETWORK, and this is the arm Fetch §4.3 "Scheme fetch" names in as many words — its switch ends "Return a
     network error", and `file:` is "left as an exercise for the reader. When in doubt, return a network
     error." A page fetching `file:///etc/passwd` gets §5.6's TypeError in Chrome, so the flow's failure path
     is where a real browser puts it too. */
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return _refused("network", "blocked-scheme:" + parsed.protocol, [parsed.href], {});
  // Origin-relative SSRF (see header). The PRINCIPAL is the analysed DOCUMENT's OWN
  // address, passed PER CALL as opts.pageUrl — NOT a shared global: two grinds run
  // concurrently in one worker, so a worker-global principal would let one page's
  // origin contaminate another's fetch. It is the browser's `MessageSender.url` for
  // that document and NEVER `sender.tab.url`, which this comment named until now: the
  // tab's address is the address of the TOP of the tab, and using it here is the exact
  // bug SECURITY.md records fixing — a sub-frame in a tab whose top is
  // http://localhost/ inherits that PRIVATE classification and reaches the user's
  // intranet on its embedder's behalf. `sender.tab.url` still travels, as
  // `topLevelUrl`, for HTML §8.1.3.1's top-level creation URL; it is not a fetch
  // principal. A cross-origin script uses the origin of the PAGE it is loaded into
  // (gstatic.com JS in google.com acts as google.com), never the asset's own host.
  // Block a PRIVATE target only when the page principal is NOT itself private.
  /* THREE STATES USED TO ARRIVE HERE AS ONE, which is worth ending at a security
     decision even though all three fail closed. `opts.pageUrl || ""` followed by
     `try { new URL(_po).hostname } catch (e) {}` handed the EMPTY HOST to an ABSENT
     principal, to an UNPARSEABLE one, and to a perfectly valid one that simply names
     no host — and `_isPrivateHost("")` answers false for all three, so "our contract
     is broken" and "this document has no server" were reported as one public
     classification with nothing able to tell them apart.
     TWO OF THEM ARE THIS ZONE'S CONTRACT BROKEN, and the discriminator is the one this
     file already states seventy lines down: WHO DETERMINES THE VALUE. `opts.pageUrl` is
     never on the wire — SECURITY.md deleted every principal-shaped field an untrusted
     zone could send, so this one has no argument to travel in — which makes it always
     `_browserFacts`' `url`, minted in the trusted zone out of `MessageSender` and
     already DCHECKed there as a non-empty string. Every call site takes it from that
     mint: four in `bridge.js`, where `navigationLoad` asserts the same fact again at
     its own door, and one in `lib/discovery-probe.js`, whose automatic discovery sweep
     reads `doc.url` off the DocData that `handleContentMessage` stamps FROM that mint
     and asserts it before building its fetch fn. (That sentence said "all four call
     sites are in `bridge.js`" and stopped being true the day the sweep was moved off
     the page-context relay onto this chokepoint — a claim about the call graph, which
     a diff can falsify without touching the line that makes it.) So an absent or
     unparseable principal is OUR bug and it asserts. This is NOT the hostile input SECURITY.md refuses to abort on:
     that is `CONTENT_SEED`'s page-suggested address, which is dropped closed, and it is
     a different value that never reaches this argument.
     DCHECK AND NOT CHECK, because the release question is only whether we may PROCEED —
     and with the guard below an unparsed principal still classifies as public and still
     blocks every private target, so release has a safe answer and does not need to
     abort. That guard is also what keeps this DCHECK from becoming load-bearing in the
     build that compiles it out: a bare `.hostname` would trade a dev abort for a
     release TypeError inside the chokepoint.
     THE THIRD IS A REAL INPUT AND IS NOW A POSITIVE STATEMENT RATHER THAN A HOLE.
     `manifest.json` sets `match_about_blank` and `match_origin_as_fallback` on the
     content script, so a document whose own address is `about:blank` — or a `data:` /
     `blob:` document — does reach the mint, passes its non-empty check, parses, and
     states NO HOST. A principal that names no server is not a private-network
     principal. That is an answer about what the address SAYS, so it classifies public
     here deliberately, and the code now reads it as the statement it is. */
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
  /* A principal that PARSES and names no host is the third state above, read as the positive statement
     it is: no server, therefore not private. The `!!_pageUrl &&` is the release guard the paragraph
     above names — with the DCHECKs compiled out, an unparseable principal lands here as "public",
     which is the same fail-closed answer this line has always given it. */
  var _pagePrivate = !!_pageUrl && _isPrivateHost(_pageUrl.hostname);
  /* NETWORK. This gate exists because the extension's host permissions BYPASS the browser's own answer, so
     what it does is REINSTATE it: a public page reaching the person's intranet is refused by Private Network
     Access, and by Fetch §4.10 "CORS check" besides, both of which reach the page as a network error. */
  if (_isPrivateHost(parsed.hostname) && !_pagePrivate)
    return _refused("network", "blocked-private-from-public", [parsed.href], {});
  /* THE FIRING DECISION, BEFORE THE REQUEST EXISTS — see `_firingRefusal`. It is placed after the
     well-formedness gates and before the credential one because that is the order the questions are
     answerable in: an address that will not parse, names a scheme this zone cannot speak, or points into the
     person's intranet is refused on facts about ITSELF, while this one is about WHOSE ACT it is and is the
     first gate that needed a field to be carried here to be askable at all.
     THE REFUSAL NAMES THE GRADE, exactly as `blocked-scheme:` and `blocked-destructive:` name theirs. A
     reader of a request that did not happen needs WHICH RULE refused it, and `forced` in this position is the
     whole account: the origin is not widened for exploration and the address is reported instead. */
  /* DECLINE, AND THIS IS THE ARM THE WHOLE GRADE EXISTS FOR. No browser refuses this — there is no server
     behaviour here at all, only this zone declining to ASK, because the address stands on an arm a gate was
     forced onto. So there is nothing to hand a flow back, and §@S says exactly what that state is: a search
     not yet solved, a PARKED flow, never a verdict. Parking is also what makes the widening MEAN anything —
     the flow fires the day `safeFetchWiden` is told about this origin, which a flow that has already run its
     failure path cannot do. */
  var _ptok = _firingRefusal(provenance, parsed.origin);
  if (_ptok)
    return _refused("decline", "blocked-provenance:" + _ptok, [parsed.href], {});
  // opts.credentialed: replay a learned GET with the user's COOKIES to fetch the REAL
  // authenticated reply (the logged-in API surface), instead of a useless 401. Still
  // GET-only (method is forced below) so a well-designed server performs no account
  // action. The reply is gated by our OWN SOP/CORS check after the fetch (see below) —
  // the browser's does not apply to an extension fetch with host_permissions.
  /* THE FLAG ITSELF IS DERIVED AT THE ENTRY (`_credentialedOf`) AND READ HERE. It used to be derived on this
     line, which put the one fact three rules below are scoped by downstream of the door where the request's
     shape is judged — and left the invariant that IS scoped by it (no bundle-stated header list on a
     cookie-bearing request) with nowhere early enough to stand. One derivation, above every gate that reads
     it, is the same rule this file already applies to the landed URL and to `provenance`. */
  // THE DENY LIST, BEFORE THE REQUEST EXISTS — see _destructiveToken. Scoped to the
  // credentialed case because that is half of where the harm is: without the person's
  // cookies a logout path ends no session. The refusal names the token so it is
  // attributable, exactly as `blocked-scheme:` and `blocked-corb:` name their ground.
  /* AND THE SECOND HALF IS THE PROVENANCE, WHICH THIS GATE HAS BEEN WAITING FOR. The
     list's own paragraph states the harm exactly: "Forced execution builds requests NO
     REAL CLIENT MAKES", and "a GET that ends the person's session mid-analysis is a CSRF
     we committed against our own user". Both halves of that sentence are load-bearing and
     only one of them was askable here, so the gate stood over the whole credentialed
     population — including the one caller for which the first half is FALSE BY
     CONSTRUCTION. `observed` is solver/engine.h's "a real load of this document makes
     exactly this request", and the ambient seed is that in its strongest form: the address
     is the one the browser ACTUALLY NAVIGATED TO, so the person's own browser performed
     this exact credentialed GET seconds ago in this same profile. Refusing to repeat it
     prevents no state change — the state change already happened, by the person's own act
     — and costs the analysis the document they are looking at, reported unanalysed with a
     token in place of a reason anybody would recognise.
     THIS IS A LOOSENING AND IS NAMED AS ONE. The list's cheap direction is being over-broad
     and its expensive one is being loosened by accident, so the condition is narrowed to
     exactly the population the harm argument covers and to nothing wider: a credentialed
     request THIS TOOL originated (`derived`, `forced`) is still refused on a destructive
     path, on the initial URL and again after a redirect. What changes is only the request
     the person had already made. */
  if (credentialed && provenance !== "observed") {
    /* DECLINE, AND IT IS THE PERMANENT ONE — no widening reopens this list, which is the case the grade above
       argues does not need a third word. A browser would send this request; this tool will not send it with
       the person's session. So there is no observation to relay, and a network error here would be the same
       lie the provenance arm makes, told about a request the server would have answered. */
    var _dtok = _destructiveToken(parsed);
    if (_dtok)
      return _refused("decline", "blocked-destructive:" + _dtok, [parsed.href], {});
  }
  var init = { method: "GET", credentials: credentialed ? "include" : "omit", redirect: "follow" };
  /* THE CALLER'S HEADER LIST — AND IT IS NOT "ANALYZER PROBE HEADERS ONLY", WHICH IS WHAT THIS COMMENT SAID.
     SECURITY.md §Network corrects it by name: on the XHR path `fetchedXhr` forwards the analysed BUNDLE's
     own header list, so the values here come from the zone this one does not trust. That is within the model
     for the request this file actually issues — an uncredentialed GET, whose forbidden header names the
     browser strips, to a host the page could ask itself — and it is refused outright on a cookie-bearing one
     (`_credentialedOf`), which is where the correction stops being a note and becomes a rule.
     Auth headers are never ADDED here; cookies, in credentialed mode, are the browser's own, attached by
     `credentials:"include"` and gated by the SOP/CORS check below. */
  if (opts.headers) init.headers = opts.headers;
  if (opts.signal) init.signal = opts.signal;
  var resp = await fetch(parsed.href, init);
  /* THE URL THE BYTES CAME FROM, PARSED ONCE, ABOVE EVERY GATE THAT JUDGES IT. Fetch §2.2.5
     "Requests": "A request has an associated current URL. It is a pointer to the last URL in
     request's URL list" — and §4.1 "Main fetch" gives a response the readable "basic" tainting
     on "request's current URL's origin is same origin with request's origin", never on the
     address that was merely requested. `redirect: "follow"` means the chain has already been
     walked by the time this line runs, so "where we asked" and "where it came from" are two
     different facts, and FOUR readers below need the second one.
     THEY EACH USED TO DERIVE IT FOR THEMSELVES, INSIDE A `try`/`catch` OF THEIR OWN, AND THAT
     IS THE DEFECT THIS BLOCK EXISTS TO END. The private-host re-check parsed `resp.url` under
     `catch (e) {}`, the destructive-path re-check parsed it again under another, and
     `_finalHref`/`_finalOrigin` were computed a third and fourth time under two more, seventy
     lines further down. Every one of those arms was a SKIP: an address this zone could not
     parse silently disabled the gate standing over it, so a credentialed reply arrived with
     no `blocked-` row anywhere and read exactly like an address that legitimately matched
     nothing. Fail-open, four times, for one unanswerable question.
     AND THE SECOND CATCH SWALLOWED AN INVARIANT ABORT. `_destructiveToken` asserts its own
     percent-decode termination; on this side an assert is a THROW, so `catch (e) {}` around
     the call turned a broken decoder into a permitted request — which is precisely the defect
     `check.js` wrote `RETHROW_FATAL` for and records having already paid for once.
     SO IT IS ASKED ONCE, AND AN UNANSWERABLE ANSWER IS A REFUSAL RATHER THAN A SKIP. An empty
     `resp.url` is not unanswerable and is not a hole: §5.5 "Response class" gives `url` "the
     empty string if this's response's URL is null", i.e. no redirect information at all, so the URL
     the bytes came from IS the address requested — which the pre-request deny list and the
     initial private-host check have already judged. That is a positive statement, so `parsed`
     is the answer rather than a default filling a gap.
     WHY A REFUSAL AND NOT AN ASSERT, WHICH IS THE WHOLE OF THE FAILURE-MODE QUESTION: the
     discriminator is WHO DETERMINES THE VALUE. `_destructiveToken`'s argument is determined by
     this zone's own code, so a bad one is our contract broken and CHECK is right. `resp.url`
     is determined by the SERVER's redirect chain, and SECURITY.md says what an assert on a
     value the other side picks costs — "a DCHECK on a hostile input hands any web renderer an
     abort of the only trusted zone in the extension". Both directions fail closed; only one of
     them lets a server end the analysis. The request is already sent by the time this runs, so
     refusing costs an ingest and nothing else. */
  var _finalUrl = parsed;
  if (resp.url) {
    var _fu = null;
    /* A URL that will not parse is a REAL OUTCOME the constructor is defined to report this
       way — URL Standard §6.1 "URL class": "Let parsedURL be the result of running the API URL
       parser on url with base, if given. If parsedURL is failure, then throw a TypeError" — so
       this catch HAS a job and is not a swallow. It opens by letting an invariant abort travel
       on, which is the one thing a catch in this zone must never absorb. */
    try { _fu = new URL(resp.url); }
    catch (e) { RETHROW_FATAL(e); _fu = null; }
    /* NETWORK. The request went out and the browser answered with an address this zone's own URL parser
       refuses, so what is unusable is the RESPONSE — "this zone tried and the reply cannot be read" is what a
       network error says, and no policy of ours declined anything. */
    if (!_fu)
      return _refused("network", "blocked-final-url-unparseable", [parsed.href], {});
    _finalUrl = _fu;
  }
  /* AND THE HREF IS TAKEN OFF THAT SAME RECORD RATHER THAN OFF `resp.url` AGAIN, which is
     what makes "one answer" true of the string that LEAVES this function and not only of the
     gates inside it: `bridge.js` reads the URL list's last item and decides a Document's
     agent cluster from it, so a second reading of `resp.url` would be a different string
     deciding a principal from the one the gates judged. It is the SAME bytes either way —
     Fetch §5.5 "Response class" hands back the response's URL "serialized with exclude
     fragment set to true", and re-serializing an already-serialized fragment-less URL is
     the identity — which is why this is a structural guarantee rather than a normalization. */
  var _finalHref = _finalUrl.href;
  var _finalOrigin = _finalUrl.origin;
  /* IS THE LANDED RESOURCE SAME-ORIGIN WITH THE PAGE PRINCIPAL — ONE QUESTION, ONE ANSWER, COMPUTED
     HERE BECAUSE BOTH POST-FETCH GATES ASK IT AND THEY MUST NOT BE ABLE TO DISAGREE. The block above
     ended four private derivations of the landed URL; this ends the last derivation of the COMPARISON
     over it. CORB asked it through a helper that took `_finalHref` and re-parsed it under a `catch` of
     its own, and the credentialed SOP asked it again inline over `_finalOrigin` — the same question,
     answered twice, from two parses, with two failure arms. The helper's catch was unreachable (its
     argument was the `.href` of a URL object, which a URL parser cannot refuse) and its arm was
     fail-closed, so this is dead code rather than a hole; it is deleted rather than commented because
     the assumption it hid — that the two gates judge the SAME origin — is now structural instead of
     something a reader has to verify. A body CORB exempts as same-origin and the credentialed gate
     refuses as cross-origin is the disagreement that can no longer be written.
     THE REASONING THE DELETED HELPER CARRIED, KEPT: `_finalOrigin` is a network resource's own origin,
     which IS its URL's origin. `pageOrigin` is the AUTHORITATIVE browser origin of the loading document
     (`MessageSender.origin`), passed in and NEVER url-parsed — a sandboxed frame reports an ordinary
     address and an OPAQUE origin, so parsing its address would fabricate a tuple origin it does not
     have and hand it same-origin access to its embedder's credentialed document. No real `pageOrigin`
     -> not same-origin -> strict CORB and a credentialed read that needs CORS, which an opaque origin
     can never be granted (ACAO cannot equal it). That is the fail-closed direction in both gates.
     THE PAGE ORIGIN IS ALSO READ ONCE, for the same reason: the credentialed gate compares ACAO against
     it, and a second spelling of the principal beside the one the same-origin test used is how those
     two stop being about one origin. */
  var _pageOrigin = opts.pageOrigin || "";
  var _resourceSameOrigin = _isRealOrigin(_pageOrigin) && _isRealOrigin(_finalOrigin) &&
                            _finalOrigin === _pageOrigin;
  // SSRF-via-redirect: the initial-URL check can't see a 30x to the intranet.
  // Re-validate the FINAL url (redirects were followed) BEFORE reading the body,
  // so a public page's request that landed on a private host never feeds internal
  // data into the analysis. (Modern Chrome's Private Network Access also gates the
  // request itself for extension fetches; this stops the data from being ingested.)
  // Where nothing redirected `_finalUrl` IS `parsed`, so this re-asks a question already
  // answered above rather than skipping one — the same answer, never an unasked gate.
  /* NETWORK, by the same argument the pre-request private-host check carries: PNA and CORS both refuse a
     public page a private host's bytes, and both reach the page as a network error. */
  if (_isPrivateHost(_finalUrl.hostname) && !_pagePrivate)
    return _refused("network", "blocked-private-redirect", [parsed.href], {});
  /* THE DENY LIST AGAIN ON THE FINAL URL, AND WHAT IT CAN AND CANNOT DO. `redirect:
     "follow"` means a 30x into a destructive path was ALREADY followed by the time this
     runs, so unlike the pre-request check this one cannot un-send anything — it refuses
     to INGEST, which is exactly the shape and exactly the limit of the private-host
     check immediately above it, and it is placed here for the same reason.
     That is not a hole being papered over: following a redirect the SERVER chose is the
     server's own behaviour, and a person who navigated to the initial address would have
     been carried to the same place. The harm this file exists to prevent is INITIATING a
     request the person never would, and that decision is the pre-request check. What is
     left here is refusing to build analysis on the reply. */
  if (credentialed && provenance !== "observed") {
    /* DECLINE, AND THE FACT THAT THE REQUEST ALREADY WENT OUT DOES NOT MOVE IT. The server was reachable and
       answered; a network error would state the opposite, which is a lie about the world with a reply sitting
       right there to contradict it. What this zone is doing is refusing to build analysis on that reply, and
       a refusal to INGEST is a decline whatever side of the wire it is made on. */
    var _rtok = _destructiveToken(_finalUrl);
    if (_rtok)
      return _refused("decline", "blocked-destructive-redirect:" + _rtok,
                      _urlList(parsed.href, _finalHref, resp.redirected), {});
  }
  /* AND THE FIRING DECISION AGAIN ON THE FINAL URL, WITH THE SAME SHAPE AND THE SAME LIMIT
     AS THE TWO GATES ABOVE IT. `redirect: "follow"` means a 30x off the widened origin was
     ALREADY followed by the time this runs, so this cannot un-send anything — it refuses to
     INGEST, and the argument the destructive re-check makes for that is this one's too:
     following a redirect the SERVER chose is the server's own behaviour, and what is left
     here is refusing to build analysis on the reply. What makes it worth refusing: a
     widening is a sentence about ONE HOST — "at this host, fire what the bundle reaches past
     a forced gate" — and a reply that came from somewhere else is a reply about a server
     nobody said that about, carried under a grade the person granted to a different one. */
  /* DECLINE, for the reason the destructive re-check states one line up, and with the widening still live: a
     person who widens the LANDED origin makes this same load fire, and a flow that has run its failure path
     is not there to fire. */
  var _rptok = _firingRefusal(provenance, _finalUrl.origin);
  if (_rptok)
    return _refused("decline", "blocked-provenance-redirect:" + _rptok,
                    _urlList(parsed.href, _finalHref, resp.redirected), {});
  /* THE HEADERS, WITH NOTHING BETWEEN THEM AND THE TWO GATES THAT READ THEM. This walk stood
     inside `catch (e) {}`, whose arm was an EMPTY header map — and an empty map is not an
     absent input to the rules below, it is a wrong one: CORB then judges a body labelled with
     nothing, and the credentialed CORS check reads an absent `Access-Control-Allow-Origin`.
     Fetch §5.5 "Response class" — "The headers getter steps are to return this's headers" —
     hands back the `Headers` of §5.1 "Headers class", whose iteration can throw only what the
     callback throws, and this callback lowercases a string; so the arm was unreachable as well
     as wrong. Unwrapped, a producer that ever stopped answering one aborts here instead of
     handing a security decision a header map this zone invented. */
  var headers = {};
  resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; });
  /* §2.2.5's BODY, READ AS THE BYTE SEQUENCE IT IS — after both SSRF checks (the
     initial URL above, and the post-redirect final URL immediately above this), which
     is where they were and where they must stay: nothing internal is ingested before
     the target is judged. `arrayBuffer()` is Fetch §5.3 "Body mixin"'s "consume
     body" with NO decode after it — §5.3 defines that algorithm and both methods over
     it, and §5.2 "BodyInit unions", which stood here, is the EXTRACT that runs in the
     other direction — which is the whole difference from the `text()` this used to be:
     what the engine receives is what the server sent, and every standard that has an
     opinion about how those bytes become characters gets to hold it. */
  var body = new Uint8Array(await resp.arrayBuffer());
  /* THE ONE SNIFF, AND THE ONE TYPE IT PRODUCES. Both readers below are handed THIS
     pass: the CORB gate reads `protected` and the record reads `type`. That is what
     "safeFetch is the only source of sniffing" means as a shape rather than as a
     rule someone follows — there is no second call to make, on this path or on any
     other, so a second answer cannot exist to disagree with the first.
     ABSENCE IS READ AS A POSITIVE STATEMENT, never filled in: a response with no
     `Content-Type` is §5.1's "the supplied MIME type is undefined", which is what
     `_computedType` reads a missing header as, and is a different input from the
     empty string a `|| ""` would manufacture. */
  var _nosniff = _determineNosniff(headers["x-content-type-options"]);
  var _sn = _sniff(body);
  var _computed = _computedType(headers["content-type"], _nosniff, _sn);
  /* THE TWO GATES BELOW READ `_finalHref` / `_finalOrigin` / `_resourceSameOrigin`, WHICH THE
     BLOCK ABOVE THE FIRST POST-FETCH GATE COMPUTED — and that placement is the fix, not a
     preference. They used to be derived HERE, below the two post-fetch gates, while those gates
     each re-derived the same fact for themselves; the two gates below then read `parsed`, the
     requested address, so a SAME-ORIGIN request that 302'd to another host was CORB-exempt
     as "same-origin" and passed the credentialed SOP as same-origin, and the other host's
     cookie-bearing reply was handed back readable. One question, one answer, computed once
     — and computed ABOVE the first reader, so no gate can be reached before it exists.
     THE COMPARISON MOVED UP WITH THE VALUES IT IS OVER, and that was the residue of the same
     fix rather than a separate one: the addresses were computed once here while the SAME-ORIGIN
     TEST over them was still asked twice — once by a CORB helper that re-parsed `_finalHref`,
     once inline by the credentialed gate. Two derivations of one security question is the shape
     the paragraph above is about, whatever it is a derivation OF. */
  // CORB policy BY THE REQUEST'S DESTINATION (opts.destination, Fetch §2.2.5). A
  // SCRIPT-LIKE destination is bytes that will RUN as code under QuickJS control, so
  // the body must be JS-typed or same-origin; every other destination ("" for a
  // `fetch()`, "image", "font", "style" …) is data and is exempt. Whether the result
  // later REACHES QuickJS is the caller's documented contract, not enforced here
  // (safeFetch returns bytes; the engine boundary is downstream).
  //
  // IT USED TO BE `opts.as === "script"` OVER A LOAD-TYPE KEYWORD THIS FILE INVENTED
  // ("script"/"sourcemap"/other), AND THE KEYWORD WAS THE HOLE. A caller had to KNOW a
  // load was code and say so, which meant the classification lived wherever a caller
  // happened to compute it — for the extension, in a side list only the module loader
  // filled, so a document's own `<script src>` arrived here with no `as` at all and a
  // cross-origin HTML or JSON body served for it was returned to be compiled. The
  // destination is a property of the request that the engine states at every park, so
  // there is no longer a caller that can forget to classify: `opts.as` is GONE rather
  // than accepted alongside, and the entry asserts on a caller still passing it,
  // because silently ignoring the old key is exactly a code load fetched as data.
  if (_isScriptLike(_destinationOf(opts)) && resp.ok) {
    // The DECLARED essence is what CORB's two tables are stated over — the rule is
    // "was this labelled as data", and the sniff is the separate confirmation step
    // for a body whose label lied.
    var _declared = String(headers["content-type"] == null ? "" : headers["content-type"])
      .split(";")[0].trim().toLowerCase();
    var _deny = _corbDeniesScript(_declared, _nosniff, _sn, _resourceSameOrigin);
    // The rule that decided rides the status message with what this file computed the
    // resource to be, which is where every other refusal already puts its ground
    // (`blocked-scheme:https:`).
    /* NETWORK, because the browser refuses this load too — Chromium's CORB, and ORB after it, exist precisely
       to stop a cross-origin data body reaching a code loader, and an ORB-blocked script load reaches the page
       as Fetch §5.6's network error. This zone applies the rule only because a host-permission fetch bypasses
       the browser's copy of it, so the grade is the browser's answer relayed rather than a policy of ours.
       ONE ARM OF `_corbDeniesScript` DIVERGES AND IT IS NAMED RATHER THAN GLOSSED: `same-origin-protected`
       refuses a page its OWN HTML as script text, which a browser would hand over — the load would then die
       at COMPILE with a SyntaxError instead of on the wire with `onerror`. That is a browser-half fidelity
       residual (the wrong failure event, not a fabricated one), and it is not a decline: no policy of ours
       declined the act, and parking a flow on a load a browser completes would be the opposite error. */
    if (_deny)
      return _refused("network", "blocked-corb:" + _deny + ":" + _computed,
                      _urlList(parsed.href, _finalHref, resp.redirected), headers);
  }
  // OWN SOP/CORS for a CREDENTIALED reply. The browser does NOT apply the same-origin
  // policy to an extension fetch with host_permissions (it can read any origin), so
  // when cookies are attached we MUST enforce SOP + CORS HERE on the bytes before
  // returning them — else a malicious bundle could record a cross-origin endpoint and
  // exfiltrate the user's authenticated data from any site they are signed into.
  //   • SOP:  same-origin to the page principal (opts.pageUrl) is readable.
  //   • CORS: a CROSS-origin credentialed read is allowed ONLY if the server granted
  //           the page's EXACT origin a credentialed read — Access-Control-Allow-Origin
  //           == that origin (never "*") AND Access-Control-Allow-Credentials == true.
  // This is precisely the browser's credentialed-CORS rule, re-implemented because the
  // host-permission fetch bypasses it. Blocked reads return no body (the request was a
  // GET, so nothing was mutated; we simply refuse to hand the bundle the bytes).
  if (credentialed) {
    // SAME-ORIGIN principal = the BROWSER-provided origin (opts.pageOrigin, from
    // MessageSender.origin), authoritative and NEVER re-derived from a URL string:
    // a sandboxed frame reports a normal URL but an OPAQUE "null" origin, so
    // parsing the URL would wrongly grant it same-origin access to the embedder's
    // credentialed data. An opaque origin ("null" / "null:<uuid>" / empty — incl. a
    // mixed-origin buffer's minted token) is UNIQUE per the spec → never same-origin
    // with ANYTHING → a credentialed cross-origin read that needs CORS, which an
    // opaque origin can never be granted (ACAO can't equal it). No pageOrigin ->
    // "" -> fail closed.
    // BOTH `_pageOrigin` AND THE SAME-ORIGIN ANSWER ARE READ, NOT RECOMPUTED. They are
    // the ones the block above `_finalHref` computed, over `_finalOrigin` and not
    // `parsed.origin`; this gate asking the question a second time for itself is what
    // let it and CORB disagree about one origin, and is what that block ended.
    if (!_resourceSameOrigin) {
      var _acao = headers["access-control-allow-origin"] || "";
      var _acac = (headers["access-control-allow-credentials"] || "").toLowerCase();
      // Fetch §4.10 "CORS check": ACAO must byte-match the request's own origin (`*` is
      // refused once credentials mode is "include") and ACAC must be `true`.
      if (!_isRealOrigin(_pageOrigin) || _acao !== _pageOrigin || _acac !== "true")
        /* AND THE REFUSAL NAMES THE ORIGIN THAT FAILED THE CHECK, because after the fix
           above this is the arm a same-origin credentialed load that LANDED somewhere
           else comes out of, and "blocked" with no ground sends its reader hunting
           which of three rules fired. Every other refusal in this file already names
           one (`blocked-scheme:https:`, `blocked-corb:<rule>:<type>`,
           `blocked-destructive:<token>`); this was the last that did not.
           AND THE `|| "unparseable"` THAT STOOD HERE IS GONE WITH THE STATE IT NAMED. It was
           written when `_finalOrigin` came out of a `catch (e) {}` that could leave it empty;
           an address this zone cannot parse is now its own refusal above
           (`blocked-final-url-unparseable`) and never reaches this line, so the default's arm
           is unreachable and keeping it would be a second name for a state that no longer
           exists — the one thing a reader of a refusal message must not be handed. */
        /* NETWORK, and this one is the browser's own rule almost verbatim: Fetch §4.10 "CORS check" failing
           makes §4.4 "HTTP fetch" "return a network error", which §5.6 turns into the page's TypeError. This
           zone runs the check only because a host-permission fetch bypasses the browser's, so the flow's
           failure path is exactly where a real browser would put it.
           THE HEADERS ARE `{}` HERE AND `headers` AT CORB, WHICH IS DELIBERATE. CORB refuses a body whose own
           label is the evidence; this refuses a CROSS-ORIGIN credentialed read the server never granted, and
           handing its response headers back is handing over part of the very thing the check refused. */
        return _refused("network", "blocked-cors-credentialed:" + _finalOrigin,
                        _urlList(parsed.href, _finalHref, resp.redirected), {});
    }
  }
  /* AND THE TYPE THIS ZONE COMPUTED TRAVELS WITH THE BYTES. `computedType` is the
     whole of what "safe-fetch tells the renderer the guessed content type" means:
     the renderer is handed an answer rather than the evidence, exactly as it is
     handed a browser-stated origin on a delivered message rather than a URL to parse.
     engine/host/solver/reply_decode.c reads this field and DCHECKs its presence — an
     absent stamp is a producer that failed, never a type called "unknown". */
  /* AND `refusal: null` IS A POSITIVE STATEMENT, WHICH IS WHY IT IS WRITTEN AND NOT LEFT OFF. It says THIS
     REQUEST REACHED THE WIRE AND THE REPLY BELOW IS WHAT CAME BACK — including an HTTP error status, which is
     a server's answer and never a refusal of ours. A record that simply omitted the field would make "the
     chokepoint permitted this" and "the chokepoint stopped grading" the same absence, and the second of those
     is a decline arriving at a host as a reply.
     THE TWO SIGNALS ARE BOUND SO THEY CANNOT DRIFT. `status: 0` is the one status no HTTP response has and it
     is what every refusal answers; a graded refusal is the same fact stated in a vocabulary. Asserting the
     equivalence here is what stops a future arm from carrying one without the other — a refusal with a real
     status would be ingested as a reply, and a status-0 reply would be a network error nothing declined.
     `mode` is unset on the request below, so Fetch's default `cors` applies and there is no opaque filtered
     response to answer 0 legitimately. */
  DCHECK(resp.status !== 0,
         "the wire answered with status 0, which is the one status no HTTP response has and which every " +
         "refusal in this file answers with — `mode` is unset so Fetch's default `cors` applies and an " +
         "opaque filtered response cannot arise, so a 0 here is a reply and a refusal wearing one number");
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: body,
           urlList: _urlList(parsed.href, _finalHref, resp.redirected), computedType: _computed,
           refusal: null };
}
/* THE CHOKEPOINT AND THE POLICY THAT DECIDES WHETHER IT FIRES, INSTALLED TOGETHER — because they are one
   thing and a host that could obtain one without the other would be a host holding half the contract. Both
   hosts reach them the same way: `engine/trusted.mjs` runs this file in a vm context and reads them off it,
   `ast-worker.html` loads it into the offscreen document before bridge.js.
   `safeFetchWiden` IS THE POLICY'S ONE INPUT and it takes a person's sentence. `safeFetchFiringRefusal` is the
   same answer for a caller whose act is not a fetch, and `safeFetchWidenedOrigins` is for a caller that must
   ASSERT no widening exists in its zone. None of the three is a second policy: all four names resolve to
   `_firingRefusal`, which is the only thing in this project that answers the firing question.
   `safeFetchMethodRefusal` IS THE OTHER HALF OF THE SAME QUESTION and is installed beside them for the same
   reason: the method decides whether an act may be spent (RFC 9110 §9.2.1 "Safe Methods") and this file
   answers it by ABSENCE, so a host that cannot see the answer writes its own — which both of them did, with
   two different grades. It answers in the SAME refusal vocabulary the reply record's `refusal` field carries,
   so a host has ONE shape to read whether the refusal came before the fetch or out of it.
   THERE IS DELIBERATELY NO `safeFetchDestructiveRefusal` HERE, AND ITS ABSENCE IS A DECISION RATHER THAN
   AN OVERSIGHT. One was exported and read by `lib/schema.js`'s page-context relay, and both halves were
   removed: the deny list is a floor under requests THIS FILE COMPOSES AND FIRES ON ITS OWN, and the relay
   carries requests an OPERATOR composed at a surface that showed them the bytes. A token list may refuse
   this file's own autonomy cheaply — a wrong deny costs one unfired request the report still carries in
   full — and may never overrule a human's explicit act, where the same wrong deny is the tool vetoing its
   operator on a substring they never saw. So the list stays scoped INSIDE `safeFetch`, and a reader who
   finds the other transport ungated has found the thing that was taken out on purpose. */
if (typeof self !== "undefined") {
  self.safeFetch = safeFetch;
  self.safeFetchMethodRefusal = safeFetchMethodRefusal;
  self.safeFetchWiden = safeFetchWiden;
  self.safeFetchFiringRefusal = safeFetchFiringRefusal;
  self.safeFetchWidenedOrigins = safeFetchWidenedOrigins;
}
