/*---
description: >
  A CallSite can answer the three questions that are about the frame's RECEIVER: isToplevel, getThis and
  getTypeName. They were absent, so V8's own stack formatter — the one mjsunit installs for every assertion
  failure — threw on `each.isEval()` and fell into its catch, and the frame it could not describe was reported
  by re-reading error.stack, which is how the recursion this fork now guards against was reached in the first
  place.

  A frame did not carry a receiver at all. Adding one exposed two more: a coroutine base copied the field
  before the value was stored, so the top-level script's receiver was whatever the allocator had left there
  (a stack trace reported it as an integer), and no trampolined frame ever initialised is_constructor, so
  CallSite#isConstructor had been reading uninitialised memory since the trampoline was built.

  `this` here is the BINDING, not the raw receiver: 10.2.1.2 OrdinaryCallBindThis converts for a non-strict
  function, and quickjs performs that conversion at the body's OP_push_this and keeps the result in the
  function's own `this` var — so the exact object is read out of the live frame.

  None of this is in ECMAScript, which is why it is here.
---*/
var frames = null;
Error.prepareStackTrace = function (e, sites) {
    return sites.map(function (s) {
        var t = s.getTypeName();
        return [s.getFunctionName(), s.isToplevel(), t === null ? "-" : t,
                typeof s.getThis(), s.isConstructor()].join("|");
    });
};
function capture() { frames = new Error("x").stack; return frames; }

function Foo() {}
Foo.prototype.method = function method() { return capture()[1]; };
assert.sameValue(new Foo().method(), "method|false|Foo|object|false",
                 "a method call names the receiver's type and is not top level");

function plain() { return capture()[1]; }
assert.sameValue(plain(), "plain|true|-|object|false",
                 "a non-strict plain call binds `this` to the global object, so it IS top level");

function Ctor() { this.frame = capture()[1]; }
assert.sameValue(new Ctor().frame, "Ctor|false|-|object|true",
                 "a construct reports isConstructor and no type name — the formatter spells it `new Ctor`");

var strictFrame = (function () {
    "use strict";
    return ({ q: function q() { return capture()[1]; } }).q();
})();
assert.sameValue(strictFrame, "q|false|Object|undefined|false",
                 "a strict frame names its receiver's type but does not hand the receiver over");

assert.sameValue(capture()[0], "capture|true|-|object|false",
                 "the top-level script's own receiver is the global object, not uninitialised memory");
