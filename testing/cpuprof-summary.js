// Summarize a Chromium .cpuprofile — top self-time leaves with location.
// Standard CPU profile format: nodes array each with {id, callFrame, hitCount, children},
// timeDeltas array, and samples array. Self-time per node = hitCount.
const fs = require("fs");
const path = require("path");

const file = process.argv[2];
if (!file) { console.error("usage: node cpuprof-summary.js <file.cpuprofile> [topN]"); process.exit(1); }
const topN = Number(process.argv[3]) || 30;

const profile = JSON.parse(fs.readFileSync(file, "utf8"));
const nodes = profile.nodes || [];
const totalSamples = nodes.reduce((s, n) => s + (n.hitCount || 0), 0);
// Node CPU profiler default sample interval is 1ms (1000µs). Read from
// metadata if present.
const sampleIntervalUs = profile.samples && profile.timeDeltas
  ? Math.round(profile.timeDeltas.reduce((s, d) => s + d, 0) / profile.timeDeltas.length)
  : 1000;
const intervalMs = sampleIntervalUs / 1000;
console.log(`samples=${totalSamples}  interval≈${sampleIntervalUs}µs  total≈${(totalSamples * intervalMs).toFixed(0)}ms`);

// Build child→parent map for inclusive-time reconstruction
const idToNode = new Map();
for (const n of nodes) idToNode.set(n.id, n);
function inclusiveHits(node, seen) {
  if (!node) return 0;
  if (seen.has(node.id)) return 0;
  seen.add(node.id);
  let h = node.hitCount || 0;
  if (Array.isArray(node.children)) {
    for (const cid of node.children) h += inclusiveHits(idToNode.get(cid), seen);
  }
  return h;
}

const top = nodes
  .filter(n => (n.hitCount || 0) > 0)
  .sort((a, b) => (b.hitCount || 0) - (a.hitCount || 0))
  .slice(0, topN);

console.log(`\ntop ${topN} self-time leaves:`);
console.log("self%   selfMs  inclMs  function  url:line:col");
for (const n of top) {
  const fn = n.callFrame || {};
  const selfPct = ((n.hitCount / totalSamples) * 100).toFixed(1);
  const selfMs = (n.hitCount * intervalMs).toFixed(0);
  const inclMs = (inclusiveHits(n, new Set()) * intervalMs).toFixed(0);
  const fname = fn.functionName || "(anon)";
  const url = fn.url ? fn.url.replace(/\\/g, "/").split("/").pop() : "?";
  const loc = fn.lineNumber != null ? ":" + (fn.lineNumber + 1) + ":" + (fn.columnNumber + 1) : "";
  console.log(`${selfPct.padStart(5)}%  ${selfMs.padStart(6)}  ${inclMs.padStart(6)}  ${fname}  ${url}${loc}`);
}
