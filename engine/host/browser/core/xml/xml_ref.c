/* See xml_ref.h. */
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_name.h"
#include "core/xml/xml_ref.h"

/* §4.6 Predefined Entities' five, in the standard's own order — "A set of general entities (amp, lt, gt,
 * apos, quot) is specified for this purpose" — with the character each escapes taken from the declarations
 * §4.6 prints for them: `<!ENTITY lt "&#38;#60;">`, `<!ENTITY gt "&#62;">`, `<!ENTITY amp "&#38;#38;">`,
 * `<!ENTITY apos "&#39;">`, `<!ENTITY quot "&#34;">`.
 *
 * THE MATCH IS EXACT BYTES AND THAT IS A RULE, NOT AN IMPLEMENTATION CHOICE. §1.2 Terminology defines match
 * as "Two strings or names being compared are identical ... No case folding is performed", so `&AMP;` is not
 * this entity — it is a general entity reference to a name no declaration in this table answers, which is
 * §4.2's question and, in a document with no DTD, `[WFC: Entity Declared]`'s fatal error. An ASCII-folding
 * lookup here would silently make a namespace-ill-formed document parse, and it is the exact mistake
 * core/xml/xml_ns.h refuses lexbor's interning hashes for. */
static const struct { const char *name; size_t len; uint32_t cp; } PREDEFINED[] = {
    { "amp",  3, 0x26 },   /* AMPERSAND */
    { "lt",   2, 0x3C },   /* LESS-THAN SIGN */
    { "gt",   2, 0x3E },   /* GREATER-THAN SIGN */
    { "apos", 4, 0x27 },   /* APOSTROPHE */
    { "quot", 4, 0x22 },   /* QUOTATION MARK */
};

/* ONE CODE POINT ABOVE [2] `Char`'s CEILING, as the accumulator's saturation value. A character reference may
   carry arbitrarily many digits — `&#99999999999999;` is a perfectly good match for [66] — so the accumulator
   would overflow long before the production ended, and an overflowed value that WRAPPED into [2] Char's range
   would report a legal character for a reference that names nothing. Saturating at a value [2] rejects makes
   `[WFC: Legal Character]` the answer for every out-of-range reference, which is what it is. */
#define XML_REF_CP_OVER ((uint32_t)0x110000u)

const char *xml_ref_error_message(XmlRefError err)
{
    switch (err) {
    case XML_REF_OK:
        return "no reference-level well-formedness constraint was violated";
    case XML_REF_ERR_NOT_A_REFERENCE:
        return "fatal error (§2.4 Character Data and Markup): the ampersand character MUST NOT appear in its "
               "literal form except as a markup delimiter — this one begins neither §4.1's [66] CharRef nor "
               "its [68] EntityRef, and MUST be escaped as \"&amp;\" or a character reference";
    case XML_REF_ERR_NO_DIGITS:
        return "fatal error (§4.1 Character and Entity References): [66] CharRef is '&#' followed by ONE OR "
               "MORE decimal digits, or '&#x' followed by one or more hexadecimal digits, and this reference "
               "has none";
    case XML_REF_ERR_UNTERMINATED:
        return "fatal error (§4.1 Character and Entity References): a reference is closed by a semicolon — "
               "[66] CharRef and [68] EntityRef both end in ';' and this one does not";
    case XML_REF_ERR_LEGAL_CHARACTER:
        return "fatal error (§4.1 Character and Entity References, [WFC: Legal Character]): characters "
               "referred to using character references MUST match the production for Char";
    case XML_REF_ERR_CHARACTER:
        return "fatal error inside a reference, detected by §2.2/§4.3.3's character layer — the reader's own "
               "latch names which one";
    }
    DFAIL("xml_ref_error_message was handed a value that is not an XmlRefError — the enum is the whole list of "
          "sentences this component can report and a value outside it names no constraint");
    return "";
}

/* [66]'s two digit classes, EXACTLY as it writes them: `[0-9]` for the decimal alternative and `[0-9a-fA-F]`
   for the hexadecimal one. Returns -1 for a character that is not a digit of this base, which is how both the
   `+` and the end of the run are decided. */
static int digit_value(uint32_t cp, unsigned base)
{
    DCHECK(base == 10 || base == 16, "a character reference was accumulated in a base [66] does not have — the "
                                     "production's two alternatives are decimal and hexadecimal and there is "
                                     "no third");
    if (cp >= '0' && cp <= '9') return (int)(cp - '0');
    if (base == 16) {
        if (cp >= 'a' && cp <= 'f') return (int)(cp - 'a') + 10;
        if (cp >= 'A' && cp <= 'F') return (int)(cp - 'A') + 10;
    }
    return -1;
}

