// testing/req2proto_thirdparty.cjs — WHAT A TARGET'S OWN ERROR REPLY DOES TO THE ERROR PROBE.
//
// The subject is lib/req2proto.js's `parseJsonErrors` and the two probes above it, driven in a REAL Chrome
// DOM with the REAL extension scripts, in ast-worker.html's own load order (check.js, lib/field-def.js,
// lib/protobuf.js, lib/store-record.js, lib/req2proto.js). Nothing here is a mock of the code under test —
// only the page-context relay is stubbed, because the whole question is what the parser does with bytes the
// relay HANDED IT, and those bytes are a stranger's `google.rpc.Status`.
//
// Every case is a WELL-FORMED rejection carrying ONE malformed value plus a violation the same reply
// legitimately describes, so each row answers two questions at once: does the parser survive the malformed
// value, and does the good field beside it reach the operator.
//
//   node testing/req2proto_thirdparty.cjs <dir-containing-extension/>
"use strict";
const http = require("http");
const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");

const ROOT = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const EXT = path.join(ROOT, "extension");
if (!fs.existsSync(path.join(EXT, "lib", "req2proto.js"))) {
  console.error("no extension/lib/req2proto.js under " + ROOT);
  process.exit(1);
}

const MIME = { ".js": "text/javascript", ".html": "text/html", ".json": "application/json" };

// ─── The corpus. One row per shape a third party can put in a google.rpc.Status. ──────────────────────────

const GOOD_VIOLATION = { field: "browseId", description: "Invalid value at 'browse_id' (TYPE_STRING), 2" };
const badRequest = (violations) => ({ "@type": "type.googleapis.com/google.rpc.BadRequest", fieldViolations: violations });
const status = (error) => ({ error: Object.assign({ code: 400, message: "Invalid JSON payload received." }, error) });

const CASES = [
  ["baseline: nothing malformed",
   status({ details: [badRequest([GOOD_VIOLATION])] })],

  ["A  error.details is a record, not a list",
   status({ details: { "0": badRequest([GOOD_VIOLATION]) } })],

  ["B  a null sits in error.details",
   status({ details: [null, badRequest([GOOD_VIOLATION])] })],

  ["C  a detail's @type is a number",
   status({ details: [{ "@type": 7, fieldViolations: [] }, badRequest([GOOD_VIOLATION])] })],

  ["D  fieldViolations is a record, not a list",
   status({ details: [badRequest({ "0": GOOD_VIOLATION }), badRequest([GOOD_VIOLATION])] })],

  ["E  a null sits in fieldViolations",
   status({ details: [badRequest([null, GOOD_VIOLATION])] })],

  ["F  error.message is a number",
   status({ message: 400, details: [badRequest([GOOD_VIOLATION])] })],

  ["G  a violation's description is a number",
   status({ details: [badRequest([{ field: "x", description: 7 }, GOOD_VIOLATION])] })],

  ["H  a violation names __proto__ as the required field's parent",
   status({ details: [badRequest([{ field: "__proto__", description: "Missing required field pageToken at 'request'" },
                                  GOOD_VIOLATION])] })],

  ["H2 a violation names constructor as the required field's parent",
   status({ details: [badRequest([{ field: "constructor", description: "Missing required field pageToken at 'request'" },
                                  GOOD_VIOLATION])] })],

  ["I  a description names 'constructor' where a proto type goes",
   status({ details: [badRequest([{ field: "evil", description: "Invalid value at 'evil' (constructor), 1" },
                                  GOOD_VIOLATION])] })],

  ["J  a generic-pattern description reflects a blank value",
   status({ details: [badRequest([{ field: "gen", description: "invalid string value ' ' for field 'gen'" },
                                  GOOD_VIOLATION])] })],

  ["K  an ErrorInfo names a numeric service",
   status({ details: [{ "@type": "type.googleapis.com/google.rpc.ErrorInfo", metadata: { service: 7, method: "m.List" } },
                      badRequest([GOOD_VIOLATION])] })],

  ["L  the whole envelope is a JSPB array whose message is a number",
   [400, 500, [badRequest([GOOD_VIOLATION])]]],

  ["M  a violation's fieldViolations entry is a bare string",
   status({ details: [badRequest(["not a violation", GOOD_VIOLATION])] })],
];

