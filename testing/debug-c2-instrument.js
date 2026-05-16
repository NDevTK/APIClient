// Instrument WITHOUT editing the engine: wrap _resolveAvBySubstitutingCallerArgs
// and _specInstantiateAv to confirm unbounded AV growth on the cyclic `ce`.
var fs = require("fs"), path = require("path"), rd = path.join(__dirname, "..");
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
var astCode = fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8");
new Function(astCode +
  "globalThis.analyzeJSBundle = analyzeJSBundle;\n" +
  "globalThis.__probe = {\n" +
  "  rabsca: _resolveAvBySubstitutingCallerArgs,\n" +
  "  inst: _specInstantiateAv,\n" +
  "  hash: _avStructuralHash,\n" +
  "  setRabsca: function(f){ _resolveAvBySubstitutingCallerArgs = f; },\n" +
  "  setInst: function(f){ _specInstantiateAv = f; }\n" +
  "};\n").call(globalThis);

var P = globalThis.__probe;
var rabscaN = 0, instN = 0, maxHashLen = 0;
var hashSeenAtRabsca = []; // structural hash length per first N rabsca calls
var origRabsca = P.rabsca;
P.setRabsca(function (funcPath, av) {
  rabscaN++;
  if (rabscaN <= 60) {
    var h = "";
    try { h = av ? P.hash(av) : ""; } catch (e) { h = "<hashErr:" + e.message + ">"; }
    if (h.length > maxHashLen) maxHashLen = h.length;
    hashSeenAtRabsca.push({ n: rabscaN, fn: (funcPath && funcPath.node && funcPath.node.start), hLen: h.length });
  }
  if (rabscaN === 200 || rabscaN === 1000 || rabscaN === 5000) {
    console.log("[probe] rabsca call #" + rabscaN + "  instN=" + instN + "  maxHashLen=" + maxHashLen);
  }
  if (rabscaN > 20000) {
    console.log("[probe] ABORT: rabsca exceeded 20000 calls — non-terminating. instN=" + instN + " maxHashLen=" + maxHashLen);
    console.log("[probe] first 60 rabsca hash-lengths:");
    hashSeenAtRabsca.forEach(function (e) { console.log("   #" + e.n + " fn@" + e.fn + " hashLen=" + e.hLen); });
    process.exit(7);
  }
  return origRabsca(funcPath, av);
});
var origInst = P.inst;
P.setInst(function (av, args, thisAv, fnCtx) {
  instN++;
  if (instN > 400000) {
    console.log("[probe] ABORT: _specInstantiateAv exceeded 400000 calls. rabscaN=" + rabscaN + " maxHashLen=" + maxHashLen);
    process.exit(8);
  }
  return origInst(av, args, thisAv, fnCtx);
});

var code = 'var ce = {};\n' +
  'ce.ajax = function(s){ var x = new XMLHttpRequest(); x.open(s.type, s.url); };\n' +
  'ce.get = function(u, d){ return ce.ajax({ url: u, type: "GET" }); };\n' +
  'ce.get("/api/profile", {a:1});\n';
var t0 = Date.now();
var r = globalThis.analyzeJSBundle(code, "test://c2i", true, null);
console.log("DONE ELAPSED_MS=" + (Date.now() - t0) + " rabscaN=" + rabscaN + " instN=" + instN + " maxHashLen=" + maxHashLen);
console.log("fetchCallSites:", JSON.stringify(r.fetchCallSites || []));
