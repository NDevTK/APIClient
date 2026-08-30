/* See core/xml/xml_external_id.h. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_external_id.h"
#include "core/xml/xml_literal.h"

/* §4.2.2's [75] `ExternalID ::= 'SYSTEM' S SystemLiteral | 'PUBLIC' S PubidLiteral S SystemLiteral`, AS ITS
   TWO KEYWORDS AND NOWHERE ELSE IN THIS TREE. §1.2 Terminology's `match` performs no case folding, so
   `system` is not this string and no production of [75] matches it. The byte compare is exact for
   core/xml/xml_literal.h's reason: every character of both keywords is ASCII, so none can occur as a
   continuation byte of some other code point. */
#define EXTID_SYSTEM     "SYSTEM"
#define EXTID_SYSTEM_LEN 6
#define EXTID_PUBLIC     "PUBLIC"
#define EXTID_PUBLIC_LEN 6

const char *xml_external_id_error_message(XmlExternalIdError err)
{
    switch (err) {
    case XML_EXTERNAL_ID_OK:
        return "no external-identifier well-formedness constraint was violated";
    case XML_EXTERNAL_ID_ERR_SPACE:
        return "fatal error (§4.2.2 External Entities): [75] ExternalID ::= 'SYSTEM' S SystemLiteral | "
               "'PUBLIC' S PubidLiteral S SystemLiteral — the S after the keyword is written in both "
               "alternatives and PUBLIC writes a second one between its two literals, and [3] S is one or "
               "more white space characters rather than an optional run";
    case XML_EXTERNAL_ID_ERR_QUOTE:
        return "fatal error (§2.3 Common Syntactic Constructs, reached from §4.2.2's [75] ExternalID): [11] "
               "SystemLiteral ::= ('\"' [^\"]* '\"') | (\"'\" [^']* \"'\") and [12] PubidLiteral opens the "
               "same two ways, and what stands here opens with neither quotation mark";
    case XML_EXTERNAL_ID_ERR_LITERAL:
        return "fatal error (§2.3 Common Syntactic Constructs' [11] SystemLiteral or [12] PubidLiteral): ask "
               "xml_literal_error_message(lit), whose sentence this is";
    case XML_EXTERNAL_ID_ERR_CHARACTER:
        return "fatal error (§2.2 Characters or §4.3.3 Character Encoding in Entities): ask "
               "xml_char_error_message(r->fatal), whose sentence this is";
    }
    DFAIL("xml_external_id_error_message was handed a value that is not an XmlExternalIdError — the enum is "
          "the whole list of sentences this component can report and a value outside it names no constraint");
    return "";
}

/* §2.3's [3] `S`, as the RUN [75] spells `S`. Answers how many characters it consumed, because [75] writes
   `S` and never `S?` — zero is never a legal answer to this production, which is the one thing the count is
   for. A latched character error is the CALLER's to notice, exactly as it is in every other scan in this
   family. */
static size_t eat_s(XmlCharReader *r)
{
    size_t n = 0;

    for (;;) {
        XmlCharReader at = *r;
        uint32_t cp = 0;

        if (xml_char_read(r, &cp) != XML_CHAR_OK) return n;
        if (cp == XML_CHAR_EOF || !xml_char_is_s(cp)) { *r = at; return n; }
        n++;
    }
}

/* CONSUME A KEYWORD THE CALLER HAS ALREADY PEEKED, through the READER rather than by advancing the cursor, so
   `line` and `column` count it — the position a `parsererror` quotes for anything after it is measured from
   there. The reads cannot fail: the peek matched the bytes and every character of both keywords is ASCII. */
static void eat_keyword(XmlCharReader *r, const char *kw, size_t kw_len)
{
    size_t i;

    DCHECK((size_t)(r->end - r->p) >= kw_len && memcmp(r->p, kw, kw_len) == 0,
           "an external-identifier scan consumed a keyword its peek had not matched");
    for (i = 0; i < kw_len; i++) {
        uint32_t cp = 0;
        XmlCharError e = xml_char_read(r, &cp);

        DCHECK(e == XML_CHAR_OK && cp == (uint32_t)(unsigned char)kw[i],
               "an external-identifier keyword did not read back the characters its peek matched — the peek "
               "is a byte compare over ASCII and the reader produces those same bytes as characters, so a "
               "disagreement means the two spellings of that keyword have drifted apart");
        (void)e;
    }
}

static bool at_keyword(const XmlCharReader *r, const char *kw, size_t kw_len)
{
    return (size_t)(r->end - r->p) >= kw_len && memcmp(r->p, kw, kw_len) == 0;
}

bool xml_external_id_at(const XmlCharReader *r)
{
    DCHECK(r != NULL, "an external-identifier peek was asked for with no reader");
    DCHECK(r->fatal == XML_CHAR_OK,
           "an external-identifier peek was taken from a reader that has already reported a fatal error — "
           "§1.2 Terminology: once a fatal error is detected the processor MUST NOT continue normal "
           "processing, so the caller owes a stop here and not another construct");
    return at_keyword(r, EXTID_SYSTEM, EXTID_SYSTEM_LEN) || at_keyword(r, EXTID_PUBLIC, EXTID_PUBLIC_LEN);
}

