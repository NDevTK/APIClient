/* CSS Syntax §3.3 "Preprocessing the input stream" and §4.2 "Definitions" — WHAT THE CSS TOKENIZER SEES AT A
 * POSITION OF SOURCE TEXT, AND WHICH OF §4.2's CLASSES IT IS IN.
 *
 * ITS OWN FILE BECAUSE THREE GRAMMARS ASK IT AND EACH ANSWERED IT SEPARATELY, IN THE SAME WRONG WAY. The
 * §4.2 class of a code point is not a detail of any one grammar: core/css/css_property_syntax.c needs it for
 * the Properties and Values API's syntax string, core/css/css_font_shorthand.c for css-fonts-4 §2.1.1's
 * `<font-family-name>`, and core/css/media_query.c for a media feature name. Each wrote its own predicate over
 * BYTES, and a byte test cannot express §4.2's set at all — which is the whole reason this file exists rather
 * than a fourth copy appearing beside the three.
 *
 * WHY A BYTE TEST IS NOT A NARROWING BUT A DIFFERENT ANSWER. §4.2's non-ASCII ident code point is an
 * ENUMERATED SET, not "above ASCII": it names U+00B7, fourteen further ranges and everything at or above
 * U+10000, and the holes in it are deliberate — U+00D7 MULTIPLICATION SIGN and U+00F7 DIVISION SIGN sit
 * between the letter ranges, U+037E GREEK QUESTION MARK between them and the Greek block, and U+FFFE/U+FFFF
 * above the last range. Every byte of each of their UTF-8 encodings is >= 0x80, so a byte walk admits all of
 * them, and `font-family: \xC3\x97Arial` was a valid declaration in this engine and an invalid one in a
 * browser. §4.2 states why it chose that set: "This matches the list of non-ASCII codepoints allowed to be
 * used in HTML valid custom element names".
 *
 * §3.3 IS PART OF THE ANSWER AND NOT A SEPARATE STEP A CALLER MIGHT SKIP, WHICH IS WHY THE WALK AND THE
 * PREDICATES ARE ONE COMPONENT. §4.2 is asked of the FILTERED code points, and §3.3's second bullet is what
 * decides two of the cases a reader would otherwise get backwards: "Replace any U+0000 NULL or surrogate code
 * points in input with U+FFFD REPLACEMENT CHARACTER". U+FFFD is inside §4.2's U+FDF0 to U+FFFD range, so a NUL
 * and a lone surrogate are both IDENT-START code points — a predicate that decoded and then asked §4.2 without
 * the filter would reject them, which is a browser divergence the byte view did not have for the surrogate
 * (its three bytes are each >= 0x80) and DID have for the NUL (0x00 is not). §3.3's own note says where a
 * surrogate comes from: "The only way to produce a surrogate code point in CSS content is by directly
 * assigning a DOMString with one in it via an OM operation", which is exactly the path every caller here is
 * on.
 *
 * ILL-FORMED UTF-8 ANSWERS U+FFFD AND DOES NOT ASSERT, and the two halves of that have different reasons.
 * It does not assert because a stylesheet's bytes are a STRANGER'S — CLAUDE.md's rule that a DCHECK may only
 * stand on a value this codebase computed — so a malformed sequence is input and not an engine bug. It
 * answers U+FFFD because that is what the standard already did: §3.2 "The input byte stream" decodes the
 * stream with the Encoding Standard's decode, whose error mode is replacement, so by the time §4.2 is asked
 * anything the ill-formed sequence IS a U+FFFD. The engine holds decoded source as UTF-8 rather than as an
 * array of code points, so the two steps are folded into one walk here; the ANSWER is the browser's either
 * way.
 *
 * WHAT IT IS NOT: A TOKENIZER. §4.3's algorithms — a valid escape, would-start-an-ident-sequence, consume an
 * ident sequence — are each grammar's own, because each of the three callers implements only the part of §4.3
 * its own production needs and a shared one would be a fourth CSS tokenizer beside lexbor's. This file answers
 * about ONE code point, which is the part all three genuinely share. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CODE_POINT_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CODE_POINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* §4.2's EOF code point: "A conceptual code point representing the end of the input stream." It is above
   §4.2's maximum allowed code point ("The greatest code point defined by Unicode: U+10FFFF"), so no predicate
   here can confuse the two and a caller may pass it to any of them — core/xml/xml_char.h spells its own EOF
   the same way and for the same reason. */
#define CSS_CP_EOF ((uint32_t)0xFFFFFFFFu)

/* §3.3's FILTERED CODE POINT at `p`, over decoded source text this engine holds as UTF-8. `end` is one past
   the last byte; `p == end` is the end of the stream and answers CSS_CP_EOF.
     `*n_out`, when it is not NULL, is how many BYTES the answer stands for, which is what a caller advances
   by. It is never zero for a code point, so a walk always terminates. It is 2 for the one case where §3.3
   collapses two code points into one — "Replace any U+000D CARRIAGE RETURN (CR) code points, U+000C FORM FEED
   (FF) code points, or pairs of U+000D CARRIAGE RETURN (CR) followed by U+000A LINE FEED (LF) in input by a
   single U+000A LINE FEED (LF) code point" — so a CRLF is ONE U+000A and a caller cannot see the CR.
     A CALLER THAT COPIES SOURCE TEXT MUST RE-ENCODE THE ANSWER RATHER THAN THE BYTES IT STANDS FOR, because
   for a filtered code point they are different: the two bytes of a CRLF, the one byte of a NUL and the bytes
   of an ill-formed sequence each stand for a code point the source does not spell. */
uint32_t css_cp_at(const char *p, const char *end, size_t *n_out);

/* §4.2's non-ASCII ident code point — "A code point whose value is any of" the fifteen entries it lists, of
   which "All of these ranges are inclusive". */
bool css_cp_is_non_ascii_ident(uint32_t cp);
/* §4.2's ident-start code point: "A letter, a non-ASCII ident code point, or U+005F LOW LINE (_)". §4.2's
   letter is an uppercase or lowercase ASCII letter and nothing else, so this is never `isalpha`, whose answer
   is the C locale's. */
bool css_cp_is_ident_start(uint32_t cp);
/* §4.2's ident code point: "An ident-start code point, a digit, or U+002D HYPHEN-MINUS (-)". */
bool css_cp_is_ident(uint32_t cp);

#endif
