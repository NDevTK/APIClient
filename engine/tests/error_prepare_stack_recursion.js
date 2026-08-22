/*---
description: >
  A `.stack` read that happens INSIDE Error.prepareStackTrace gets the default rendering instead of
  re-entering the hook.

  V8 keeps this on the isolate (Isolate::formatting_stack_trace) and it is a contract, not an accident:
  mjsunit's own stack formatter ends with `catch (e) {}; return error.stack;`, deliberately re-entering to
  reach the default. Without the answer the hook calls itself, and this engine has no stack bound to stop it
  — the calls trampoline onto the heap — so it does not overflow, it runs until the process is killed. That
  is how a V8 regression file that finishes in milliseconds there became an OOM here.

  The guard is asked of the running flow's frames rather than of a process-wide flag: a flow parked inside a
  hook must not change what a sibling flow's `.stack` read does.
---*/
var calls = 0;
var nested;

Error.prepareStackTrace = function (error, sites) {
    calls++;
    nested = error.stack;          /* must NOT re-enter this function */
    return "hooked:" + sites.length;
};

function boom() { return new Error("boom"); }

var e = boom();
var out = e.stack;

assert.sameValue(calls, 1, "the hook ran exactly once for one read");
assert.sameValue(out.slice(0, 7), "hooked:", "the outer read ran the hook");
assert.sameValue(typeof nested, "string", "the nested read produced a rendering");
assert.sameValue(nested.slice(0, 7) === "hooked:", false, "the nested read did not run the hook");
assert(nested.indexOf("    at ") >= 0, "the nested read produced the default frame rendering");

/* The hook's result is memoized: a second read of the SAME error does not run it again. */
assert.sameValue(e.stack, out, "the formatted stack is the same value on a second read");
assert.sameValue(calls, 1, "a second read of the same error did not run the hook again");

/* The guard is the hook's DYNAMIC EXTENT, not a latch: once it returns, the next error formats normally. */
function again() { return new Error("again"); }
var e2 = again();
assert.sameValue(e2.stack.slice(0, 7), "hooked:", "a later error still runs the hook");
assert.sameValue(calls, 2, "the hook ran once more, after the first call had returned");
