/* See xml_char.h. */
#include <string.h>

#include <lexbor/encoding/decode.h>
#include <lexbor/encoding/encode.h>

#include "check.h"
#include "core/xml/xml_char.h"

/* §2.2's [2] Char, transcribed range by range in the standard's own order:
 *   [2] Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
 * with its own inline gloss "any Unicode character, excluding the surrogate blocks, FFFE, and FFFF". The three
 * singletons are written as singletons rather than folded into a leading run because the standard writes them
 * that way and because the gap between them is the point: every other C0 control is NOT a character of an XML
 * document. */
static const XmlCpRange CHAR_RANGE[] = {
    { 0x0009, 0x0009 }, { 0x000A, 0x000A }, { 0x000D, 0x000D },
    { 0x0020, 0xD7FF }, { 0xE000, 0xFFFD }, { 0x10000, 0x10FFFF },
};

const char *xml_char_error_message(XmlCharError err)
{
    switch (err) {
    case XML_CHAR_OK:
        return "no character-level well-formedness constraint was violated";
    case XML_CHAR_ERR_ILL_FORMED_UTF8:
        return "fatal error (§4.3.3 Character Encoding in Entities): an entity encoded in UTF-8 MUST NOT "
               "contain ill-formed code unit sequences, as defined in section 3.9 of Unicode";
    case XML_CHAR_ERR_NOT_A_CHAR:
        return "fatal error (§2.2 Characters): a character of an XML document MUST match production [2] Char — "
               "tab, line feed, carriage return, and the Unicode characters excluding the surrogate blocks, "
               "#xFFFE and #xFFFF";
    }
    DFAIL("xml_char_error_message was handed a value that is not an XmlCharError — the enum is the whole list "
          "of sentences this component can report and a value outside it names no constraint");
    return "";
}

bool xml_char_in_ranges(const XmlCpRange *r, size_t n, uint32_t cp)
{
    size_t i;

    DCHECK(r != NULL && n > 0, "an XML character-class walk was handed no table — every production this walk "
                               "reads is a transcribed range list and an empty one names no class at all");
    for (i = 0; i < n; i++) {
        DCHECK(r[i].lo <= r[i].hi && (i == 0 || r[i].lo > r[i - 1].hi),
               "an XML character-class table is not strictly ascending and non-overlapping — it is transcribed "
               "range by range in the standard's own order, so a reversed pair or a transposition here is a "
               "mis-transcription of a normative production and the range it displaced is a class of code "
               "points this engine would get wrong in both directions");
        if (cp >= r[i].lo && cp <= r[i].hi) return true;
    }
    return false;
}

bool xml_char_is_char(uint32_t cp)
{
    return xml_char_in_ranges(CHAR_RANGE, XML_CP_RANGE_N(CHAR_RANGE), cp);
}

/* [3] S ::= (#x20 | #x9 | #xD | #xA)+ — one character of it. Four singletons are four comparisons and not a
   table: a range list would be four one-wide rows carrying an ordering assert about nothing, which is the
   opposite of what CHAR_RANGE's assert exists for. See xml_char.h for why #xD is here at all. */
bool xml_char_is_s(uint32_t cp)
{
    return cp == 0x20 || cp == 0x09 || cp == 0x0D || cp == 0x0A;
}

/* ONE CHARACTER OUT OF THE BYTES, through lexbor's own UTF-8 decoder — CLAUDE.md's bind-before-build order has
   an existing Lexbor module at the rung above a hand-rolled walk, and core/xml/xml_name.c reaches the same
   module for the same reason.
   THE DECODE CONTEXT IS A LOCAL AND ONLY TWO OF ITS FIELDS ARE LIVE. lxb_encoding_decode_utf_8_single reads
   `u.utf_8.need` first and reads `u.utf_8.lower` only when `need` is non-zero, so a zeroed pair is the whole
   of a fresh decoder; everything else in that struct belongs to the buffered whole-string entry points. Zero
   `need` also states the invariant this component relies on: the caller is handed a WHOLE entity, so no
   character is ever split across a call and there is no mid-sequence state for XmlCharReader to carry.
   `*after` is written in every outcome, including the two failures — this function's caller must not commit
   it on a failure, which is what makes a fatal error leave the reader standing on the offending character. */
