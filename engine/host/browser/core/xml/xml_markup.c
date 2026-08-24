/* See xml_markup.h. */
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_markup.h"

/* §2.7's [19] `CDStart ::= '<![CDATA['` and [21] `CDEnd ::= ']]>'`, written down ONCE — the peek predicate and
   the scan read the same two literals, which is what makes "the caller peeked" an assertion rather than a
   convention between two spellings. */
static const char CDSTART[] = "<![CDATA[";
static const char CDEND[] = "]]>";
/* THE BRACKETS OF [21] `CDEnd` THAT STAND BEFORE ITS `>`, derived from the literal rather than written as a
   `2` — the scan finds the delimiter by breaking on the `>`, so every position it computes is measured
   BACKWARDS from that character and the count has to come from the same place the string does. */
#define CDEND_BRACKETS (sizeof CDEND - 2)

const char *xml_markup_error_message(XmlMarkupError err)
{
    switch (err) {
    case XML_MARKUP_OK:
        return "no markup-construct well-formedness constraint was violated";
    case XML_MARKUP_ERR_UNTERMINATED_CDATA_SECTION:
        return "fatal error (§2.7 CDATA Sections): a CDATA section ends with the string \"]]>\" — [18] CDSect "
               "is [19] CDStart, [20] CData and [21] CDEnd, and this section has no CDEnd";
    case XML_MARKUP_ERR_CHARACTER:
        return "fatal error inside a markup construct, detected by §2.2/§4.3.3's character layer — the "
               "reader's own latch names which one";
    }
    DFAIL("xml_markup_error_message was handed a value that is not an XmlMarkupError — the enum is the whole "
          "list of sentences this component can report and a value outside it names no constraint");
    return "";
}

bool xml_markup_at_cdsect(const XmlCharReader *r)
{
    DCHECK(r != NULL, "a CDATA-section peek was asked for with no reader");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a CDATA-section peek was taken from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing, so "
           "the caller owes a stop here and not another construct");
    DCHECK(r->start <= r->p && r->p <= r->end,
           "a CDATA-section peek was taken from a reader whose cursor is outside its own entity");
    return (size_t)(r->end - r->p) >= sizeof CDSTART - 1
        && memcmp(r->p, CDSTART, sizeof CDSTART - 1) == 0;
}

