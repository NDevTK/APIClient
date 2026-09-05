/* See css_code_point.h. */
#include <string.h>

#include <lexbor/encoding/decode.h>

#include "check.h"
#include "core/css/css_code_point.h"

/* §4.2's non-ASCII ident code point, transcribed entry by entry in the standard's own order from the fifteen
 * it lists — "A code point whose value is any of" U+00B7, between U+00C0 and U+00D6, between U+00D8 and
 * U+00F6, between U+00F8 and U+037D, between U+037F and U+1FFF, U+200C, U+200D, U+203F, U+2040, between
 * U+2070 and U+218F, between U+2C00 and U+2FEF, between U+3001 and U+D7FF, between U+F900 and U+FDCF, between
 * U+FDF0 and U+FFFD, and greater than or equal to U+10000 — of which "All of these ranges are inclusive".
 *
 * THE FOUR SINGLETONS ARE WRITTEN AS SINGLETONS because the standard writes them that way and because the
 * gaps around them are the entire content of this table: U+00B7 stands alone above the C1 block, and U+200C
 * ZERO WIDTH NON-JOINER, U+200D ZERO WIDTH JOINER, U+203F UNDERTIE and U+2040 CHARACTER TIE are four
 * survivors of a General Punctuation block §4.2 otherwise excludes. Folding a singleton into a neighbour
 * would admit exactly the code points §4.2 is written to keep out.
 *
 * THE LAST ROW'S CEILING IS §4.2's OWN maximum allowed code point, "The greatest code point defined by
 * Unicode: U+10FFFF" — the entry says "greater than or equal to U+10000" with no upper bound because a code
 * point has one already, and writing it here is what lets the ordering assert below read the table as a
 * closed set of ranges. */
typedef struct { uint32_t lo, hi; } CssCpRange;

static const CssCpRange NON_ASCII_IDENT[] = {
    { 0x00B7, 0x00B7 },  { 0x00C0, 0x00D6 },  { 0x00D8, 0x00F6 },  { 0x00F8, 0x037D },
    { 0x037F, 0x1FFF },  { 0x200C, 0x200C },  { 0x200D, 0x200D },  { 0x203F, 0x203F },
    { 0x2040, 0x2040 },  { 0x2070, 0x218F },  { 0x2C00, 0x2FEF },  { 0x3001, 0xD7FF },
    { 0xF900, 0xFDCF },  { 0xFDF0, 0xFFFD },  { 0x10000, 0x10FFFF },
};

/* THE ORDERING DISCIPLINE IS core/xml/xml_char.h'S AND THE WALK IS NOT SHARED WITH IT, WHICH IS A DECISION AND
   NOT AN OVERSIGHT. That file states the reason a hand-transcribed normative range table needs an assert on
   every lookup rather than a comment: a transposed or duplicated row is a mis-transcription no compiler can
   catch and whose only symptom is a code point silently changing class. The assert earns that only by NAMING
   the production it is about — a reader who meets it has to know which standard's table is wrong — and one
   shared walk can name exactly one standard, so `xml_char_in_ranges` would report a CSS mis-transcription as
   an XML one. The table below is CSS Syntax's; the discipline is the same discipline. */
static bool cp_in_ranges(const CssCpRange *r, size_t n, uint32_t cp)
{
    size_t i;

    DCHECK(r != NULL && n > 0,
           "a CSS Syntax §4.2 code-point class was asked with no table — the only class here that is a range "
           "list is the non-ASCII ident code point, and an empty table names no code point at all");
    for (i = 0; i < n; i++) {
        DCHECK(r[i].lo <= r[i].hi && (i == 0 || r[i].lo > r[i - 1].hi),
               "CSS Syntax §4.2's non-ASCII ident code point table is not strictly ascending and "
               "non-overlapping — it is transcribed entry by entry in §4.2's own order, so a reversed pair or "
               "a transposition here is a mis-transcription of a normative set whose holes (U+00D7, U+00F7, "
               "U+037E, the surrogate blocks, U+FFFE and U+FFFF) are the whole reason it is a table");
        if (cp >= r[i].lo && cp <= r[i].hi) return true;
    }
    return false;
}

/* EVERY PREDICATE IS ASKED ABOUT A CODE POINT §3.3 HAS ALREADY FILTERED, AND THAT IS ASSERTED RATHER THAN
   ASSUMED. A NUL or a surrogate reaching §4.2 means a caller decoded source text without going through
   `css_cp_at`, which is not a narrower answer but a DIFFERENT one — U+FFFD is an ident-start code point and
   both of those are not — so the assert is what forces a new caller onto the one walk. §4.2's EOF code point
   is admitted because every caller asks about it: it is above the maximum allowed code point, so no range
   here can contain it. */
static void cp_check_filtered(uint32_t cp, const char *asked)
{
    DCHECKF(cp == CSS_CP_EOF || (cp <= 0x10FFFF && cp != 0x0000 && !(cp >= 0xD800 && cp <= 0xDFFF)),
            "CSS Syntax §4.2's %s was asked about U+%04X, which is not a filtered code point — §3.3 replaces "
            "U+0000 NULL and every surrogate with U+FFFD before §4.2 is asked anything, and a value above "
            "U+10FFFF is not a code point at all, so this operand did not come through css_cp_at",
            asked, (unsigned)cp);
}

