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
 * the ENGINE getting §8.1.4.2 wrong. A refusal that names the pipe is a fact about this server; a silent 200
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

const srv = http.createServer((req, res) => {
  // Strip any ?query / #hash before resolving the file
  const reqUrlFull = String(req.url || "/");
  console.log(`[${new Date().toISOString()}] ${req.method} ${reqUrlFull}  referer=${req.headers.referer || "-"}`);
  const url = pick(reqUrlFull.split("?")[0].split("#")[0]);
  const qs = reqUrlFull.split("#")[0].split("?")[1] || "";
  const pipe = pipeStatus(qs);
  if (pipe.refuse) {
    res.writeHead(501, { "content-type": "text/plain; charset=utf-8" });
    res.end("fixtures_server: unimplemented pipe " + JSON.stringify(pipe.refuse) +
            " — this server implements wptserve's status(NNN) pipe only, and refuses rather than serving 200 " +
            "for a request that asked for something else\n");
    return;
  }
  const fp = path.join(ROOT, url);
  if (!fp.startsWith(ROOT) || !fs.existsSync(fp) || !fs.statSync(fp).isFile()) {
    res.writeHead(404); res.end("404"); return;
  }
  res.writeHead(pipe.status, {
    "content-type": CT[path.extname(fp)] || "application/octet-stream",
    // Permissive CSP so inline scripts (the PoC fixture HAS an inline
    // script handler) and onload= attributes execute — REAL EXPLOIT
    // payload (svg onload=) must run, otherwise the verify rightly
    // reports NOT REPRODUCED.
    "content-security-policy": "default-src 'self' 'unsafe-inline' 'unsafe-eval' data: blob: *; script-src * 'unsafe-inline' 'unsafe-eval'; img-src * data: blob:;",
  });
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
