/* See xml_name.h. */
#include <stdint.h>
#include <string.h>

#include <lexbor/encoding/decode.h>

#include "check.h"
#include "core/xml/xml_name.h"

/* THE PRODUCTIONS, TRANSCRIBED IN §2.3'S OWN ORDER AND SPLIT THE WAY §2.3 SPLITS THEM: [4] is the whole of
 * NameStartChar, and [4a] is "NameStartChar plus six more ranges", so the second table holds ONLY those six and
 * `name_char` asks the first as well. Keeping the six separate is not a size saving — it is what makes "a
 * NameChar that is not a NameStartChar" a fact this file STATES rather than one a reader has to find by
 * diffing two lists, and that difference is the whole of the U+00B7 case.
 *
 * ONE TRANSLATION UNIT, ON THE EVIDENCE, AND BY core/fetch/port_blocking.c'S OWN TEST. That file keeps Fetch
 * §2.9's 83-row table with the three-step algorithm that reads it, because "those two files would only ever
 * change together, the algorithm is meaningless without the table and the table is unreadable without the
 * sentence that says what a row MEANS"; what core/url separates into idna_table.h and public_suffix_table.h is
 * 9251 and 2596 lines of MACHINE-DERIVED data with no algorithm attached at all. Twenty-two ranges
 * hand-transcribed from three lines of spec text, read by two predicates that mean nothing without them, are
 * the first situation and not the second.
 *
 * AND THE TRANSCRIPTION IS HELD TO THE STANDARD'S OWN ORDER BY A DCHECK ON EVERY LOOKUP — port_blocking.c's
 * mechanism, for its reason: both productions are listed in strictly ascending, non-overlapping order, so a
 * transposed or duplicated range is a mis-transcription of a normative table that no compiler can catch and
 * whose only symptom would be a code point silently changing class. */
typedef struct { uint32_t lo, hi; } XmlCpRange;

/* [4] NameStartChar ::= ":" | [A-Z] | "_" | [a-z] | [#xC0-#xD6] | [#xD8-#xF6] | [#xF8-#x2FF] | [#x370-#x37D]
 *                     | [#x37F-#x1FFF] | [#x200C-#x200D] | [#x2070-#x218F] | [#x2C00-#x2FEF] | [#x3001-#xD7FF]
 *                     | [#xF900-#xFDCF] | [#xFDF0-#xFFFD] | [#x10000-#xEFFFF]
 *
 * The COLON is in it, and that is deliberate in the standard: §2.3's own note says authors should not use a
 * colon except for namespace purposes but "XML processors MUST accept the colon as a name character". Which is
 * why `xml:fail` is in that WPT file's VALID list — Namespaces in XML narrows this to NCName for the names IT
 * governs, and that narrowing belongs to the namespace layer rather than here. */
static const XmlCpRange NAME_START_CHAR[] = {
    { ':', ':' }, { 'A', 'Z' }, { '_', '_' }, { 'a', 'z' },
    { 0x00C0, 0x00D6 }, { 0x00D8, 0x00F6 }, { 0x00F8, 0x02FF }, { 0x0370, 0x037D },
    { 0x037F, 0x1FFF }, { 0x200C, 0x200D }, { 0x2070, 0x218F }, { 0x2C00, 0x2FEF },
    { 0x3001, 0xD7FF }, { 0xF900, 0xFDCF }, { 0xFDF0, 0xFFFD }, { 0x10000, 0xEFFFF },
};

/* [4a] NameChar ::= NameStartChar | "-" | "." | [0-9] | #xB7 | [#x0300-#x036F] | [#x203F-#x2040] */
static const XmlCpRange NAME_CHAR_ONLY[] = {
    { '-', '-' }, { '.', '.' }, { '0', '9' }, { 0x00B7, 0x00B7 },
    { 0x0300, 0x036F }, { 0x203F, 0x2040 },
};

#define XML_CP_RANGE_N(t) (sizeof(t) / sizeof((t)[0]))

/* The walk is LINEAR and exhaustive rather than a binary search, for port_blocking.c's reason: a linear walk
   depends on no ordering, so a transposed row cannot make a code point silently change class, while the DCHECK
   still holds the table to the order §2.3 prints it in. */
