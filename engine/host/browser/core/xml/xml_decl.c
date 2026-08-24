/* See xml_decl.h. */
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_decl.h"

/* THE TERMINALS, WRITTEN DOWN ONCE — the peek predicate and the scan behind it read the same literals, which
   is what makes "the caller peeked" an assertion rather than a convention between two spellings.
   `'?>'` is spelled here rather than shared with core/xml/xml_markup.c's [16] PI: it is [23]'s and [77]'s own
   terminal, in their own productions, and a shared constant would tie two grammars together on the accident
   that the same two characters close both. */
static const char DECL_START[] = "<?xml";
static const char DECL_END[] = "?>";
static const char W_VERSION[] = "version";
static const char W_ENCODING[] = "encoding";
static const char W_STANDALONE[] = "standalone";
static const char V_YES[] = "yes";
static const char V_NO[] = "no";

/* Length of a terminal literal, in the characters it has rather than the bytes its array has. */
#define LIT_LEN(s) (sizeof (s) - 1)

const char *xml_decl_error_message(XmlDeclError err)
{
    switch (err) {
    case XML_DECL_OK:
        return "no declaration-level well-formedness constraint was violated";
    case XML_DECL_ERR_SPACE:
        return "fatal error (§2.8 Prolog and Document Type Declaration): [24] VersionInfo, §4.3.3's [80] "
               "EncodingDecl and §2.9's [32] SDDecl each begin with a required [3] S, so a component of a "
               "declaration MUST be separated from what precedes it by white space";
    case XML_DECL_ERR_VERSION_MISSING:
        return "fatal error (§2.8 Prolog and Document Type Declaration): [23] XMLDecl is '<?xml' VersionInfo "
               "EncodingDecl? SDDecl? S? '?>' — the version information is the FIRST component and is not "
               "optional in the declaration of a document entity";
    case XML_DECL_ERR_VERSION_NUM:
        return "fatal error (§2.8 Prolog and Document Type Declaration): [26] VersionNum is '1.' followed by "
               "ONE OR MORE decimal digits, so a version that does not begin `1.` is not a version number "
               "this standard's grammar has";
    case XML_DECL_ERR_ENCODING_MISSING:
        return "fatal error (§4.3.1 The Text Declaration): [77] TextDecl is '<?xml' VersionInfo? EncodingDecl "
               "S? '?>' — the encoding declaration is not optional at the head of an external parsed entity, "
               "which is the whole purpose of a text declaration";
    case XML_DECL_ERR_ENCODING_NAME:
        return "fatal error (§4.3.3 Character Encoding in Entities): [81] EncName is a Latin letter followed "
               "by any number of Latin letters, ASCII digits, '.', '_' and '-' — the production's own comment "
               "is that an encoding name contains only Latin characters";
    case XML_DECL_ERR_STANDALONE_VALUE:
        return "fatal error (§2.9 Standalone Document Declaration): [32] SDDecl's value is the string 'yes' "
               "or the string 'no' and this standard defines no third — an absent declaration is a different "
               "fact again, and §2.9 says the value 'no' is assumed only where there are external markup "
               "declarations";
    case XML_DECL_ERR_STANDALONE_IN_TEXT_DECL:
        return "fatal error (§4.3.1 The Text Declaration): [77] TextDecl is '<?xml' VersionInfo? EncodingDecl "
               "S? '?>' and has no SDDecl — §2.9's standalone document declaration is a component of §2.8's "
               "[23] XMLDecl alone, because it is a statement about the document and an external parsed "
               "entity is not one";
    case XML_DECL_ERR_EQ:
        return "fatal error (§2.8 Prolog and Document Type Declaration): [25] Eq is S? '=' S?, so each "
               "component of a declaration names its value with an equals sign, optionally surrounded by "
               "white space";
    case XML_DECL_ERR_QUOTE:
        return "fatal error (§2.8 Prolog and Document Type Declaration): [24] VersionInfo, §4.3.3's [80] "
               "EncodingDecl and §2.9's [32] SDDecl each write their value as (\"'\" X \"'\" | '\"' X '\"'), "
               "so the value MUST be a whole X and MUST be closed by the same quotation character that "
               "opened it";
    case XML_DECL_ERR_COMPONENT:
        return "fatal error (§2.8 Prolog and Document Type Declaration): a declaration's components are the "
               "version information, then an optional encoding declaration, then an optional standalone "
               "document declaration, IN THAT ORDER, and then '?>' — what stands here is none of the ones "
               "still permitted";
    case XML_DECL_ERR_UNTERMINATED:
        return "fatal error (§2.8 Prolog and Document Type Declaration): a declaration ends with the string "
               "\"?>\" — [23] XMLDecl and §4.3.1's [77] TextDecl both close with one and this entity ended "
               "first";
    case XML_DECL_ERR_CHARACTER:
        return "fatal error inside a declaration, detected by §2.2/§4.3.3's character layer — the reader's "
               "own latch names which one";
    }
    DFAIL("xml_decl_error_message was handed a value that is not an XmlDeclError — the enum is the whole list "
          "of sentences this component can report and a value outside it names no constraint");
    return "";
}

