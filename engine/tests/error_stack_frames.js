/*---
description: >
  A stack trace names the page's activations, all of them and nothing else.

  Two things used to be wrong at once, and each hid the other. (1) A flow's CALL ROOT — a promise reaction's
  base, which holds the handler in cur_func because the handler is what it is about to call — was reported as
  a frame, so a handler appeared TWICE, the second time with no position (upstream rendered that as
  "(missing)"; the walk feeding Error.prepareStackTrace subtracted from a null pointer instead).
  (2) `new Error` skipped its innermost frame. That was right while the Error constructor was a C function —
  js_call_c_function pushes a frame for the builtin, and naming Error() as the origin is wrong — but a step
  machine pushes no frame, so the skip ate the PAGE's frame. A top-level `new Error().stack` was the empty
  string, and inside a call it lost the function that constructed the error.

  Nothing in ECMAScript says what `.stack` holds, which is exactly why this is here and not in test262.
flags: [async]
---*/
function normalize(s) {
    return s.replace(/\(.*\)/g, "(F)").split("\n").filter(function (l) { return l !== ""; });
}

function inner() { return new Error("boom"); }
function outer() { return inner(); }

var deep = normalize(outer().stack);
assert.sameValue(deep.length, 3, "the constructing frame, its caller, and the script: " + deep.join(" / "));
assert.sameValue(deep[0], "    at inner (F)", "the frame that ran `new Error` is the innermost");
assert.sameValue(deep[1], "    at outer (F)", "its caller is next");

var top = normalize(new Error("t").stack);
assert.sameValue(top.length, 1, "a top-level `new Error` still has the script's own frame");

/* Error.captureStackTrace IS a C function, so it does have a frame of its own, and skipping it is right. */
var o = {};
function capture() { Error.captureStackTrace(o); }
capture();
var cap = normalize(o.stack);
assert.sameValue(cap[0], "    at capture (F)", "captureStackTrace's own frame is not a place in the page");

/* A promise reaction handler runs on a call-root flow base. The root is not an activation. */
Promise.resolve().then(function handler() {
    var seen = normalize(new Error("h").stack);
    assert.sameValue(seen.length, 1, "the handler is ONE frame, not the handler and its flow root: "
                     + seen.join(" / "));
    assert.sameValue(seen[0], "    at handler (F)", "and it is the handler's own");
}).then($DONE, $DONE);
