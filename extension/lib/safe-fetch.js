// safe-fetch.js — THE single external-fetch entry point for the analyzer.
//
// ALL external requests (lazy chunks, source maps, discovery probes, anything the
// analyzer pulls off the network) go through safeFetch so the security invariants
// live in ONE auditable place:
//   • cookies OMITTED  — credentials:"omit": never attach the user's session to an
//                        analyzer-initiated request (no credentialed exfiltration).
//   • GET only         — method is forced to GET: forced execution explores many
//                        paths; a real POST/PUT/DELETE replay would mutate server
//                        state. Credentialed/same-origin verification (schema.verify)
//                        uses pageContextFetch in the page renderer, not this.
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
// (text) } — NOT a Response — so it is identical in the offscreen document and the
// Worker. Loaded in both: offscreen-brain.js via <script> (ast-worker.html) and
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
function _corbAllowsScript(mime, nosniff, body, scriptUrl, pageUrl) {
  mime = String(mime || "").split(";")[0].trim().toLowerCase();
  var cross = true;
  try { cross = new URL(scriptUrl).origin !== new URL(pageUrl).origin; } catch (e) { cross = true; }
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

async function safeFetch(url, opts) {
  opts = opts || {};
  var parsed;
  try { parsed = new URL(String(url)); }
  catch (e) { return { ok: false, status: 0, statusText: "bad-url", headers: {}, body: "" }; }
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return { ok: false, status: 0, statusText: "blocked-scheme:" + parsed.protocol, headers: {}, body: "" };
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
    return { ok: false, status: 0, statusText: "blocked-private-from-public", headers: {}, body: "" };
  var init = { method: "GET", credentials: "omit", redirect: "follow" };
  // Analyzer probe headers only (e.g. discovery's X-Goog-Api-Key / X-Http-Method-
  // Override). Never auth/cookies — credentials are omitted above regardless.
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
      return { ok: false, status: 0, statusText: "blocked-private-redirect", headers: {}, body: "" };
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
        body, parsed.href, _po))
    return { ok: false, status: 0, statusText: "blocked-corb", headers: headers, body: "" };
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: body };
}
if (typeof self !== "undefined") self.safeFetch = safeFetch;
