/*---
description: >
  ClassSetExpression — `[[[[a]]]]` in `v` mode — recursed on the C stack for each level of nesting, with the
  depth chosen by the input, and the lre_check_stack_overflow in front of re_parse_nested_class turned that
  into "stack overflow": the same bound in an error's clothing the disjunction parser had. One frame per
  nested class now, with a child's result delivered into the level above's scratch. That hook has no callers
  left in the engine and is deleted.
---*/
assert.sameValue(new RegExp("[".repeat(20000) + "a" + "]".repeat(20000), "v").test("a"), true);

var sub = "a";
for (var i = 0; i < 5000; i++) sub = "[" + sub + "--[b]]";
assert.sameValue(new RegExp(sub, "v").test("a"), true);

var inter = "[a-z]";
for (var i = 0; i < 5000; i++) inter = "[" + inter + "&&[a-y]]";
assert.sameValue(new RegExp(inter, "v").test("q"), true);
assert.sameValue(new RegExp(inter, "v").test("z"), false);

/* the shallow shapes have to stay exactly right. `[[a--[b]]]` is the one that caught a real bug: the inner
   class descends for its own `--` operand, and writing that descent's return address onto the PARENT frame
   as well as the child's made the level above subtract where it should have unioned. */
assert.sameValue(/[[a--[b]]]/v.test("a"), true);
assert.sameValue(/[[a--[b]]--[c]]/v.test("a"), true);
assert.sameValue(/[[a]--[[b]]]/v.test("a"), true);
assert.sameValue(/[[a-z]--[aeiou]]/v.test("b"), true);
assert.sameValue(/[[a-z]--[aeiou]]/v.test("e"), false);
assert.sameValue(/[[a-z]&&[aeiou]]/v.test("e"), true);
assert.sameValue(/[[a-z]&&[aeiou]]/v.test("b"), false);
assert.sameValue(/[\q{abc|d}]/v.test("abc"), true);
assert.sameValue(/[^[a-c]]/v.test("z"), true);
assert.sameValue(/[^[a-c]]/v.test("b"), false);

/* a malformed nested class must still be a SyntaxError, and must not leak the levels it had opened */
assert.throws(SyntaxError, function () { new RegExp("[[a--[b]", "v"); });
assert.throws(SyntaxError, function () { new RegExp("[[a]--", "v"); });
assert.throws(SyntaxError, function () { new RegExp("[" + "[a--[b]]".repeat(1) + "&&", "v"); });

/* 22.2.1: MayContainStrings is a STATIC property of the productions, not a question about what survived. The
   intersection of two string disjunctions is empty, and negating it is still an early error — reading
   n_strings instead let this one through, which is the gap the code's own XXX had named. */
assert.throws(SyntaxError, function () { new RegExp("[^\\q{foo}&&\\q{bar}]", "v"); });
assert.throws(SyntaxError, function () { new RegExp("[^\\q{foo}--\\q{bar}]", "v"); });
assert.throws(SyntaxError, function () { new RegExp("[^\\q{foo}]", "v"); });
assert.throws(SyntaxError, function () { new RegExp("[^\\q{}]", "v"); });        /* the empty string is a string */
assert.throws(SyntaxError, function () { new RegExp("[^[\\q{foo}]]", "v"); });

/* …and the same shapes UNnegated stay valid, including the one whose strings all cancel */
assert.sameValue(new RegExp("[\\q{foo}&&\\q{bar}]", "v").test("foo"), false);
assert.sameValue(new RegExp("[\\q{foo}&&\\q{foo}]", "v").test("foo"), true);
assert.sameValue(new RegExp("[\\q{foo}--\\q{bar}]", "v").test("foo"), true);
assert.sameValue(new RegExp("[^\\q{a}]", "v").test("b"), true);   /* a one-character \q is not a string */
