/* DOM §1.4 — WHICH STRINGS MAY BE AN ELEMENT'S LOCAL NAME.
 *
 * ITS OWN FILE BECAUSE IT IS ASKED FROM TWO STANDARDS. `createElement` throws InvalidCharacterError by it and
 * HTML §4.13.1's "valid custom element name" is it plus four further requirements, so a copy inside either one
 * is a copy that drifts: the custom-element registry's was "the first byte is a-z and there is a hyphen
 * somewhere", which accepted `a-A`, accepted `annotation-xml`, and accepted every name whose later code points
 * the DOM forbids — 1704 subtests of one WPT file disagreeing with one four-line function.
 *
 * LENGTH-CARRYING, NOT NUL-TERMINATED, because U+0000 is a code point a page can put in a name and rejecting
 * it is half of what step 2.1 is for. `strlen` would answer about a prefix of the string.
 *
 * BYTES, NOT CODE POINTS, AND THAT IS EXACT. Every ASCII code point is one byte < 0x80 in UTF-8 and every
 * non-ASCII code point is a run of bytes >= 0x80, so "is an ASCII x" and "is in the range U+0080 to U+10FFFF"
 * are both byte tests — and a continuation byte of a permitted non-ASCII code point is itself >= 0x80, so a
 * byte walk accepts exactly the code-point walk's set. */
#ifndef APICLIENT_DOM_NAMES_H
#define APICLIENT_DOM_NAMES_H

#include <stdbool.h>
#include <stddef.h>

/* DOM §1.4 "valid element local name", the five steps exactly. */
bool dom_valid_element_local_name(const char *name, size_t len);

#endif
