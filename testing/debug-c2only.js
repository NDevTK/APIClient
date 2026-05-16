var fs=require("fs"),path=require("path"),rd=path.join(__dirname,"..");
if (process.env.__C2_DIAG) globalThis.__C2_DIAG = true;
new Function(fs.readFileSync(path.join(rd,"extension/lib/babel-bundle.js"),"utf8").replace(/^var BabelBundle/,"globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd,"extension/lib/ast.js"),"utf8")+"globalThis.analyzeJSBundle=analyzeJSBundle;\n").call(globalThis);
var code='var ce = {};\nce.ajax = function(s){ var x = new XMLHttpRequest(); x.open(s.type, s.url); };\nce.get = function(u, d){ return ce.ajax({ url: u, type: "GET" }); };\nce.get("/api/profile", {a:1});\n';
var t0=Date.now();
var r=globalThis.analyzeJSBundle(code,"test://c2only",true,null);
console.log("ELAPSED_MS="+(Date.now()-t0));
console.log("fetchCallSites:",JSON.stringify(r.fetchCallSites||[]));