bool css_cp_is_non_ascii_ident(uint32_t cp)
{
    cp_check_filtered(cp, "non-ASCII ident code point");
    return cp_in_ranges(NON_ASCII_IDENT, COUNTOF(NON_ASCII_IDENT), cp);
}

bool css_cp_is_ident_start(uint32_t cp)
{
    cp_check_filtered(cp, "ident-start code point");
    /* §4.2's letter is "An uppercase letter or a lowercase letter", each of which it defines as an ASCII
       range, so the two comparisons here ARE the definition and there is no locale in it. */
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_'
        || css_cp_is_non_ascii_ident(cp);
}

bool css_cp_is_ident(uint32_t cp)
{
    cp_check_filtered(cp, "ident code point");
    return css_cp_is_ident_start(cp) || (cp >= '0' && cp <= '9') || cp == '-';
}

uint32_t css_cp_at(const char *p, const char *end, size_t *n_out)
{
    const lxb_char_t *q = (const lxb_char_t *)p;
    lxb_encoding_decode_t dec;
    lxb_codepoint_t cp;
    size_t n;

    DCHECK(p != NULL && end != NULL && p <= end,
           "CSS Syntax §3.3's filter was handed a source range that is not one — a position past the end of "
           "the text names bytes no stylesheet contains, and the answer for the end of the stream is §4.2's "
           "EOF code point rather than a read");
    if (p == end) {
        if (n_out) *n_out = 0;
        return CSS_CP_EOF;
    }
    /* THE ONE SEQUENCE THE UTF-8 DECODER BELOW CALLS ILL-FORMED AND §3.3 CALLS A SURROGATE, ANSWERED HERE SO
       THAT §3.3's REPLACEMENT IS ONE CODE POINT AND NOT THREE. quickjs holds a DOMString as UTF-16 and its
       encoder KEEPS an unpaired surrogate as the three-byte encoding of U+D800 to U+DFFF, which is exactly
       the path §3.3's own note describes ("The only way to produce a surrogate code point in CSS content is
       by directly assigning a DOMString with one in it via an OM operation"). The Encoding Standard's decoder
       rejects those three bytes one at a time, so leaving it to the decoder would spell one surrogate as
       three U+FFFDs and a name built out of the source would be three code points longer than the browser's.
       Same answer for the class, different answer for the text, and the text is what a caller copies. */
    if ((size_t)(end - (const char *)q) >= 3 && q[0] == 0xED && q[1] >= 0xA0 && q[1] <= 0xBF
        && q[2] >= 0x80 && q[2] <= 0xBF) {
        if (n_out) *n_out = 3;
        return 0xFFFD;
    }
    /* THE DECODE CONTEXT IS A LOCAL AND ONLY TWO OF ITS FIELDS ARE LIVE — core/xml/xml_char.c reaches the same
       module the same way and states why: the decoder reads `u.utf_8.need` first and `u.utf_8.lower` only
       when `need` is non-zero, so a zeroed pair is the whole of a fresh decoder, and a zero `need` also states
       that this walk is handed a WHOLE buffer and never a byte at a time. */
    dec.u.utf_8.need = 0;
    dec.u.utf_8.lower = 0x00;
    cp = lxb_encoding_decode_utf_8_single(&dec, &q, (const lxb_char_t *)end);
    n = (size_t)((const char *)q - p);
    DCHECK(n >= 1,
           "lexbor's UTF-8 decoder consumed no bytes for a non-empty source range — every one of its answers "
           "advances past at least the lead byte, so a zero here would make §3.3's walk never terminate");
    if (cp == LXB_ENCODING_DECODE_ERROR || cp == LXB_ENCODING_DECODE_CONTINUE) {
        /* §3.2 "The input byte stream" decodes with the Encoding Standard, whose error mode is replacement, so
           an ill-formed sequence and a sequence truncated at the end of the text are BOTH already a U+FFFD by
           the time §4.2 is asked. This is a stylesheet's own bytes, so it is input and never an assert. */
        if (n_out) *n_out = n;
        return 0xFFFD;
    }
    DCHECK(cp <= 0x10FFFF,
           "lexbor's UTF-8 decoder returned a value that is neither a code point nor one of its two sentinels "
           "— this walk reads that answer as a three-way choice and a fourth outcome would be handed to §4.2 "
           "as a character");
    /* §3.3's first bullet. A CR followed by an LF is ONE U+000A, which is why the pair is looked at here and
       is the only answer this walk gives that stands for two code points of the source. */
    if (cp == 0x000D) {
        cp = 0x000A;
        if ((const char *)q < end && *(const char *)q == 0x0A) n++;
    } else if (cp == 0x000C) {
        cp = 0x000A;
    } else if (cp == 0x0000) {
        cp = 0xFFFD;   /* §3.3's second bullet; its surrogate half is answered above */
    }
    if (n_out) *n_out = n;
    return cp;
}
