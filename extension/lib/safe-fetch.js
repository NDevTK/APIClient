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
//   • origin-relative  — the analyzer acts with the analyzed PAGE's origin, passed
//     SSRF guard         PER CALL as opts.pageUrl (the trusted sender.tab.url) —
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
// It was `await resp.text()`, which is Fetch §5.2's `text()`: "run consume body
// with this and UTF-8 DECODE". A decode is a SEMANTIC, and CLAUDE.md §Architecture
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
// CORB/ORB for a SCRIPT load (opts.as==="script"): a chunk/import becomes
// executable code under QuickJS control, so the response must be JS-typed (or
// same-origin) — never a cross-origin HTML/JSON/etc. DATA body read as code.
// Lives here (the chokepoint) so every code-loader gets it and a new one can't
// forget it (the chunk path previously had none).
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
// THE PRINCIPAL COMPARISON, factored out of `_corbAllowsScript` and kept that way:
// it is the one line of the CORB rule that is about SECURITY.md rather than about
// bytes, and it is worth being able to read on its own. `scriptUrl` is a network
// resource, so its origin IS its URL's origin; `pageOrigin` is the AUTHORITATIVE
// browser origin of the loading document, passed in and NEVER url-parsed (a
// sandboxed frame's url would fabricate a tuple origin it lacks). No real
// pageOrigin -> not same-origin -> strict CORB, which is the fail-closed direction:
// an opaque origin is same-origin with nothing.
function _corbSameOrigin(scriptUrl, pageOrigin) {
  var so;
  try { so = new URL(scriptUrl).origin; } catch (e) { return false; }
  return _isRealOrigin(so) && _isRealOrigin(pageOrigin) && so === pageOrigin;
}
// CORB FOR A SCRIPT LOAD, over the facts already computed above so nothing here
// re-reads a header or re-decodes a body. Answers the RULE THAT REFUSED, or null for
// allowed — because "blocked" alone sends whoever reads the status message hunting
// for which of four rules fired, and the four are not interchangeable.
// IT WAS `_corbAllowsScript`, RETURNING A BOOLEAN, and the name is renamed with the
// return rather than kept over it: a function called "allows" that answers a deny
// reason reads correctly at exactly zero of its call sites. Every rule below is the
// one that stood in that function, in the order it stood in.
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
//               The learned-GET replay path (`fetched`) passes no pageOrigin and is
//               still uncredentialed — a learned address may be FORCED, which is a
//               per-origin decision that does not exist yet.
//   opts.as:    "script"          -> + CORB (cross-origin must be JS-typed)
//               "sourcemap"/other -> data, no CORB (not executed as code)
//   answers:    computedType — the ONE type decision made about this response, the
//               same one CORB was decided from, stamped on the record for the
//               renderer so no downstream zone repeats it.
//   principal:  opts.pageUrl PER CALL (the page's trusted sender.tab.url) classifies
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
   without the list having to enumerate spellings. */
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
/* Returns the token that refused this URL, or "" — a POSITIVE statement that nothing in
   the list matched, never a boolean whose false could also mean "not asked". The token
   travels into the status message so a refusal is attributable and a reviewer can
   disagree with THIS entry rather than with the list. */
function _destructiveToken(u) {
  var raw;
  try { raw = (String(u.pathname) + "&" + String(u.search)).toLowerCase(); }
  catch (e) { return ""; }
  var parts = raw.split(/[^a-z0-9._-]+/);
  for (var i = 0; i < parts.length; i++) {
    var t = parts[i];
    if (!t) continue;
    if (_DESTRUCTIVE_SET[t]) return t;
    var squashed = t.replace(/[-._]/g, "");
    if (squashed && _DESTRUCTIVE_SET[squashed]) return squashed;
  }
  return "";
}

function _urlList(requested, resp) {
  var redirected = false, final = requested;
  try { redirected = !!(resp && resp.redirected); } catch (e) {}
  try { if (resp && resp.url) final = String(resp.url); } catch (e) {}
  return redirected ? [requested, final] : [requested];
}

