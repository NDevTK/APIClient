/* See xml_literal.h. */
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_literal.h"

/* §2.3's [13] `PubidChar`'s PUNCTUATION SET, TRANSCRIBED VERBATIM AND IN THE STANDARD'S OWN ORDER:
 *   [13] PubidChar ::= #x20 | #xD | #xA | [a-zA-Z0-9] | [-'()+,./:=?;!*#@$_%]
 * This string is the bracket's contents character for character, including the LEADING HYPHEN, which is a
 * literal `-` and not a range operator — a set written in ascending code-point order instead would be a
 * different arrangement of the same characters and a reader could no longer diff it against the spec by eye,
 * which is the only check a hand-copied class ever gets.
 *   WHAT IS NOT IN IT is the half worth naming: there is no `"` (which is why [12] needs no subtraction for
 * the quotation mark), no `<`, no `>`, no `&`, no `[`, `]`, `{`, `}`, `|`, `\`, `^`, backtick or `~`. */
static const char PUBID_PUNCT[] = "-'()+,./:=?;!*#@$_%";

/* Length of a literal, in the characters it has rather than the bytes its array has. */
#define LIT_LEN(s) (sizeof (s) - 1)

const char *xml_literal_error_message(XmlLiteralError err)
{
    switch (err) {
    case XML_LITERAL_OK:
        return "no literal-level well-formedness constraint was violated";
    case XML_LITERAL_ERR_UNTERMINATED:
        return "fatal error (§2.3 Common Syntactic Constructs): literal data is a quoted string not "
               "containing the quotation mark used as a delimiter for that string, so the character that "
               "opened this literal is the one that MUST close it and the entity ended first";
    case XML_LITERAL_ERR_PUBID_CHAR:
        return "fatal error (§2.3 Common Syntactic Constructs): [13] PubidChar is a space, a carriage return, "
               "a line feed, an ASCII letter or digit, or one of the nineteen punctuation characters "
               "-'()+,./:=?;!*#@$_% — and a public identifier is made of nothing else";
    case XML_LITERAL_ERR_CHARACTER:
        return "fatal error inside a literal, detected by §2.2/§4.3.3's character layer — the reader's own "
               "latch names which one";
    }
    DFAIL("xml_literal_error_message was handed a value that is not an XmlLiteralError — the enum is the "
          "whole list of sentences this component can report and a value outside it names no constraint");
    return "";
}

/* READ THE NEXT CHARACTER, REMEMBERING WHERE IT STOOD — core/xml/xml_markup.c's pair, for its reason: `*at` is
   the reader as it was BEFORE the character was read, so `at->p` is the byte that character begins at and the
   closing delimiter can be measured from it. A copy is the peek and the copy is the park. */
static XmlCharError step(XmlCharReader *r, XmlCharReader *at, uint32_t *cp)
{
    *at = *r;
    return xml_char_read(r, cp);
}

/* HOW MANY BYTES §2.11 REMOVED IN PRODUCING THIS CHARACTER, counted as the scan reads so the borrowed slice
   can be held to the byte-level spelling of the same rule — core/xml/xml_markup.c's counter, and its
   reasoning: a #xA the reader produced from TWO bytes is the `#xD #xA` pair; from one it is either a literal
   #xA or a lone #xD, and neither of those changes the length. Nothing else in §2.11 removes a byte. */
static size_t eol_removed(uint32_t cp, const XmlCharReader *at, const XmlCharReader *after)
{
    size_t consumed;

    if (cp != 0x0A) return 0;
    consumed = (size_t)(after->p - at->p);
    DCHECK(consumed == 1 || consumed == 2,
           "§2.11's line break was decoded from a number of bytes it cannot have — a #xA is one byte, a lone "
           "#xD is one, and the #xD#xA pair is two, so a third length means the reader produced this "
           "character from something that is not a line break");
    return consumed - 1;
}

bool xml_literal_is_pubid_char(uint32_t cp)
{
    /* [13] PubidChar ::= #x20 | #xD | #xA | [a-zA-Z0-9] | [-'()+,./:=?;!*#@$_%] — alternative by alternative,
       in the standard's own order. See xml_literal.h for why the #xD alternative is complete and unreachable.
       The `cp <= 0x7F` guard is what makes the `memchr` a question about ASCII rather than about a byte: every
       character of PUBID_PUNCT is ASCII, so a code point above that range is in no alternative and must not be
       truncated into one. */
    if (cp == 0x20 || cp == 0x0D || cp == 0x0A) return true;
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9')) return true;
    return cp <= 0x7F && memchr(PUBID_PUNCT, (int)cp, LIT_LEN(PUBID_PUNCT)) != NULL;
}

bool xml_literal_at(const XmlCharReader *r)
{
    DCHECK(r != NULL, "a literal peek was asked for with no reader");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a literal peek was taken from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing, so "
           "the caller owes a stop here and not another construct");
    DCHECK(r->start <= r->p && r->p <= r->end,
           "a literal peek was taken from a reader whose cursor is outside its own entity");
    return r->p < r->end && (*r->p == '\'' || *r->p == '"');
}