XmlMarkupError xml_markup_scan_cdsect(XmlCharReader *r, XmlMarkupText *out)
{
    XmlCharReader start, before;
    const char *content;
    size_t i, brackets = 0, removed = 0, raw_len;
    uint32_t cp = 0;

    DCHECK(r != NULL && out != NULL, "a CDATA-section scan was asked for with no reader or nowhere to put it");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a CDATA section was scanned from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing");
    DCHECK(xml_markup_at_cdsect(r),
           "a CDATA-section scan was handed a reader that does not stand at [19] CDStart — a caller in [43] "
           "content decides between this construct, a comment, a processing instruction and a tag, and that "
           "decision is its grammar rule, so a reader standing anywhere else is a caller that has not peeked "
           "and this is not a document to report about");
    start = *r;

    /* [19] `CDStart`, consumed through the READER rather than by advancing the cursor, so `line` and `column`
       count the delimiter's own characters — the position a `parsererror` quotes for anything inside the
       section is measured from here. None of these reads can fail: the peek above matched their bytes, and
       every one of them is ASCII. */
    for (i = 0; i < sizeof CDSTART - 1; i++) {
        XmlCharError e = xml_char_read(r, &cp);

        DCHECK(e == XML_CHAR_OK && cp == (uint32_t)(unsigned char)CDSTART[i],
               "[19] CDStart did not read back the characters the peek matched — the peek is a byte compare "
               "over ASCII and the reader produces those same bytes as characters, so a disagreement means "
               "the two spellings of this delimiter have drifted apart");
        (void)e;
    }
    content = r->p;

    /* [20] `CData ::= (Char* - (Char* ']]>' Char*))` — every Char until [21] CDEnd, and NOTHING ELSE IS
       MARKUP. §2.7: "Within a CDATA section, only the CDEnd string is recognized as markup", so an `&` here
       is character data and §4.1's reference layer is never asked; "CDATA sections cannot nest", so a
       `<![CDATA[` inside is nine characters of content and the FIRST `]]>` closes the one section there is.
       THE RUN IS UNBOUNDED BY DESIGN — [20] puts no limit on it, and neither does this scan. */
    for (;;) {
        before = *r;
        if (xml_char_read(r, &cp) != XML_CHAR_OK) {
            DCHECK(r->fatal != XML_CHAR_OK, "the character layer reported a fatal error and did not latch one");
            /* THE ONE RETURN THAT DOES NOT REWIND — see xml_markup.h. Restoring `start` would restore `fatal`
               to XML_CHAR_OK and un-report the error the character layer just detected. */
            return XML_MARKUP_ERR_CHARACTER;
        }
        if (cp == XML_CHAR_EOF) {
            *r = start;
            return XML_MARKUP_ERR_UNTERMINATED_CDATA_SECTION;
        }
        /* The `]` run is counted rather than matched three characters at a time, which is what makes `]]]>`
           end a section whose content is a single `]`: [21] CDEnd is the LAST two brackets and the `>`, and a
           fixed three-character window starting at the first `]` would miss it. */
        if (cp == '>' && brackets >= 2) break;
        brackets = (cp == ']') ? brackets + 1 : 0;
        /* §2.11's removal, counted as it happens so the slice this scan hands back can be held to the
           byte-level spelling of the same rule below. A #xA the reader produced from TWO bytes is the
           `#xD #xA` pair; from one it is either a literal #xA or a lone #xD, and neither changes the length. */
        if (cp == 0x0A) {
            size_t consumed = (size_t)(r->p - before.p);

            DCHECK(consumed == 1 || consumed == 2,
                   "§2.11's line break was decoded from a number of bytes it cannot have — a #xA is one byte, "
                   "a lone #xD is one, and the #xD#xA pair is two, so a third length means the reader "
                   "produced this character from something that is not a line break");
            removed += consumed - 1;
        }
    }

    /* `before.p` is the byte the closing `>` stands at, because the read that ended the loop was taken from
       there. [21] CDEnd is that `>` and the two brackets before it, both of which are single ASCII bytes. */
    DCHECK((size_t)(before.p - content) >= CDEND_BRACKETS,
           "a CDATA section closed before its content could hold [21] CDEnd's own brackets — the loop counts "
           "a `]` run and breaks on a `>`, so fewer than two bytes behind that `>` means the run was counted "
           "over characters that are not in this slice");
    DCHECK(memcmp(before.p - CDEND_BRACKETS, CDEND, sizeof CDEND - 1) == 0,
           "the bytes a CDATA-section scan measured [21] CDEnd from are not `]]>` — the run counter and the "
           "byte positions are two views of one delimiter and a disagreement means the content slice is "
           "measured against the wrong place");
    raw_len = (size_t)(before.p - content) - CDEND_BRACKETS;

    DCHECK(raw_len - removed == xml_char_normalized_len(content, raw_len),
           "the number of bytes §2.11 removed while this scan READ the section and the number its byte-level "
           "spelling measures over the same slice disagree — those are one rule written twice, and a caller "
           "materializing this content would build a node the parser never read");
    out->raw = content;
    out->raw_len = raw_len;

    DCHECK(r->p > start.p && r->fatal == XML_CHAR_OK,
           "a CDATA-section scan succeeded without consuming anything, so a caller looping over [43] content "
           "would never advance");
    DCHECK(out->raw >= start.start && out->raw + out->raw_len <= r->p,
           "a CDATA section's content does not lie inside the run the scan consumed — it is a borrowed slice "
           "and not a copy, so a slice outside means it was measured against something else");
    return XML_MARKUP_OK;
}