static lxb_codepoint_t decode_one(const char *p, const char *end, const char **after)
{
    lxb_encoding_decode_t dec;
    const lxb_char_t *q = (const lxb_char_t *)p;
    lxb_codepoint_t cp;

    dec.u.utf_8.need = 0;
    dec.u.utf_8.lower = 0x00;
    cp = lxb_encoding_decode_utf_8_single(&dec, &q, (const lxb_char_t *)end);
    *after = (const char *)q;
    DCHECK(cp <= LXB_ENCODING_DECODE_MAX_CODEPOINT || cp == LXB_ENCODING_DECODE_ERROR
               || cp == LXB_ENCODING_DECODE_CONTINUE,
           "lexbor's single-character UTF-8 decoder returned a value that is neither a code point nor one of "
           "its two sentinels — this component reads that answer as a three-way choice and a fourth outcome "
           "would be silently classified as a character");
    return cp;
}

/* THE BYTES BACK OUT, through the same module the decode goes through — CLAUDE.md's bind-before-build order
   puts an existing Lexbor module above a hand-rolled walk, and `lxb_encoding_encode_utf_8_single` is the exact
   inverse of the decoder above with no WHATWG substitution in it: it encodes what it is given and answers
   LXB_ENCODING_ENCODE_ERROR above U+10FFFF rather than emitting U+FFFD. That matters here — a replacement
   character would turn a caller's bug into a character the document is then said to have contained.
   THE ENCODE CONTEXT IS A ZEROED LOCAL AND NOTHING READS IT. The UTF-8 encoder never touches `ctx` (it is
   there for the stateful encoders in the same table), which is why the whole of a fresh one is a zero. */
size_t xml_char_encode(uint32_t cp, char *dst)
{
    lxb_encoding_encode_t enc;
    lxb_char_t *q = (lxb_char_t *)dst;
    int8_t n;

    DCHECK(dst != NULL, "an XML character was encoded with nowhere to put it");
    DCHECK(xml_char_is_char(cp),
           "an XML character encode was handed a code point that is not §2.2's [2] Char — every caller has one "
           "from this file's own reader, which checked it, or from core/xml/xml_ref.h's [66] scan, which "
           "enforces [WFC: Legal Character], so a value outside the production is an engine bug and not a "
           "document that contains one");
    memset(&enc, 0, sizeof enc);
    n = lxb_encoding_encode_utf_8_single(&enc, &q, (lxb_char_t *)dst + XML_CHAR_ENCODE_MAX, (lxb_codepoint_t)cp);
    DCHECK(n >= 1 && n <= XML_CHAR_ENCODE_MAX,
           "lexbor's single-character UTF-8 encoder could not spell a code point this file had already decided "
           "is §2.2's [2] Char — the two are one module and [2] Char is a subset of what UTF-8 can encode, so "
           "a refusal here means the buffer bound and the production's ceiling have drifted apart");
    DCHECK((char *)q == dst + n, "lexbor's UTF-8 encoder advanced its cursor by a different number of bytes "
                                 "than it reported writing");
    /* TWO-SIDED, ON EVERY CALL. The encode and the decode are one rule read in two directions, and a
       mis-transcription of either would produce bytes that are not the character asked for — which nothing
       downstream could tell from a document that held those bytes. Reading it back through THIS file's
       decoder is what makes that impossible rather than unlikely; it is not a reader, so §4.3.3's Byte Order
       Mark question never arises and a legitimate `&#xFEFF;` round-trips like any other character.
       The decode stands INSIDE the assertion so that a release build does not perform it: `after` is a local
       nothing outside the check can observe, which is what makes the call side-effect-free in the sense
       check.h requires of a DCHECK condition. */
    {
        const char *after = NULL;

        DCHECK(decode_one(dst, dst + n, &after) == (lxb_codepoint_t)cp && after == dst + n,
               "an XML character did not read back as the character it was encoded from — the encoder and the "
               "decoder are one rule in two directions and a disagreement means one of the two is wrong");
        (void)after;
    }
    return (size_t)n;
}

