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
 * A LONE SURROGATE OR AN ILL-FORMED SEQUENCE IS NOT THIS COMPONENT'S TO REPAIR. CSSOMString is a USVString in
 * this engine's binding and the CSS tokenizer's own output is well-formed UTF-8, so what arrives here is
 * already scalar values; the walk asserts that rather than substituting a replacement character, because a
 * substitution here would hide a decoder bug one component away. */
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
