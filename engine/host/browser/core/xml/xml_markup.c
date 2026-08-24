/* See xml_markup.h. */
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_markup.h"
#include "core/xml/xml_name.h"

/* THE SIX DELIMITERS, WRITTEN DOWN ONCE — each peek predicate and the scan behind it read the same literal,
   which is what makes "the caller peeked" an assertion rather than a convention between two spellings, and
   what makes every position a scan computes measurable against the string it came from. */
static const char COMMENT_START[] = "<!--";
static const char COMMENT_END[] = "-->";
static const char PI_START[] = "<?";
static const char PI_END[] = "?>";
static const char CDSTART[] = "<![CDATA[";
static const char CDEND[] = "]]>";

/* Length of a delimiter literal, in the characters it has rather than the bytes its array has. */
#define LIT_LEN(s) (sizeof (s) - 1)
/* THE BRACKETS OF [21] `CDEnd` THAT STAND BEFORE ITS `>`, derived from the literal rather than written as a
   `2` — the CDATA scan finds its delimiter by breaking on the `>`, so every position it computes is measured
   BACKWARDS from that character and the count has to come from the same place the string does. */
#define CDEND_BRACKETS (LIT_LEN(CDEND) - 1)

const char *xml_markup_error_message(XmlMarkupError err)
{
    switch (err) {
    case XML_MARKUP_OK:
        return "no markup-construct well-formedness constraint was violated";
    case XML_MARKUP_ERR_UNTERMINATED_COMMENT:
        return "fatal error (§2.5 Comments): a comment ends with the string \"-->\" — [15] Comment is '<!--', "
               "a run of characters, and '-->', and this comment has no '-->'";
    case XML_MARKUP_ERR_DOUBLE_HYPHEN:
        return "fatal error (§2.5 Comments): \"For compatibility, the string \"--\" (double-hyphen) MUST NOT "
               "occur within comments\" — [15] Comment's content is (Char - '-') or '-' followed by "
               "(Char - '-'), which is also why the grammar does not allow a comment ending in \"--->\"";
    case XML_MARKUP_ERR_PI_TARGET:
        return "fatal error (§2.6 Processing Instructions): [17] PITarget is a §2.3 [5] Name, which is one [4] "
               "NameStartChar followed by zero or more [4a] NameChars — and this processing instruction begins "
               "with something that is not one";
    case XML_MARKUP_ERR_PI_TARGET_RESERVED:
        return "fatal error (§2.6 Processing Instructions): [17] PITarget is Name minus "
               "(('X' | 'x') ('M' | 'm') ('L' | 'l')), so the target \"xml\" in any of its ASCII case "
               "spellings is not a PITarget at all — §2.8's [23] XMLDecl is a different production and is "
               "permitted only at the start of the document entity";
    case XML_MARKUP_ERR_PI_TARGET_END:
        return "fatal error (§2.6 Processing Instructions): [16] PI is '<?' PITarget, then either the "
               "closing '?>' immediately or [3] S and the instruction — and this target is followed by "
               "neither";
    case XML_MARKUP_ERR_UNTERMINATED_PI:
        return "fatal error (§2.6 Processing Instructions): a processing instruction ends with the string "
               "\"?>\" — [16] PI closes with one and this instruction does not";
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

/* READ THE NEXT CHARACTER, REMEMBERING WHERE IT STOOD — the pair every scan in this file walks with. `*at` is
   the reader as it was BEFORE the character was read, so `at->p` is the byte the character begins at and a
   closing delimiter can be measured backwards from it. It is a copy and not a pointer for the reason
   core/xml/xml_char.h gives for the reader being a POD at all: a copy is the peek and the copy is the park. */
static XmlCharError step(XmlCharReader *r, XmlCharReader *at, uint32_t *cp)
{
    *at = *r;
    return xml_char_read(r, cp);
}

/* HOW MANY BYTES §2.11 REMOVED IN PRODUCING THIS CHARACTER, counted as the scan reads so the borrowed slice
   it hands back can be held to the byte-level spelling of the same rule. A #xA the reader produced from TWO
   bytes is the `#xD #xA` pair; from one it is either a literal #xA or a lone #xD, and neither of those
   changes the length. Nothing else in §2.11 removes a byte. */
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

/* Does the reader stand at `lit`? The shared body of the three peeks — see xml_markup.h for why a byte
   compare is exact and why the caller owns the peek at all. */
static bool at_literal(const XmlCharReader *r, const char *lit, size_t len)
{
    DCHECK(r != NULL, "a markup-construct peek was asked for with no reader");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a markup-construct peek was taken from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing, so "
           "the caller owes a stop here and not another construct");
    DCHECK(r->start <= r->p && r->p <= r->end,
           "a markup-construct peek was taken from a reader whose cursor is outside its own entity");
    return (size_t)(r->end - r->p) >= len && memcmp(r->p, lit, len) == 0;
}

bool xml_markup_at_comment(const XmlCharReader *r) { return at_literal(r, COMMENT_START, LIT_LEN(COMMENT_START)); }
bool xml_markup_at_pi(const XmlCharReader *r)      { return at_literal(r, PI_START, LIT_LEN(PI_START)); }
bool xml_markup_at_cdsect(const XmlCharReader *r)  { return at_literal(r, CDSTART, LIT_LEN(CDSTART)); }

/* CONSUME AN OPENING DELIMITER THE CALLER HAS ALREADY PEEKED, through the READER rather than by advancing the
   cursor, so `line` and `column` count the delimiter's own characters — the position a `parsererror` quotes
   for anything inside the construct is measured from there. None of these reads can fail: the peek matched
   their bytes, and every one of them is ASCII. */
static void consume_literal(XmlCharReader *r, const char *lit, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        uint32_t cp = 0;
        XmlCharError e = xml_char_read(r, &cp);

        DCHECK(e == XML_CHAR_OK && cp == (uint32_t)(unsigned char)lit[i],
               "an opening delimiter did not read back the characters its peek matched — the peek is a byte "
               "compare over ASCII and the reader produces those same bytes as characters, so a disagreement "
               "means the two spellings of that delimiter have drifted apart");
        (void)e;
    }
}

