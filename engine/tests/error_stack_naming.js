/*---
description: >
  A stack frame is named by HOW it was called, not just by the callee's own name.

  V8's default rendering: `new Plonk` for a construct, `Foo.bar` for a method call, `Nirk.valueOf` when the
  function has no name of its own, `Wookie.a$b$c$d [as d]` when its name and the property it was reached
  through differ, `Array.<anonymous>` for an anonymous function called on an array. This engine printed the
  callee's name alone, which is the one part a reader already knows.

  The two pieces are the receiver's TYPE and the PROPERTY the call used. `X.prototype.y = function () {}`
  gives the function no name — 13.15.2 applies NamedEvaluation to an IdentifierReference target, not to a
  MemberExpression — so `y` exists only on the receiver, and it is found there by walking own data properties
  along the prototype chain. Nothing is invoked while doing it: an accessor is not read and a Proxy is not
  walked, because a stack trace is built with an exception in flight.

  Nothing in ECMAScript says what `.stack` holds, which is why this is here.
---*/
function frame(stack) { return stack.split("\n")[0].replace(/ \(.*/, ""); }

function Foo() {}
Foo.prototype.bar = function bar() { return new Error("x").stack; };
assert.sameValue(frame(new Foo().bar()), "    at Foo.bar",
                 "a method whose name matches its property says it once");

function Nirk() {}
Nirk.prototype.valueOf = function () { return new Error("x").stack; };
assert.sameValue(frame(new Nirk().valueOf()), "    at Nirk.valueOf",
                 "a function with no name of its own is named by the property it was reached through");

function a$b$c$d() { return new Error("x").stack; }
function Wookie() {}
Wookie.prototype.d = a$b$c$d;
assert.sameValue(frame(new Wookie().d()), "    at Wookie.a$b$c$d [as d]",
                 "a renamed method says both names — either alone sends a reader to the wrong place");

function Plonk() { this.s = new Error("x").stack; }
assert.sameValue(frame(new Plonk().s), "    at new Plonk", "a construct is spelled `new`");

assert.sameValue(frame((function () { return new Error("x").stack; }).call([1, 2, 3])),
                 "    at Array.<anonymous>", "an anonymous function still names what it was called on");

/* A plain call binds `this` to the global object, so there is no type to name. */
function plain() { return new Error("x").stack; }
assert.sameValue(frame(plain()), "    at plain", "a top-level call is named by the function alone");

/* Two names for one function is not a name: the search gives up, and the frame keeps the type it does know
   plus the function's own name — no `[as one]`, because the search cannot tell `one` from `two`. */
function twice() { return new Error("x").stack; }
var host = { one: twice, two: twice };
assert.sameValue(frame(host.one()), "    at Object.twice",
                 "an ambiguous property search adds nothing rather than guessing");
