/*---
description: >
  CAPTURE_COUNT_MAX and REGISTER_COUNT_MAX were both 255, because a capture or register index was a BYTE in the
  compiled byte code. Both were caps on distinct work: a pattern with three hundred groups has an answer, and
  the parser said "too many captures"; three hundred nested counted quantifiers got "too many imbricated
  quantifiers". The index is a u32 now, so what remains is memory.
---*/
function groups(n) { var s = ""; for (var i = 0; i < n; i++) s += "(a)"; return s; }

var re = new RegExp("^" + groups(400) + "$");
var m = re.exec("a".repeat(400));
assert.sameValue(m.length, 401);
assert.sameValue(m[400], "a");
assert.sameValue(m[1], "a");

/* a back reference to a group past 255 */
var re2 = new RegExp("^" + groups(300) + "\\300$");
assert.sameValue(re2.test("a".repeat(301)), true);
assert.sameValue(re2.test("a".repeat(300) + "b"), false);

/* named groups past 255 */
var src = "";
for (var i = 0; i < 300; i++) src += "(?<g" + i + ">a)";
var re3 = new RegExp("^" + src + "$");
assert.sameValue(re3.exec("a".repeat(300)).groups.g299, "a");

/* nested counted quantifiers past the old 255 register limit */
var q = "a", close = "";
for (var i = 0; i < 300; i++) { q = "(?:" + q; close += "){1,2}"; }
assert.sameValue(new RegExp("^" + q + close + "$").test("a"), true);