/* The invariants every successful scan in this file owes, asserted in one place because they are one
   contract: the construct advanced, the §1.2 latch is clear, the content slice lies inside the run that was
   consumed, and the characters §2.11 removed while READING agree with what its byte-level spelling measures
   over the slice about to be handed back. That last one is the whole reason a borrowed slice is safe. */
static void check_scanned(const XmlCharReader *start, const XmlCharReader *r,
                          const XmlMarkupText *t, size_t removed)
{
    DCHECK(r->p > start->p && r->fatal == XML_CHAR_OK,
           "a markup-construct scan succeeded without consuming anything, so a caller looping over [43] "
           "content would never advance");
    DCHECK(t->raw != NULL && t->raw >= start->start && t->raw + t->raw_len <= r->p,
           "a markup construct's content does not lie inside the run the scan consumed — it is a borrowed "
           "slice and not a copy, so a slice outside means it was measured against something else");
    DCHECK(t->raw_len - removed == xml_char_normalized_len(t->raw, t->raw_len),
           "the number of bytes §2.11 removed while this scan READ the construct and the number its "
           "byte-level spelling measures over the same slice disagree — those are one rule written twice, and "
           "a caller materializing this content would build a node the parser never read");
}

/* §2.5's [15] `Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'`.
 *
 * THE CONTENT RULE AND THE TERMINATOR ARE ONE QUESTION, which is what that alternation says once it is read
 * as a scanner rather than as a generator: a body in which no two hyphens are adjacent, closed by a literal
 * '-->'. So the scan reads until it meets `--` and then asks what follows. A `>` is [15]'s close and the
 * content ended at the FIRST of the two hyphens; anything else is §2.5's "For compatibility, the string "--"
 * (double-hyphen) MUST NOT occur within comments". That single answer covers the case the standard's own note
 * calls out separately — "the grammar does not allow a comment ending in --->", with `<!-- B+, B, or B--->`
 * as its not-well-formed example — because reading left to right that document's `--` is followed by a third
 * hyphen and not by the `>`.
 *
 * AN END OF ENTITY AFTER THE `--` IS THE UNTERMINATED ANSWER AND NOT THE DOUBLE-HYPHEN ONE, because there is
 * no comment for the pair to be WITHIN: `<!--a--` is a construct the author never closed, and sending them to
 * §2.5's compatibility sentence would name a rule they did not break. */
