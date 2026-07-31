/*---
description: >
  GetSubstitution's `$<name>` reads the PAGE's `groups` object — an accessor, or a Proxy trap — and ToString's
  whatever comes back. Both ran from C, and they were the LAST drive-to-completion the harness reported: a
  `groups` getter containing a loop preempted in an activation with no flow base. The template walk is a
  resumable sub-sequence now, parking on that read and on the coercion after it. Every other form (`$&`,
  '$' backtick, `$'`, `$1`..`$99`) reads a matched substring or the engine's own captures array and runs
  nothing, so those stay straight-line.
---*/
assert.sameValue("abc".replace("b", "[$&]"), "a[b]c");
assert.sameValue("a1b".replace(/(\d)/, "<$1>"), "a<1>b");
assert.sameValue("a1b".replace(/(?<d>\d)/, "<$<d>>"), "a<1>b");
assert.sameValue("a1b".replace(/(?<d>\d)/, "<$<zz>>"), "a<>b");
assert.sameValue("xaby".replace(/ab/, "[$`|$'|$&]"), "x[x|y|ab]y");
assert.sameValue("aXbXc".replaceAll("X", "[$`]"), "a[a]b[aXb]c");

/* a `groups` GETTER with a loop in it: the case that used to drive to completion */
function withGroups(makeGroups) {
  var rx = /(?<g>b)/;
  rx.exec = function (s) {
    var m = ["b"];
    m.index = 1;
    m.input = s;
    makeGroups(m);
    return m;
  };
  return rx;
}
assert.sameValue("abc".replace(withGroups(function (m) {
  Object.defineProperty(m, "groups", { get: function () {
    var n = 0;
    for (var i = 0; i < 400; i++) n += i;
    return { g: "G" + n };
  }});
}), "<$<g>>"), "a<G79800>c");

/* …and a VALUE whose toString loops: the second request in the same sub-sequence */
assert.sameValue("abc".replace(withGroups(function (m) {
  m.groups = { g: { toString: function () {
    var n = 0;
    for (var i = 0; i < 300; i++) n += i;
    return "T" + n;
  }}};
}), "<$<g>>"), "a<T44850>c");

/* …and a Proxy, whose `get` trap is the page's code on every key */
assert.sameValue("abc".replace(withGroups(function (m) {
  m.groups = new Proxy({}, { get: function () {
    var n = 0;
    for (var i = 0; i < 200; i++) n += i;
    return "P" + n;
  }});
}), "<$<g>>"), "a<P19900>c");

/* a throw from either request propagates out of the replace, and the half-built buffer is not leaked */
assert.throws(RangeError, function () {
  "abc".replace(withGroups(function (m) {
    Object.defineProperty(m, "groups", { get: function () { throw new RangeError("from the getter"); }});
  }), "<$<g>>");
});
assert.throws(RangeError, function () {
  "abc".replace(withGroups(function (m) {
    m.groups = { g: { toString: function () { throw new RangeError("from toString"); } } };
  }), "<$<g>>");
});

/* two named substitutions in one template: the walk resumes mid-string and keeps its cursor */
assert.sameValue("abc".replace(withGroups(function (m) {
  m.groups = new Proxy({}, { get: function (t, k) { return "<" + String(k) + ">"; } });
}), "[$<one>|$<two>]"), "a[<one>|<two>]c");
