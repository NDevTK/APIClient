/*---
description: >
  A character that needs a code unit above 0xFF cannot match any unit of a Latin1 subject, so a program that
  must consume one cannot match — and the loop in FRONT of it need never be explored. Without that,
  `/(((.*)*)*x)Ā/` on an ASCII subject backtracks 2^n times before reaching the character that already
  decided the answer: measured on this engine, instant at 8 subject characters and unbounded at 12. The
  analysis is a least fixpoint over the compiled program — a character opcode no one-byte unit can satisfy has
  no successor — consulted once per opcode, which is what prunes a single dead ALTERNATIVE rather than only a
  dead program. It is NOT a bound: a bound refuses work that has an answer, this proves the answer is "no
  match" and stops computing it.
---*/
/* V8's mjsunit regexp-capture-3 NoHang list, verbatim. Every one of these hangs without the filter. */
function NoHang(re) { "This is an ASCII string that could take forever".match(re); }
NoHang(/(((.*)*)*x)Ā/);
NoHang(/(((.*)*)*Ā)foo/);
NoHang(/Ā(((.*)*)*x)/);
NoHang(/[ćăĀ](((.*)*)*x)/);
NoHang(/(((.*)*)*x)[ćăĀ]/);
NoHang(/[^\x00-\xff](((.*)*)*x)/);
NoHang(/(((.*)*)*x)[^\x00-\xff]/);
NoHang(/(?!(((.*)*)*x)Ā)foo/);
NoHang(/(?!(((.*)*)*x))Ā/);
NoHang(/(?=(((.*)*)*x)Ā)foo/);
NoHang(/(?=(((.*)*)*x))Ā/);
NoHang(/(?=Ā)(((.*)*)*x)/);
NoHang(/(æ|ø|Ā)(((.*)*)*x)/);
NoHang(/(a|b|(((.*)*)*x))Ā/);
NoHang(/(a|(((.*)*)*x)ă|(((.*)*)*x)Ā)/);
NoHang(/(((.*)*)*x)Ā{2}/);
NoHang(/(((.*)*)*x)Ā{2,}/);
NoHang(/(((.*)*)*x)Ā{5,10}/);
NoHang(/(((.*)*)*x)Ā{5,}/);
NoHang(/(((.*)*)*x).{2}Ā/);
NoHang(/(((.*)*)*x).{2,}Ā/);
NoHang(/(((.*)*)*x).{2,10}Ā/);
NoHang(/(((.*)*)*x).{0,2}Ā/);
NoHang(/(((.*)*)*x).{5,10}Ā/);
NoHang(/(((.*)*)*x)(.?){5,10}Ā/);

/* THE CORRECTNESS GATE. Pruning a live path is a wrong answer, which is worse than the hang it replaces, so
   every shape where the non-Latin1 atom is OPTIONAL — or where the subject is two-byte after all — is here. */
assert.sameValue(/Ā/.exec("xĀy")[0], "Ā");
assert.sameValue(/Ā/.exec("xĀy").index, 1);
assert.sameValue(/[ćăĀ]+/.exec("qăĀc")[0], "ăĀ");
assert.sameValue(/(((.*)*)*x)Ā/.exec("abxĀ")[0], "abxĀ");
assert.sameValue(/(a|(b)ă|(c)Ā)/.exec("zcĀ")[0], "cĀ");
assert.sameValue(/(?!Ā)./.exec("Āb")[0], "b");
assert.sameValue(/(?=Ā)./.exec("xĀ")[0], "Ā");

assert.sameValue(/Ā/.exec("xy"), null);
assert.sameValue(/[ćăĀ]/.exec("abc"), null);
assert.sameValue(/(a|(b)ă|(c)Ā)/.exec("zab")[0], "a", "a live branch beside two dead ones");
assert.sameValue(/(?!Ā)b/.exec("ab")[0], "b", "a negative lookaround whose body cannot match HOLDS");
assert.sameValue(/(?=Ā)b/.exec("ab"), null, "…and a positive one does not");

assert.sameValue(/aĀ?b/.exec("ab")[0], "ab", "an OPTIONAL non-Latin1 atom must not kill the program");
assert.sameValue(/aĀ*b/.exec("ab")[0], "ab");
assert.sameValue(/a(Ā|b)c/.exec("abc")[0], "abc");
assert.sameValue(/aĀ{0,3}b/.exec("ab")[0], "ab");
assert.sameValue(/a[^Ā]b/.exec("axb")[0], "axb", "a NEGATED class containing it matches everything else");
assert.sameValue(/[\x41-Ā]+/.exec("ABC")[0], "ABC", "a range CROSSING into Latin1 is live");
assert.sameValue(/[ā-Ȁ]/.exec("ABC"), null);

/* the same regexp object against subjects of both widths, in both orders */
var re = /(((.*)*)*x)Ā/;
assert.sameValue(re.exec("abcx"), null);
assert.sameValue(re.exec("abxĀ")[0], "abxĀ");
assert.sameValue(re.exec("abcx"), null);

assert.sameValue(/Ā/i.exec("xĀ")[0], "Ā");
assert.sameValue(/Ā/i.exec("xy"), null);
assert.sameValue(/aĀ?b/i.exec("AB")[0], "AB");
assert.sameValue(/(?<w>ab)Ā?\k<w>/.exec("abab")[0], "abab");
assert.sameValue(/(?<=Ā)b/.exec("Āb")[0], "b");
assert.sameValue(/(?<=Ā)b/.exec("ab"), null);
