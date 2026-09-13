/* ONE REAL DOCUMENT, THROUGH THE PRODUCTION ABI, WITH ITS SCRIPTS INGESTED — and the scheduler's own census
 * published as a SERIES.
 *
 * WHAT IT IS FOR. §Testing: "a before/after belongs on FROZEN BYTES — a mirror, a fixture, a recorded payload
 * replayed — where the only thing that changed is the engine." This tree HAS such a mirror, with a manifest
 * tying every resource to (url, date, sha256), and a server that replays it faithfully. What it did not have
 * was a way to put those bytes through the engine and read `_wfq` and `_cold` off the result WITHOUT a
 * browser. This is that, and it is a measurement driver rather than a gate: it asserts its own host contract
 * and states no expectation whatever about what the engine finds.
 *
 * WHY NOT engine/solvergate.mjs, WHICH ALREADY DRIVES THIS ABI. Two reasons, and each one alone decides it.
 *   - Its ORACLE is set-equality of a document's FINDINGS across several schedules, and it drops `_wfq`,
 *     `_cold`, `_heap`, `_swap` and `_forkAt` from that comparison BY NAME, as "READINGS OF AN INSTANT rather
 *     than costs". The census is therefore the one thing that gate deliberately does not produce; asking it
 *     for one is asking it to be a second instrument.
 *   - Its reply policy MINTS one JSON body for every request it answers, which is the right answer for a data
 *     fetch and a wrong one for a request whose reply is COMPILED — its own refusal says so and names what is
 *     needed: "a document that needs its scripts INGESTED needs a host that has them". This is a host that
 *     has them, and that refusal now names this file.
 * Neither of those makes a THIRD driver of the raw entry free. engine/renderer_abi.mjs names the two that
 * `ccall` it with no transport and centralises the one fact that has gone wrong three times between them —
 * the operand list — so this file places through the same walk and can only go short by the same walk going
 * short. What it still duplicates is the SESSION: boot, init, the three step codes, the reply seam. That is a
 * named residual at the bottom of this banner.
 *
 * WHY IT RE-FETCHES OVER HTTP INSTEAD OF MAPPING A URL TO A FILE. The mirror's on-disk layout is
 * `<id>/<host>/<path>` with a query folded into a `__q<sha256[0:8]>` suffix, and that rule is stated in
 * exactly one place — testing/corpus/serve-faithful.mjs, whose own header records why the query may not be
 * dropped ("vuejs.org's banner code does `params = parse(document.currentScript.getAttribute('src'))`… 122 of
 * the 740 mirrored resources carry a query"). A path mapping here would be a second copy of that rule, and
 * the copy anyone writes first is the one that drops the query. So this driver knows nothing about the mirror:
 * it takes a URL, fetches the document from it, and answers every park by fetching that park's own URL from
 * the same origin. Point it at serve-faithful and it replays the mirror; point it at any other local origin
 * and it drives that. One serving rule, one statement of it, two consumers.
 *
 * WHY THE CENSUS IS A SERIES AND WHICH HALF OF IT IS QUOTED WHERE. solver/result.c states the kinds and they
 * are not alike. `_wfq` is a reading of an INSTANT: on a frontier that has drained it is `{members: 0}`, which
 * is the true value of that instant and says nothing about the run, so its useful readings are the PARTIALS
 * taken while the frontier is live — which is the cadence extension/bridge.js's `streamPartial` already runs
 * the shipped path at, and the reason it says "A PARTIAL'S CENSUS IS THE VALUABLE ONE". The larger half of
 * `_cold` is LIFETIME counts, so its terminal reading is the one to quote. Hence: EVERY instrument member
 * whole on every sample line, and every one of them whole again on the terminal one — a lifetime census read
 * at the last sample and a reading of an instant read at that same instant, from one document, so a reader
 * can align them. None of it is filtered to a chosen row list, for the stated reason that "a row added to the
 * census reaches the popup with no edit on this path", and a driver that named rows would be the hand-kept
 * copy.
 * THAT LAST SENTENCE WAS TRUE OF THE ROWS AND FALSE OF THE MEMBERS FOR AS LONG AS IT STOOD, which is why it
 * now says EVERY rather than naming two: the line beneath it published `_wfq` and `_cold` and ten scalars and
 * dropped the other nineteen members of the same document, `_forkAt` among them. See `censusOf` for what that
 * cost and for why the surviving set is a COMPLEMENT rather than a list. A reader who wants the terminal
 * reading of a lifetime census on a run the external budget ended takes the LAST sample line: §NO BOUNDS means
 * a real document's frontier does not drain, so the `final` line is not reached for exactly the documents this
 * driver exists for — which is the same trap `surface` below records paying for once.
 *
 * THERE IS NO STEP CAP, NO TIME CAP AND NO SAMPLE CAP IN THIS FILE. §NO BOUNDS. It is bounded from OUTSIDE by
 * the same rlimit and timeout every other driver here is run under, and because each sample is APPENDED to the
 * transcript as it is taken, a run the external budget ends has already published everything it reached. A
 * driver that stopped itself would be choosing which part of the frontier never got measured.
 * THAT SENTENCE WAS TRUE OF THE CENSUS AND FALSE OF THE PRODUCT'S OWN FINDINGS, WHICH IS THE ONE THING THIS
 * FILE GOT WRONG AND THE HARDEST KIND TO SEE: the rule was stated, correctly, and applied to one half of what
 * the driver reads. The endpoints, the sinks and the page errors were read ONCE, after the loop, from a
 * `qjs_result` the loop only reaches on ENGINE_STEP_DONE — and §NO BOUNDS means a real document's frontier does
 * not drain, so for exactly the documents this exists for the process was killed before that line. Measured on
 * a real bundle: 130 samples, SIGXCPU at the rlimit, and `fetchCallSites` occurring ZERO times in the whole
 * transcript. The surface is published per sample now; see `surface` for why a row is keyed by its own text
 * and for the cost.
 *
 * NAMED RESIDUAL — the sniff. `computedType` is what the ZONE THAT READ THE BYTES decided, and
 * extension/lib/safe-fetch.js is where this tree states MIME Sniffing §7 "Determining the computed MIME
 * type of a resource". This driver states its own decision instead, and that decision is the response's
 * own `Content-Type` essence with no sniffing. NOT COVERED: a resource whose bytes disagree with the type
 * its server stated — a browser sniffs, this takes the server at its word. WHAT THE NEXT DIFF BUILDS: one
 * statement of MIME Sniffing §7 a Node driver can call, so this seam and the extension read one rule. HOW
 * ITS ABSENCE WOULD SHOW: a run in which a resource the origin labels `text/plain` reaches the engine as data
 * where a browser would have compiled it, visible as a `_cold.replyAnswered` that advanced with no program
 * start behind it. It is narrower rather than wrong for a MIRROR replay, because serve-faithful answers the
 * content type the capture RECORDED, which is what the real server said on the day the bytes were frozen.
 *
 * NAMED RESIDUAL — the session. Boot, the print sink, the operand placement, the three step codes and the
 * reply seam are spelled here and in engine/solvergate.mjs. NOT COVERED: a contract that moves on that seam
 * has to be repaired in both, which is the defect that already cost this project once — solvergate's own
 * comment records `computedType` "stopped engine/route.mjs, left behind in the one driver whose whole subject
 * is documents that consume replies". WHAT THE NEXT DIFF BUILDS: the session as a module both import, the way
 * both already import `abiOperands`. HOW ITS ABSENCE WOULD SHOW: a field added to the reply record makes one
 * driver abort at its first park while the other keeps running.
 *
 * Usage:  node engine/pagecensus.mjs <document-url> [transcript.jsonl]
 *   e.g.  node testing/corpus/serve-faithful.mjs gitlab 8977 &
 *         `timeout 400 sh -c "ulimit -S -t 300; node engine/pagecensus.mjs http://127.0.0.1:8977/ /tmp/gl.jsonl"`
 */
