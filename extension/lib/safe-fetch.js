// safe-fetch.js — THE single external-fetch entry point for the analyzer.
//
// ALL external requests (lazy chunks, source maps, discovery probes, anything the
// analyzer pulls off the network) go through safeFetch so the security invariants
// live in ONE auditable place:
//   • cookies OMITTED by default (credentials:"omit") — no credentialed exfiltration.
//                        In CREDENTIALED mode (opts.credentialed) the user's cookies
//                        ARE attached to replay a learned GET and read the REAL
//                        authenticated reply (the logged-in API surface) — but the
//                        reply is gated by safeFetch's OWN SOP/CORS check (a host-
//                        permission fetch bypasses the browser's; see below).
//   • GET only         — method is forced to GET: forced execution explores many
//                        paths; a real POST/PUT/DELETE replay would mutate server
//                        state. A well-designed server never mutates on GET, so even
//                        the credentialed replay is side-effect-free; POST/PUT/DELETE
//                        endpoints are only RECORDED by forced exec, never issued.
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
// (text), urlList } — NOT a Response — so it is identical in the offscreen document
// and the Worker.
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
// [requested]: the request URL is a fact even when the reply is not. Loaded in both: offscreen-brain.js via <script> (ast-worker.html) and
// ast-thread.js via importScripts.
// CORB/ORB for a SCRIPT load (opts.as==="script"): a chunk/import becomes
// executable code under QuickJS control, so the response must be JS-typed (or
// same-origin) — never a cross-origin HTML/JSON/etc. DATA body read as code.
// Lives here (the chokepoint) so every code-loader gets it and a new one can't
// forget it (the chunk path previously had none).
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
function _sniffsProtected(s) {
  var h = String(s == null ? "" : s).slice(0, 4096).replace(/^﻿/, "");
  h = h.replace(/^\s+/, "");
  if (h.charAt(0) === "<") return true; // HTML/XML/SVG/markup
  if (h.charAt(0) === "{" || h.charAt(0) === "[") {
    try { JSON.parse(s); return true; } catch (e) {
      try { JSON.parse(h); return true; } catch (_) {}
    }
  }
  return false;
}
// A REAL (tuple) origin — scheme://host[:port] — usable for same-origin and CORS.
// An OPAQUE origin reports "null" (sandboxed iframe / data: / sandboxed doc) or
// our minted "null:<uuid>" token (per-document, and for a mixed-origin buffer);
// per the spec each opaque origin is UNIQUE, so it is NEVER same-origin with
// anything — not even another "null". A real origin is the only form containing
// "://"; "null" / "" / "null:<uuid>" do not, so this one test distinguishes them.
function _isRealOrigin(o) { return typeof o === "string" && o.indexOf("://") > 0; }
function _corbAllowsScript(mime, nosniff, body, scriptUrl, pageOrigin) {
  mime = String(mime || "").split(";")[0].trim().toLowerCase();
  var cross = true;
  // scriptUrl is a network resource — its origin IS its url's origin. pageOrigin is
  // the AUTHORITATIVE browser origin of the loading document, passed in; NEVER
  // url-parsed (a sandboxed frame's url would fabricate a tuple origin it lacks).
  // No real pageOrigin -> treat as cross-origin (strict CORB).
  try { var _so = new URL(scriptUrl).origin; cross = !(_isRealOrigin(_so) && _isRealOrigin(pageOrigin) && _so === pageOrigin); } catch (e) { cross = true; }
  if (!cross) return !(_corbProtectedMime(mime) && !_jsMime(mime)); // same-origin: only skip the page's own non-JS data
  if (_corbProtectedMime(mime)) return false;          // CORB-protected type
  if (nosniff && !_jsMime(mime)) return false;          // browser blocks too
  if (_sniffsProtected(body)) return false;             // mislabeled data
  return true;
}

