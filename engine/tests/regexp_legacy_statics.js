/*---
description: >
  RegExp.$1, RegExp.lastMatch, RegExp.leftContext and the rest — proposal-regexp-legacy-features, Stage 3 and
  shipped by every browser. A page bundle that reads RegExp.$1 threw here, and a throw during boot is the end
  of that bundle's run, so their absence was a capability gap and not a conformance detail. The slots belong
  to %RegExp%, so there is one set per realm; `empty` is the state they start in and the state a NON-legacy
  regexp's match puts them back into, and the getters answer it with a TypeError rather than undefined.
---*/
/* empty until something matches */
assert.throws(TypeError, function () { return RegExp.lastMatch; });
assert.throws(TypeError, function () { return RegExp.$1; });

/(\d+)-(\w+)/.exec("ab 123-xy cd");
assert.sameValue(RegExp.input, "ab 123-xy cd");
assert.sameValue(RegExp.$_, "ab 123-xy cd");
assert.sameValue(RegExp.lastMatch, "123-xy");
assert.sameValue(RegExp["$&"], "123-xy");
assert.sameValue(RegExp.lastParen, "xy");
assert.sameValue(RegExp["$+"], "xy");
assert.sameValue(RegExp.leftContext, "ab ");
assert.sameValue(RegExp["$`"], "ab ");
assert.sameValue(RegExp.rightContext, " cd");
assert.sameValue(RegExp["$'"], " cd");
assert.sameValue(RegExp.$1, "123");
assert.sameValue(RegExp.$2, "xy");
assert.sameValue(RegExp.$3, "", "a group that does not exist reads as the empty string, not undefined");

/* lastParen is the last group that PARTICIPATED, over all of them and not just the first nine */
/(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(L)/.exec("abcdefghijkL");
assert.sameValue(RegExp.lastParen, "L");
assert.sameValue(RegExp.$9, "i");

/* a group that did not participate is empty, and does not become lastParen */
/(x)|(y)/.exec("y");
assert.sameValue(RegExp.$1, "");
assert.sameValue(RegExp.$2, "y");
assert.sameValue(RegExp.lastParen, "y");

/* the one writable slot coerces, and that coercion is the page's code */
RegExp.input = 42;
assert.sameValue(RegExp.$_, "42");
var coerced = 0;
RegExp.input = { toString: function () { coerced++; var n = 0; for (var i = 0; i < 300; i++) n += i; return "via-" + n; } };
assert.sameValue(coerced, 1);
assert.sameValue(RegExp.input, "via-44850");

/* a SUBCLASS match invalidates them: a page must not be able to smuggle values into an intrinsic every other
   script on the origin can read */
/(keep)/.exec("keep");
assert.sameValue(RegExp.$1, "keep");
class R extends RegExp {}
new R("(sneak)").exec("sneak");
assert.throws(TypeError, function () { return RegExp.$1; });
assert.throws(TypeError, function () { return RegExp.lastMatch; });

/* …and the receiver check is on every one of them */
var desc = Object.getOwnPropertyDescriptor(RegExp, "lastMatch");
assert.throws(TypeError, function () { return desc.get.call({}); });
assert.throws(TypeError, function () { return desc.get.call(RegExp.prototype); });
var inp = Object.getOwnPropertyDescriptor(RegExp, "input");
assert.throws(TypeError, function () { inp.set.call({}, "x"); });

/* a failed match leaves them alone */
/(again)/.exec("again");
assert.sameValue(/nope/.test("zzz"), false);
assert.sameValue(RegExp.$1, "again");

/* Function.prototype.toString on a built-in must PARSE as a NativeFunction, and `$&` is not an
   IdentifierName — so `function get $&() { [native code] }` was not one. PropertyName is optional in that
   production, so a name that cannot appear in it is omitted; the accessor prefix stays, because that part is
   grammar. The `name` PROPERTY is untouched — the spec pins it and only the source rendering is
   implementation-defined. Caught by test262's built-in-function-object.js, which walks every intrinsic. */
function getterOf(o, k) { return Object.getOwnPropertyDescriptor(o, k).get; }
assert.sameValue(String(getterOf(RegExp, "$&")), "function get () {\n    [native code]\n}");
assert.sameValue(String(getterOf(RegExp, "$+")), "function get () {\n    [native code]\n}");
assert.sameValue(String(getterOf(RegExp, "$`")), "function get () {\n    [native code]\n}");
assert.sameValue(String(getterOf(RegExp, "$'")), "function get () {\n    [native code]\n}");
assert.sameValue(getterOf(RegExp, "$&").name, "get $&", "the name property is the spec's, not the rendering's");

/* …and a name that IS an IdentifierName, or already the computed form, still appears */
assert.sameValue(String(getterOf(RegExp, "$1")), "function get $1() {\n    [native code]\n}");
assert.sameValue(String(getterOf(RegExp, "lastMatch")), "function get lastMatch() {\n    [native code]\n}");
assert.sameValue(String(getterOf(RegExp, Symbol.species)),
                 "function get [Symbol.species]() {\n    [native code]\n}");
assert.sameValue(String(Object.getOwnPropertyDescriptor(RegExp, "$_").set),
                 "function set $_() {\n    [native code]\n}");
assert.sameValue(String(Array.prototype.map), "function map() {\n    [native code]\n}");