import { appendFileSync, existsSync } from "node:fs";
import { GLUE_PATH as WASM, abiOperands } from "./renderer_abi.mjs";

/* THE PRODUCTION CADENCE, NOT ONE OF THIS FILE'S OWN. extension/bridge.js composes a partial every
   `PARTIAL_MS` on every real page, so sampling at any other rate would measure a run production never has —
   §Testing's "an instrument that costs enough to shorten the run is measuring a different run", where the
   instrument is one the product already pays for. */
const SAMPLE_MS = 750;

/* NOT AN `@` STREAM MARKER, AND THAT IS THE WHOLE POINT OF THE NAME CHANGE. Every other `@` token this
   project prints — @H, @S, @WFQ, @COLD, @SWAP, @HWORK, @QUANTUM, @E, @WHY — is PARSED by something: the
   build's readers, the extension, a census consumer. The prefix is a promise that a machine reads the line.
   `@CENSUSFAIL` made that promise and nothing anywhere matched it, which the record-field audit reports as a
   written marker with no reader — correctly, and permanently, because there was no consumer to build.
   THE FAILURE IS ALREADY MACHINE-READABLE AND IT IS THE EXIT CODE. This driver is invoked by hand (its two
   mentions in the tree are prose), its whole configuration is one argv, and it exits 1 beside this line — so
   a caller that wants to know THAT it failed has the answer, and no caller exists that wants to know WHY in a
   form it could act on. Building a reader for a hypothetical one is the follow-up a comment may not stand in
   for, in reverse: inventing the consumer to justify the marker.
   SO THE PREFIX GOES AND THE REASON STAYS HERE, because a reader who re-derives "our diagnostics wear @" will
   put it back and send the next auditor hunting a consumer that was never intended. If this driver ever gains
   a programmatic caller, the marker comes back WITH that caller's parser in the same diff. */