static bool in_ranges(const XmlCpRange *r, size_t n, uint32_t cp)
{
    size_t i;

    for (i = 0; i < n; i++) {
        DCHECK(r[i].lo <= r[i].hi && (i == 0 || r[i].lo > r[i - 1].hi),
               "an XML §2.3 character-class table is not strictly ascending and non-overlapping — it is "
               "transcribed range by range in the standard's own order, so a reversed pair or a transposition "
               "here is a mis-transcription of a normative production and the range it displaced is a class of "
               "code points this engine would get wrong in both directions");
        if (cp >= r[i].lo && cp <= r[i].hi) return true;
    }
    return false;
}

static bool name_start_char(uint32_t cp)
{
    return in_ranges(NAME_START_CHAR, XML_CP_RANGE_N(NAME_START_CHAR), cp);
}

static bool name_char(uint32_t cp)
{
    return name_start_char(cp) || in_ranges(NAME_CHAR_ONLY, XML_CP_RANGE_N(NAME_CHAR_ONLY), cp);
}

bool xml_name_is_name(const char *s, size_t len)
{
    const lxb_char_t *p = (const lxb_char_t *)s;
    const lxb_char_t *end = p + len;
    bool first = true;

    /* [5] is ONE NameStartChar and then zero or more NameChars, so the empty string is not a Name — which is
       what makes `document.createProcessingInstruction("", "x")` an InvalidCharacterError and not a node. */
    if (!s || len == 0) return false;
    while (p < end) {
        const lxb_char_t *was = p;
        lxb_codepoint_t cp = lxb_encoding_decode_valid_utf_8_single(&p, end);

        /* THE DECODER IS LEXBOR'S — bind before build: the UTF-8 walk is an existing Lexbor module function and
           not a fourth hand-rolled one in this tree. Its contract is that the bytes are well formed, and they
           are: a caller's bytes are quickjs's own string encoder's output. The one shape that reaches here
           looking unusual is an UNPAIRED SURROGATE, which that encoder KEEPS as the three-byte encoding of
           U+D800..U+DFFF; it decodes to exactly that code point, which [4] stops below at [#x3001-#xD7FF] and
           resumes above at [#xF900-#xFDCF], so it is rejected in either position. That is also XML §2.2's own
           answer — Char excludes the surrogate blocks outright — so the two agree rather than one covering for
           the other. */
        DCHECK(cp != LXB_ENCODING_DECODE_ERROR,
               "the XML Name production was asked about bytes that are not well-formed UTF-8 — every caller "
               "hands it the JS string encoder's output, so a truncated or structurally invalid sequence here "
               "is an engine bug and not a name to answer about");
        DCHECK(p > was, "the UTF-8 walk consumed no bytes, so the Name production would never terminate");
        if (!(first ? name_start_char(cp) : name_char(cp))) return false;
        first = false;
    }
    return true;
}

/* Namespaces in XML 1.0 §3 [4]. The colon test runs FIRST and is a `memchr` — see xml_name.h for why one byte
   is the whole of it — and the rest is [5] unchanged, because NCName subtracts a language from Name rather
   than restricting Name's character classes. Note what this does NOT do: it does not ask whether the first
   code point is a colon and then hand the tail to xml_name_is_name, which would accept `a:b`. */
bool xml_name_is_ncname(const char *s, size_t len)
{
    if (!s || len == 0) return false;
    if (memchr(s, ':', len) != NULL) return false;
    return xml_name_is_name(s, len);
}

bool xml_name_parse_qname(const char *s, size_t len, XmlQName *out)
{
    const char *colon;

    DCHECK(out != NULL, "the QName production was asked to parse into nothing");
    if (!s || len == 0) return false;
    colon = memchr(s, ':', len);
    if (!colon) {                                     /* [9] UnprefixedName ::= LocalPart */
        if (!xml_name_is_ncname(s, len)) return false;
        out->prefix = NULL; out->prefix_len = 0;
        out->local = s; out->local_len = len;
        return true;
    }
    /* [8] PrefixedName ::= Prefix ':' LocalPart. The split is at the FIRST colon and BOTH halves must be
       NCNames, which is what rejects a second colon: `a:b:c` splits to `a` and `b:c`, and the NCName test on
       the tail is where the extra colon is found. So there is no separate "at most one colon" rule to state —
       [10] and [11] already say it, and stating it twice is how the two spellings drift apart. */
    {
        size_t plen = (size_t)(colon - s);
        const char *local = colon + 1;
        size_t llen = len - plen - 1;

        if (!xml_name_is_ncname(s, plen)) return false;
        if (!xml_name_is_ncname(local, llen)) return false;
        out->prefix = s; out->prefix_len = plen;
        out->local = local; out->local_len = llen;
    }
    return true;
}
