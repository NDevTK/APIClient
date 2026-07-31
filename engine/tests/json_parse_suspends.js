/*---
description: >
  JSON.parse is unbounded work, and until the JP_PARSE stage existed it was work the scheduler could not
  interrupt: the parse ran to completion inside one opcode however its frame stack was written. It is a step
  machine now whose parse yields once per COMPLETED VALUE — an exact suspension, resumed at the same character
  with the same tokenizer position and the same frame stack, never a re-parse. Both spellings park: the
  no-reviver one too.
---*/
var n = 500, parts = [], i;
for (i = 0; i < n; i++) parts.push('{"k":[' + i + ',"s' + i + '",null,true]}');
var txt = "[" + parts.join(",") + "]";

var v = JSON.parse(txt);
assert.sameValue(v.length, n);
assert.sameValue(v[n - 1].k[0], n - 1);
assert.sameValue(v[0].k[1], "s0");
assert.sameValue(v[17].k[2], null);
assert.sameValue(v[17].k[3], true);

/* the reviver path suspends across the same parse AND then across its own walk */
var seen = 0;
var w = JSON.parse(txt, function (k, val) { seen++; return val; });
assert.sameValue(w.length, n);
assert.sameValue(w[n - 1].k[3], true);
assert.sameValue(seen, n * 6 + 1);   /* per element: the object, its "k", 4 members; plus the root array as "" */

/* a parse that fails LATE has already yielded many times; the throw must still be the spec's */
assert.throws(SyntaxError, function () { JSON.parse(txt.slice(0, txt.length - 1)); });
assert.throws(SyntaxError, function () { JSON.parse(txt + "x"); });

/* the FIRST token failing tears the machine down with the parse still marked live — the one path that reaches
   js_json_parse_abandon through an ordinary program, and the reason it is not dead code. */
assert.throws(SyntaxError, function () { JSON.parse("@"); });
assert.throws(SyntaxError, function () { JSON.parse("[1,@]"); });

/* the value grammar is a frame stack, so nesting has an answer at every depth rather than a RangeError */
var deep = "[".repeat(200000) + "1" + "]".repeat(200000);
var d = JSON.parse(deep);
for (i = 0; i < 200000; i++) d = d[0];
assert.sameValue(d, 1);