function fail(msg) { console.log("pagecensus: " + msg); process.exit(1); }

const docUrl = process.argv[2];
const transcript = process.argv[3] || null;
if (!docUrl) fail("usage: node engine/pagecensus.mjs <document-url> [transcript.jsonl] — this driver answers " +
                  "every park by fetching that park's own URL from the origin the document came from, so the " +
                  "URL is the whole of its configuration and there is nothing to default it to");
if (!existsSync(WASM))
  fail("the shipped engine is not built — this driver loads the artifact the extension loads (" + WASM + ")");

const emit = (o) => {
  const line = JSON.stringify(o);
  console.log(line);
  /* APPENDED PER SAMPLE RATHER THAN WRITTEN AT THE END, because there is no bound in this file and the run is
     ended from outside: a transcript composed after the loop would be empty for exactly the long runs it
     exists for. */
  if (transcript) appendFileSync(transcript, line + "\n");
};

/* THE REPLY RECORD extension/bridge.js DECLARES, as THIS zone read it — and the two not-a-reply answers
   kept apart, because extension/bridge.js keeps them apart and the engine branches on the difference. A
   response that arrived is a reply WHATEVER its status: a 404 is a real answer and the page's own error path
   is a path worth exploring. A fetch that THREW never reached a server, and Fetch §5.6's network error is what
   that is; it crosses as the JSON `null` with no bytes beside it, which is the spelling bridge.js uses. */
async function readReply(u) {
  let res;
  try { res = await fetch(u, { redirect: "follow" }); }
  catch (e) { return { meta: null, bytes: null, note: "network-error: " + String(e && e.message || e) }; }
  const buf = new Uint8Array(await res.arrayBuffer());
  const headers = [...res.headers.entries()];
  /* MIME Sniffing §4.2 "MIME type miscellaneous"'s ESSENCE — the type and subtype with the parameters
     dropped, which is the term extension/lib/safe-fetch.js states for this same field. See the sniff
     residual in the banner: this is what this zone DECIDED, stated by the zone that read the bytes, and it is
     narrower than extension/lib/safe-fetch.js's §7. `""` is a positive answer (the response stated no type),
     never a producer that stopped writing the field. */
  const ct = res.headers.get("content-type");
  const computedType = ct === null ? "" : ct.split(";")[0].trim().toLowerCase();
  return { meta: { status: res.status, statusText: res.statusText, headers,
                   urlList: [res.url || u], computedType },
           bytes: buf, note: null };
}

const factory = await import(WASM);
const boot = factory.default ?? factory;

/* THE DOCUMENT, FETCHED THE WAY EVERY OTHER RESOURCE IS. Its FINAL url is what the engine is told, because
   HTML §7.4 "Navigation and session history" makes the document's address the one the response came back
   at, and every park this driver answers is resolved against it. A document that did not arrive is fatal
   HERE rather than at the parser: an engine handed a 404 body compiles it. testing/corpus/serve-faithful.mjs
   records what that costs, in its own words and not a standard's — `a fixture that answers 200-with-prose
   where it means 404 manufactures engine bugs` — having watched the abort it raises rank as a corpus-wide
   engine defect across five sites. */
