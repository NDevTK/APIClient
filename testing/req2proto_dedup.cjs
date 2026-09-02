// testing/req2proto_dedup.cjs — WHAT `probeApiEndpoint` DOES WHEN TWO PROBES DESCRIBE THE SAME FIELD.
//
// The subject is the dedup-and-merge block in lib/req2proto.js `probeApiEndpoint`, driven in a REAL Chrome
// DOM with the REAL extension scripts in ast-worker.html's load order. The relay this lane was handed says
// two violations sharing a wire tag MERGE and that the merge fails to upgrade `name`; this instrument asks
// the code instead, by answering the str probe and the int probe with DIFFERENT descriptions of the same tag
// and printing what came out.
//
//   node testing/req2proto_dedup.cjs <dir-containing-extension/>
"use strict";
const http = require("http");
const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");

const ROOT = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const EXT = path.join(ROOT, "extension");
const MIME = { ".js": "text/javascript", ".html": "text/html", ".json": "application/json" };

(async () => {
  const srv = http.createServer((req, res) => {
    const p = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (p === "/") {
      res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      res.end(`<!doctype html><meta charset=utf-8><title>req2proto dedup</title>
<script src="/check.js"></script>
<script src="/lib/field-def.js"></script>
<script src="/lib/protobuf.js"></script>
<script src="/lib/store-record.js"></script>
<script src="/lib/req2proto.js"></script>`);
      return;
    }
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
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: "load" });

  const rows = await page.evaluate(async () => {
    const bad = (violations) => ({
      error: {
        code: 400, message: "Invalid JSON payload received.",
        details: [{ "@type": "type.googleapis.com/google.rpc.BadRequest", fieldViolations: violations }],
      },
    });
    // `probeApiEndpoint` runs the STR payload first and the INT payload second against one content type.
    // The request body tells them apart: ["x1","x2",…] against [1,2,…].
    const twoAnswers = (strViolations, intViolations) => async (url, opts) => ({
      status: 400,
      headers: { "content-type": "application/json; charset=UTF-8" },
      body: JSON.stringify(bad(String(opts.body).startsWith('["x') ? strViolations : intViolations)),
      bodyEncoding: null,
    });

    const V = (field, desc) => ({ field, description: desc });
    const out = [];
    const show = (r) => Object.entries(r.fields).map(([k, f]) =>
      k + " => name=" + JSON.stringify(f.name) + " type=" + f.type + " number=" + f.number +
      " label=" + f.label + " required=" + f.required);
    /* EVERY CASE IS RUN FORWARDS AND WITH THE TWO PROBE ANSWERS SWAPPED, because the merge's central property
       is that where the probes are COMPLEMENTARY the arrival order cannot change the answer — and a property
       that is asserted in a comment instead of measured is one nobody finds out about. This found a real
       over-claim on its first run: the merge fills what is absent and never overwrites what is stated, so two
       descriptions CONTRADICTING each other (a path called a string by one probe and an integer by the other)
       legitimately resolve to whichever arrived first. That is the no-invention answer, and it is order
       dependent; the rows below say which cases move rather than leaving it to a sentence. */
    const run = async (name, strV, intV) => {
      const fwd = await probeApiEndpoint("https://t.example/v1/x:list", {}, {
        fetchFn: twoAnswers(strV, intV), maxDepth: 0,
      });
      const rev = await probeApiEndpoint("https://t.example/v1/x:list", {}, {
        fetchFn: twoAnswers(intV, strV), maxDepth: 0,
      });
      const a = show(fwd), b = show(rev);
      const agree = a.slice().sort().join("\n") === b.slice().sort().join("\n");
      out.push({ name, records: a, order: agree ? "agrees under swap" : "MOVES under swap: " + b.join(" | ") });
    };

    // 1. THE RELAY'S OWN CASE, aimed at the merge's OWN GUARD: the first description leaves `type` exactly
    //    `"unknown"` (an empty type in the parentheses), which is the one value `existing.type === "unknown"`
    //    tests for, and the second states TYPE_INT32. If the merge branch runs at all, this is `int32`.
    await run("two descriptions of tag 3; the FIRST is unknown, the SECOND states the type",
      [V("", "Invalid value at 'thing' (), 3")],
      [V("", "Invalid value at 'thing' (TYPE_INT32), 3")]);

    // 2. Two descriptions of tag 3 under DIFFERENT names.
    await run("two DIFFERENT names on tag 3",
      [V("", "Invalid value at 'browse_id' (TYPE_STRING), 3")],
      [V("", "Invalid value at 'page_size' (TYPE_INT32), 3")]);

    /* 3. The second description adds `repeated` and `required` to the same field. REQUIRED_FIELD_RE is
          /Missing required field (.+) at '([^']+)'/ — `([^']+)` needs a NON-EMPTY parent, so an earlier
          spelling of this row used `at ''`, matched nothing, and was testing the required-upgrade against a
          reply that never stated one. The parent is named. */
    await run("the second description makes req.thing repeated + required",
      [V("", "Invalid value at 'req.thing' (TYPE_STRING), 3")],
      [V("", "Missing required field thing at 'req'"), V("", "Invalid value at 'req.thing[0]' (TYPE_STRING), 3")]);

    // 4. UNNUMBERED fields sharing a name — the one route into the merge branch, if the guard above it
    //    really does swallow every numbered one.
    await run("two UNNUMBERED descriptions of the same name",
      [V("", "'thing' is not a valid string")],
      [V("", "'thing' is not a valid integer")]);

    // 5. A generic-pattern description whose REFLECTED VALUE lands on the same integer as a real wire tag.
    await run("a reflected VALUE 3 meeting a real wire tag 3",
      [V("", "Invalid value at 'browse_id' (TYPE_STRING), 3")],
      [V("", "invalid integer value '3' for field 'page_size'")]);

    // 6. ONE REPLY, TWO PARENTS, ONE TAG. Tag 2 of `request` and tag 2 of `filter` are DIFFERENT FIELDS of
    //    DIFFERENT messages; the dedup key is the bare number, so it cannot tell them apart. This is the
    //    sharpest form of "two different names on one tag" and it needs no second probe to reach.
    await run("tag 2 of `request` and tag 2 of `filter`, in ONE reply",
      [V("", "Invalid value at 'request.browse_id' (TYPE_STRING), 2"),
       V("", "Invalid value at 'filter.max_results' (TYPE_INT32), 2")],
      []);

    // 7. The same collision ACROSS the two probes rather than within one reply.
    await run("tag 2 of `request` from the str probe, tag 2 of `filter` from the int probe",
      [V("", "Invalid value at 'request.browse_id' (TYPE_STRING), 2")],
      [V("", "Invalid value at 'filter.max_results' (TYPE_INT32), 2")]);

    return out;
  });

  for (const r of rows) {
    console.log("\n── " + r.name);
    for (const rec of r.records) console.log("     " + rec);
    if (!r.records.length) console.log("     (no fields)");
    console.log("     order: " + r.order);
  }
  await browser.close();
  srv.close();
})().catch((e) => { console.error(e.stack || e); process.exit(1); });