void xml_char_reader_init(XmlCharReader *r, const char *s, size_t len)
{
    DCHECK(r != NULL, "an XML character reader was initialised over nothing");
    DCHECK(s != NULL, "an XML character reader was initialised over a null entity — an entity of zero bytes is "
                      "a document that ends immediately and still has a first byte to point at, so a null here "
                      "is a caller that has no entity rather than one with an empty entity");
    /* §4.3.3: the Byte Order Mark "is an encoding signature, not part of either the markup or the character
       data of the XML document", so the step that DETERMINED the encoding consumed it. One left in front of
       the document entity would be read as a character of §2.8's prolog, and the failure would surface as a
       grammar error about a document that has none — a wrong answer three layers from its cause. */
    DCHECK(!(len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF),
           "an XML character reader was handed an entity still carrying its UTF-8 Byte Order Mark — §4.3.3 "
           "makes that an encoding signature and not markup, so whichever step determined this entity's "
           "encoding owes its removal before a character is read");
    r->start = s;
    r->p = s;
    r->end = s + len;
    r->line = 1;
    r->column = 1;
    r->fatal = XML_CHAR_OK;
}

XmlCharError xml_char_read(XmlCharReader *r, uint32_t *out)
{
    const char *q;
    lxb_codepoint_t cp;

    DCHECK(r != NULL && out != NULL, "an XML character read was asked for with no reader or nowhere to put it");
    DCHECK(r->fatal == XML_CHAR_OK,
           "an XML character reader was read again after it reported a fatal error — §1.2 Terminology: once a "
           "fatal error is detected the processor MUST NOT continue normal processing, so the caller owes a "
           "stop here and not another character");
    DCHECK(r->start <= r->p && r->p <= r->end,
           "an XML character reader's cursor is outside its own entity — the only supported way to rewind one "
           "is to assign back a copy this component produced");
    DCHECK(r->line >= 1 && r->column >= 1,
           "an XML character reader's position is not 1-based — a `parsererror` quotes these numbers and a "
           "zero row or column names a place no author can find");

    if (r->p == r->end) {
        *out = XML_CHAR_EOF;
        return XML_CHAR_OK;
    }

    cp = decode_one(r->p, r->end, &q);
    /* CONTINUE MEANS THE ENTITY ENDED MID-SEQUENCE, which is the truncated half of §4.3.3's ill-formed case
       and not a request for more bytes: this reader is given the whole entity, so there are no more. */
    if (cp == LXB_ENCODING_DECODE_ERROR || cp == LXB_ENCODING_DECODE_CONTINUE) {
        r->fatal = XML_CHAR_ERR_ILL_FORMED_UTF8;
        return r->fatal;
    }
    if (!xml_char_is_char(cp)) {
        r->fatal = XML_CHAR_ERR_NOT_A_CHAR;
        return r->fatal;
    }
    DCHECK(q > r->p, "the UTF-8 walk consumed no bytes and reported no error, so the entity would never end");

    /* §2.11 End-of-Line Handling: "translating both the two-character sequence #xD #xA and any #xD that is not
       followed by #xA to a single #xA character".
       THE LOOKAHEAD IS A BYTE TEST AND THAT IS EXACT, by core/xml/xml_name.h's argument for the colon: #xA is
       ASCII, so in UTF-8 it is the single byte 0x0A, and that byte can never occur as a CONTINUATION byte of
       some other code point because every continuation byte is 0x80..0xBF. So "is the next character #xA" and
       "is the next byte 0x0A" are the same question — which also means a following sequence that is ill-formed
       is not decoded here, is not consumed, and reports itself on the next read. */
    if (cp == 0x0D) {
        if (q < r->end && (unsigned char)*q == 0x0A) q++;
        cp = 0x0A;
    }

    r->p = q;
    if (cp == 0x0A) { r->line++; r->column = 1; }
    else            { r->column++; }

    DCHECK(cp != 0x0D,
           "an XML character reader produced a literal carriage return — §2.11 requires that a processor behave "
           "as if every #xD in the entity were removed or replaced before any other processing, and §2.3's own "
           "note says the only #xD a production may ever match comes from a character reference, which is a "
           "layer above this one");
    DCHECK(xml_char_is_char(cp),
           "an XML character reader produced a code point that is not [2] Char after having checked it — the "
           "only value between the check and here is §2.11's replacement, and #xA is a Char");
    *out = (uint32_t)cp;
    return XML_CHAR_OK;
}