XmlMarkupError xml_markup_scan_comment(XmlCharReader *r, XmlMarkupText *out)
{
    XmlCharReader start, at;
    const char *content;
    size_t removed = 0;
    uint32_t cp = 0;

    DCHECK(r != NULL && out != NULL, "a comment scan was asked for with no reader or nowhere to put it");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a comment was scanned from a reader that has already reported a fatal error — §1.2 Terminology: "
           "once a fatal error is detected the processor MUST NOT continue normal processing");
    DCHECK(xml_markup_at_comment(r),
           "a comment scan was handed a reader that does not stand at [15] Comment's '<!--' — a caller in "
           "[43] content decides between this construct, a processing instruction, a CDATA section and a tag, "
           "and that decision is its grammar rule, so a reader standing anywhere else is a caller that has "
           "not peeked and this is not a document to report about");
    start = *r;
    consume_literal(r, COMMENT_START, LIT_LEN(COMMENT_START));
    content = r->p;

    for (;;) {
        if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
        if (cp == XML_CHAR_EOF) { *r = start; return XML_MARKUP_ERR_UNTERMINATED_COMMENT; }
        if (cp == '-') {
            XmlCharReader hyphen = at;   /* where [15]'s closing '-->' would begin */

            if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
            if (cp == XML_CHAR_EOF) { *r = start; return XML_MARKUP_ERR_UNTERMINATED_COMMENT; }
            if (cp == '-') {
                if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
                if (cp == XML_CHAR_EOF) { *r = start; return XML_MARKUP_ERR_UNTERMINATED_COMMENT; }
                if (cp != '>') { *r = start; return XML_MARKUP_ERR_DOUBLE_HYPHEN; }
                DCHECK((size_t)(r->end - hyphen.p) >= LIT_LEN(COMMENT_END)
                           && memcmp(hyphen.p, COMMENT_END, LIT_LEN(COMMENT_END)) == 0,
                       "the bytes a comment scan measured [15]'s closing '-->' from are not that string — the "
                       "three characters it read and the byte position it recorded are two views of one "
                       "delimiter and a disagreement means the content slice ends in the wrong place");
                out->raw = content;
                out->raw_len = (size_t)(hyphen.p - content);
                check_scanned(&start, r, out, removed);
                return XML_MARKUP_OK;
            }
            /* `('-' (Char - '-'))`: the hyphen AND the character after it are content, and the hyphen itself
               can never be a line break, so only the second one is offered to §2.11's counter. */
        }
        removed += eol_removed(cp, &at, r);
    }
}

/* [17] `PITarget ::= Name - (('X' | 'x') ('M' | 'm') ('L' | 'l'))`.
 *
 * THE SUBTRACTED LANGUAGE IS EXACTLY THE THREE-CHARACTER STRINGS, WHICH IS THE HALF A PARSER WRITTEN FROM
 * MEMORY GETS WRONG. §6 Notation defines `A - B` as "matches any string that matches A but does not match B",
 * and B here is three single-character alternations in sequence — so `xml` in any of its eight ASCII case
 * spellings is excluded and NOTHING ELSE IS. `xml-stylesheet` is fourteen characters, matches no string of B,
 * and is therefore a perfectly good PITarget; §2.6's prose that "the target names "XML", "xml", and so on are
 * reserved for standardization" is a statement about what the WORKING GROUP may define, not a rule of the
 * grammar, and reading it as one would reject the single most common processing instruction on the web.
 *
 * AND THE EIGHT SPELLINGS ARE ASCII CASE AND NOTHING WIDER. §1.2 Terminology's `match` performs no case
 * folding anywhere else in this standard; [17] gets its case-insensitivity from writing the alternation out
 * by hand, which is why this is a fixed comparison against six letters and not a call to any general-purpose
 * folding. */
static bool target_is_reserved(const char *s, size_t len)
{
    return len == 3
        && (s[0] == 'X' || s[0] == 'x')
        && (s[1] == 'M' || s[1] == 'm')
        && (s[2] == 'L' || s[2] == 'l');
}