/* READ THE NEXT CHARACTER, REMEMBERING WHERE IT STOOD — core/xml/xml_markup.c's pair, for its reason: `*at` is
   the reader as it was BEFORE the character was read, so `at->p` is the byte that character begins at and a
   borrowed slice can be measured back to it. It is a copy and not a pointer because a copy is the peek and
   the copy is the park. */
static XmlCharError step(XmlCharReader *r, XmlCharReader *at, uint32_t *cp)
{
    *at = *r;
    return xml_char_read(r, cp);
}

/* Does the reader stand at `lit`? See xml_decl.h for why a byte compare is exact for every terminal in these
   productions and why the caller owns the peek at the construct's own delimiter. */
static bool at_lit(const XmlCharReader *r, const char *lit, size_t len)
{
    DCHECK(r != NULL, "a declaration terminal was peeked for with no reader");
    DCHECK(r->start <= r->p && r->p <= r->end,
           "a declaration terminal was peeked for from a reader whose cursor is outside its own entity");
    return (size_t)(r->end - r->p) >= len && memcmp(r->p, lit, len) == 0;
}

/* CONSUME A TERMINAL THE SCAN HAS ALREADY PEEKED, through the READER rather than by advancing the cursor, so
   `line` and `column` count its own characters — the position a `parsererror` quotes for anything after it is
   measured from there. None of these reads can fail: the peek matched their bytes and every one is ASCII. */
static void take_lit(XmlCharReader *r, const char *lit, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        uint32_t cp = 0;
        XmlCharError e = xml_char_read(r, &cp);

        DCHECK(e == XML_CHAR_OK && cp == (uint32_t)(unsigned char)lit[i],
               "a declaration terminal did not read back the characters its peek matched — the peek is a byte "
               "compare over ASCII and the reader produces those same bytes as characters, so a disagreement "
               "means the two spellings of that terminal have drifted apart");
        (void)e;
    }
}

/* §2.3's [3] `S` AS A RUN, leaving the reader on the first character that is not one. `*n` is how many
   characters the run had, which is what tells a caller whether a REQUIRED S was present: [24], [80] and [32]
   each open with one, and [23]/[77]'s trailing `S?` is the same run read as optional.
   The restore is an assignment of a copy taken BEFORE the read that ended the run, so a non-S character is
   never consumed — and a #xD in the run is one character here because core/xml/xml_char.h has already applied
   §2.11 End-of-Line Handling to it. */
static XmlCharError take_s(XmlCharReader *r, size_t *n)
{
    *n = 0;
    for (;;) {
        XmlCharReader at;
        uint32_t cp = 0;
        XmlCharError e = step(r, &at, &cp);

        if (e != XML_CHAR_OK) return e;
        if (!xml_char_is_s(cp)) { *r = at; return XML_CHAR_OK; }
        (*n)++;
    }
}

/* [25] `Eq ::= S? '=' S?` — shared by [24], [80] and [32], which is why it is a function rather than three
   copies of two optional runs around one character. */
