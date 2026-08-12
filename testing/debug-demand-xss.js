// Demand engine's OTHER deliverable: XSS / taint findings. Same backward
// query, but the result AV should carry a taint-source leaf (location.*,
// document.referrer, …) reaching a DOM/eval sink. Verify the engine
// resolves the sink arg to a taint-source AV (the finding signal), incl.
// through param-backward + concat — the unified design's second half.
var fs = require("fs"), path = require("path"), rd = "d:/APIClient";
globalThis.__DEMAND_PROBE = true;
new Function(fs.readFileSync(path.join(rd, "extension/lib/babel-bundle.js"), "utf8")
  .replace(/^var BabelBundle/, "globalThis.BabelBundle"))();
new Function(fs.readFileSync(path.join(rd, "extension/lib/ast.js"), "utf8") +
  "globalThis.analyzeJSBundle=analyzeJSBundle;").call(globalThis);

function run(label, code) { console.log("=== " + label + " ==="); globalThis.analyzeJSBundle(code, "t://" + label, true, null); }

// X1: direct location.search → insertAdjacentHTML (DOM-XSS sink).
run("X1-direct-locsearch-html", `
document.body.insertAdjacentHTML("beforeend", location.search);
`);

// X2: param-backward — taint flows through a helper into the sink.
run("X2-param-backward-xss", `
function render(h){ document.body.insertAdjacentHTML("beforeend", h); }
render(location.hash);
`);

// X3: concat — "<div>" + location.search into the sink (engine must
// compose the binop and surface the taint-source leaf).
run("X3-concat-taint", `
function render(h){ document.body.insertAdjacentHTML("beforeend", "<div>" + h + "</div>"); }
render(location.search);
`);

// X4: eval sink via param-backward (request-forgery/eval family).
run("X4-eval-param", `
function go(code){ eval(code); }
go(location.hash);
`);

// X5: NEGATIVE — static literal into sink, must NOT be a taint finding
// (and base already resolves it ⇒ 0 seeds expected).
run("X5-negative-static", `
document.body.insertAdjacentHTML("beforeend", "<b>static</b>");
`);
