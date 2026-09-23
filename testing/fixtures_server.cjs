// Persistent HTTP server hosting testing/fixtures/* on a fixed port
// so the extension's PoC verify (which opens an attacker tab that then
// window.opens the target + dispatches postMessage) has a stable
// reachable target URL. Run once; survives across multiple verify
// invocations (matches the user's real workflow: target is a real
// site, not torn down between runs).
"use strict";
const http = require("http");
const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "fixtures");
const PORT = parseInt(process.env.FIX_PORT || "8765", 10);
const LOCK = path.resolve(__dirname, "fixtures.lock");

/* THE DEFAULT ROUTE IS RESOLVED ONCE, AGAINST THE DISK, AND ITS ABSENCE IS SAID OUT LOUD.
 *
 * This mapped `/` to a fixture unconditionally and the banner below announced the mapping, while the file it
 * named was in no revision and on no disk — so the one route this server advertises answered 404, and a 404 for
 * a fixture reads as the SUBJECT failing rather than as a route that was never provisioned. CLAUDE.md's rule is
 * that a value the producer can legitimately omit is a POSITIVE statement the consumer reads as one, never a
 * hole: the absence is stated here and printed at startup, rather than being discovered per request as a status
 * code that names nothing.
 *
 * IT IS ASKED ONCE rather than per request because the answer is a fact about the checkout, not about the
 * request — and because a per-request `existsSync` would make the banner and the behaviour able to disagree.
 * `/` falling through to the ordinary path handling is the correct arm when the default is absent: the request
 * still gets this server's own 404 for a path that is genuinely not here, and the banner has already said why. */
const DEFAULT_ROUTE = "/poc_multi.html";
const DEFAULT_ROUTE_PRESENT = fs.existsSync(path.join(ROOT, DEFAULT_ROUTE.slice(1)));

function pick(p) {
  if ((p === "/" || p === "") && DEFAULT_ROUTE_PRESENT) return DEFAULT_ROUTE;
  return p;
}
const CT = { ".html": "text/html; charset=utf-8", ".js": "application/javascript", ".json": "application/json", ".css": "text/css" };

/* WPTSERVE'S `status` PIPE, SPELLED THE WAY WPTSERVE SPELLS IT — `?pipe=status(404)`.
 *
 * WHY A PIPE AND NOT A KEYWORD OF THIS SERVER'S OWN. CLAUDE.md: never coin a system when an established one
 * exists. The corpus server the engine already meets is wptserve, whose tools/wptserve/wptserve/pipes.py
 * declares `def status(request, response, code): response.status = code`, and every WPT test that needs a
 * chosen status names it in exactly this query. A second grammar here would be a second thing to learn and a
 * second thing to get wrong, for a question one document already answers.
 *
 * IT IS THE `status` PIPE AND NOTHING ELSE, AND AN UNIMPLEMENTED ONE IS REFUSED RATHER THAN IGNORED. Ignoring
 * a pipe this server has not built serves the file at 200 under a URL that asked for something else — which
 * for the fixture below would fire `load` where the document asserts `error`, and the run would then read as
 * the ENGINE getting HTML §8.1.4.2 wrong. A refusal that names the pipe is a fact about this server; a silent 200
 * is a false accusation of the subject, which is the worse of the two by a long way.
 *
 * THE FILE'S OWN CONTENT-TYPE IS KEPT. A status pipe changes the STATUS; the whole point of the case below is
 * a body a compiler would happily accept, correctly typed, that HTML §8.1.4.2 refuses on the status alone. */
function pipeStatus(qs) {
  const m = /(?:^|&)pipe=([^&]*)/.exec(qs || "");
  if (!m) return { status: 200 };
  const spec = decodeURIComponent(m[1]);
  const st = /^status\((\d{3})\)$/.exec(spec);
  if (!st) return { refuse: spec };
  const code = parseInt(st[1], 10);
  /* Node's writeHead rejects a code outside 100..999 by throwing, which would kill the server for every other
     lane sharing it. The three-digit match above already bounds it; this is the range Fetch §2.2.3 "Statuses"
     itself states ("A status is an integer in the range 0 to 999, inclusive") narrowed to what HTTP can put on
     a status line, asserted here rather than discovered as an exception in a shared process. */
  if (code < 100) return { refuse: spec };
  return { status: code };
}

