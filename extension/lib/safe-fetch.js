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
async function safeFetch(url, opts) {
  opts = opts || {};
  var parsed;
  try { parsed = new URL(String(url)); }
  catch (e) { return { ok: false, status: 0, statusText: "bad-url", headers: {}, body: "" }; }
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:")
    return { ok: false, status: 0, statusText: "blocked-scheme:" + parsed.protocol, headers: {}, body: "" };
  var init = { method: "GET", credentials: "omit", redirect: "follow" };
  // Analyzer probe headers only (e.g. discovery's X-Goog-Api-Key / X-Http-Method-
  // Override). Never auth/cookies — credentials are omitted above regardless.
  if (opts.headers) init.headers = opts.headers;
  if (opts.signal) init.signal = opts.signal;
  var resp = await fetch(parsed.href, init);
  var headers = {};
  try { resp.headers.forEach(function (v, k) { headers[String(k).toLowerCase()] = v; }); } catch (e) {}
  return { ok: resp.ok, status: resp.status, statusText: resp.statusText, headers: headers, body: await resp.text() };
}
if (typeof self !== "undefined") self.safeFetch = safeFetch;
