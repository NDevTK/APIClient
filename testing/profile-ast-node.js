// Node-based AST analyzer profiler.
// Bypasses MV3 SW lifecycle entirely — loads Babel + ast.js into a Node
// V8 process where Node's --cpu-prof flag captures a real CPU profile
// loadable in Chrome DevTools Performance tab.
//
// Usage:
//   node --cpu-prof --cpu-prof-dir=. --cpu-prof-name=ast.cpuprofile testing/profile-ast-node.js <input.js>
// or via the audit.profile.local harness command.

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const inputPath = process.argv[2];
if (!inputPath) {
  console.error("usage: node profile-ast-node.js <input.js>");
  process.exit(1);
}

// Build a BabelBundle global mirroring extension/lib/babel-bundle.js's
// public surface (parse, traverse, t). ast.js reads `BabelBundle.parse`,
// `BabelBundle.traverse`, `BabelBundle.t` only.
const babelParser = require("@babel/parser");
const babelTraverseModule = require("@babel/traverse");
const babelTraverse = babelTraverseModule.default || babelTraverseModule;
const babelTypes = require("@babel/types");

const sandbox = {
  console: console,
  performance: { now: () => Number(process.hrtime.bigint() / 1000n) / 1000 },
  Date: Date,
  Math: Math,
  Set: Set,
  Map: Map,
  WeakMap: WeakMap,
  WeakSet: WeakSet,
  RegExp: RegExp,
  Error: Error,
  TypeError: TypeError,
  RangeError: RangeError,
  Object: Object,
  Array: Array,
  JSON: JSON,
  String: String,
  Number: Number,
  Boolean: Boolean,
  encodeURIComponent: encodeURIComponent,
  decodeURIComponent: decodeURIComponent,
  encodeURI: encodeURI,
  decodeURI: decodeURI,
  btoa: typeof btoa !== "undefined" ? btoa : (s) => Buffer.from(s, "binary").toString("base64"),
  atob: typeof atob !== "undefined" ? atob : (s) => Buffer.from(s, "base64").toString("binary"),
  URL: URL,
  URLSearchParams: URLSearchParams,
  Symbol: Symbol,
  Reflect: Reflect,
  Promise: Promise,
  setTimeout: setTimeout,
  clearTimeout: clearTimeout,
  BabelBundle: {
    parse: babelParser.parse,
    traverse: babelTraverse,
    t: babelTypes,
  },
};
vm.createContext(sandbox);

const astSrc = fs.readFileSync(path.join(__dirname, "..", "extension", "lib", "ast.js"), "utf8");
vm.runInContext(astSrc, sandbox, { filename: "ast.js" });

const code = fs.readFileSync(inputPath, "utf8");
console.log(`analyzing ${inputPath} (${code.length} chars)…`);

const t0 = process.hrtime.bigint();
let result;
try {
  result = sandbox.analyzeJSBundle(code, "profile://" + path.basename(inputPath), true);
} catch (e) {
  console.error("analyzer threw:", e.message);
  console.error(e.stack);
  process.exit(2);
}
const totalMs = Number(process.hrtime.bigint() - t0) / 1e6;

console.log(`\nanalyzed in ${totalMs.toFixed(0)}ms`);
console.log(`  fetchSites=${result.fetchCallSites?.length || 0}`);
console.log(`  sinks=${result.securitySinks?.length || 0}`);
console.log(`  dangerous=${result.dangerousPatterns?.length || 0}`);
console.log(`  resolverErrors=${result.resolverErrors?.length || 0}`);
if (result.perf) {
  const ph = result.perf.phaseMs || {};
  console.log(`  phases: parse=${ph.parse?.toFixed(0)}ms prePass=${ph.prePass?.toFixed(0)}ms mainPass=${ph.mainPass?.toFixed(0)}ms structuralExport=${ph.structuralExport?.toFixed(0)}ms`);
}
