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

/* A CONTINUATION-HOLDING BUILTIN IS ON THE STACK, and until now it was on no stack trace. `Array.prototype.map`
   is a step machine in this engine — it pushes no JSStackFrame, because it is driven from the interpreter's
   own activation — so its callback's frame linked straight past it to the caller and the builtin vanished.
   V8 reports `at Array.map`, and that is the truthful answer: the builtin really is between the two. */
function fromCallback() { return [1, 2, 3].map(function () { return new Error("x").stack; })[0]; }
var mapped = fromCallback().split("\n").map(function (l) { return l.replace(/ \(.*/, ""); });
assert.sameValue(mapped[0], "    at <anonymous>", "the callback is innermost");
assert.sameValue(mapped[1], "    at Array.map", "and the builtin driving it is named, with its receiver's type");
assert.sameValue(mapped[2], "    at fromCallback", "then the caller");

/* The receiver of a C builtin is not boxed — no ToObject happens — so a primitive one is named directly
   rather than by allocating a wrapper to read its class back off. */
var replaced = "abc".replace(/b/, function () { return new Error("x").stack.split("\n")[1]; });
assert.sameValue(replaced, "a    at String.replace (native)c",
                 "a builtin on a primitive names the primitive's type, and has no position of its own");

/* The machine BUILDING a trace must never name itself: `new Error()` inside a callback reports the callback,
   not `Function.Error`. That is what the skip-first-level flag has always meant, and it applies to the step
   machine's own frame now that the machine has one. */
assert.sameValue(new Error("x").stack.split("\n")[0].slice(0, 12), "    at <eval",
                 "the Error constructor is not the origin of the error");

/* A BUILTIN THAT THROWS BEFORE IT CALLS ANYTHING is still on the stack when it throws, and the trace has to be
   taken THEN. quickjs defers the capture when the innermost frame is a bytecode one, because that frame's
   program counter is only synced at the unwind — but when a step machine is running, the innermost activation
   is the builtin, and the frame below it already had its pc stored by the call opcode that invoked the
   builtin. Deferring there bought nothing and cost the builtin: the machine had returned by the time the
   unwind captured, so `[1,2,3].map(1)` named the caller of map and never map. */
var threw = null;
function callsMap() { [1, 2, 3].map(1); }
try { callsMap(); } catch (e) { threw = e.stack.split("\n").map(function (l) { return l.replace(/ \(.*/, ""); }); }
assert.sameValue(threw[0], "    at Array.map", "the builtin that threw is the innermost frame");
assert.sameValue(threw[1], "    at callsMap", "and its caller is below it");
