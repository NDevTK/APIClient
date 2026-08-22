/*---
description: >
  22.2.1's Disjunction / Alternative / Term recursed on the C stack, one frame per `(` of nesting with the
  depth chosen by the input, and lre_check_stack_overflow turned that into "stack overflow" — a bound in an
  error's clothing, for a grammar that has an answer at every depth. They are one frame stack now, and that
  hook has no callers left in the engine.
---*/
assert.sameValue(new RegExp("(?:".repeat(50000) + "a" + ")".repeat(50000)).test("a"), true);
assert.sameValue(new RegExp("(?=".repeat(20000) + "a" + ")".repeat(20000)).test("a"), true);
assert.sameValue(new RegExp("a|".repeat(50000) + "b").test("b"), true);
assert.sameValue(new RegExp("(".repeat(2000) + "a" + ")".repeat(2000)).exec("a")[2000], "a");

/* a lookbehind nested the same way parses backwards through the same frames */
assert.sameValue(new RegExp("(?<=" + "(?:".repeat(5000) + "a" + ")".repeat(5000) + ")b").test("ab"), true);