/* WPTSERVE'S `.headers` SIDECAR, SPELLED THE WAY WPTSERVE SPELLS IT — `<file>.headers` BESIDE THE FILE.
 *
 * WHY A SIDECAR AND NOT A KEYWORD OF THIS SERVER'S OWN: the argument is the one the `status` pipe above
 * already makes and it is not re-made here. What is new is that the convention was DERIVED FROM THE ARTIFACT
 * rather than recalled — engine/.work/wpt/tools/wptserve/wptserve/handlers.py is IN THIS CHECKOUT, and its
 * `load_headers` plus `FileHandler.get_headers` are the whole of the rule:
 *   - `PATH.headers` beside the served file AND `__dir__.headers` in its directory, concatenated with the
 *     DIRECTORY'S FIRST — `_load(request, os.path.join(os.path.dirname(path), "__dir__")) + _load(request, path)`.
 *   - each non-empty line split on the FIRST colon only (`line.split(b":", 1)`), both halves stripped.
 *   - an absent sidecar is `[]` and not an error, because having none is the overwhelmingly common case.
 *   - the guessed content-type is inserted ONLY where the sidecar names none:
 *     `if not any(key.lower() == b"content-type" for (key, _) in rv): rv.insert(0, ...)`.
 *
 * THE REPEAT IS LOAD-BEARING, WHICH IS WHY `ResponseHeaders.update` IS `append` AND NOT `set`. CSP §3.1 "The
 * Content-Security-Policy HTTP Response Header Field" says of a received header that the user agent "MUST
 * parse and enforce each serialized CSP it contains", so two policy lines are two policies enforced together
 * rather than the second replacing the first. Node spells `append` as an ARRAY VALUE, MEASURED rather than
 * assumed: `writeHead(200, { "content-security-policy": [a, b] })` puts TWO lines on the wire — the client's
 * `rawHeaders` holds two — where its joined `headers` view shows one comma-separated string, so a reader who
 * checks the convenience view alone cannot tell the two spellings apart. Folding repeats to a single value
 * would silently turn an intersection of two policies into whichever one was written last.
 *
 * THE DEFAULT CSP IS SUPPRESSED BY THE SAME RULE THAT SUPPRESSES THE GUESSED CONTENT-TYPE, and this server
 * has to choose that because wptserve has NO default policy for its rule to be about. The permissive policy
 * below is a GUESS about what a document wants, exactly as the extension-derived content-type is: the PoC
 * fixture needs its inline handler to run, and until now nothing here could say otherwise. So it stands
 * where the document states no policy and GOES where the document states one.
 * APPENDING IT INSTEAD WOULD HAVE BEEN WRONG IN THE DIRECTION NOBODY TESTS. A restrictive sidecar would
 * still block — `connect-src 'none'` beside a permissive policy is an intersection and the refusal survives —
 * so the first fixture anyone wrote would pass and the choice would look settled. What breaks is everything
 * that reads the POLICY ITSELF: a `securitypolicyviolation` handler's `originalPolicy` would name a policy
 * the document never declared, and the engine-versus-Chrome comparisons this corpus already makes out of
 * those fields would be comparing the wrong string. A default that cannot be turned off is not a default. */
const DEFAULT_CSP =
  "default-src 'self' 'unsafe-inline' 'unsafe-eval' data: blob: *; " +
  "script-src * 'unsafe-inline' 'unsafe-eval'; img-src * data: blob:;";

