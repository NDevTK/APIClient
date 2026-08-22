/*---
description: >
  A join of an object this flow is already joining contributes nothing, so a self-referential array has a
  string instead of a hang.

  `a[0] = a; String(a)` asks join for the string of an element that IS the array: join coerces it, the
  coercion calls join, forever. Every engine answers "" for it. That answer is not a bound — nothing distinct
  is truncated, because the recursion has no output to lose — it is what the operation MEANS on a cyclic
  input, and without it the flow never finishes.

  The question is asked about the RECEIVER when a join starts, not about each element, because the cycle need
  not run through the array: `e.name = [e]` closes it through the Error, whose toString coerces `name` and
  arrives back at the same array. V8's error-tostring.js asserts exactly that shape returns "".

  It is asked of the FLOW's own continuation chain, like the prepareStackTrace recursion guard and for the
  same reason: two flows have two stacks, and one parked inside a join must not decide what another flow's
  join does. The chain is not homogeneous — an element coercion goes join, ToPrimitive, toString, join — so
  the walk asks each link's kind, and each join records the join enclosing it so the question is a walk over
  joins alone.

  ECMAScript says nothing about any of this, which is why it is here and not in test262.
---*/
var a = [];
a[0] = a;
assert.sameValue(String(a), "", "an array holding itself");

var b = [1, [2, 3]];
b[2] = b;
assert.sameValue(b.join("-"), "1-2,3-", "the cycle is empty; everything else still joins");

/* Two arrays holding each other: the cycle closes on the second visit, not the first. */
var c = [], d = [c];
c[0] = d;
assert.sameValue(String(c), "", "a mutual cycle");
assert.sameValue(String(d), "", "from either end");

/* Through an object that is not an array at all. */
var e = new Error();
e.name = [e];
e.message = [e];
assert.sameValue(e.toString(), "", "a cycle that closes through Error.prototype.toString");

/* Nesting that is deep but acyclic still joins all the way down — the guard is about identity, not depth. */
var nested = [0];
for (var i = 1; i <= 200; i++) nested = [i, nested];
assert.sameValue(nested.join("").length, 692, "200 levels of nesting are joined, not cut off");

assert.sameValue([1, 2].join(), "1,2", "an ordinary join is untouched");
assert.sameValue([1, [2, [3]]].join(), "1,2,3", "and so is an ordinary nested one");
assert.sameValue([].join(), "", "as is the empty one");

/* toLocaleString walks the same machine, so it answers the same way. */
var f = [];
f[0] = f;
assert.sameValue(f.toLocaleString(), "", "toLocaleString shares the join and its answer");