/* §2.11's rule read off the BYTES of a slice — see xml_char.h for why this spelling exists and why bytes are
   exact. The two functions are written as one loop twice rather than as a measure-then-copy pair sharing a
   helper, because the shared helper would be a callback per byte and the divergence they are exposed to is a
   MIS-TRANSCRIPTION of the rule, which a shared helper would hide by making both answers wrong together. The
   copy asserts the measurement instead, which catches exactly that. */
size_t xml_char_normalized_len(const char *s, size_t len)
{
    size_t i, n = 0;

    DCHECK(s != NULL || len == 0,
           "§2.11's normalization was measured over a null slice of non-zero length — a run of zero characters "
           "still stands AT a position in the entity, so a null pointer here is a caller with no slice rather "
           "than one with an empty slice");
    for (i = 0; i < len; i++) {
        /* "the two-character sequence #xD #xA": the #xD is not a character of its own, so it is not counted;
           the #xA after it is counted and IS the single character the pair becomes. A lone #xD is counted
           because it BECOMES a #xA — same length, different byte, which is why a caller cannot decide that a
           slice needs no copy from this number alone. */
        if ((unsigned char)s[i] == 0x0D && i + 1 < len && (unsigned char)s[i + 1] == 0x0A) continue;
        n++;
    }
    DCHECK(n <= len, "§2.11's normalization measured more characters than the slice has bytes — it only ever "
                     "removes a byte and never adds one");
    return n;
}

size_t xml_char_normalize(const char *s, size_t len, char *dst)
{
    size_t i, n = 0;

    DCHECK((s != NULL && dst != NULL) || len == 0,
           "§2.11's normalization was asked to copy a slice with no source or nowhere to put it");
    DCHECK(len == 0 || dst >= s + len || dst + len <= s,
           "§2.11's normalization was asked to write into the slice it is reading — it walks forward and the "
           "output is shorter than the input, so an overlapping copy would read bytes it had already "
           "overwritten and the corruption would look like a document that contained them");
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == 0x0D) {
            if (i + 1 < len && (unsigned char)s[i + 1] == 0x0A) continue;
            dst[n++] = 0x0A;                   /* "any #xD that is not followed by #xA" */
        } else {
            dst[n++] = (char)c;
        }
    }
    DCHECK(n == xml_char_normalized_len(s, len),
           "§2.11's normalization wrote a different number of bytes than it measured — the measure and the "
           "copy are one rule written twice, and a disagreement means one of the two transcriptions is wrong");
    DCHECK(memchr(dst, 0x0D, n) == NULL,
           "§2.11's normalization left a carriage return in its output — the whole of the rule is that a "
           "processor MUST behave as if every #xD in the entity were removed or replaced before any other "
           "processing, and §2.3's own note says the only #xD a production may ever match comes from a "
           "character reference, which is a layer above this one");
    return n;
}
