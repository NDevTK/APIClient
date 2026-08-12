// Fetch the SW's globalThis._dumpCombined string in chunks via the
// harness's `sw` eval channel and write to a local file. Bypasses
// puppeteer's stringify-everything output so we can recover the raw
// 6MB analyzer input as text.

const { execSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const out = process.argv[2];
if (!out) { console.error("usage: node dump-combined.js <outpath>"); process.exit(1); }

function swCall(expr) {
  const cmd = `node testing/harness.js sw "${expr.replace(/"/g, '\\"')}"`;
  const buf = execSync(cmd, { cwd: path.resolve(__dirname, ".."), maxBuffer: 256 * 1024 * 1024 });
  // Strip puppeteer's outer JSON.stringify of the return value
  return JSON.parse(buf.toString().trim());
}

const total = swCall(`return globalThis._dumpCombined ? globalThis._dumpCombined.length : 0`);
console.log("total chars:", total);
if (!total) { console.error("no _dumpCombined in SW; stash one first"); process.exit(2); }

const CHUNK = 1500000;  // chars per fetch — keeps base64 expansion under puppeteer message limit
const wrote = fs.openSync(out, "w");
let pos = 0;
while (pos < total) {
  const end = Math.min(pos + CHUNK, total);
  console.log(`fetching ${pos}..${end}`);
  const expr = `return btoa(unescape(encodeURIComponent(globalThis._dumpCombined.substring(${pos}, ${end}))))`;
  const b64 = swCall(expr);
  const chunk = Buffer.from(b64, "base64").toString("utf8");
  fs.writeSync(wrote, chunk);
  pos = end;
}
fs.closeSync(wrote);
console.log("wrote", out);
