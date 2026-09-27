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
/* TWO LOCK FILES, BECAUSE ONE FILE WAS ANSWERING TWO QUESTIONS AND THE STRICTER ONE WAS SILENTLY LOSING.
   The well-known path is a DISCOVERY entry: testing/poc_multi_e2e.cjs reads `fix.port` out of it, so its whole
   value is that a reader who does NOT know the port can find one. The per-server record is an ISOLATION fact:
   two servers on two ports are two subjects. A single file cannot be both, and writing it unconditionally made
   it neither — a second server on another port OVERWROTE the first's port with its own, so a reader following
   the discovery entry was sent to the wrong server; and either server's SIGTERM then unlinked whatever lock was
   there, so a reader could get ENOENT while a server was up. Both failures are silent and both read as the
   OTHER component being wrong.
   PORT-SCOPING THE WELL-KNOWN PATH IS THE FIX THAT LOOKS RIGHT AND IS CIRCULAR: a reader would need the port in
   order to find the file that tells it the port. Recorded because that is the repair a reader re-derives from
   the word `lock`. So the two questions get two files, each with one job and one lifetime:
     fixtures.lock          the well-known DISCOVERY entry. Claimed only when absent or held by a DEAD pid, and
                            released at exit only when it names THIS pid — a destructive step gated on a check's
                            result rather than chained after one. A live claim is left alone and said out loud.
     fixtures.<port>.lock   this server's own record, ours by construction, always written and always released.
   A pid can be recycled, which a liveness probe cannot see; that is acceptable in a test instrument and is why
   the probe decides only whether to CLAIM a path, never whether to serve anything. */
const LOCK = path.resolve(__dirname, "fixtures.lock");
const LOCK_PORT = path.resolve(__dirname, `fixtures.${PORT}.lock`);

function lockHolderIfLive(file) {
  let held;
  try { held = JSON.parse(fs.readFileSync(file, "utf8")); } catch { return null; }
  if (!held || typeof held.pid !== "number") return null;
  try { process.kill(held.pid, 0); } catch { return null; }
  return held;
}

/* THE ACCESS LINE NEEDS A READER THAT IS NOT A REDIRECT SOMEBODY REMEMBERED, WHICH IS WHY THIS FILE EXISTS.
   CLAUDE.md §A-WITNESS-MAY-NOT-BE-COMPOSED-FROM-A-VALUE-THE-SUBJECT-CAN-MAKE-UNKNOWN judges a witness on two
   axes: can its payload become unknown, and does its CHANNEL have a reader that is not the thing under test.
   A request landing in a serving host's own access log is the good channel and is the one that worked when a
   console line did not — and this server's access line was written by `console.log` ALONE, so whether it was
   ever readable afterwards depended on whether whoever started the server happened to redirect stdout.

   WHAT THAT DEGRADED INTO IS WORSE THAN NO LOG, and it was measured rather than feared. A tracked
   `testing/fixtures_access.log` sat in this tree with NOTHING IN THE REPOSITORY WRITING IT — a one-off
   redirect from a single afternoon, committed, and then read by everybody who asked whether a fixture's
   witness had fired. Asked for `/api/` it answered 0, with `poc_hash` at 41 as an armed positive control and
   an invented path at 0, and that 0 means NOTHING HAS WRITTEN THIS FILE FOR MONTHS while reading exactly like
   THE SUBJECT NEVER MADE THE REQUEST. Those two take opposite work: one is a missing capture and one is a
   finding about the engine. That is §A-FIELD-A-CONSUMER-DEFAULTS with the artifact in the field's place — a
   reader with no writer — and §AND-A-`NEVER`-OVER-A-LOG-CORPUS, whose population was chosen by a redirect.

   AND THE SENTENCE ABOVE IS THAT SAME RULE VIOLATED BY ITS OWN AUTHOR ONE COMMIT LATER, WHICH IS WHY THE
   CORRECTION LIVES HERE RATHER THAN IN A REWRITE OF IT. `/api/ answered 0` is TRUE OF THE FILE THAT WAS READ
   and was published as if it were a fact about the WITNESS. A SIBLING tracked log — `testing/fixtures.log`,
   886 lines, also written by nothing in this repository, also a one-off stdout redirect, from a WINDOWS machine
   two months LATER — holds 55 hits each of `/api/h/1` through `/api/h/6` and `/api/h/done`. The witness had
   fired, repeatedly, and the population that said otherwise was chosen by which of two stale files happened to
   be greped. That is the recency-and-selection defect the paragraph above NAMES, committed in the act of
   naming it, and the direction is the expensive one: an absence read off one file argues for building a
   mechanism that already worked.
   SO THE SIBLING IS KEPT, DELIBERATELY, AND IS NOT A SECOND INSTANCE TO TIDY AWAY. It is the only surviving
   evidence that this channel has ever fired, it is cited by nothing, and the machine that produced it is gone —
   so deleting it would destroy an unrecoverable record to remove a hazard that a sentence closes. Read it as
   what it is: ONE run, one afternoon, one machine, and never as a statement about this checkout. The durable
   repair is the per-run stamped file below, which cannot be mistaken for either.

   SO THE SERVER WRITES IT, THE PATH IS PORT-SCOPED, AND THE FILE IS TRUNCATED AND STAMPED AT STARTUP.
   Port-scoped because two servers on two ports are two subjects and a shared file interleaves them into one
   that answers about neither. TRUNCATED because the question a reader asks of this file is always `did it fire
   in THIS run`, and an accumulating file answers it out of a previous one — the recency bias that rule is
   about, arriving through the instrument instead of through the reader. STAMPED because an EMPTY file and an
   ABSENT one are different facts: the header line means `the server ran and nothing asked it for anything`,
   which is separable from `no server ever ran`, and averaging those two is what produced the incident above.

   THE WRITE IS SYNCHRONOUS AND ITS FAILURE IS NOT CAUGHT. A served request whose record is lost is exactly the
   unarmed witness this exists to end, so a channel that cannot record must not quietly go on serving; the fd is
   opened BEFORE the bind so a bad path is a configuration error at startup rather than a 500 mid-run. */
