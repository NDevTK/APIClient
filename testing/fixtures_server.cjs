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

function pick(p) {
  if (p === "/" || p === "") return "/poc_multi.html";
  return p;
}
const CT = { ".html": "text/html; charset=utf-8", ".js": "application/javascript", ".json": "application/json", ".css": "text/css" };

const srv = http.createServer((req, res) => {
  // Strip any ?query / #hash before resolving the file
  const reqUrlFull = String(req.url || "/");
  console.log(`[${new Date().toISOString()}] ${req.method} ${reqUrlFull}  referer=${req.headers.referer || "-"}`);
  const url = pick(reqUrlFull.split("?")[0].split("#")[0]);
  const fp = path.join(ROOT, url);
  if (!fp.startsWith(ROOT) || !fs.existsSync(fp) || !fs.statSync(fp).isFile()) {
    res.writeHead(404); res.end("404"); return;
  }
  res.writeHead(200, {
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
  console.log(`  GET / -> /poc_multi.html`);
  console.log(`  lock: ${LOCK}`);
});

process.on("SIGTERM", () => { try { fs.unlinkSync(LOCK); } catch {} srv.close(() => process.exit(0)); });
process.on("SIGINT",  () => { try { fs.unlinkSync(LOCK); } catch {} srv.close(() => process.exit(0)); });
