/* CSSOM §2.1 — COMMON SERIALIZING IDIOMS, the three primitives every serialize-a-CSS-rule arm is stated over.
 *
 * THEY ARE ONE COMPONENT BECAUSE THE SPEC STATES THEM ONCE. §6.4's CSSNamespaceRule arm is "serialize an
 * identifier of the prefix ... serialize as URL of the namespaceURI", its CSSImportRule arm is "serialize a
 * URL on the rule's location", its CSSFontFaceRule arm is "serialize a string on the rule's font family name",
 * and §8.1's `CSS.escape()` is serialize-an-identifier under another name. A copy of the escape table per
 * caller is a copy that can disagree, and the disagreement is invisible: every one of them produces a string
 * that LOOKS like CSS.
 *
 * THE ESCAPES ARE ON CODE POINTS, NOT ON BYTES, and that is what the UTF-8 walk below is for. §2.1's rules are
 * "for each character": everything at or above U+0080 is emitted AS ITSELF, so a multi-byte sequence passes
 * through untouched — but "is the first character" and "is the second character and the first is a `-`" are
 * questions about characters, so a byte walk would ask them of a continuation byte. The escapes themselves are
 * all ASCII, which is why the pass-through can be byte-wise once the position questions are answered.
 *
 * A LONE SURROGATE PASSES THROUGH UNCHANGED, AND THAT IS THE CSSOMString BINDING CHOICE MADE EXPLICIT.
 *
 * CSSOM §3 CSSOMString does not decide it: "Most strings in CSSOM interfaces use the CSSOMString type. Each
 * implementation chooses to define it as either USVString or DOMString", and it states the consequence —
 * "DOMString would preserve them, whereas USVString would replace them with U+FFFD REPLACEMENT CHARACTER" —
 * then says outright that "this choice effectively allows implementations to do this replacement, but does not
 * require it". THIS ENGINE CHOOSES DOMString, and every CSSOM member's argument declaration says so
 * (IDL_DOMSTRING, never IDL_USVSTRING).
 *
 * IT IS NOT A FREE CHOICE IN PRACTICE, WHICH IS WHY IT IS WRITTEN DOWN HERE RATHER THAN LEFT TO EACH MEMBER.
 * §8.1's own test file asserts `CSS.escape('\uD834')` is `'\uD834'` and `CSS.escape('\uDF06')` is `'\uDF06'` —
 * a lone surrogate RETURNED UNCHANGED — which the scalar-value conversion Web IDL §3.2.12 USVString performs
 * would destroy. So a USVString binding is permitted by CSSOM §3 and refuted by the corpus, and the sentence
 * that stood here claiming this engine had one was wrong about this tree in the direction that reads as
 * authoritative: the members were right and the claim above them was not.
 *
 * SO WHAT ARRIVES HERE IS NOT GUARANTEED TO BE WELL-FORMED UTF-8, and the walk below does not pretend it is.
 * quickjs encodes a JS string's unmatched surrogate rather than replacing it, so `ED A0 B4` reaches this
 * component and must leave it byte-identical — which the pass-through arm already does, because §2.1's rules
 * escape nothing at or above U+0080. The DCHECK is therefore about a TRUNCATED or otherwise unopenable
 * sequence, which no encoder produces and which would mean a caller handed over a partial buffer; it is not a
 * surrogate check, and a replacement character substituted here would break §8.1's tests and hide whatever
 * produced the bytes. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SERIALIZE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SERIALIZE_H

#include <stddef.h>

/* §2.1's SERIALIZE A STRING: `"`, then each character under §2.1's four rules, then `"`. NULL becomes U+FFFD,
   a C0 control or U+007F is escaped as a code point, `"` and `\` are escaped as characters, and `'` is NOT
   escaped — "because strings are always serialized with U+0022". OWNED: the caller frees. */
char *css_serialize_string(const char *s, size_t len);

/* §2.1's SERIALIZE AN IDENTIFIER, which is also §8.1's `CSS.escape()`. OWNED. */
char *css_serialize_identifier(const char *s, size_t len);

/* §2.1's SERIALIZE A URL: `url(`, the URL serialized AS A STRING, `)`. It is never the bare `url(x)` form a
   page may have written — every engine round-trips `@import url(a.css)` as `@import url("a.css");` — because
   the spec names serialize-a-string here and gives no unquoted arm at all. OWNED. */
char *css_serialize_url(const char *s, size_t len);

#endif