static XmlDeclError take_eq(XmlCharReader *r)
{
    XmlCharReader at;
    uint32_t cp = 0;
    size_t n;

    if (take_s(r, &n) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
    if (step(r, &at, &cp) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
    if (cp != '=') return XML_DECL_ERR_EQ;
    if (take_s(r, &n) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
    return XML_DECL_OK;
}

/* THE OPENING QUOTATION CHARACTER, which the author chooses and which the SAME character must close: all
   three components write their value as `("'" X "'" | '"' X '"')`, two alternatives rather than one rule
   about "a quote", so `version="1.0'` matches neither. */
static XmlDeclError take_quote(XmlCharReader *r, uint32_t *quote)
{
    XmlCharReader at;
    uint32_t cp = 0;

    if (step(r, &at, &cp) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
    if (cp != '\'' && cp != '"') return XML_DECL_ERR_QUOTE;
    *quote = cp;
    return XML_DECL_OK;
}

/* §2.8's [26] `VersionNum ::= '1.' [0-9]+`, entered with the opening quote consumed.
 *
 * THE MAJOR VERSION IS A LITERAL `1` AND THE `+` IS ONE-OR-MORE, so `1.0`, `1.1` and `1.10` are all version
 * numbers and `1.`, `2.0` and the empty string are not. §2.8's own note is what makes accepting `1.1` here
 * correct rather than lax: "Even though the VersionNum production matches any version number of the form
 * '1.x', XML 1.0 documents SHOULD NOT specify a version number other than '1.0'", and "When an XML 1.0
 * processor encounters a document that specifies a 1.x version number other than '1.0', it will process it as
 * a 1.0 document." This engine is an XML 1.0 processor, so it matches the production, reports what the author
 * wrote, and processes the document under this standard — which is the standard's instruction and not an
 * absent XML 1.1 implementation. */
static XmlDeclError scan_version_num(XmlCharReader *r, uint32_t quote, const char **s, size_t *n)
{
    XmlCharReader at;
    uint32_t cp = 0;
    const char *from = r->p;
    size_t digits = 0;

    if (step(r, &at, &cp) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
    if (cp != '1') return XML_DECL_ERR_VERSION_NUM;
    if (step(r, &at, &cp) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
    if (cp != '.') return XML_DECL_ERR_VERSION_NUM;
    for (;;) {
        if (step(r, &at, &cp) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
        if (cp < '0' || cp > '9') break;
        digits++;
    }
    if (digits == 0) return XML_DECL_ERR_VERSION_NUM;
    /* `at.p` is the byte the character that ENDED the run stands at, so it is one past the last digit. */
    if (cp != quote) return XML_DECL_ERR_QUOTE;
    *s = from;
    *n = (size_t)(at.p - from);
    return XML_DECL_OK;
}

/* §4.3.3's [81] `EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')*`, one code point at a time. The production's own
   inline comment is "Encoding name contains only Latin characters", which is why these are fixed ASCII
   comparisons and not a call to any general-purpose letter or digit class: an encoding name is spelled in
   this one alphabet by the grammar itself. */
static bool enc_name_start(uint32_t cp)
{
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

static bool enc_name_char(uint32_t cp)
{
    return enc_name_start(cp) || (cp >= '0' && cp <= '9') || cp == '.' || cp == '_' || cp == '-';
}

static XmlDeclError scan_enc_name(XmlCharReader *r, uint32_t quote, const char **s, size_t *n)
{
    XmlCharReader at;
    uint32_t cp = 0;
    const char *from = r->p;

    if (step(r, &at, &cp) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
    if (!enc_name_start(cp)) return XML_DECL_ERR_ENCODING_NAME;
    for (;;) {
        if (step(r, &at, &cp) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
        if (!enc_name_char(cp)) break;
    }
    if (cp != quote) return XML_DECL_ERR_QUOTE;
    *s = from;
    *n = (size_t)(at.p - from);
    return XML_DECL_OK;
}

/* §2.9's [32] `SDDecl`'s value, entered with the opening quote consumed. The alternation names two STRINGS
   rather than a class, so this is two literal peeks and there is nothing to scan: §1.2 Terminology's `match`
   performs no case folding, so `YES` is not the string 'yes' and this reports that the value is neither. */
static XmlDeclError scan_standalone(XmlCharReader *r, uint32_t quote, XmlStandalone *out)
{
    XmlCharReader at;
    uint32_t cp = 0;
    XmlStandalone got;

    if (at_lit(r, V_YES, LIT_LEN(V_YES))) {
        take_lit(r, V_YES, LIT_LEN(V_YES));
        got = XML_STANDALONE_YES;
    } else if (at_lit(r, V_NO, LIT_LEN(V_NO))) {
        take_lit(r, V_NO, LIT_LEN(V_NO));
        got = XML_STANDALONE_NO;
    } else {
        return XML_DECL_ERR_STANDALONE_VALUE;
    }
    if (step(r, &at, &cp) != XML_CHAR_OK) return XML_DECL_ERR_CHARACTER;
    if (cp != quote) return XML_DECL_ERR_QUOTE;
    *out = got;
    return XML_DECL_OK;
}

/* THE TWO PRODUCTIONS' VALUES READ OFF THEIR SLICES, which is the SECOND spelling of [26] and [81] and is
   here for core/xml/xml_char.c's reason: the scan above walks CODE POINTS and this walks BYTES, so a
   mis-transcription of either class shows up as a disagreement rather than as two answers that are wrong
   together. Bytes are exact because both productions are ASCII-only, and every ASCII byte in well-formed
   UTF-8 stands for itself — no ASCII byte can be a continuation byte, which are all 0x80..0xBF. */
static bool version_num_bytes(const char *s, size_t n)
{
    size_t i;

    if (n < 3 || s[0] != '1' || s[1] != '.') return false;
    for (i = 2; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static bool enc_name_bytes(const char *s, size_t n)
{
    size_t i;

    if (n < 1 || !enc_name_start((unsigned char)s[0])) return false;
    for (i = 1; i < n; i++) if (!enc_name_char((unsigned char)s[i])) return false;
    return true;
}

bool xml_decl_at(const XmlCharReader *r)
{
    DCHECK(r != NULL, "a declaration peek was asked for with no reader");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a declaration peek was taken from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing, so "
           "the caller owes a stop here and not another construct");
    /* `'<?xml'` AND THEN [3] S — see xml_decl.h. Both productions continue with a component that opens with a
       required S, so `<?xml` followed by anything else is §2.6's [16] PI and is that component's to answer. */
    return at_lit(r, DECL_START, LIT_LEN(DECL_START))
        && (size_t)(r->end - r->p) > LIT_LEN(DECL_START)
        && xml_char_is_s((uint32_t)(unsigned char)r->p[LIT_LEN(DECL_START)]);
}

/* §2.8's [23] `XMLDecl ::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'` and §4.3.1's [77]
 * `TextDecl ::= '<?xml' VersionInfo? EncodingDecl S? '?>'`, as ONE walk over an ORDERED sequence of optional
 * components — see xml_decl.h for why they are one scan and not two.
 *
 * `stage` IS THE GRAMMAR'S ORDER AND NOTHING ELSE. Each component may appear at most once and only after the
 * ones before it, which is what makes `<?xml encoding='UTF-8' version='1.0'?>` a fatal error rather than a
 * reordering the processor tidies up; a component the stage has already passed is not the next one permitted,
 * which is exactly what XML_DECL_ERR_COMPONENT reports.
 *
 * WHICH COMPONENTS ARE REQUIRED IS THE ONLY DIFFERENCE BETWEEN THE TWO, and it is checked AFTER the walk
 * rather than during it: the walk decides what MATCHED, and the production decides what had to. Checking
 * during would have to know, at the moment `encoding` is seen, whether a `version` that is optional here was
 * merely absent or wrongly skipped — two questions the sequence answers by itself once it has finished. */
static XmlDeclError scan(XmlCharReader *r, bool text_decl, XmlDecl *out)
{
    XmlCharReader start;
    XmlDecl got;
    XmlDeclError err = XML_DECL_OK;
    int stage = 0;   /* 0: [24] may still come; 1: [80] may; 2: [32] may; 3: only S? '?>' remains */
    bool first = true;

    DCHECK(r != NULL && out != NULL, "a declaration scan was asked for with no reader or nowhere to put it");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a declaration was scanned from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing");
    DCHECK(xml_decl_at(r),
           "a declaration scan was handed a reader that does not stand at '<?xml' followed by [3] S — a "
           "caller at the head of an entity decides between §2.8's [23] XMLDecl, §4.3.1's [77] TextDecl and "
           "§2.6's [16] PI, and that decision is its grammar rule, so a reader standing anywhere else is a "
           "caller that has not peeked and this is not a document to report about");
    start = *r;
    got.version = NULL;   got.version_len = 0;
    got.encoding = NULL;  got.encoding_len = 0;
    got.standalone = XML_STANDALONE_ABSENT;

    take_lit(r, DECL_START, LIT_LEN(DECL_START));

    for (;;) {
        uint32_t quote = 0;
        size_t s = 0;

        if (take_s(r, &s) != XML_CHAR_OK) { err = XML_DECL_ERR_CHARACTER; goto fail; }
        DCHECK(!first || s > 0,
               "the [3] S the peek matched immediately after '<?xml' did not read back as a run of white "
               "space — the peek is a byte compare over the four ASCII characters of [3] S and the reader "
               "produces those same bytes as characters, so a disagreement means the peek and the scan "
               "disagree about where this declaration's first component begins");
        first = false;

        /* [23]/[77]'s trailing `S?` is the run just consumed, read as optional. */
        if (at_lit(r, DECL_END, LIT_LEN(DECL_END))) {
            take_lit(r, DECL_END, LIT_LEN(DECL_END));
            break;
        }
        if (stage <= 0 && at_lit(r, W_VERSION, LIT_LEN(W_VERSION))) {
            if (s == 0) { err = XML_DECL_ERR_SPACE; goto fail; }
            take_lit(r, W_VERSION, LIT_LEN(W_VERSION));
            if ((err = take_eq(r)) != XML_DECL_OK) goto fail;
            if ((err = take_quote(r, &quote)) != XML_DECL_OK) goto fail;
            if ((err = scan_version_num(r, quote, &got.version, &got.version_len)) != XML_DECL_OK) goto fail;
            stage = 1;
            continue;
        }
        if (stage <= 1 && at_lit(r, W_ENCODING, LIT_LEN(W_ENCODING))) {
            if (s == 0) { err = XML_DECL_ERR_SPACE; goto fail; }
            take_lit(r, W_ENCODING, LIT_LEN(W_ENCODING));
            if ((err = take_eq(r)) != XML_DECL_OK) goto fail;
            if ((err = take_quote(r, &quote)) != XML_DECL_OK) goto fail;
            if ((err = scan_enc_name(r, quote, &got.encoding, &got.encoding_len)) != XML_DECL_OK) goto fail;
            stage = 2;
            continue;
        }
        if (stage <= 2 && at_lit(r, W_STANDALONE, LIT_LEN(W_STANDALONE))) {
            /* [77] HAS NO SDDecl AT ALL, so the white-space question below never arises for it: the keyword
               is not a component of that production however it is spaced, and reporting a missing S would
               name a rule the author did not break. */
            if (text_decl) { err = XML_DECL_ERR_STANDALONE_IN_TEXT_DECL; goto fail; }
            if (s == 0) { err = XML_DECL_ERR_SPACE; goto fail; }
            take_lit(r, W_STANDALONE, LIT_LEN(W_STANDALONE));
            if ((err = take_eq(r)) != XML_DECL_OK) goto fail;
            if ((err = take_quote(r, &quote)) != XML_DECL_OK) goto fail;
            if ((err = scan_standalone(r, quote, &got.standalone)) != XML_DECL_OK) goto fail;
            stage = 3;
            continue;
        }
        {
            XmlCharReader at;
            uint32_t cp = 0;

            if (step(r, &at, &cp) != XML_CHAR_OK) { err = XML_DECL_ERR_CHARACTER; goto fail; }
            /* AN ENTITY THAT ENDED AND A COMPONENT THAT IS NOT ONE ARE TWO PLACES TO LOOK: `<?xml version=
               "1.0"` is a declaration the author never closed, while `<?xml version="1.0" lang="en"?>` is one
               they closed around something [23] does not have. */
            err = (cp == XML_CHAR_EOF) ? XML_DECL_ERR_UNTERMINATED : XML_DECL_ERR_COMPONENT;
            goto fail;
        }
    }

    if (!text_decl && got.version == NULL)  { err = XML_DECL_ERR_VERSION_MISSING; goto fail; }
    if (text_decl && got.encoding == NULL)  { err = XML_DECL_ERR_ENCODING_MISSING; goto fail; }

    DCHECK(r->p > start.p && r->fatal == XML_CHAR_OK,
           "a declaration scan succeeded without consuming anything, so a caller walking [22] prolog would "
           "never advance");
    DCHECK((size_t)(r->p - start.p) >= LIT_LEN(DECL_START) + LIT_LEN(DECL_END)
               && memcmp(r->p - LIT_LEN(DECL_END), DECL_END, LIT_LEN(DECL_END)) == 0,
           "the bytes a declaration scan finished on are not [23]'s closing '?>' — the terminal it peeked and "
           "the position it left the reader at are two views of one delimiter and a disagreement means the "
           "construct ends in the wrong place");
    DCHECK(got.version == NULL
               || (got.version > start.p && got.version + got.version_len < r->p
                   && version_num_bytes(got.version, got.version_len)),
           "a declaration's [26] VersionNum was scanned code point by code point and its borrowed slice does "
           "not read back as that production, or does not lie strictly inside the construct — those are one "
           "transcription read two ways, and a disagreement means the character-at-a-time and byte spellings "
           "of §2.8's version number have drifted apart");
    DCHECK(got.encoding == NULL
               || (got.encoding > start.p && got.encoding + got.encoding_len < r->p
                   && enc_name_bytes(got.encoding, got.encoding_len)),
           "a declaration's [81] EncName was scanned code point by code point and its borrowed slice does not "
           "read back as that production, or does not lie strictly inside the construct — §4.3.3's own "
           "comment is that an encoding name contains only Latin characters, so the two spellings answer "
           "about the same bytes and must agree");
    DCHECK((got.version == NULL || memchr(got.version, 0x0D, got.version_len) == NULL)
               && (got.encoding == NULL || memchr(got.encoding, 0x0D, got.encoding_len) == NULL),
           "a declaration's borrowed value contains a literal carriage return byte — §2.11 End-of-Line "
           "Handling rewrites #xD and would make the borrowed bytes differ from the characters that were "
           "scanned, but #xD is a decimal digit in neither [26] nor a Latin character in [81], so a #xD "
           "inside one of these values means the scan accepted a character the production does not have. "
           "A #xD elsewhere in the construct is ordinary: [3] S has one, and the white space BETWEEN "
           "components is not borrowed by anybody");
    DCHECK(!text_decl || (got.standalone == XML_STANDALONE_ABSENT && got.encoding != NULL),
           "a §4.3.1 [77] TextDecl came back carrying §2.9's standalone document declaration, or without the "
           "encoding declaration that production requires — [77] is '<?xml' VersionInfo? EncodingDecl S? "
           "'?>' and has no SDDecl in it");
    DCHECK(text_decl || got.version != NULL,
           "a §2.8 [23] XMLDecl came back without [26] VersionNum — VersionInfo is that production's first "
           "component and is not optional");
    DCHECK(got.standalone == XML_STANDALONE_ABSENT || got.standalone == XML_STANDALONE_YES
               || got.standalone == XML_STANDALONE_NO,
           "a declaration's standalone answer is not one of §2.9's three — the two values [32] spells and the "
           "positive statement that the document carried no [32] at all");
    *out = got;
    return XML_DECL_OK;

fail:
    DCHECK(err != XML_DECL_OK,
           "a declaration scan jumped to its failure path carrying no error — every branch that gets here "
           "names the sentence it violated, so a clear code means one of them reported nothing and the caller "
           "would be handed an untouched result as though the scan had succeeded");
    if (err == XML_DECL_ERR_CHARACTER) {
        /* THE ONE RETURN THAT DOES NOT REWIND — see xml_decl.h. Restoring `start` would restore `fatal` to
           XML_CHAR_OK and un-report the error the character layer just detected. */
        DCHECK(r->fatal != XML_CHAR_OK,
               "a declaration scan reported the character layer's fatal error while the reader's §1.2 latch "
               "is clear — this return says `ask the reader which one`, and a clear latch has no answer");
        return err;
    }
    DCHECK(r->fatal == XML_CHAR_OK,
           "a declaration scan is about to rewind a reader that has latched a fatal error, which would "
           "silently clear it");
    *r = start;
    return err;
}

XmlDeclError xml_decl_scan_xmldecl(XmlCharReader *r, XmlDecl *out)  { return scan(r, false, out); }
XmlDeclError xml_decl_scan_textdecl(XmlCharReader *r, XmlDecl *out) { return scan(r, true, out); }
