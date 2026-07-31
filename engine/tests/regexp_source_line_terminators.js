/*---
description: >
  RegExp.prototype.source escapes every LineTerminator, so the result can be read back as a literal.

  22.2.6.13.1 EscapeRegExpPattern requires the returned string, wrapped in slashes, to parse as a
  RegularExpressionLiteral with the same meaning — and a literal admits no LineTerminator ANYWHERE: not at the
  top level, not inside a class, and not after a backslash. Two of the three cases were missing. U+2028 and
  U+2029 went through raw, and the backslash arm copied the next code unit verbatim, so an escaped line
  terminator stayed raw too.

  test262 covers exactly one shape here (a raw LF), which is why the corpus was green while
  `new RegExp(" ").source` returned something that cannot be parsed back. The assertion below is the
  spec's own: read it back and it must match what the original matched.
---*/
var LS = " ", PS = " ";

function roundTrip(pattern, subject, name) {
    var original = new RegExp(pattern);
    var reparsed = eval("/" + original.source + "/");
    assert.sameValue(reparsed.source, original.source, name + ": the source is stable under a round trip");
    assert.sameValue(reparsed.test(subject), original.test(subject),
                     name + ": the round trip matches the same input");
    assert.sameValue(original.test(subject), true, name + ": and it matched to begin with");
}

roundTrip(LS, LS, "a raw U+2028");
roundTrip(PS, PS, "a raw U+2029");
roundTrip("\n", "\n", "a raw LF");
roundTrip("\r", "\r", "a raw CR");
roundTrip("a" + LS + "b", "a" + LS + "b", "a line separator between two characters");
roundTrip("[" + LS + "]", LS, "a line separator inside a class");
roundTrip("\\" + LS, LS, "an ESCAPED line separator — the pair denotes the character, so it escapes as one");
roundTrip("\\" + "\n", "\n", "an escaped LF");
roundTrip("\\" + "\r", "\r", "an escaped CR");

/* The spellings that were already right stay right. */
assert.sameValue(new RegExp("/").source, "\\/", "a slash outside a class is escaped");
assert.sameValue(new RegExp("[/]").source, "[/]", "and inside one it is an ordinary class character");
assert.sameValue(new RegExp("\\\\").source, "\\\\", "an escaped backslash is left alone");
assert.sameValue(new RegExp("").source, "(?:)", "the empty pattern still reads back as a literal");