let docRes;
try { docRes = await fetch(docUrl, { redirect: "follow" }); }
catch (e) { fail(`the document at ${docUrl} could not be fetched (${String(e && e.message || e)}) — this ` +
                 "driver has no bytes of its own and nothing to measure"); }
if (!docRes.ok)
  fail(`the document at ${docUrl} answered ${docRes.status} ${docRes.statusText} — an error body handed to ` +
       "the engine is compiled like any other document and the abort it raises names the PAGE, so it is " +
       "refused here instead of measured");
const docBytes = new Uint8Array(await docRes.arrayBuffer());
const finalUrl = docRes.url || docUrl;
const docId = new URL(finalUrl).pathname.split("/").filter(Boolean).pop() || "index";

/* THE PRINT SINK, TEED — engine/solvergate.mjs's, for its reason: `qjs_emit_partial` does not RETURN its
   document, it prints one `@RESULT` line, and extension/bridge.js reads the product's findings off that same
   channel. Everything is forwarded unchanged so an abort's `@WHY` reaches this transcript. */
const sink = [];
const M = await boot({ print: (s) => { console.log(s); if (String(s).startsWith("@RESULT ")) sink.push(String(s)); } });
const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
const str = (f, ...a) => String(M.ccall(f, "string", a.map(() => "number"), a.map(cs)) ?? "");

/* THE DOCUMENT'S BYTES ARE RETAINED BY THE ENTRY AND ARE NOT FREED — `extension/mojom.js` declares `document`
   as the one parameter of this interface whose bytes the engine keeps, and core/loader/html_document.c reads
   through that block across every fill of the parse. solvergate paid for this once by freeing it. */
const hp = M._malloc(docBytes.length + 1);
M.HEAPU8.set(docBytes, hp);
M.HEAPU8[hp + docBytes.length] = 0;

/* EVERY FACT A DOCUMENT ARRIVAL CARRIES, BY `content.mojom.Renderer.Init`'S OWN PARAMETER NAMES. The values
   are the ones engine/solvergate.mjs states for a document nothing embeds, and each is a POSITIVE claim
   rather than a silence: HTML §7.1.7's inherited policy container is absent (no creator), §7.1.4's embedder
   policy is that container's own "new embedder policy", §7.3.1.3's parent and Permissions Policy §9.5's
   container are both absent, §3.1.3's ancestor origins list is EMPTY and §7.1.5's creation sandboxing flag
   set is EMPTY — `none` being this grammar's word for each, because the engine refuses an empty field there
   and reading silence as the empty list is what tells a framed document it is the top of its own tree. */
const operands = abiOperands("Init", "qjs_init", {
  document: [hp, docBytes.length], url: finalUrl, docId, headers: "", topLevelUrl: finalUrl,
  inheritedCsp: "", inheritedCspSelfOrigin: "",
  inheritedCoep: "unsafe-none", inheritedCoepEndpoint: "",
  inheritedCoepReportOnly: "unsafe-none", inheritedCoepReportOnlyEndpoint: "",
  parentNavigable: "u", containerPolicy: "null", ancestorOrigins: "none", creationSandboxFlags: "none",
}, cs);
M.ccall("qjs_init", "number", operands.map(() => "number"), operands);
M.ccall("qjs_begin", "void", ["number"], [cs("")]);

emit({ at: "start", url: finalUrl, docBytes: docBytes.length, sampleMs: SAMPLE_MS, artifact: WASM });

/* ONE CENSUS, OFF THE CHANNEL THE PRODUCT READS. `qjs_emit_partial` prints exactly one `@RESULT` line per
   call — main.c is the engine's only writer of that token — so a count other than one is this channel
   carrying something nobody declared, and it is checked rather than papered over with a last-line scan. */
function census() {
  sink.length = 0;
  M.ccall("qjs_emit_partial", "void", [], []);
  if (sink.length !== 1)
    fail(`qjs_emit_partial printed ${sink.length} \`@RESULT\` lines and it prints exactly one per call — ` +
         "extension/bridge.js reads the product's findings off this same channel, so any other count is the " +
         "print sink between the engine and its host dropping a document or carrying a second writer");
  const line = sink[0];
  sink.length = 0;
  try { return JSON.parse(line.slice("@RESULT ".length)); }
  catch (err) { fail("the `@RESULT` line is not JSON — the incremental merge does one JSON.parse of exactly " +
                     "this text: " + String(err).slice(0, 160)); }
}