/* §2.6's [16] `PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'`.
 *
 * THE WHITE SPACE AFTER THE TARGET IS CONSUMED WHOLE, AND THE GRAMMAR DOES NOT DECIDE THAT. [3] `S` is
 * `(#x20 | #x9 | #xD | #xA)+` and the instruction that follows is `Char*`, which may itself begin with white
 * space — so `<?t  a?>` matches [16] under two different splits, one leaving the instruction as `a` and one
 * as ` a`. The LANGUAGE is the same either way, so well-formedness is unaffected and no reading is more
 * conformant than the other; what differs is the `data` of the DOM §4.7 `ProcessingInstruction` the parse
 * builds. This engine takes the whole run, which is what expat answers for `<?t  a?>`, for `<?t ?>` and for
 * `<?t\r\n a?>` — measured, not recalled — and it is the only reading under which the instruction a target's
 * application receives does not depend on how many spaces the author typed to line the document up.
 *
 * `?>` IS FOUND ON A ONE-CHARACTER LOOKAHEAD AND NOT A RUN, which is where it differs from §2.7's `]]>`:
 * `<?t a??>` closes with its LAST `?`, so the instruction is `a?`. */
XmlMarkupError xml_markup_scan_pi(XmlCharReader *r, XmlPi *out)
{
    XmlCharReader start, at;
    const char *target, *data;
    size_t target_len, removed = 0;
    uint32_t cp = 0;
    bool question = false;

    DCHECK(r != NULL && out != NULL,
           "a processing-instruction scan was asked for with no reader or nowhere to put it");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a processing instruction was scanned from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing");
    DCHECK(xml_markup_at_pi(r),
           "a processing-instruction scan was handed a reader that does not stand at [16] PI's '<?' — a "
           "caller in [43] content decides between this construct, a comment, a CDATA section and a tag, and "
           "that decision is its grammar rule, so a reader standing anywhere else is a caller that has not "
           "peeked and this is not a document to report about");
    start = *r;
    consume_literal(r, PI_START, LIT_LEN(PI_START));
    target = r->p;

    /* [17]'s `Name`, scanned as §2.3's [4] then [4a]* — core/xml/xml_name.h's per-character predicates, which
       are what a scanner has to ask because it does not yet know where the name ends. */
    if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
    if (!xml_name_is_name_start_char(cp)) { *r = start; return XML_MARKUP_ERR_PI_TARGET; }
    for (;;) {
        if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
        if (!xml_name_is_name_char(cp)) break;
    }
    target_len = (size_t)(at.p - target);
    DCHECK(target_len > 0,
           "a processing instruction's [17] PITarget is empty — the scan consumed a [4] NameStartChar before "
           "the run began, so a zero-length target means the two halves of the scan disagree about who "
           "consumed what");
    DCHECK(memchr(target, 0x0D, target_len) == NULL,
           "a [17] PITarget slice contains a literal carriage return byte — §2.11 rewrites #xD and would make "
           "the borrowed bytes differ from the characters that were scanned, but #xD is in neither [4] "
           "NameStartChar nor [4a] NameChar, so a #xD inside a target means the scan accepted a character the "
           "production does not have");
    DCHECK(xml_name_is_name(target, target_len),
           "a [17] PITarget was scanned as [4] followed by [4a]* and the slice predicate for [5] Name "
           "disagrees — those are one transcription read two ways, and a disagreement means the "
           "character-at-a-time and slice spellings of §2.3 have drifted apart");
    if (target_is_reserved(target, target_len)) { *r = start; return XML_MARKUP_ERR_PI_TARGET_RESERVED; }

    /* `cp` is the character that ENDED the Name, and [16] allows it to be exactly two things. */
    if (cp == XML_CHAR_EOF) { *r = start; return XML_MARKUP_ERR_UNTERMINATED_PI; }
    if (xml_char_is_s(cp)) {
        do {
            if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
        } while (xml_char_is_s(cp));
        data = at.p;                       /* the byte the first non-S character stands at */
    } else if (cp == '?') {
        data = at.p;                       /* an absent instruction still stands AT a position */
        if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
        if (cp != '>') { *r = start; return XML_MARKUP_ERR_PI_TARGET_END; }
        out->target = target;
        out->target_len = target_len;
        out->data.raw = data;
        out->data.raw_len = 0;
        check_scanned(&start, r, &out->data, 0);
        return XML_MARKUP_OK;
    } else {
        *r = start;
        return XML_MARKUP_ERR_PI_TARGET_END;
    }

    /* The instruction: `Char*` minus any string containing `?>`. `cp` and `at` already hold its first
       character, which is what the greedy [3] S run above left standing. */
    for (;;) {
        if (cp == XML_CHAR_EOF) { *r = start; return XML_MARKUP_ERR_UNTERMINATED_PI; }
        if (cp == '>' && question) break;
        question = (cp == '?');
        removed += eol_removed(cp, &at, r);
        if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
    }
    DCHECK((size_t)(at.p - data) >= LIT_LEN(PI_END) - 1,
           "a processing instruction closed before its instruction could hold [16]'s '?>' — the scan breaks "
           "on a `>` whose predecessor was a `?`, so nothing behind that `>` means the lookahead was set over "
           "a character that is not in this slice");
    DCHECK(memcmp(at.p - (LIT_LEN(PI_END) - 1), PI_END, LIT_LEN(PI_END)) == 0,
           "the bytes a processing-instruction scan measured [16]'s '?>' from are not that string — the "
           "one-character lookahead and the byte position it recorded are two views of one delimiter and a "
           "disagreement means the instruction slice ends in the wrong place");
    out->target = target;
    out->target_len = target_len;
    out->data.raw = data;
    out->data.raw_len = (size_t)(at.p - data) - (LIT_LEN(PI_END) - 1);
    check_scanned(&start, r, &out->data, removed);
    return XML_MARKUP_OK;
}

