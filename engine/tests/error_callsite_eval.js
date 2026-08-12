/*---
description: >
  A CallSite says whether its frame is eval code and where that eval was called.

  Eval-ness is a property of the SCRIPT, not of the function: a function declared inside an eval'd source is
  eval code too, so the flag and the origin ride down the whole nest of function definitions.

  `new Function` compiles as an indirect eval and is NOT one — its code has no eval origin. That is the only
  thing distinguishing the two, and they reach the compiler through one shared body, so the distinction is a
  parameter of that body rather than a constant inside it. Hardcoded, it made indirect eval report itself as
  not an eval.

  The origin is the CALLER's position, so it can only be read while the caller is still on the stack — which
  is when the eval'd code is being compiled. A CallSite cannot go looking for it later; by then the frame is
  gone. It is rendered once, at compile time, and the compiled code carries it.

  V8's stack formatter asks isEval before any other question about a frame, which is why its absence took the
  whole formatter out. None of this is in ECMAScript.
---*/
Error.prepareStackTrace = function (e, sites) {
    return sites.map(function (s) {
        return [s.getFunctionName(), s.isEval(), s.getEvalOrigin() === null ? "-" : "origin"].join("|");
    });
};
function grab() { return new Error("x").stack; }

function caller() { return eval("grab()"); }
var direct = caller()[1];
assert.sameValue(direct.slice(0, 12), "<eval>|true|", "direct eval code is eval code: " + direct);

var indirect = (0, eval)("grab()")[1];
assert.sameValue(indirect.slice(0, 12), "<eval>|true|", "indirect eval is eval too: " + indirect);

var nested = eval("(function nested() { return grab(); })")()[1];
assert.sameValue(nested, "nested|true|origin",
                 "a function DECLARED inside an eval is eval code, and inherits the origin");

var ctor = new Function("return grab();")()[1];
assert.sameValue(ctor, "anonymous|false|-", "the Function constructor is not an eval");

assert.sameValue(grab()[0], "grab|false|-", "ordinary script code is not eval code");

/* The origin names where the eval was CALLED, not where the eval'd code is. */
var origin = null;
Error.prepareStackTrace = function (e, sites) { return sites[1].getEvalOrigin(); };
function namedCaller() { return eval("grab()"); }
origin = namedCaller();
assert.sameValue(origin.slice(0, 20), "eval at namedCaller ", "the origin names the calling function: " + origin);
assert(origin.indexOf(":") > 0, "and its position: " + origin);

/* AND THE DEFAULT RENDERING SAYS IT TOO. A frame from eval'd code has a file name of `<input>` and a line of
   1; on its own that says nothing, and the origin is the only thing tying the frame back to the page. It goes
   inside the same parentheses, before the position, which is where V8's stack-traces.js looks for it. */
Error.prepareStackTrace = undefined;
function outerFn() { return eval("(function inEval() { return new Error('x').stack; })()"); }
var line = outerFn().split("\n")[0];
assert.sameValue(line.slice(0, 32), "    at inEval (eval at outerFn (",
                 "an eval frame is located by where the eval was: " + line);

/* An eval called from eval'd code carries the whole chain, by the same composition. */
function outermost() { return eval("(function mid() { return eval('new Error(\"x\").stack'); })()"); }
var nested = outermost().split("\n")[0];
assert(nested.indexOf("eval at mid (eval at outermost (") >= 0,
       "a nested eval names both evals: " + nested);