(async () => {
  const srv = http.createServer((req, res) => {
    const p = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (p === "/") {
      res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      // ast-worker.html's own order for the four files this subject needs.
      res.end(`<!doctype html><meta charset=utf-8><title>req2proto third-party corpus</title>
<script src="/check.js"></script>
<script src="/lib/field-def.js"></script>
<script src="/lib/protobuf.js"></script>
<script src="/lib/store-record.js"></script>
<script src="/lib/req2proto.js"></script>`);
      return;
    }
    /* ANSWERED SO IT IS NOT A CONSOLE LINE. Chrome asks for this unprompted and a 404 logs a `console.error`
       that RACES the assertion messages this instrument is here to read — three runs of the unchanged
       instrument against the unchanged tree printed the two lines in two different orders. */
    if (p === "/favicon.ico") { res.writeHead(204); res.end(); return; }
    const f = path.join(EXT, p);
    if (!f.startsWith(EXT) || !fs.existsSync(f)) { res.writeHead(404); res.end("404"); return; }
    res.writeHead(200, { "content-type": MIME[path.extname(f)] || "text/plain" });
    res.end(fs.readFileSync(f));
  });
  await new Promise((r) => srv.listen(0, "127.0.0.1", r));
  const port = srv.address().port;

  const browser = await puppeteer.launch({ headless: "new", args: ["--no-sandbox"] });
  const page = await browser.newPage();
  const consoleErrs = [];
  page.on("console", (m) => { if (m.type() === "error") consoleErrs.push(m.text()); });
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: "load" });

  const loaded = await page.evaluate(() => ({
    parseJsonErrors: typeof parseJsonErrors,
    probeApiEndpoint: typeof probeApiEndpoint,
    discoverServiceInfo: typeof discoverServiceInfo,
    checkStoreRecord: typeof checkStoreRecord,
    fdDocString: typeof fdDocString,
  }));
  console.log("loaded: " + JSON.stringify(loaded));
  if (loaded.parseJsonErrors !== "function") { console.log("FATAL: subject not loaded"); process.exit(1); }

  const rows = await page.evaluate(async (cases) => {
    // The page-context relay's reply record, as lib/schema.js's `_checkPageFetchReply` proves it: a status, a
    // header bag, a text body, and the base64/null encoding vocabulary. One reply for every probe request, so
    // each case measures the parser and not a fetch sequence.
    const mkFetch = (bodyObj, st) => async () => ({
      status: st,
      headers: { "content-type": "application/json; charset=UTF-8" },
      body: JSON.stringify(bodyObj),
      bodyEncoding: null,
    });
    // `discoverServiceInfo` treats 400 and 404 as "this content type is not configured / no such endpoint"
    // and never looks at the body, so its arm is driven at 500 — the status at which it DOES parse.
    const SVC_STATUS = 500;

    const out = [];
    for (const [name, bodyObj] of cases) {
      const row = { name };

      // 1. The parser itself, on the exact bytes.
      try {
        const p = parseJsonErrors(JSON.parse(JSON.stringify(bodyObj)));
        row.parse = {
          fields: p.fields.map((f) => f.name + ":" + (typeof f.type === "function" ? "<FUNCTION " + f.type.name + ">" : String(f.type)) +
                                     "#" + String(f.number)),
          metaService: typeof p.metadata.service + " " + String(p.metadata.service),
        };
      } catch (e) { row.parse = { threw: e.constructor.name + ": " + e.message }; }

      // 2. What a live probe answers — probeApiEndpoint end to end through the relay stub.
      try {
        const r = await probeApiEndpoint("https://t.example/v1/x:list", {}, { fetchFn: mkFetch(bodyObj, 400), maxDepth: 0 });
        row.probe = {
          fieldCount: r.fieldCount,
          fields: Object.keys(r.fields),
          errors: r.probeDetails.filter((d) => d.error).map((d) => d.error),
        };
        // 3. Does the answer survive the store's own door?
        try {
          checkStoreRecord("probeResults", "https://t.example/v1/x:list", r, "testing/req2proto_thirdparty.cjs");
          row.probe.door = "ok";
        } catch (e) { row.probe.door = "ABORT " + e.message.slice(0, 120); }
        // 4. Is it something chrome.runtime.sendMessage / IndexedDB could carry?
        try { structuredClone(r); row.probe.clone = "ok"; }
        catch (e) { row.probe.clone = "THROW " + e.constructor.name + ": " + e.message.slice(0, 90); }
      } catch (e) { row.probe = { threw: e.constructor.name + ": " + e.message }; }

      // 5. The sibling probe on the same bytes.
      try {
        const s = await discoverServiceInfo("https://t.example/v1/x:list", {}, { fetchFn: mkFetch(bodyObj, SVC_STATUS) });
        row.svc = { service: typeof s.service + " " + String(s.service), method: String(s.method) };
        try {
          checkStoreRecord("probeResults", "svc:k", s, "testing/req2proto_thirdparty.cjs");
          row.svc.door = "ok";
        } catch (e) { row.svc.door = "ABORT " + e.message.slice(0, 120); }
      } catch (e) { row.svc = { threw: e.constructor.name + ": " + e.message }; }

      out.push(row);
    }
    return out;
  }, CASES);

  for (const r of rows) {
    console.log("\n── " + r.name);
    console.log("   parse : " + JSON.stringify(r.parse));
    console.log("   probe : " + JSON.stringify(r.probe));
    console.log("   svc   : " + JSON.stringify(r.svc));
  }
  /* THE CONSOLE LINES ARE check.js's OWN `fail()` WRITING THE DCHECK MESSAGE, and they are delivered to this
     process ASYNCHRONOUSLY — three back-to-back runs of the unchanged instrument against the unchanged
     `before` tree printed "1" twice and "2" once while every measured row was byte-identical, so the count
     was a race between Chrome's console event and `browser.close()`, not a fact about the subject. Drained
     before it is read, and the TEXTS are printed rather than a number: a count nobody can attribute is the
     thing this tree calls a plausible datum. */
  await new Promise((r) => setTimeout(r, 500));
  console.log("\nconsole.error lines: " + consoleErrs.length);
  for (const t of consoleErrs) console.log("   ! " + t.slice(0, 160));
  await browser.close();
  srv.close();
})().catch((e) => { console.error(e.stack || e); process.exit(1); });