/* `.sub.headers` IS REFUSED AND NOT READ, FOR THE REASON THE PIPE ABOVE IS REFUSED RATHER THAN IGNORED.
 * wptserve tries `PATH.sub.headers` FIRST and runs `template()` over it, which expands `{{host}}` and
 * `{{ports[http][0]}}`. That substitution is not built here, and serving such a file LITERALLY is the worst
 * of the three available outcomes: the corpus really contains one —
 * `service-workers/service-worker/resources/fetch-csp-iframe.html.sub.headers` reads
 * `Content-Security-Policy: img-src https://{{host}}:{{ports[https][0]}}; connect-src 'unsafe-inline' 'self'`
 * — and an un-substituted `{{host}}` is not a permissive source, it is a source that matches nothing, so the
 * document would be handed a policy blocking its own images and the run would read as the ENGINE getting
 * CSP §6.1.6 "img-src" wrong.
 * NAMED RESIDUAL. NOT COVERED: a sidecar whose name carries `.sub.`, for which wptserve performs the
 * substitution in tools/wptserve/wptserve/pipes.py's `template`. THE NEXT DIFF BUILDS that substitution over
 * `{{host}}` and `{{ports[...][n]}}` against this server's own bound address. HOW ITS ABSENCE SHOWS: a
 * request for a file that has one answers 501 naming the sidecar, in this server's own log, rather than 200. */
function sidecarFor(base) {
  const sub = base + ".sub.headers";
  if (fs.existsSync(sub)) return { sub: sub };
  const plain = base + ".headers";
  if (fs.existsSync(plain)) return { plain: plain };
  return {};
}

/* THE PARSE IS WPTSERVE'S, WITH ONE STATED DEVIATION AND ONE REFUSAL IT DOES NOT MAKE.
 * DEVIATION: wptserve skips a line on `if line`, which is false only for a ZERO-LENGTH line, so a line of
 * spaces reaches its `split(b":", 1)`, yields a 1-tuple, and raises when the caller unpacks it. Skipping
 * whitespace-only lines instead cannot change what any correct sidecar means and removes a crash.
 * REFUSAL: a line with NO colon has no header name in it and there is nothing to send. It names the file and
 * the 1-BASED line, because the reader of that message is editing that file. */
function parseHeaderFile(p) {
  const lines = fs.readFileSync(p, "utf8").split(/\r?\n/);
  const pairs = [];
  for (let i = 0; i < lines.length; i++) {
    if (!lines[i].trim()) continue;
    const c = lines[i].indexOf(":");
    if (c < 0)
      return { refuse: path.relative(ROOT, p) + " line " + (i + 1) + ": no \":\" — wptserve reads a .headers " +
                       "line as NAME \":\" VALUE and splits on the first colon, so a line without one names no header" };
    pairs.push([lines[i].slice(0, c).trim(), lines[i].slice(c + 1).trim()]);
  }
  return { pairs: pairs };
}

function loadHeaders(fp) {
  const pairs = [];
  /* THE DIRECTORY'S SIDECAR IS FIRST, which is `load_headers`'s own concatenation order and is not cosmetic:
     with append semantics the per-file entry lands AFTER the directory's, so a reader of the wire and of a
     violation report sees the directory's policy first and the file's second. Both are enforced either way
     (CSP §3.1), so the order decides what a log reads like and never what is allowed. */
  for (const base of [path.join(path.dirname(fp), "__dir__"), fp]) {
    const s = sidecarFor(base);
    if (s.sub)
      return { refuse: path.relative(ROOT, s.sub) + " needs wptserve's {{...}} substitution, which this " +
                       "server does not implement — serving it literally would hand the document a policy " +
                       "whose sources match nothing" };
    if (!s.plain) continue;
    const r = parseHeaderFile(s.plain);
    if (r.refuse) return r;
    for (const kv of r.pairs) pairs.push(kv);
  }
  return { pairs: pairs };
}

function getHeaders(fp) {
  const r = loadHeaders(fp);
  if (r.refuse) return r;
  const states = (n) => r.pairs.some((kv) => kv[0].toLowerCase() === n);
  if (!states("content-type"))
    r.pairs.unshift(["Content-Type", CT[path.extname(fp)] || "application/octet-stream"]);
  if (!states("content-security-policy"))
    r.pairs.unshift(["Content-Security-Policy", DEFAULT_CSP]);
  return r;
}