async function safeFetch(url, opts) {
  opts = opts || {};
  var parsed;
  try { parsed = new URL(String(url)); }
  // No URL at all, so there is no URL list either — « » is the honest report, and
  // the engine's `response.url` is then the empty string the spec names for it.
  catch (e) { return { ok: false, status: 0, statusText: "bad-url", headers: {}, body: _NO_BYTES(), urlList: [],
                       computedType: "" }; }
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return { ok: false, status: 0, statusText: "blocked-scheme:" + parsed.protocol, headers: {},
             body: _NO_BYTES(), urlList: [parsed.href], computedType: "" };
  // Origin-relative SSRF (see header). The PRINCIPAL is the analyzed PAGE's origin,
  // passed PER CALL as opts.pageUrl — NOT a shared global: two grinds run
  // concurrently in one worker, so a worker-global principal would let one page's
  // origin contaminate another's fetch. Every caller passes the trusted
  // sender.tab.url. A cross-origin script/source-map uses the origin of the PAGE
  // it is loaded into (gstatic.com JS in google.com acts as google.com), never the
  // asset's own host. Block a PRIVATE target only when the page origin is NOT
  // itself private. Unknown page origin -> treated as public -> blocked.
  var _po = opts.pageUrl || "";
  var _pageHost = ""; try { if (_po) _pageHost = new URL(_po).hostname; } catch (e) {}
  var _pagePrivate = _isPrivateHost(_pageHost);
  if (_isPrivateHost(parsed.hostname) && !_pagePrivate)
    return { ok: false, status: 0, statusText: "blocked-private-from-public", headers: {},
             body: _NO_BYTES(), urlList: [parsed.href], computedType: "" };
  // opts.credentialed: replay a learned GET with the user's COOKIES to fetch the REAL
  // authenticated reply (the logged-in API surface), instead of a useless 401. Still
  // GET-only (method is forced below) so a well-designed server performs no account
  // action. The reply is gated by our OWN SOP/CORS check after the fetch (see below) —
  // the browser's does not apply to an extension fetch with host_permissions.
  var credentialed = !!opts.credentialed;
  // THE DENY LIST, BEFORE THE REQUEST EXISTS — see _destructiveToken. Scoped to the
  // credentialed case because that is the whole of where the harm is: without the
  // person's cookies a logout path ends no session. The refusal names the token so it
  // is attributable, exactly as `blocked-scheme:` and `blocked-corb:` name their ground.
  if (credentialed) {
    var _dtok = _destructiveToken(parsed);
    if (_dtok)
      return { ok: false, status: 0, statusText: "blocked-destructive:" + _dtok, headers: {},
               body: _NO_BYTES(), urlList: [parsed.href], computedType: "" };
  }
  var init = { method: "GET", credentials: credentialed ? "include" : "omit", redirect: "follow" };
  // Analyzer probe headers only (e.g. discovery's X-Goog-Api-Key / X-Http-Method-
  // Override). Auth headers are never added here; cookies (credentialed mode) are the
  // browser's, attached by credentials:"include", and gated by the SOP/CORS check below.
  if (opts.headers) init.headers = opts.headers;
  if (opts.signal) init.signal = opts.signal;
  var resp = await fetch(parsed.href, init);
  // SSRF-via-redirect: the initial-URL check can't see a 30x to the intranet.
  // Re-validate the FINAL url (redirects were followed) BEFORE reading the body,
  // so a public page's request that landed on a private host never feeds internal
  // data into the analysis. (Modern Chrome's Private Network Access also gates the
  // request itself for extension fetches; this stops the data from being ingested.)
  try {
    if (resp.url && _isPrivateHost(new URL(resp.url).hostname) && !_pagePrivate)
      return { ok: false, status: 0, statusText: "blocked-private-redirect", headers: {},
               body: _NO_BYTES(), urlList: [parsed.href], computedType: "" };
  } catch (e) {}
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
  if (credentialed) {
    try {
      var _rtok = resp.url ? _destructiveToken(new URL(resp.url)) : "";
      if (_rtok)
        return { ok: false, status: 0, statusText: "blocked-destructive-redirect:" + _rtok,
                 headers: {}, body: _NO_BYTES(), urlList: _urlList(parsed.href, resp), computedType: "" };
    } catch (e) {}
  }
  var headers = {};
  try { resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; }); } catch (e) {}
  /* §2.2.5's BODY, READ AS THE BYTE SEQUENCE IT IS — after both SSRF checks (the
     initial URL above, and the post-redirect final URL immediately above this), which
     is where they were and where they must stay: nothing internal is ingested before
     the target is judged. `arrayBuffer()` is Fetch §5.2's "consume body" with NO
     decode after it, which is the whole difference from the `text()` this used to be:
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
  /* THE URL THE BYTES CAME FROM, WHICH IS WHAT EVERY GATE BELOW IS ACTUALLY ABOUT.
     Fetch §2.2.5 "Requests": "A request has an associated current URL. It is a pointer
     to the last URL in request's URL list" — and §4.1 "Main fetch" gives a response the
     readable "basic" tainting on "request's CURRENT URL's origin is same origin with
     request's origin", never on the address that was merely requested. `redirect:
     "follow"` means the chain has already been walked by the time this line runs, so
     "where we asked" and "where it came from" are two different facts.
     THIS FILE ANSWERED THAT QUESTION TWO WAYS, AND THE SPLIT WAS A HOLE. The two
     post-fetch gates above — the private-host re-check and the destructive-path
     re-check — both read `resp.url`, for the reason each states: a 30x is followed
     before either can see it, so the only defensible thing left is to refuse to INGEST
     what came back. The two gates BELOW read `parsed`, the requested address. So a
     SAME-ORIGIN request that 302'd to another host was CORB-exempt as "same-origin"
     and passed the credentialed SOP as same-origin, and the other host's cookie-bearing
     reply was handed back readable. That was harmless only while `opts.credentialed`
     had no caller; a navigation that carries the session makes it live. One question,
     one answer, computed once. */
  var _finalHref = parsed.href;
  try { if (resp.url) _finalHref = String(resp.url); } catch (e) {}
  var _finalOrigin = "";
  try { _finalOrigin = new URL(_finalHref).origin; } catch (e) {}
  // CORB policy by LOAD TYPE (opts.as). "script" = bytes that will RUN as code
  // (chunk/import, under QuickJS control) -> must be JS-typed/same-origin. Other
  // loads ("sourcemap"/"config"/data — not executed) are exempt. Whether the
  // result later REACHES QuickJS is the caller's documented contract, not
  // enforced here (safeFetch returns bytes; the engine boundary is downstream).
  if (opts.as === "script" && resp.ok) {
    // The DECLARED essence is what CORB's two tables are stated over — the rule is
    // "was this labelled as data", and the sniff is the separate confirmation step
    // for a body whose label lied.
    var _declared = String(headers["content-type"] == null ? "" : headers["content-type"])
      .split(";")[0].trim().toLowerCase();
    var _deny = _corbDeniesScript(_declared, _nosniff, _sn,
                                  _corbSameOrigin(_finalHref, opts.pageOrigin));
    // The rule that decided rides the status message with what this file computed the
    // resource to be, which is where every other refusal already puts its ground
    // (`blocked-scheme:https:`).
    if (_deny)
      return { ok: false, status: 0, statusText: "blocked-corb:" + _deny + ":" + _computed,
               headers: headers, body: _NO_BYTES(), urlList: _urlList(parsed.href, resp),
               computedType: "" };
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
    // "" -> fail closed: the principal is NEVER re-derived by parsing a URL (that
    // would fabricate an origin for a sandboxed/about:blank frame).
    var _pageOrigin = opts.pageOrigin || "";
    // OVER `_finalOrigin`, NOT `parsed.origin` — see the paragraph that computes it.
    var _sameOrigin = _isRealOrigin(_pageOrigin) && _isRealOrigin(_finalOrigin) && _finalOrigin === _pageOrigin;
    if (!_sameOrigin) {
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
           `blocked-destructive:<token>`); this was the last that did not. */
        return { ok: false, status: 0, statusText: "blocked-cors-credentialed:" + (_finalOrigin || "unparseable"),
                 headers: {}, body: _NO_BYTES(), urlList: _urlList(parsed.href, resp), computedType: "" };
    }
  }
  /* AND THE TYPE THIS ZONE COMPUTED TRAVELS WITH THE BYTES. `computedType` is the
     whole of what "safe-fetch tells the renderer the guessed content type" means:
     the renderer is handed an answer rather than the evidence, exactly as it is
     handed a browser-stated origin on a delivered message rather than a URL to parse.
     engine/host/solver/reply_decode.c reads this field and DCHECKs its presence — an
     absent stamp is a producer that failed, never a type called "unknown". */
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: body,
           urlList: _urlList(parsed.href, resp), computedType: _computed };
}
if (typeof self !== "undefined") self.safeFetch = safeFetch;
