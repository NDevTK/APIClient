/*---
description: >
  Error.prepareStackTrace is the PAGE's function, and build_backtrace runs under every internal throw — an
  activation with no flow base, where calling it drove its body to completion and its first loop back-edge
  aborted. build_backtrace parks the pending [prepare, callsites] pair and the `.stack` ACCESSOR makes the
  call, as a step machine that suspends like any other. V8 formats lazily for the same reason.
---*/
var saved = Error.prepareStackTrace;
var calls = 0;

Error.prepareStackTrace = function (err, frames) {
  calls++;
  var n = 0;
  for (var i = 0; i < 500; i++) n += i;          /* a LOOP in the hook: the case that used to abort */
  return "prepared:" + err.message + ":" + (frames.length > 0) + ":" + n;
};

function make(msg) { return new Error(msg); }
var e = make("boom");
assert.sameValue(calls, 0, "the hook must not run while the error is being constructed");
var s = e.stack;
assert.sameValue(calls, 1);
assert.sameValue(s.indexOf("prepared:boom:true:124750"), 0, s);
assert.sameValue(e.stack, s, "the formatted stack is memoized");
assert.sameValue(calls, 1, "the hook runs once per error");

/* an INTERNAL throw takes the same path — the pair is parked by the engine, not by a constructor */
var caught = null;
try { null.x; } catch (err2) { caught = err2; }
assert.sameValue(typeof caught.stack, "string");
assert.sameValue(caught.stack.indexOf("prepared:"), 0, caught.stack);

/* the hook's own throw propagates out of the accessor, exactly as any getter's does */
Error.prepareStackTrace = function () { throw new RangeError("from the hook"); };
var e2 = make("x");
assert.throws(RangeError, function () { return e2.stack; });

Error.prepareStackTrace = saved;