/* [11] `SystemLiteral` and [12] `PubidLiteral` as ONE walk, because they differ in exactly one thing: whether
 * a character that is not the delimiter has a class to satisfy. [11]'s content is `[^"]*` / `[^']*` — any
 * character at all except the delimiter, which is why §2.3's note can say a SystemLiteral "can be parsed
 * without scanning for markup" — while [12]'s is `PubidChar*` / `(PubidChar - "'")*`.
 *
 * BREAKING ON THE DELIMITER IS WHAT IMPLEMENTS [12]'s SUBTRACTION, and it is worth seeing rather than
 * assuming: the apostrophe IS a PubidChar, so `(PubidChar - "'")` and `PubidChar` differ only for the
 * apostrophe-delimited alternative, and there the apostrophe has already ended the literal before the class is
 * asked. The quotation mark needs no subtraction in either alternative because [13] has no `"` in it at all,
 * which is why an apostrophe-delimited public identifier holding one reports the CLASS — that character is in
 * no alternative — rather than reporting a literal nobody closed. */
static XmlLiteralError scan(XmlCharReader *r, bool pubid, const char **raw, size_t *raw_len)
{
    XmlCharReader start, at;
    const char *content;
    size_t removed = 0;
    uint32_t quote = 0, cp = 0;

    DCHECK(r != NULL && raw != NULL && raw_len != NULL,
           "a literal scan was asked for with no reader or nowhere to put it");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a literal was scanned from a reader that has already reported a fatal error — §1.2 Terminology: "
           "once a fatal error is detected the processor MUST NOT continue normal processing");
    DCHECK(xml_literal_at(r),
           "a literal scan was handed a reader that does not stand at an apostrophe or a quotation mark — "
           "§4.2.2's [75] ExternalID decides which literal stands here from the keyword before it, and that "
           "decision is its grammar rule, so a reader standing anywhere else is a caller that has not peeked "
           "and this is not a document to report about");
    start = *r;

    if (step(r, &at, &quote) != XML_CHAR_OK) return XML_LITERAL_ERR_CHARACTER;
    DCHECK(quote == '\'' || quote == '"',
           "a literal's opening delimiter did not read back as the character its peek matched — the peek is a "
           "byte compare over two ASCII characters and the reader produces those same bytes as characters, so "
           "a disagreement means the two spellings of that delimiter have drifted apart");
    content = r->p;

    for (;;) {
        if (step(r, &at, &cp) != XML_CHAR_OK) return XML_LITERAL_ERR_CHARACTER;
        if (cp == quote) break;
        if (cp == XML_CHAR_EOF) { *r = start; return XML_LITERAL_ERR_UNTERMINATED; }
        if (pubid && !xml_literal_is_pubid_char(cp)) { *r = start; return XML_LITERAL_ERR_PUBID_CHAR; }
        removed += eol_removed(cp, &at, r);
    }
    /* `at.p` is the byte the closing delimiter stands at, because the read that ended the loop was taken from
       there — and it is a single ASCII byte, so the content ends exactly one byte before the reader does. */
    DCHECK(at.p >= content && (size_t)(r->p - at.p) == 1 && (unsigned char)*at.p == (unsigned char)quote,
           "the byte a literal scan measured its closing delimiter from is not the character that opened it, "
           "or the delimiter was decoded from more than one byte — an apostrophe and a quotation mark are "
           "both ASCII, so a disagreement means the content slice ends in the wrong place");
    DCHECK(r->p > start.p && r->fatal == XML_CHAR_OK,
           "a literal scan succeeded without consuming anything, so a caller walking §4.2.2's [75] ExternalID "
           "would never advance");
    *raw = content;
    *raw_len = (size_t)(at.p - content);
    DCHECK(*raw_len - removed == xml_char_normalized_len(*raw, *raw_len),
           "the number of bytes §2.11 removed while this scan READ the literal and the number its byte-level "
           "spelling measures over the same slice disagree — those are one rule written twice, and a caller "
           "materializing this literal would build a system or public identifier the parser never read");
    DCHECK(memchr(*raw, (int)quote, *raw_len) == NULL,
           "a literal's content contains the delimiter that closed it — §2.3: literal data is any quoted "
           "string NOT containing the quotation mark used as a delimiter for that string, so the scan has "
           "measured past its own terminator");
    return XML_LITERAL_OK;
}

XmlLiteralError xml_literal_scan_system(XmlCharReader *r, const char **raw, size_t *raw_len)
{
    return scan(r, false, raw, raw_len);
}

XmlLiteralError xml_literal_scan_pubid(XmlCharReader *r, const char **raw, size_t *raw_len)
{
    return scan(r, true, raw, raw_len);
}
