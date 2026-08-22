/*---
description: >
  Two same-named capture groups are legal exactly when they lie in DIFFERENT alternatives of some common
  enclosing Disjunction. That is a question about the parse TREE, and the flat uint8_t counter that used to
  answer it could not see nesting: in `(?<b>.)((?<a>.)|(?<b>.))` the inner disjunction had already bumped the
  counter, so an outer group that is always reachable read as an alternative of the inner one and the pattern
  was accepted. The counter also WRAPPED at 255, which made two unrelated alternatives collide and rejected a
  legal pattern. Each alternative is a node with a parent now; two groups conflict iff one's alternative is an
  ancestor of, or the same as, the other's.
---*/
/* every one of these is reachable-together and must be rejected — V8's regexp-duplicate-named-groups list */
var bad = [
  "(?<a>.)(?<a>.)", "((?<a>.)(?<a>.))", "(?<a>.)((?<a>.))", "((?<a>.))(?<a>.)",
  "(?<a>(?<a>.)|.)", "(?<a>.|(?<b>.(?<b>.)|.))",
  "(?<a>.)((?<a>.)|(?<b>.))", "(?<b>.)((?<a>.)|(?<b>.))",
  "((?<a>.)|(?<b>.))(?<a>.)", "((?<a>.)|(?<b>.))(?<b>.)",
  "(?<a>.)|(?<b>.)(?:(?<c>.)|(?<b>.)(?:(?<e>.)|(?<f>.)))",
  "(?<a>.)|(?<b>.)(?:(?<c>.)|(?<d>.)(?:(?<b>.)|(?<f>.)))",
  "(?<a>.)|(?<b>.)(?:(?<c>.)|(?<d>.)(?:(?<e>.)|(?<b>.)))",
  "(?<a>.)|(?<b>.)(?:(?<c>.)|(?<d>.)(?:(?<e>.)|(?<d>.)))"
];
for (var i = 0; i < bad.length; i++) {
  var src = bad[i];
  assert.throws(SyntaxError, function () { new RegExp(src); }, src);
}

/* …and the legal shape keeps working, including through a backreference */
assert.sameValue(/(?<a>x)|(?<a>y)/.exec("y").groups.a, "y");
assert.sameValue(/(?<a>x)|(?<a>y)/.exec("x").groups.a, "x");
assert.sameValue(/(?:(?<n>1)|(?<n>2))z/.exec("2z").groups.n, "2");
assert.sameValue(/^(?:(?:(?<d>p)|(?<d>q))r)$/.exec("qr").groups.d, "q");
assert.sameValue(/(?:(?<r>a)|(?<r>b))\k<r>/.test("bb"), true);
var mix = /(?<k>a)(?<j>b)|(?<k>c)(?<j>d)/.exec("cd");
assert.sameValue(mix.groups.k, "c");
assert.sameValue(mix.groups.j, "d");

/* the group-name trailer lost the scope byte it carried and nobody read; ordinary lookup is unaffected */
var m = /(?<one>a)(b)(?<two>c)/.exec("abc");
assert.sameValue(m.groups.one, "a");
assert.sameValue(m.groups.two, "c");
assert.sameValue(m[2], "b");
assert.sameValue(m.length, 4);
assert.sameValue(/(a)(b)/.exec("ab").groups, undefined);

/* 600 alternatives all declaring the same name — the old byte wrapped here */
var alts = [];
for (var i = 0; i < 600; i++) alts.push("(?<w>x" + i + "$)");
assert.sameValue(new RegExp("^(?:" + alts.join("|") + ")").exec("x599").groups.w, "x599");