const ACCESS_LOG = path.resolve(__dirname, `fixtures_access.${PORT}.log`);
const ACCESS_FD = fs.openSync(ACCESS_LOG, "w");
fs.writeSync(ACCESS_FD,
  `# fixtures_server access log — port ${PORT}, pid ${process.pid}, opened ${new Date().toISOString()}\n` +
  `# ONE RUN. This file is truncated at startup, so an absence here is an absence in THIS run and in no other.\n` +
  `# A file holding only these two lines means the server ran and NOTHING requested anything of it.\n`);

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

/* A BODY THE SERVER NEVER ENDS — WPT'S OWN `infinite-slow-response`, BY ITS OWN NAME AND IN ITS OWN SHAPE.
 *
 * WHY A ROUTE OF THIS SERVER'S AND NOT A PIPE. CLAUDE.md: never coin a system when an established one exists,
 * and the established one here is a HANDLER rather than a pipe. wptserve's `trickle` pipe is the nearest thing
 * and cannot express this: its own docstring is "Send the response in parts, with time delays" and every
 * spelling of it sends "the remainder of the file", so it always ENDS. What WPT uses for a body with no end is
 * a handler, and the handler is in this checkout —
 * `engine/.work/wpt/fetch/api/resources/infinite-slow-response.py` — whose whole body is:
 *   response.headers.set(b"Content-type", b"text/plain"); response.write_status_headers()
 *   response.writer.write(b"." * 2048)                 # "Writing an initial 2k so browsers realise it's there"
 *   while True: if not response.writer.write(b"."): break; ...; time.sleep(0.01)
 * The name, the content type, the 2048-byte opening chunk and the 10ms cadence below are that file's, so a
 * reader who knows the corpus needs to learn nothing here. Its `stateKey`/`abortKey` stash parameters are NOT
 * built: they exist so a WPT test can ask the server whether the connection is still open, and the oracle for
 * the fixtures that use this route is this server's own ACCESS LOG, which answers that question already.
 *
 * NO DEADLINE, NO BYTE CAP AND NO CONNECTION LIMIT, WHICH IS THE WHOLE POINT RATHER THAN AN OVERSIGHT. The
 * subject under test is a reply whose arrival OUTLIVES a service round, and a route that quietly ended after N
 * bytes or N seconds would test a SLOW reply — a different population, and the one every arm of the mechanism
 * already handles. CLAUDE.md §NO BOUNDS is the same rule one zone over.
 *
 * IT IS STILL SHUT DOWN, AND THAT IS NOT A BOUND: the writers are held in a set so this SHARED process can
 * still exit for the lanes using it, and a client that goes away closes its own socket. Neither ends a body for
 * a client that is still reading, which is the only thing the bound would have done. */