/* THE SURFACE ITSELF, PUBLISHED PER SAMPLE — WHICH IS WHAT THIS FILE ALREADY SAYS IT DOES AND DID NOT.
 * `emit`'s own banner states the rule: a transcript composed after the loop "would be empty for exactly the
 * long runs it exists for", because there is no bound here and the run is ended from OUTSIDE. That was true of
 * the census series and FALSE of the product's own findings, which were read once, after the loop, from a
 * `qjs_result` the loop only reaches on ENGINE_STEP_DONE. §NO BOUNDS means a real document's frontier does not
 * drain, so the endpoints, the sinks and the page errors of every document worth pointing this at were
 * serialized into a variable the process was killed before reading.
 * MEASURED: a real 4.5 MB bundle, 130 samples, SIGXCPU at the rlimit — `fetchCallSites` occurs ZERO times in
 * the whole transcript, and the run's entire API surface was unobservable no matter how long anyone ran it.
 * THE DOCUMENT WAS ALREADY IN HAND, WHICH IS THE PART WORTH KEEPING. `census()` calls `qjs_emit_partial` and
 * parses the WHOLE result document every sample — the same channel extension/bridge.js reads the product's
 * findings off — and then named `_wfq`, `_cold` and ten cost columns out of it. So the surface was not missing,
 * it was projected away: the hand-picked-subset defect, in a driver, over the one part of the document the
 * driver exists to be pointed at. No new ABI call, no engine change, and no extra parse.
 * A ROW IS PUBLISHED WHEN ITS TEXT IS NEW TO THIS TRANSCRIPT, which makes no identity claim of its own. The
 * engine dedups its own array and MERGES into a row later (endpoint.c: "first body wins, never overwritten"),
 * so an index-keyed delta would miss an update and a key composed here would be a second opinion about what
 * an endpoint IS. Comparing the serialized row catches both cases and asserts nothing: a second row carrying
 * an address already seen is the engine having merged into it, never a second endpoint.
 * THE COST IS STATED RATHER THAN LEFT TO BE MEASURED: one `JSON.stringify` per row per sample, against a
 * `JSON.parse` of the whole document that this sample already performs. The set grows with the surface, which
 * is the quantity being measured, and it bounds nothing. */
const SURFACES = ["fetchCallSites", "securitySinks", "pageErrors"];
const seenRow = new Map(SURFACES.map((k) => [k, new Set()]));
function surface(d, n) {
  for (const k of SURFACES) {
    /* NOT DEFAULTED. extension/bridge.js DCHECKs that the engine's result document carries these arrays, and a
       `|| []` here would turn "the producer stopped naming this" into "this run found nothing" — the same
       absent-versus-zero the rest of this driver refuses. */
    if (!Array.isArray(d[k]))
      fail(`the engine's result document carries no \`${k}\` array — extension/bridge.js asserts the same ` +
           "shape on the same channel, so this is the producer having stopped naming it and NOT a run that " +
           "found nothing; those are different facts and only one of them is a measurement");
    const seen = seenRow.get(k);
    for (const row of d[k]) {
      const text = JSON.stringify(row);
      if (seen.has(text)) continue;
      seen.add(text);
      emit({ n, at: k, row });
    }
  }
}