/* `ResponseHeaders.append` IN NODE'S SPELLING: a name already present becomes an ARRAY, which Node writes as
   repeated header lines; a name seen once stays a string. Keys are lower-cased because `append` itself keys
   on `key.lower()`, and a sidecar is free to spell a header name in any case it likes. */
function foldPairs(pairs) {
  const out = Object.create(null);
  for (const kv of pairs) {
    const k = kv[0].toLowerCase();
    if (k in out) out[k] = [].concat(out[k], kv[1]);
    else out[k] = kv[1];
  }
  return out;
}

function refuse501(res, why) {
  res.writeHead(501, { "content-type": "text/plain; charset=utf-8" });
  res.end("fixtures_server: " + why + "\n");
}

const srv = http.createServer((req, res) => {
  // Strip any ?query / #hash before resolving the file
  const reqUrlFull = String(req.url || "/");
  /* TWO DISCRIMINATORS, BECAUSE THE TWO THIS CORPUS USES ARE NOT INTERCHANGEABLE AND EITHER CAN BE BLANK.
     `referer` is what testing/fixtures/scrstat_status_error.html reads — Chrome sends one on a subresource
     and safeFetch sends none — and `sec-fetch-dest` is what testing/corpus/control/serve.mjs reads, in its
     own words: "a browser subresource load states `image` and ... `empty` is the engine's". A top-level
     navigation carries NEITHER, so neither channel can attribute the document's own line. Logging both means
     a reader picks whichever is populated for the request kind in front of them, rather than discovering
     after a run that this server happened to record the other one. */
  console.log(`[${new Date().toISOString()}] ${req.method} ${reqUrlFull}  referer=${req.headers.referer || "-"}` +
              `  dest=${req.headers["sec-fetch-dest"] || "-"}`);
  const url = pick(reqUrlFull.split("?")[0].split("#")[0]);
  const qs = reqUrlFull.split("#")[0].split("?")[1] || "";
  const pipe = pipeStatus(qs);
  if (pipe.refuse) {
    refuse501(res, "unimplemented pipe " + JSON.stringify(pipe.refuse) +
                   " — this server implements wptserve's status(NNN) pipe only, and refuses rather than " +
                   "serving 200 for a request that asked for something else");
    return;
  }
  const fp = path.join(ROOT, url);
  if (!fp.startsWith(ROOT) || !fs.existsSync(fp) || !fs.statSync(fp).isFile()) {
    res.writeHead(404); res.end("404"); return;
  }
  /* THE HEADERS ARE RESOLVED, NOT LITERAL. This was one fixed object for every 200: an extension-keyed
     content-type beside a single permissive CSP that no file could vary, so a fixture needing a policy of
     its own could not be served here at all. Both are DEFAULTS now and a `.headers` sidecar replaces either. */
  const hdrs = getHeaders(fp);
  if (hdrs.refuse) { refuse501(res, hdrs.refuse); return; }
  res.writeHead(pipe.status, foldPairs(hdrs.pairs));
  fs.createReadStream(fp).pipe(res);
});

srv.listen(PORT, "127.0.0.1", () => {
  const lock = { pid: process.pid, port: PORT, startedAt: Date.now() };
  fs.writeFileSync(LOCK, JSON.stringify(lock, null, 2), "utf8");
  console.log(`fixtures server listening on http://127.0.0.1:${PORT}/`);
  console.log(DEFAULT_ROUTE_PRESENT
    ? `  GET / -> ${DEFAULT_ROUTE}`
    : `  GET / -> UNMAPPED: ${DEFAULT_ROUTE} is not in this checkout, so / answers 404`);
  console.log(`  lock: ${LOCK}`);
});

process.on("SIGTERM", () => { try { fs.unlinkSync(LOCK); } catch {} srv.close(() => process.exit(0)); });
process.on("SIGINT",  () => { try { fs.unlinkSync(LOCK); } catch {} srv.close(() => process.exit(0)); });