const HOLD_PATH = "/infinite-slow-response";
const _holding = new Set();
function holdOpen(req, res) {
  /* THE HEAD IS COMPLETE AND THE BODY IS NOT, which is what makes this route the subject: `safeFetch` has its
     status, its header map and its landed URL the instant `fetch` resolves, so every gate in that file has
     already run and the only thing outstanding is `_readBody`'s reader loop. No `content-length`, so Node
     frames this chunked (RFC 9112 §7.1), which is what a real streaming endpoint does. */
  res.writeHead(200, { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" });
  res.write(".".repeat(2048));
  const t = setInterval(() => { res.write("."); }, 10);
  const stop = () => { clearInterval(t); _holding.delete(stop); };
  _holding.add(stop);
  res.on("close", stop);
  res.on("error", stop);
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
  const accessLine = `[${new Date().toISOString()}] ${req.method} ${reqUrlFull}` +
                     `  referer=${req.headers.referer || "-"}` +
                     `  dest=${req.headers["sec-fetch-dest"] || "-"}`;
  console.log(accessLine);
  fs.writeSync(ACCESS_FD, accessLine + "\n");
  const url = pick(reqUrlFull.split("?")[0].split("#")[0]);
  const qs = reqUrlFull.split("#")[0].split("?")[1] || "";
  const pipe = pipeStatus(qs);
  if (pipe.refuse) {
    refuse501(res, "unimplemented pipe " + JSON.stringify(pipe.refuse) +
                   " — this server implements wptserve's status(NNN) pipe only, and refuses rather than " +
                   "serving 200 for a request that asked for something else");
    return;
  }
  /* THE HOLD ROUTE IS ANSWERED BEFORE THE DISK IS CONSULTED, because it names no file — reaching the lookup
     below would answer it 404, and a 404 for this route reads as the SUBJECT not making the request rather than
     as a route that was never provisioned, which is the reading the default route above already had to state
     out loud. It is after the pipe check so a `?pipe=` on it is still refused rather than ignored. */
  if (url === HOLD_PATH) { holdOpen(req, res); return; }
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
  fs.writeFileSync(LOCK_PORT, JSON.stringify(lock, null, 2), "utf8");
  const wellKnownHeldBy = lockHolderIfLive(LOCK);
  if (!wellKnownHeldBy) fs.writeFileSync(LOCK, JSON.stringify(lock, null, 2), "utf8");
  console.log(`fixtures server listening on http://127.0.0.1:${PORT}/`);
  console.log(DEFAULT_ROUTE_PRESENT
    ? `  GET / -> ${DEFAULT_ROUTE}`
    : `  GET / -> UNMAPPED: ${DEFAULT_ROUTE} is not in this checkout, so / answers 404`);
  console.log(`  GET ${HOLD_PATH} -> a body with NO END (wpt fetch/api/resources/infinite-slow-response.py)`);
  /* THE BANNER SAYS WHICH LOCKS THIS SERVER OWNS, NEVER WHICH LOCKS EXIST. Printing the well-known path
     unconditionally put a line reading `lock: <path>` underneath a line saying another server held it, so one
     banner contradicted itself and the wrong half was the one that looked like a fact. */
  if (wellKnownHeldBy) {
    console.log(`  lock: ${LOCK} is held by a LIVE server (pid ${wellKnownHeldBy.pid}, port ` +
                `${wellKnownHeldBy.port}) — NOT claimed here. A reader with no port in hand is sent THERE, ` +
                `not here; name this server by ${LOCK_PORT}.`);
  } else {
    console.log(`  lock: ${LOCK}  (well-known discovery entry, claimed by this server)`);
  }
  console.log(`  lock: ${LOCK_PORT}  (this server's own record)`);
  console.log(`  access log: ${ACCESS_LOG}  (truncated at this startup; two lines and no more means nothing was requested)`);
});

/* RELEASE IS GATED ON OWNERSHIP, NOT CHAINED AFTER A READ. The per-port record is ours by construction and
   goes unconditionally; the well-known entry goes only if it still names THIS pid, so a server that correctly
   declined to claim it cannot take another server's on the way out. */
function releaseLocks() {
  try { fs.unlinkSync(LOCK_PORT); } catch {}
  let held;
  try { held = JSON.parse(fs.readFileSync(LOCK, "utf8")); } catch { return; }
  if (held && held.pid === process.pid) { try { fs.unlinkSync(LOCK); } catch {} }
}
process.on("SIGTERM", () => { releaseLocks(); for (const f of [..._holding]) f();
                                 srv.close(() => process.exit(0)); });
process.on("SIGINT",  () => { releaseLocks(); for (const f of [..._holding]) f();
                                srv.close(() => process.exit(0)); });
