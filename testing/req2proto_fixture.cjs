// testing/req2proto_fixture.cjs — the ONE fixture for the two recovered systems.
//
// It is a server rather than a static file set because the whole question is what LEAVES the browser: the
// error probe (lib/req2proto.js) and the discovery-document fetch (lib/discovery-probe.js) both go out
// through the page-context relay, and the only place their verb, their headers, their body and the page's
// COOKIE are visible as facts is the far end. Every request is appended to a JSONL log so the harness can
// read back exactly what arrived, in order.
//
// Routes:
//   GET  /                     the page. Sets a cookie, then POSTs to the API below so the extension learns
//                              an endpoint, an API key and a protobuf-ish content type.
//   POST /v1/things:list       answers 400 with a google.rpc.Status envelope carrying fieldViolations in the
//                              exact form lib/req2proto.js's GOOGLE_FIELD_DESC_RE reads, plus an ErrorInfo
//                              detail naming a service and a method.
//   GET  /openapi.json         a published description. Its SECOND serving differs from the first (a method
//                              is added), which is what makes _diffDiscoveryDocs produce a change record.
//   *                          404, which is what the other buildDiscoveryUrls candidates must get.
"use strict";
const http = require("http");
const fs = require("fs");
const path = require("path");

const PORT = parseInt(process.env.FIX_PORT || "8791", 10);
const LOG = path.resolve(__dirname, ".req2proto-fixture.jsonl");
try { fs.unlinkSync(LOG); } catch {}

let openapiServes = 0;

/* `?delay=N` DELAYS THE PAGE'S ONE API CALL BY N MILLISECONDS, and it exists to tell two indistinguishable
   things apart. If the request the extension makes in response carries a real tab id when the call is late
   and a null one when it is immediate, the defect is an ORDERING race against the document's registration; if
   it is null either way, the tab id is never derived at all. Neither answer is available from one timing. */
const PAGE = (delay) => `<!doctype html><meta charset=utf-8><title>req2proto fixture</title>
<h1>req2proto fixture</h1>
<script>
// One live API call, so the brain learns the endpoint + the key + the content type.
setTimeout(function () {
fetch("/v1/things:list?key=AIzaSyFIXTUREKEY0000000000000000000000", {
  method: "POST",
  headers: { "content-type": "application/json+protobuf" },
  body: JSON.stringify([["seed"]]),
  credentials: "same-origin",
}).then(r => r.text()).then(t => { window.__apiStatus = t.length; });
}, ${delay});
</script>`;

// A 400 shaped like a real Google API validation rejection.
const RPC_STATUS = {
  error: {
    code: 400,
    message: "Invalid JSON payload received.",
    status: "INVALID_ARGUMENT",
    details: [
      {
        "@type": "type.googleapis.com/google.rpc.BadRequest",
        fieldViolations: [
          { field: "browseId", description: "Invalid value at 'browse_id' (TYPE_STRING), 2" },
          { field: "maxResults", description: "Invalid value at 'max_results' (TYPE_INT32), 5" },
          { description: "Missing required field pageToken at 'request'" },
        ],
      },
      {
        "@type": "type.googleapis.com/google.rpc.ErrorInfo",
        reason: "INVALID_ARGUMENT",
        metadata: { service: "fixture.googleapis.com", method: "fixture.v1.Things.List" },
      },
    ],
  },
};

function openapiDoc(serveIndex) {
  const paths = {
    "/v1/things:list": {
      post: {
        operationId: "things.list",
        parameters: [{ name: "pageSize", in: "query", schema: { type: "integer" } }],
        responses: { 200: { description: "ok" } },
      },
    },
  };
  // The SECOND serving adds a method — the difference _diffDiscoveryDocs must report.
  if (serveIndex >= 1) {
    paths["/v1/things:create"] = {
      post: { operationId: "things.create", responses: { 200: { description: "ok" } } },
    };
  }
  return { openapi: "3.0.0", info: { title: "Fixture API", version: "v1" }, paths };
}

const srv = http.createServer((req, res) => {
  const chunks = [];
  req.on("data", (c) => chunks.push(c));
  req.on("end", () => {
    const body = Buffer.concat(chunks);
    const u = new URL(req.url, `http://127.0.0.1:${PORT}`);
    const rec = {
      t: Date.now(),
      method: req.method,
      path: u.pathname,
      query: u.search,
      contentType: req.headers["content-type"] || null,
      cookie: req.headers.cookie || null,
      apiKeyHeader: req.headers["x-goog-api-key"] || null,
      origin: req.headers.origin || null,
      referer: req.headers.referer || null,
      secFetchMode: req.headers["sec-fetch-mode"] || null,
      bodyLen: body.length,
      bodyHead: body.length ? body.toString("utf8").slice(0, 120) : null,
    };
    fs.appendFileSync(LOG, JSON.stringify(rec) + "\n");
    console.log(`${req.method} ${u.pathname}${u.search} ct=${rec.contentType} cookie=${rec.cookie} body=${rec.bodyLen}`);

    const cors = {
      "access-control-allow-origin": req.headers.origin || "*",
      "access-control-allow-credentials": "true",
      "access-control-allow-headers": "content-type,x-goog-api-key",
      "access-control-allow-methods": "GET,POST,OPTIONS",
    };
    if (req.method === "OPTIONS") { res.writeHead(204, cors); res.end(); return; }

    if (u.pathname === "/" || u.pathname === "/index.html") {
      res.writeHead(200, { ...cors, "content-type": "text/html; charset=utf-8", "set-cookie": "fixture_session=s3cr3t; Path=/; SameSite=Lax" });
      res.end(PAGE(parseInt(u.searchParams.get("delay") || "0", 10) || 0));
      return;
    }
    if (u.pathname === "/v1/things:list") {
      res.writeHead(400, { ...cors, "content-type": "application/json; charset=UTF-8" });
      res.end(JSON.stringify(RPC_STATUS));
      return;
    }
    if (u.pathname === "/openapi.json") {
      /* `?v2` IS THE SECOND VERSION OF THE DOCUMENT, named by the caller rather than counted here. A
         serve-counter made "which version you get" depend on how many times anything had ever asked, so a
         second test run in the same server process saw version 2 twice and the differ correctly reported no
         change — a green that meant nothing. */
      const doc = openapiDoc(u.searchParams.has("v2") ? 1 : 0);
      openapiServes++;
      res.writeHead(200, { ...cors, "content-type": "application/json" });
      res.end(JSON.stringify(doc));
      return;
    }
    res.writeHead(404, { ...cors, "content-type": "text/plain" });
    res.end("404");
  });
});

srv.listen(PORT, "127.0.0.1", () => {
  console.log(`req2proto fixture on http://127.0.0.1:${PORT}/  (log: ${LOG})`);
});
