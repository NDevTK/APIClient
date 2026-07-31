/*---
description: >
  A regexp MATCH is unbounded work — that is what ReDoS is: /(a+)+b/ on a run of a's backtracks exponentially.
  lre_exec_backtrack never recursed (it has always been an explicit backtracking stack), but "does not recurse"
  is not "can be interrupted": its position lived in C locals, so the engine handed the thread away until the
  match finished. The position (program counter, input cursor, both stack cursors) is the caller's
  REExecContext, so the loop's back-edges are suspension points and the match resumes at the exact opcode —
  never re-matching. lre_check_timeout, which truncated a match instead of parking it, is gone.
---*/
var redos = /^(a+)+b$/;
assert.sameValue(redos.test("aaaaaaaaaaaaaaaaaaaa"), false);
assert.sameValue(redos.test("aaaaaaaaaaaaaaaaaaaab"), true);

/* nested quantifiers with a capture: the captures must survive every suspension unchanged */
var m = /^(?:(a+)(b+))+c$/.exec("aabbaaabbbc");
assert.sameValue(m[0], "aabbaaabbbc");
assert.sameValue(m[1], "aaa");
assert.sameValue(m[2], "bbb");

/* a global match walks lastIndex across many separate suspended matches */
var g = /a+b/g, seen = [], r;
while ((r = g.exec("aab xx aaab x ab")) !== null) seen.push(r[0] + "@" + r.index);
assert.sameValue(seen.join(","), "aab@0,aaab@7,ab@14");

/* backreferences and lookbehind hold state across the same back-edges */
assert.sameValue(/^(\w+)\s+\1$/.test("abc abc"), true);
assert.sameValue(/(?<=\d{3})x/.exec("123x")[0], "x");

/* a long counted loop: every iteration is a back-edge */
assert.sameValue(/^(?:ab){2000}$/.test("ab".repeat(2000)), true);
assert.sameValue(/^(?:ab){2000}$/.test("ab".repeat(1999)), false);