/* §2.7's [18] `CDSect ::= CDStart CData CDEnd`, whose [20] `CData ::= (Char* - (Char* ']]>' Char*))` is every
 * Char until [21] CDEnd and NOTHING ELSE IS MARKUP. §2.7: "Within a CDATA section, only the CDEnd string is
 * recognized as markup", so an `&` here is character data and §4.1's reference layer is never asked; "CDATA
 * sections cannot nest", so a `<![CDATA[` inside is nine characters of content and the FIRST `]]>` closes the
 * one section there is.
 *
 * THE `]` RUN IS COUNTED RATHER THAN MATCHED THREE CHARACTERS AT A TIME, which is what makes `<![CDATA[]]]>`
 * a section whose content is a single `]`: [21] CDEnd is the LAST two brackets and the `>`, and a fixed
 * three-character window that began matching at the first `]` would take `]]]` for a delimiter and run off
 * the end of the section it was supposed to close. */
XmlMarkupError xml_markup_scan_cdsect(XmlCharReader *r, XmlMarkupText *out)
{
    XmlCharReader start, at;
    const char *content;
    size_t brackets = 0, removed = 0;
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
    consume_literal(r, CDSTART, LIT_LEN(CDSTART));
    content = r->p;

    for (;;) {
        if (step(r, &at, &cp) != XML_CHAR_OK) return XML_MARKUP_ERR_CHARACTER;
        if (cp == XML_CHAR_EOF) { *r = start; return XML_MARKUP_ERR_UNTERMINATED_CDATA_SECTION; }
        if (cp == '>' && brackets >= CDEND_BRACKETS) break;
        brackets = (cp == ']') ? brackets + 1 : 0;
        removed += eol_removed(cp, &at, r);
    }
    /* `at.p` is the byte the closing `>` stands at, because the read that ended the loop was taken from
       there. [21] CDEnd is that `>` and the two brackets before it, all three single ASCII bytes. */
    DCHECK((size_t)(at.p - content) >= CDEND_BRACKETS,
           "a CDATA section closed before its content could hold [21] CDEnd's own brackets — the loop counts "
           "a `]` run and breaks on a `>`, so fewer than two bytes behind that `>` means the run was counted "
           "over characters that are not in this slice");
    DCHECK(memcmp(at.p - CDEND_BRACKETS, CDEND, LIT_LEN(CDEND)) == 0,
           "the bytes a CDATA-section scan measured [21] CDEnd from are not `]]>` — the run counter and the "
           "byte positions are two views of one delimiter and a disagreement means the content slice is "
           "measured against the wrong place");
    out->raw = content;
    out->raw_len = (size_t)(at.p - content) - CDEND_BRACKETS;
    check_scanned(&start, r, out, removed);
    return XML_MARKUP_OK;
}