/* EVERYTHING ELSE THE DOCUMENT CARRIES — THE COMPLEMENT OF `SURFACES`, COMPUTED RATHER THAN WRITTEN DOWN.
 * §Testing: an answered-statement count is quoted beside the work counters or it is not quoted, because a run
 * whose `workDone` is near one did not run. The same rule is what makes a census line readable: `_wfq` at
 * `{members: 0}` on a run with no jobs is a frontier that never existed, and on a run with tens of thousands
 * it is a frontier that drained. That is why the columns are published; this is why they are DERIVED.
 *
 * WHAT STOOD HERE WAS A HAND-KEPT LIST, IN THE ONE FILE WHOSE OWN BANNER FORBIDS ONE. That banner says of the
 * two censuses it published that "neither is filtered to a chosen row list — they cross whole in bridge.js for
 * the stated reason that `a row added to the census reaches the popup with no edit on this path`, and a driver
 * that named rows would be the hand-kept copy" — and then named two objects and ten scalars out of a document
 * carrying thirty-four members. The rule was stated and applied ONE LEVEL DOWN: the ROWS inside `_wfq` and
 * `_cold` crossed whole while the MEMBERS beside them were chosen by hand, so the projection the banner warns
 * about is the one the file was performing. It is the same defect `surface` above records having found over
 * the findings arrays — "the surface was not missing, it was projected away" — at the level above the one that
 * was repaired, which is why repairing that one did not reach it.
 *
 * MEASURED, AND IT COST A LANE ITS WHOLE QUESTION: nineteen of the document's twenty-nine instrument members
 * reached no transcript, `_forkAt` among them. That member is the census of WHICH SOURCE READ GREW THE
 * FRONTIER, keyed by source identity — the first question a run whose frontier explodes asks, and the one
 * decide.h says had "never once been answerable off a production run" until it started riding this document.
 * A lane pointed at a bombing multi-megabyte bundle to ask that found the answer already parsed, in hand, and
 * dropped at the emit below on every sample. `_worldSegmentsForked`, `_heap`, `_swap`, `_absent` and `_park`
 * went the same way, and each is a question somebody will arrive with.
 *
 * THE TWO NAMESPACES ARE THE ENGINE'S OWN AND ARE READ OFF THE ARTIFACT RATHER THAN RESTATED: result.c spells
 * every instrument member with a leading `_` and no finding with one. This does not KEY on that convention —
 * it takes the complement of `SURFACES`, which `surface` already asserts the shape of — so the prefix is
 * corroboration a reader can check and not a second copy of a rule this file does not own. A member added to
 * result.c now reaches this transcript with no edit here, which is what the banner already promised.
 *
 * IT IS NESTED RATHER THAN SPREAD, AND THE ENGINE'S SPELLING SURVIVES. Nesting means no document key can
 * collide with the frame keys `emit` composes around it, so nothing here rests on the two namespaces staying
 * disjoint. Preserving the spelling means one grep for `_forkAt` answers over result.c AND over this
 * transcript; the line this replaces renamed `_wfq` to `wfq`, which made the producer and its own measurement
 * un-greppable together and is the rename layer every other projection in this file refuses. */
const censusOf = (d) => Object.fromEntries(Object.entries(d).filter(([k]) => !SURFACES.includes(k)));

let n = 0, lastSample = 0, filledTotal = 0, quantum = null;
const t0 = Date.now();
for (;;) {
  const r = M.ccall("qjs_step", "number", [], []);
  if (r === 0) break;                 /* ENGINE_STEP_DONE — the frontier is empty (or was written out) */
  if (r !== 2 && r !== 3)
    fail(`qjs_step answered ${r}, which is none of DONE(0)/YIELD(2)/STALLED(3) — the ABI carries three codes ` +
         "and this driver branches on all three, so a fourth is a contract that moved under it");

  const now = Date.now();
  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    const d = census();
    if (!quantum) { quantum = d._quantum; emit({ at: "quantum", quantum }); }
    emit({ n: ++n, atMs: now - t0, step: r === 3 ? "stalled" : "yield", census: censusOf(d) });
    /* AFTER the census line and from the SAME document, so a reader can align a row with the frontier state
       that produced it. A run the budget ends has now published everything it reached, which is what `emit`'s
       banner already promised for the series. */
    surface(d, n);
  }

  /* THE PARKS, ANSWERED FROM THE ORIGIN THE DOCUMENT CAME FROM. The six fields are checked for their
     VOCABULARY and not defaulted — solver/engine.h declares them, and a producer that drifts would otherwise
     reach this driver as a token in the address slot. The DESTINATION is read and NOT acted on, which is the
     whole difference between this driver and solvergate's: that one refuses a script-like destination because
     it has no bytes for it, and this one has the bytes. */
  const pending = str("qjs_pending").split("\n").filter(Boolean);
  let filled = 0;
  for (const line of pending) {
    const t = line.split("\t");
    if (t.length !== 7 || t.some((x, i) => i !== 1 && x === ""))
      fail("a pending line is not `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>" +
           "CREDENTIALS<TAB>URL` — qjs_pending joins the seven and the reply is delivered against the " +
           "(method, url) pair, so a " +
           "short line makes a token the address. The empty DESTINATION is Fetch §2.2.5's own default and is " +
           "the one field that may be empty");
    const [method, , initiator, provenance, , , u] = t;
    if (initiator !== "parser" && initiator !== "script")
      fail(`a pending line states the initiator \`${initiator}\`, which is neither token solver/engine.h ` +
           "declares — the vocabulary moved under every host that reads it");
    if (provenance !== "observed" && provenance !== "derived" && provenance !== "forced")
      fail(`a pending line states the provenance \`${provenance}\`, which is none of the three tokens ` +
           "solver/engine.h declares — the vocabulary moved under every host that reads it");

    const abs = new URL(u, finalUrl).href;
    const rep = await readReply(abs);
    /* BOTH CHANNELS IN ONE CALL, so no path here can deliver the record and forget the bytes. A network error
       crosses as the JSON `null` with no body at all — extension/bridge.js's own spelling of
       Fetch §5.6 "Fetch methods"' network error, which is what makes the flow keep its park AND fork the
       page's `catch` arm. */
    if (rep.meta === null) {
      emit({ n, at: "reply", method, url: abs, networkError: rep.note });
      M.ccall("qjs_provide", "void", ["number", "number", "number", "number", "number"],
              [cs(method), cs(abs), cs("null"), 0, 0]);
    } else {
      const b = rep.bytes;
      const p = M._malloc(b.length + 1);
      M.HEAPU8.set(b, p);
      try {
        M.ccall("qjs_provide", "void", ["number", "number", "number", "number", "number"],
                [cs(method), cs(abs), cs(JSON.stringify(rep.meta)), p, b.length]);
      } finally { M._free(p); }
      emit({ n, at: "reply", method, url: abs, status: rep.meta.status,
             type: rep.meta.computedType, bytes: b.length });
    }
    filled++; filledTotal++;
  }

  /* A STALL THIS ROUND DID NOT MOVE IS A FRONTIER PARKED ON SOMETHING THIS DRIVER CANNOT SUPPLY, and it is
     reported with the owed list rather than spun on: this host answers replies and nothing else, so the
     register it is stuck on names the capability the document wants. It is NOT a bound — nothing is dropped
     and nothing is decided; the run ends because no further step can change anything. */
  if (r === 3 && filled === 0) {
    const owed = str("qjs_pending").split("\n").filter(Boolean);
    const notices = str("qjs_host_notices").split("\n").filter(Boolean);
    const asks = str("qjs_host_requests").split("\n").filter(Boolean);
    emit({ at: "stalled", note: "the frontier reports it cannot progress and this driver filled nothing — " +
           "every member is parked on something only a host can supply and the only thing this one supplies " +
           "is a reply", owed, notices, asks });
    break;
  }
}