/* [66] `CharRef ::= '&#' [0-9]+ ';' | '&#x' [0-9a-fA-F]+ ';'`, entered with `'&#'` consumed.
 *
 * THE `x` IS LOWERCASE AND ONLY LOWERCASE. The production spells the hexadecimal prefix `'&#x'`, so `&#X41;`
 * is not a CharRef at all — its `X` is not a decimal digit, and the answer is [66]'s missing `+` rather than
 * a hexadecimal reference. A tokenizer that accepted both spellings would be reading HTML's rules into XML's
 * grammar, and it would make a document this standard says is fatally in error parse silently.
 *
 * THE PRODUCTION IS MATCHED BEFORE THE CONSTRAINT IS ASKED, and the order is the standard's: `[WFC: Legal
 * Character]` is attached TO [66], so there is no constraint to violate until [66] has matched. That is why
 * `&#x110000` with no semicolon is UNTERMINATED and not an illegal character — the second answer would name a
 * constraint about a reference the document does not contain. */
static XmlRefError scan_char_ref(XmlCharReader *r, XmlRef *out)
{
    uint32_t cp = 0, v = 0;
    unsigned base = 10;
    size_t ndigits = 0;

    if (xml_char_read(r, &cp) != XML_CHAR_OK) return XML_REF_ERR_CHARACTER;
    if (cp == 'x') {
        base = 16;
        if (xml_char_read(r, &cp) != XML_CHAR_OK) return XML_REF_ERR_CHARACTER;
    }
    for (;;) {
        int d = digit_value(cp, base);

        if (d < 0) break;
        DCHECK(v <= XML_REF_CP_OVER,
               "a character reference's accumulator is above its own saturation value — the clamp below runs "
               "after every digit, so a larger value here means the multiply it was supposed to make "
               "impossible has already wrapped");
        v = v * (uint32_t)base + (uint32_t)d;
        if (v > 0x10FFFFu) v = XML_REF_CP_OVER;
        ndigits++;
        if (xml_char_read(r, &cp) != XML_CHAR_OK) return XML_REF_ERR_CHARACTER;
    }
    if (ndigits == 0) return XML_REF_ERR_NO_DIGITS;
    if (cp != ';')    return XML_REF_ERR_UNTERMINATED;
    if (!xml_char_is_char(v)) return XML_REF_ERR_LEGAL_CHARACTER;

    out->kind = XML_REF_CHAR_REF;
    out->cp = v;
    /* A character reference HAS no [5] Name — see xml_ref.h for why this is NULL rather than an empty slice. */
    out->name = NULL;
    out->name_len = 0;
    return XML_REF_OK;
}

/* [68] `EntityRef ::= '&' Name ';'`, entered with `'&'` and the Name's first character consumed — the caller
 * has already asked §2.3's [4] NameStartChar of it, which is what told it this was [68] and not [66].
 * `name_start` is the byte the Name begins at. */
static XmlRefError scan_entity_ref(XmlCharReader *r, const char *name_start, XmlRef *out)
{
    XmlCharReader before;
    uint32_t cp = 0;
    size_t i, name_len;

    for (;;) {
        before = *r;
        if (xml_char_read(r, &cp) != XML_CHAR_OK) return XML_REF_ERR_CHARACTER;
        if (!xml_name_is_name_char(cp)) break;
    }
    /* `before.p` is the byte AFTER the last NameChar, because the read that ended the run is the one that was
       taken from there. This is the only position arithmetic in the file and it is why the reader is a POD
       value that can be copied: a peek is a copy, and the copy is what remembers where the name ended. */
    DCHECK(before.p > name_start,
           "an entity reference's Name is empty — the caller consumed a [4] NameStartChar before calling, so "
           "the name spans at least that one character and a zero-length slice means the two halves of the "
           "scan disagree about who consumed what");
    name_len = (size_t)(before.p - name_start);
    if (cp != ';') return XML_REF_ERR_UNTERMINATED;

    DCHECK(memchr(name_start, 0x0D, name_len) == NULL,
           "an entity reference's Name slice contains a literal carriage return byte — §2.11 rewrites #xD and "
           "would make the borrowed bytes differ from the characters that were scanned, but #xD is in neither "
           "[4] NameStartChar nor [4a] NameChar, so a #xD inside a Name means the scan accepted a character "
           "the production does not have");
    DCHECK(xml_name_is_name(name_start, name_len),
           "an entity reference's Name was scanned as [4] followed by [4a]* and the slice predicate for [5] "
           "Name disagrees — those are one transcription read two ways, and a disagreement means the "
           "character-at-a-time and slice spellings of §2.3 have drifted apart");

    /* §4.6: "All XML processors MUST recognize these entities whether they are declared or not." */
    for (i = 0; i < sizeof(PREDEFINED) / sizeof(PREDEFINED[0]); i++) {
        if (PREDEFINED[i].len == name_len && memcmp(PREDEFINED[i].name, name_start, name_len) == 0) {
            DCHECK(xml_char_is_char(PREDEFINED[i].cp),
                   "a §4.6 predefined entity is transcribed as a code point that is not [2] Char — all five "
                   "escape ASCII punctuation, so a row that fails this is a mis-transcription of the "
                   "declarations §4.6 prints");
            out->kind = XML_REF_PREDEFINED;
            out->cp = PREDEFINED[i].cp;
            out->name = name_start;
            out->name_len = name_len;
            return XML_REF_OK;
        }
    }
    out->kind = XML_REF_ENTITY;
    /* NOT A CHARACTER, and deliberately a value no predicate accepts — see xml_ref.h. What this name stands
       for is §4.2 Entity Declarations' answer and, in a document with no DTD, [WFC: Entity Declared]'s fatal
       error; either way it is the caller's, because only the caller knows whether a DTD was read. */
    out->cp = XML_CHAR_EOF;
    out->name = name_start;
    out->name_len = name_len;
    return XML_REF_OK;
}