// Private/loopback/link-local classification (RFC1918 + loopback + IPv6 ULA/LL)
// for the origin-relative SSRF rule: a request is blocked ONLY when the TARGET is
// private but the PAGE origin is not — a public page reaching the intranet.
// localhost->localhost and any->public are allowed (normal web rules).
function _isPrivateHost(host) {
  if (!host) return false;
  host = String(host).toLowerCase().replace(/^\[|\]$/g, "");
  return host === "localhost" || host === "0.0.0.0" || host === "::1" ||
    host.endsWith(".local") || host.endsWith(".localhost") ||
    /^127\./.test(host) || /^169\.254\./.test(host) || /^10\./.test(host) ||
    /^192\.168\./.test(host) || /^172\.(1[6-9]|2\d|3[01])\./.test(host) ||
    /^(::1|fe80:|fc[0-9a-f][0-9a-f]:|fd[0-9a-f][0-9a-f]:|::ffff:(127|10|192\.168|169\.254))/i.test(host);
}

// @security-contract  ENFORCEMENT POINT (the single network chokepoint)
//   guarantees: cookies omitted by default — or, in opts.credentialed mode, a GET
//               replay whose REPLY is gated by safeFetch's OWN SOP/CORS (same-origin
//               to the principal, else exact-origin ACAO + ACAC == true), since a
//               host-permission fetch bypasses the browser's same-origin policy;
//               method GET; http(s) only; origin-relative SSRF (a PRIVATE target is
//               blocked unless the page principal is itself private) on BOTH the
//               initial URL and the post-redirect final URL.
//   opts.as:    "script"          -> + CORB (cross-origin must be JS-typed)
//               "sourcemap"/other -> data, no CORB (not executed as code)
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
  catch (e) { return { ok: false, status: 0, statusText: "bad-url", headers: {}, body: "", urlList: [] }; }
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return { ok: false, status: 0, statusText: "blocked-scheme:" + parsed.protocol, headers: {}, body: "",
             urlList: [parsed.href] };
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
    return { ok: false, status: 0, statusText: "blocked-private-from-public", headers: {}, body: "",
             urlList: [parsed.href] };
  // opts.credentialed: replay a learned GET with the user's COOKIES to fetch the REAL
  // authenticated reply (the logged-in API surface), instead of a useless 401. Still
  // GET-only (method is forced below) so a well-designed server performs no account
  // action. The reply is gated by our OWN SOP/CORS check after the fetch (see below) —
  // the browser's does not apply to an extension fetch with host_permissions.
  var credentialed = !!opts.credentialed;
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
      return { ok: false, status: 0, statusText: "blocked-private-redirect", headers: {}, body: "",
               urlList: [parsed.href] };
  } catch (e) {}
  var headers = {};
  try { resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; }); } catch (e) {}
  var body = await resp.text();
  // CORB policy by LOAD TYPE (opts.as). "script" = bytes that will RUN as code
  // (chunk/import, under QuickJS control) -> must be JS-typed/same-origin. Other
  // loads ("sourcemap"/"config"/data — not executed) are exempt. Whether the
  // result later REACHES QuickJS is the caller's documented contract, not
  // enforced here (safeFetch returns bytes; the engine boundary is downstream).
  if (opts.as === "script" && resp.ok &&
      !_corbAllowsScript(headers["content-type"] || "",
        (headers["x-content-type-options"] || "").toLowerCase().indexOf("nosniff") >= 0,
        body, parsed.href, opts.pageOrigin || ""))
    return { ok: false, status: 0, statusText: "blocked-corb", headers: headers, body: "",
             urlList: _urlList(parsed.href, resp) };
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
    var _sameOrigin = _isRealOrigin(_pageOrigin) && _isRealOrigin(parsed.origin) && parsed.origin === _pageOrigin;
    if (!_sameOrigin) {
      var _acao = headers["access-control-allow-origin"] || "";
      var _acac = (headers["access-control-allow-credentials"] || "").toLowerCase();
      if (!_isRealOrigin(_pageOrigin) || _acao !== _pageOrigin || _acac !== "true")
        return { ok: false, status: 0, statusText: "blocked-cors-credentialed", headers: {}, body: "",
                 urlList: _urlList(parsed.href, resp) };
    }
  }
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: body,
           urlList: _urlList(parsed.href, resp) };
}
if (typeof self !== "undefined") self.safeFetch = safeFetch;