/* [75]'s `S Literal` PAIR, WHICH BOTH ALTERNATIVES ARE MADE OF — one required run of [3] S and one literal
   the caller names by handing in the scan that reads it. Written once because writing it three times is how
   the `S` before PUBLIC's system identifier ends up spelled `S?` by accident. */
static XmlExternalIdError scan_space_and_literal(XmlCharReader *r, bool pubid,
                                                 const char **raw, size_t *raw_len, XmlLiteralError *lit)
{
    XmlLiteralError le;
    size_t          n = eat_s(r);

    /* THE LATCH IS TESTED BEFORE THE COUNT AND BEFORE THE PEEK, and both halves of that matter. A run that
       stopped because §2.2's [2] Char was violated may have consumed nothing, which would otherwise be
       reported as a missing space — a plausible diagnosis of the wrong thing, sending an author to §4.2.2 to
       check a keyword that is correct. And it may equally have consumed several characters first, in which
       case the count says nothing is wrong and the peek below would run on a reader §1.2 Terminology says
       the processor MUST NOT continue from. */
    if (r->fatal != XML_CHAR_OK) return XML_EXTERNAL_ID_ERR_CHARACTER;
    if (n == 0) return XML_EXTERNAL_ID_ERR_SPACE;
    if (!xml_literal_at(r)) return XML_EXTERNAL_ID_ERR_QUOTE;
    le = pubid ? xml_literal_scan_pubid(r, raw, raw_len) : xml_literal_scan_system(r, raw, raw_len);
    if (le != XML_LITERAL_OK) {
        *lit = le;
        /* THE CHARACTER LAYER'S SENTENCE IS THE MORE SPECIFIC ONE and the literal layer says so by name, so it
           is asked for here rather than being carried out under a heading that names the literal instead. */
        return le == XML_LITERAL_ERR_CHARACTER ? XML_EXTERNAL_ID_ERR_CHARACTER : XML_EXTERNAL_ID_ERR_LITERAL;
    }
    return XML_EXTERNAL_ID_OK;
}

XmlExternalIdError xml_external_id_scan(XmlCharReader *r, XmlExternalId *out, XmlLiteralError *lit)
{
    XmlCharReader  start;
    XmlExternalId  id;
    XmlExternalIdError e;
    bool           is_public;

    DCHECK(r != NULL && out != NULL && lit != NULL,
           "an external-identifier scan was asked for with no reader, nowhere to put the identifier, or "
           "nowhere to put the literal layer's answer");
    DCHECK(xml_external_id_at(r),
           "an external-identifier scan ran on a reader standing at neither of [75]'s two keywords — the peek "
           "is the caller's, because whether an ExternalID may stand here at all is the rule of the "
           "production that reached this one");

    *lit = XML_LITERAL_OK;
    memset(&id, 0, sizeof id);
    start = *r;
    is_public = at_keyword(r, EXTID_PUBLIC, EXTID_PUBLIC_LEN);
    eat_keyword(r, is_public ? EXTID_PUBLIC : EXTID_SYSTEM, is_public ? EXTID_PUBLIC_LEN : EXTID_SYSTEM_LEN);

    if (is_public) {
        e = scan_space_and_literal(r, true, &id.public_id, &id.public_id_len, lit);
        if (e != XML_EXTERNAL_ID_OK) goto fail;
        DCHECK(id.public_id != NULL,
               "[12] PubidLiteral scanned successfully and left no slice — core/xml/xml_literal.h states the "
               "pointer is never NULL, including for an empty run, precisely so that an EMPTY public "
               "identifier and the SYSTEM arm's ABSENT one stay two different facts here");
    }
    e = scan_space_and_literal(r, false, &id.system_id, &id.system_id_len, lit);
    if (e != XML_EXTERNAL_ID_OK) goto fail;
    DCHECK(id.system_id != NULL,
           "[11] SystemLiteral scanned successfully and left no slice — both alternatives of [75] end in one, "
           "so a NULL here is core/xml/xml_literal.h's contract broken and not an identifier with no system "
           "part");
    DCHECK(is_public || id.public_id == NULL,
           "[75]'s SYSTEM alternative produced a public identifier — its first alternative has no [12] in it "
           "at all, so a non-NULL here would make the ABSENT public identifier indistinguishable from an "
           "empty one at every consumer");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a successful external-identifier scan left the reader's §1.2 latch set — the latch is a FATAL "
           "error, after which §1.2 Terminology says a processor MUST NOT continue normal processing");
    *out = id;
    return XML_EXTERNAL_ID_OK;

fail:
    DCHECK((e == XML_EXTERNAL_ID_ERR_LITERAL) == (*lit != XML_LITERAL_OK && e != XML_EXTERNAL_ID_ERR_CHARACTER),
           "an external-identifier answer and its literal detail disagree about whether that layer reported "
           "anything");
    /* PUT A FAILED SCAN'S READER BACK, with the family's one carve-out and for its reason — the guard is keyed
       on the character layer's §1.2 LATCH and never on the error value, because restoring a saved reader over
       a set latch would put it back to XML_CHAR_OK and silently un-report a fatal error. */
    if (r->fatal == XML_CHAR_OK) *r = start;
    else DCHECK(e == XML_EXTERNAL_ID_ERR_CHARACTER,
                "an external-identifier scan left the character layer's §1.2 latch set while reporting a "
                "failure that does not name that layer — a latch was set on a path that does not say so, and "
                "the sentence a report quotes would be the wrong one");
    return e;
}