XmlRefError xml_ref_scan(XmlCharReader *r, XmlRef *out)
{
    XmlCharReader start;
    const char *name_start;
    uint32_t cp = 0;
    XmlRefError err;

    DCHECK(r != NULL && out != NULL, "a reference scan was asked for with no reader or nowhere to put it");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a reference was scanned from a reader that has already reported a fatal error — §1.2 Terminology: "
           "once a fatal error is detected the processor MUST NOT continue normal processing");
    start = *r;

    /* THE `&` IS THE CALLER'S PEEK, NOT THIS COMPONENT'S DISCOVERY. [67]'s two alternatives both begin with
       one, so a caller reaches [67] by having seen it; a reader standing anywhere else is a caller that has
       not peeked, which is this engine's mistake and not the document's. The read cannot fail here for the
       same reason: it re-reads a character the caller already produced from this position. */
    (void)xml_char_read(r, &cp);
    DCHECK(cp == '&',
           "a reference scan was handed a reader that does not stand on an ampersand — §4.1's [66] CharRef and "
           "[68] EntityRef both open with one, so the caller has not peeked and this is not a document to "
           "report about");

    name_start = r->p;
    if (xml_char_read(r, &cp) != XML_CHAR_OK) {
        DCHECK(r->fatal != XML_CHAR_OK, "the character layer reported a fatal error and did not latch one");
        return XML_REF_ERR_CHARACTER;
    }

    if (cp == '#') {
        err = scan_char_ref(r, out);
    } else if (xml_name_is_name_start_char(cp)) {
        err = scan_entity_ref(r, name_start, out);
    } else {
        /* Including XML_CHAR_EOF: an entity that ends on a bare `&` has a literal ampersand in it, which is
           §2.4's MUST NOT just as much as one followed by a space is. */
        err = XML_REF_ERR_NOT_A_REFERENCE;
    }

    if (err == XML_REF_ERR_CHARACTER) {
        /* THE ONE RETURN THAT DOES NOT REWIND — see xml_ref.h. Restoring `start` would restore `fatal` to
           XML_CHAR_OK and un-report the error the character layer just detected. */
        DCHECK(r->fatal != XML_CHAR_OK,
               "a reference scan reported the character layer's fatal error while the reader's §1.2 latch is "
               "clear — this return says `ask the reader which one`, and a clear latch has no answer");
        return err;
    }
    if (err != XML_REF_OK) {
        DCHECK(r->fatal == XML_CHAR_OK,
               "a reference scan is about to rewind a reader that has latched a fatal error, which would "
               "silently clear it");
        *r = start;
        return err;
    }

    DCHECK(r->p > start.p && r->fatal == XML_CHAR_OK,
           "a reference scan succeeded without consuming anything, so a caller looping over [43] content "
           "would never advance");
    DCHECK((out->kind == XML_REF_CHAR_REF) == (out->name == NULL),
           "a scanned reference carries a Name it should not have, or lacks one it should — [66] CharRef has "
           "no [5] Name and both spellings of [68] EntityRef have one");
    DCHECK(out->kind == XML_REF_ENTITY
               ? out->cp == XML_CHAR_EOF
               : xml_char_is_char(out->cp),
           "a scanned reference's character is not [2] Char — [66]'s [WFC: Legal Character] and §4.6's five "
           "are both checked before this point, and an unresolved [68] must carry the sentinel that fails "
           "every character predicate rather than a plausible code point");
    return XML_REF_OK;
}