/* THE TERMINAL READING. `_cold`'s larger half is LIFETIME counts, so this is the one to quote for it; `_wfq`
   here is the reading of the instant the frontier reached, which on a drained frontier is `{members: 0}` and
   is the true value of that instant rather than a summary of the run — the series above is what says how it
   got here. solver/result.c emits `{"members":0}` with NO term rows when the census is taken with nothing
   standing, so an absent row here is correct and is not a field that went missing. */
const json = str("qjs_result");
if (!json) fail("qjs_result answered nothing — the result document did not serialize");
const out = JSON.parse(json);
/* THE SURFACE ONE LAST TIME, FROM THE TERMINAL DOCUMENT, AND IT IS NOT THE ONLY COPY ANY MORE — the rows the
   loop already published stay published whether or not this line is ever reached, which is the whole change.
   A row that appears ONLY here is one the engine merged into after the last sample. */
surface(out, n + 1);
/* AND THE COUNTS, WHICH ARE A SUMMARY OF ROWS THE TRANSCRIPT NOW CARRIES RATHER THAN A SUBSTITUTE FOR THEM.
   They used to be the only statement this driver made about the product's own output, and a count cannot say
   which method, which address, which parameter or which example value — which is the question this driver is
   pointed at a real bundle to answer. `surface` asserts the arrays are named, so no `|| []` stands here: that
   default turned "the producer stopped naming this" into "this run found nothing". */
emit({ at: "final", samples: n, repliesFilled: filledTotal, elapsedMs: Date.now() - t0,
       census: censusOf(out),
       fetchCallSites: out.fetchCallSites.length,
       securitySinks: out.securitySinks.length,
       pageErrors: out.pageErrors.length });

/* THE TEARDOWN IS NOT BOOKKEEPING — main.c's runs JS_RunGC and JS_FreeRuntime, whose gc_obj_list walk aborts
   on a leaked GC object, so every run of this driver is also a leak check for the solver's own allocations
   against a document nobody designed. */
M.ccall("qjs_teardown", "void", [], []);
emit({ at: "teardown", ok: true });
