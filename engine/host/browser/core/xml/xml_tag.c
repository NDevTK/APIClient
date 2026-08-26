/* See xml_tag.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_name.h"
#include "core/xml/xml_ref.h"
#include "core/xml/xml_tag.h"

/* THE DELIMITERS, WRITTEN DOWN ONCE — the peek predicates and the scans behind them read the same bytes, which
   is what makes "the caller peeked" an assertion rather than a convention between two spellings. Each is a
   single ASCII character, so a byte test for it is exact: no continuation byte of a multi-byte sequence is
   ever below 0x80. */
#define TAG_OPEN   '<'
#define TAG_CLOSE  '>'
#define TAG_SLASH  '/'
#define TAG_EQ     '='
#define TAG_AMP    '&'
#define TAG_BANG   '!'
#define TAG_QUERY  '?'

const char *xml_tag_error_message(XmlTagError err)
{
    switch (err) {
    case XML_TAG_OK:
        return "no tag-level well-formedness constraint was violated";
    case XML_TAG_ERR_NAME:
        return "fatal error (§3.1 Start-Tags, End-Tags, and Empty-Element Tags): [40] STag is '<' Name, [42] "
               "ETag is '</' Name and [44] EmptyElemTag is '<' Name — \"the Name in the start- and end-tags "
               "gives the element's type\", and this tag opens with something that is not a §2.3 [5] Name";
    case XML_TAG_ERR_ATTRIBUTE_SEPARATOR:
        return "fatal error (§3.1 Start-Tags, End-Tags, and Empty-Element Tags): [40] STag and [44] "
               "EmptyElemTag write their attribute list as (S Attribute)*, so every attribute specification "
               "MUST be preceded by §2.3's [3] S — the S? that may be omitted is the one before the closing "
               "'>' or '/>' and not the one between two attributes";
    case XML_TAG_ERR_ATTRIBUTE_NAME:
        return "fatal error (§3.1 Start-Tags, End-Tags, and Empty-Element Tags): [41] Attribute is Name Eq "
               "AttValue, and this attribute specification begins with something that is not a §2.3 [5] Name";
    case XML_TAG_ERR_EQ:
        return "fatal error (§2.3 Common Syntactic Constructs): [25] Eq ::= S? '=' S? — [41] Attribute puts an "
               "Eq between the attribute name and its value, and this attribute has no '='";
    case XML_TAG_ERR_ATTVALUE_QUOTE:
        return "fatal error (§2.3 Common Syntactic Constructs): [10] AttValue is delimited by a matched pair of "
               "'\"' or of \"'\", and this attribute value is opened by neither";
    case XML_TAG_ERR_ATTVALUE_UNTERMINATED:
        return "fatal error (§2.3 Common Syntactic Constructs): [10] AttValue closes with the same quotation "
               "mark it opened with, and the entity ends before this value's does";
    case XML_TAG_ERR_LT_IN_ATTRIBUTE_VALUE:
        return "fatal error (§2.3 Common Syntactic Constructs, and §3.1's [WFC: No < in Attribute Values]): a "
               "literal left angle bracket is excluded from both alternatives of [10] AttValue and MUST be "
               "escaped as \"&lt;\" or a character reference — §4.6 Predefined Entities requires lt's "
               "replacement text to be a character reference for exactly this reason, so no entity's "
               "replacement text may carry one in indirectly either";
    case XML_TAG_ERR_UNIQUE_ATT_SPEC:
        return "fatal error (§3.1 Start-Tags, End-Tags, and Empty-Element Tags, [WFC: Unique Att Spec]): \"An "
               "attribute name MUST NOT appear more than once in the same start-tag or empty-element tag\"";
    case XML_TAG_ERR_ENTITY_UNDECLARED:
        return "fatal error (§4.1 Character and Entity References, [WFC: Entity Declared]): \"In a document "
               "without any DTD ... the Name given in the entity reference MUST match that in an entity "
               "declaration ... except that well-formed documents need not declare any of the following "
               "entities: amp, lt, gt, apos, quot\" — this document declares no entities, because nothing in "
               "this build reads §2.8's [28] doctypedecl, so the five are the whole of what a reference may "
               "name";
    case XML_TAG_ERR_UNTERMINATED:
        return "fatal error (§3.1 Start-Tags, End-Tags, and Empty-Element Tags): [40] STag closes with '>', "
               "[44] EmptyElemTag with '/>' and [42] ETag with '>', and the entity ends before this tag does";
    case XML_TAG_ERR_ETAG_ATTRIBUTE:
        return "fatal error (§3.1 Start-Tags, End-Tags, and Empty-Element Tags): [42] ETag ::= '</' Name S? "
               "'>' — an end-tag carries no attribute specifications and nothing else stands between its "
               "element type and its '>', because \"the end of every element that begins with a start-tag MUST "
               "be marked by an end-tag containing a name that echoes the element's type\" and a name is all "
               "it contains";
    case XML_TAG_ERR_REFERENCE:
        return "fatal error inside an attribute value, detected by §4.1's reference layer — the reference "
               "error this scan reported alongside names which one";
    case XML_TAG_ERR_CHARACTER:
        return "fatal error inside a tag, detected by §2.2/§4.3.3's character layer — the reader's own latch "
               "names which one";
    }
    DFAIL("xml_tag_error_message was handed a value that is not an XmlTagError — the enum is the whole list of "
          "sentences this component can report and a value outside it names no constraint");
    return "";
}

void xml_tag_free(XmlTag *t)
{
    size_t i;

    DCHECK(t != NULL, "an XML tag record was freed through no pointer");
    DCHECK(t->atts != NULL || t->att_n == 0,
           "an XML tag record claims attributes and has no array to hold them — a tag with none has no array, "
           "which is a positive statement this producer makes and not a hole a consumer fills in");
    for (i = 0; i < t->att_n; i++) free(t->atts[i].value);
    free(t->atts);
    memset(t, 0, sizeof *t);
}

/* HOW MANY BYTES ARE LEFT, and what the next one is — the two questions every peek in this file asks. `peek`
   answers with a value no byte has when the entity has ended, so a caller never has to test both. */
#define TAG_NO_BYTE 0x100

static unsigned peek(const XmlCharReader *r)
{
    DCHECK(r->start <= r->p && r->p <= r->end,
           "an XML tag peek was taken from a reader whose cursor is outside its own entity");
    return r->p == r->end ? TAG_NO_BYTE : (unsigned)(unsigned char)*r->p;
}

static unsigned peek_at(const XmlCharReader *r, size_t n)
{
    return (size_t)(r->end - r->p) > n ? (unsigned)(unsigned char)r->p[n] : TAG_NO_BYTE;
}

bool xml_tag_at_stag(const XmlCharReader *r)
{
    unsigned second;

    DCHECK(r != NULL, "an XML tag peek was asked for with no reader");
    DCHECK(r->fatal == XML_CHAR_OK,
           "an XML tag peek was taken from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing, so "
           "the caller owes a stop here and not another construct");
    if (peek(r) != TAG_OPEN) return false;
    second = peek_at(r, 1);
    /* The three OTHER things a `<` opens: [42] ETag's '</', §2.5/§2.7's '<!' forms and §2.6/§2.8's '<?' forms.
       A `<` at the very end of the entity is left to the scan, which reports [40]'s missing Name. */
    return second != TAG_SLASH && second != TAG_BANG && second != TAG_QUERY;
}

bool xml_tag_at_etag(const XmlCharReader *r)
{
    DCHECK(r != NULL, "an XML tag peek was asked for with no reader");
    DCHECK(r->fatal == XML_CHAR_OK,
           "an XML tag peek was taken from a reader that has already reported a fatal error — §1.2 "
           "Terminology: once a fatal error is detected the processor MUST NOT continue normal processing, so "
           "the caller owes a stop here and not another construct");
    return peek(r) == TAG_OPEN && peek_at(r, 1) == TAG_SLASH;
}

/* CONSUME ONE ASCII CHARACTER THE CALLER HAS ALREADY PEEKED, through the READER rather than by advancing the
   cursor, so `line` and `column` count it — the position a `parsererror` quotes for anything after it is
   measured from there. The read cannot fail: the peek matched the byte and every delimiter here is ASCII. */
static void eat(XmlCharReader *r, unsigned expect)
{
    uint32_t cp = 0;
    XmlCharError e;

    DCHECK(peek(r) == expect, "an XML tag scan consumed a delimiter its peek had not matched");
    e = xml_char_read(r, &cp);
    DCHECK(e == XML_CHAR_OK && cp == expect,
           "a tag delimiter did not read back the character its peek matched — the peek is a byte compare over "
           "ASCII and the reader produces those same bytes as characters, so a disagreement means the two "
           "spellings of that delimiter have drifted apart");
    (void)e;
}

/* §2.3's [3] `S`, as the RUN the grammar spells `S` or `S?`. Answers how many characters it consumed, which is
   what tells [40]'s `(S Attribute)*` apart from its `S? '>'`: zero is a legal answer to the second and never a
   legal answer to the first. */
static size_t eat_s(XmlCharReader *r)
{
    size_t n = 0;

    for (;;) {
        XmlCharReader at = *r;
        uint32_t cp = 0;

        if (xml_char_read(r, &cp) != XML_CHAR_OK) return n;    /* the latch is the caller's to notice */
        if (cp == XML_CHAR_EOF || !xml_char_is_s(cp)) { *r = at; return n; }
        n++;
    }
}

/* §2.3's [5] `Name`, scanned the way a tokenizer has to — [4] NameStartChar of the first character and [4a]
   NameChar of each one after, until one says no. The slice BORROWS the entity, which is exact by
   core/xml/xml_ref.h's argument: §2.11 only ever rewrites #xD, and #xD is in neither class, so no character of
   a Name can differ from the bytes it was decoded from. That is held to core/xml/xml_name.h's SLICE spelling
   of the same production on every successful scan rather than trusted to agree with it.
   Returns false and consumes nothing when there is no Name here. */
static bool scan_name(XmlCharReader *r, const char **name, size_t *name_len)
{
    XmlCharReader start = *r;
    uint32_t cp = 0;

    if (xml_char_read(r, &cp) != XML_CHAR_OK) return false;    /* the latch is the caller's to notice */
    if (cp == XML_CHAR_EOF || !xml_name_is_name_start_char(cp)) { *r = start; return false; }
    for (;;) {
        XmlCharReader at = *r;

        if (xml_char_read(r, &cp) != XML_CHAR_OK) return false;
        if (cp == XML_CHAR_EOF || !xml_name_is_name_char(cp)) { *r = at; break; }
    }
    *name = start.p;
    *name_len = (size_t)(r->p - start.p);
    DCHECK(*name_len > 0 && xml_name_is_name(*name, *name_len),
           "a Name scanned character by character is not a Name when the same bytes are asked of "
           "core/xml/xml_name.h's slice predicate — [4] NameStartChar, [4a] NameChar and [5] Name are one "
           "transcription read two ways, and a disagreement means one of the two readings is wrong");
    return true;
}

/* THE ATTRIBUTE LIST GROWS AND IS BOUNDED BY THE ENTITY. [40]'s `(S Attribute)*` is a Kleene star, so a
   capacity ceiling would be CLAUDE.md's banned bound wearing prudence; the doubling is the ordinary way to
   make an unbounded append affordable and the allocation failure is a CHECK because a dropped attribute is a
   document this engine would then report the wrong answer about. Capacity is derived from `att_n` rather than
   stored, because a stored capacity is a second field that can disagree with the array. */
static XmlAttribute *att_room(XmlAttribute *atts, size_t n)
{
    XmlAttribute *grown;
    size_t cap;

    /* The block always holds FOUR or a power of two, so the count alone says whether the next append needs a
       new one: `n` is 0 (there is no block), or `n` is 4, 8, 16 … (the block is exactly full). Every other
       count is inside a block with room. Deriving it beats storing it because a stored capacity is a second
       field that can disagree with the array it describes, and nothing would say so. */
    if (!(n == 0 || (n >= 4 && (n & (n - 1)) == 0))) return atts;
    cap = n == 0 ? 4 : n * 2;
    DCHECK(cap > n && cap <= (size_t)-1 / sizeof *grown,
           "an XML tag's attribute count has reached the point where doubling it overflows — the list is "
           "bounded by the entity, so this is a count no document of a representable size can reach");
    grown = realloc(atts, cap * sizeof *grown);
    CHECK(grown != NULL, "an XML tag's attribute list could not be grown — [40] STag's (S Attribute)* is "
                         "unbounded, so this is the physical memory floor and not a limit this component "
                         "chose");
    return grown;
}

/* APPEND ONE CHARACTER OF §3.3.3'S NORMALIZED VALUE, WITH THE BOUND CHECKED BEFORE THE WRITE AND NOT AFTER.
 * The bound is xml_tag.h's proof that §3.3.3 can only shrink, and the buffer under it was sized by that proof
 * from bytes a page supplied — so an assertion placed after the store would fire standing in a heap it had
 * already overrun, which is not an assertion at all. The character is encoded into a four-byte local first so
 * that its WIDTH is known while there is still a decision to make.
 *   IT IS A `CHECK` AND NOT A `DCHECK`, which is the one place in this component set where that is true. The
 * other invariants here are about the engine's own logic and vanish in release by design; this one stands
 * between attacker-supplied document bytes and a heap write, so it is check.h's data-integrity case and MUST
 * hold in production. A release build with the proof wrong would corrupt the heap silently, and a security
 * tool that can be made to do that by the document it is analysing has a worse defect than the one it
 * reports. */
static void append_char(char *buf, size_t *n, size_t span, uint32_t cp)
{
    char enc[XML_CHAR_ENCODE_MAX];
    size_t w = xml_char_encode(cp, enc);

    CHECK(*n + w <= span,
          "§3.3.3's normalized attribute value is longer than the run of the entity it was built from — every "
          "alternative of §2.3's [10] AttValue contributes at most the bytes it occupies (a literal character "
          "re-encodes to what it was decoded from, a white space character becomes one byte from one or two, "
          "and every reference is longer than its result), so a longer result means that proof is wrong and "
          "the buffer sized by it would be overrun");
    memcpy(buf + *n, enc, w);
    *n += w;
}

/* §3.3.3 ATTRIBUTE-VALUE NORMALIZATION over §2.3's [10] `AttValue`, which is where every sentence this
   component owns about VALUES lands. The four steps of §3.3.3 step 3 are the four arms of the loop and are
   commented with the standard's own wording; step 4 does not appear because it cannot be reached — see
   xml_tag.h.
   The buffer is sized before the loop from the run between the quotes and the fill is asserted against it,
   which is the whole reason no reallocation happens inside a value. */
static XmlTagError scan_attvalue(XmlCharReader *r, char **value, size_t *value_len, XmlRefError *ref)
{
    unsigned quote = peek(r);
    const char *close;
    char *buf;
    size_t span, n = 0;

    if (quote != '"' && quote != '\'') return XML_TAG_ERR_ATTVALUE_QUOTE;
    /* THE CLOSING QUOTE, BY A SINGLE BYTE SCAN — exact because the delimiter is ASCII (so it can never be a
       continuation byte) and because [10]'s grammar puts no quote of the delimiting kind anywhere inside: the
       character alternative excludes it and a [67] Reference is made of Name characters or digits. This is a
       SIZE and not a decision — the character scan below is what decides where the value ends, and the two are
       held to each other when it does. */
    close = memchr(r->p + 1, (int)quote, (size_t)(r->end - (r->p + 1)));
    if (close == NULL) return XML_TAG_ERR_ATTVALUE_UNTERMINATED;
    span = (size_t)(close - (r->p + 1));
    buf = malloc(span + 1);
    CHECK(buf != NULL, "an XML attribute value could not be allocated — §3.3.3 Attribute-Value Normalization "
                       "builds text the entity does not contain, so this is the physical memory floor");
    eat(r, quote);

    for (;;) {
        unsigned b = peek(r);
        uint32_t cp = 0;

        /* THE LOOP CANNOT REACH THE END OF THE ENTITY, and that is asserted rather than handled: the byte scan
           above already found the closing quote INSIDE the entity, and nothing in [10] can carry the character
           scan past it, so a value is unterminated at the point the byte scan says so and nowhere else. An
           `if` here would be a second site deciding one thing, reachable only when the two disagree — which is
           the state that must crash rather than be reported as a document's mistake. */
        DCHECK(b != TAG_NO_BYTE,
               "an attribute value's character scan reached the end of the entity, which its own byte scan had "
               "already proved impossible — the closing quote was found before the scan began, so the scan "
               "must stop at or before it and an end here means the two readings of the same bytes disagree");
        if (b == quote) { eat(r, quote); break; }
        if (b == TAG_OPEN) { free(buf); return XML_TAG_ERR_LT_IN_ATTRIBUTE_VALUE; }
        if (b == TAG_AMP) {
            /* THE READER ALREADY STANDS ON THE `&`, which is core/xml/xml_ref.h's precondition — the peek is a
               byte test and consumes nothing, so there is no rewind here and therefore no way to restore a
               §1.2 latch the reference layer just set. */
            XmlRef got;
            XmlRefError e = xml_ref_scan(r, &got);

            if (e != XML_REF_OK) { free(buf); *ref = e; return XML_TAG_ERR_REFERENCE; }
            if (got.kind == XML_REF_ENTITY) { free(buf); return XML_TAG_ERR_ENTITY_UNDECLARED; }
            /* §3.3.3 step 3, first bullet: "For a character reference, append the referenced character to the
               normalized value." §4.6's five take this arm too rather than the recursive one, and that is the
               standard's own construction: it declares each of them as an internal entity whose replacement
               text is a character reference (`<!ENTITY lt "&#38;#60;">`, `<!ENTITY quot "&#34;">`), so
               recursing on that text reaches this bullet at its first character and stops. The observable
               consequence is the one §3.3.3's note draws: the character arrives AS ITSELF and is not put
               through the white-space arm below, so `&#x9;` is a tab in the value where a literal tab is a
               space. */
            append_char(buf, &n, span, got.cp);
            continue;
        }
        if (xml_char_read(r, &cp) != XML_CHAR_OK) { free(buf); return XML_TAG_ERR_CHARACTER; }
        DCHECK(cp != XML_CHAR_EOF, "an XML attribute value scan read the end of the entity as a character "
                                   "after its own peek had said a byte was there");
        if (xml_char_is_s(cp)) {
            /* §3.3.3 step 3, third bullet: "For a white space character (#x20, #xD, #xA, #x9), append a space
               character (#x20) to the normalized value." A #xD cannot arrive here — §2.11 removed it before
               the grammar saw it, which is the contrast the note in §3.3.3 draws against `&#xD;`. */
            DCHECK(cp != 0x0D, "a literal carriage return reached §3.3.3's white-space arm — §2.11 End-of-Line "
                               "Handling removes it before any other processing, so one here means the reader "
                               "and this scan disagree about which rule normalized the entity");
            append_char(buf, &n, span, 0x20);
        } else {
            /* §3.3.3 step 3, fourth bullet: "For another character, append the character to the normalized
               value." */
            append_char(buf, &n, span, cp);
        }
    }

    DCHECK(r->p == close + 1,
           "an attribute value's character scan ended somewhere other than at the quote its byte scan found — "
           "[10] AttValue excludes its own delimiter from the character alternative and a [67] Reference holds "
           "no quote, so the first one after the opening quote IS the closing one, and a disagreement means "
           "that argument is wrong for some input this scan just read");
    buf[n] = '\0';
    DCHECK(memchr(buf, '\0', n) == NULL,
           "an attribute value holds an interior NUL — U+0000 is not §2.2's [2] Char, so no character of an "
           "XML document is one and every layer below this has already refused it");
    *value = buf;
    *value_len = n;
    return XML_TAG_OK;
}

/* PUT A FAILED SCAN'S READER BACK, so the position a report quotes names the TAG and not some place inside
 * the construct that failed — with the one carve-out core/xml/xml_markup.h states, because assigning back a
 * saved reader would restore its §1.2 latch to XML_CHAR_OK and silently un-report a fatal error the character
 * layer just detected.
 *   THE CARVE-OUT IS KEYED ON THE LATCH AND NOT ON THE ERROR VALUE, and that difference is a defect this
 * function exists to make impossible. XML_TAG_ERR_CHARACTER is not the only answer that can leave a reader
 * latched: §4.1's reference layer sets the same latch when a reference's own characters are ill-formed, and
 * this component reports THAT as XML_TAG_ERR_REFERENCE. A guard written as `e != XML_TAG_ERR_CHARACTER` looks
 * complete, reads as the rule, and rewinds over exactly that case — un-reporting a fatal error and handing
 * the caller a reader that will read the offending bytes again. The reader's own latch is the fact; the enum
 * value is a description of it, and only one of the two can be wrong. */
static void tag_rewind(XmlCharReader *r, const XmlCharReader *start, XmlTagError e)
{
    if (r->fatal != XML_CHAR_OK) {
        DCHECK(e == XML_TAG_ERR_CHARACTER || e == XML_TAG_ERR_REFERENCE,
               "an XML tag scan left the character layer's §1.2 latch set while reporting a failure that is "
               "neither its own nor the reference layer's — those are the only two answers this component has "
               "for a fatal error detected below it, so a third means a latch was set on a path that does not "
               "say so and the error a report quotes would name the wrong sentence");
        return;
    }
    DCHECK(e != XML_TAG_ERR_CHARACTER,
           "an XML tag scan reported that the character layer detected a fatal error while that layer's own "
           "§1.2 latch is clear — the latch is the fact and this answer is a description of it, so a "
           "disagreement means the description is wrong");
    *r = *start;
}

/* §3.1's [41] `Attribute ::= Name Eq AttValue`, appended to the tag under construction. [WFC: Unique Att Spec]
   is checked HERE rather than after the list is complete, so the position a report quotes is the SECOND
   spelling of the name and not the end of the tag. The comparison is over the Name's bytes because §3.1's
   sentence is about the name as written; Namespaces in XML 1.0 (Third Edition) §6.3 Uniqueness of Attributes
   adds the expanded-name half, which cannot be answered until this tag's own xmlns declarations are in scope
   and therefore belongs to whoever pushes the scope. The walk is linear because the question is byte equality
   and an index would be a second opinion about it. */
static XmlTagError scan_attribute(XmlCharReader *r, XmlTag *t, XmlRefError *ref)
{
    const char *name = NULL;
    size_t name_len = 0, i;
    char *value = NULL;
    size_t value_len = 0;
    XmlTagError e;

    if (!scan_name(r, &name, &name_len))
        return r->fatal != XML_CHAR_OK ? XML_TAG_ERR_CHARACTER : XML_TAG_ERR_ATTRIBUTE_NAME;
    for (i = 0; i < t->att_n; i++)
        if (t->atts[i].name_len == name_len && memcmp(t->atts[i].name, name, name_len) == 0)
            return XML_TAG_ERR_UNIQUE_ATT_SPEC;

    eat_s(r);                                        /* [25] Eq ::= S? '=' S? — the first S? */
    if (r->fatal != XML_CHAR_OK) return XML_TAG_ERR_CHARACTER;
    if (peek(r) != TAG_EQ) return XML_TAG_ERR_EQ;
    eat(r, TAG_EQ);
    eat_s(r);                                        /* the second S? */
    if (r->fatal != XML_CHAR_OK) return XML_TAG_ERR_CHARACTER;

    e = scan_attvalue(r, &value, &value_len, ref);
    if (e != XML_TAG_OK) return e;

    t->atts = att_room(t->atts, t->att_n);
    t->atts[t->att_n].name = name;
    t->atts[t->att_n].name_len = name_len;
    t->atts[t->att_n].value = value;
    t->atts[t->att_n].value_len = value_len;
    t->att_n++;
    return XML_TAG_OK;
}

XmlTagError xml_tag_scan_stag(XmlCharReader *r, XmlTag *out, XmlRefError *ref)
{
    XmlCharReader start;
    XmlTag t;
    XmlTagError e;

    DCHECK(r != NULL && out != NULL && ref != NULL,
           "a start-tag scan was asked for with no reader, nowhere to put the tag, or nowhere to put the "
           "reference layer's answer");
    /* THE `<` IS THE CALLER'S PEEK, NOT THIS COMPONENT'S DISCOVERY — [40] and [44] both begin with one and a
       reader standing anywhere else is a caller that has not peeked. */
    DCHECK(xml_tag_at_stag(r),
           "a start-tag scan was handed a reader that does not stand at a [40] STag or [44] EmptyElemTag — "
           "both begin with a `<` that opens neither an end-tag nor §2.5/§2.6/§2.7/§2.8's `<!`/`<?` forms, and "
           "xml_tag_at_stag is the question this scan's caller owes");
    *ref = XML_REF_OK;
    start = *r;
    memset(&t, 0, sizeof t);
    eat(r, TAG_OPEN);
    if (!scan_name(r, &t.name, &t.name_len)) {
        e = r->fatal != XML_CHAR_OK ? XML_TAG_ERR_CHARACTER : XML_TAG_ERR_NAME;
        goto fail;
    }

    for (;;) {
        size_t s = eat_s(r);
        unsigned b;

        if (r->fatal != XML_CHAR_OK) { e = XML_TAG_ERR_CHARACTER; goto fail; }
        b = peek(r);
        if (b == TAG_CLOSE) {                        /* [40]'s `S? '>'` */
            eat(r, TAG_CLOSE);
            t.empty = false;
            break;
        }
        if (b == TAG_SLASH && peek_at(r, 1) == TAG_CLOSE) {   /* [44]'s `S? '/>'` */
            eat(r, TAG_SLASH);
            eat(r, TAG_CLOSE);
            t.empty = true;
            break;
        }
        /* The entity ending inside a tag is [40]/[44] unterminated, and so is a `/` with nothing after it —
           that `/` can only have been [44]'s, and the `>` that would complete it is not there. */
        if (b == TAG_NO_BYTE) { e = XML_TAG_ERR_UNTERMINATED; goto fail; }
        if (b == TAG_SLASH && peek_at(r, 1) == TAG_NO_BYTE) { e = XML_TAG_ERR_UNTERMINATED; goto fail; }
        /* Neither ending stands here, so what follows must be `(S Attribute)`'s Attribute — and the S in that
           group is not the S? the two endings share, so its absence is a violation and not a shape. */
        if (s == 0) { e = XML_TAG_ERR_ATTRIBUTE_SEPARATOR; goto fail; }
        e = scan_attribute(r, &t, ref);
        if (e != XML_TAG_OK) goto fail;
    }

    DCHECK(r->p > start.p && r->fatal == XML_CHAR_OK,
           "a start-tag scan succeeded without consuming anything, so a caller walking [43] content would "
           "never advance");
    DCHECK(t.name != NULL && t.name >= start.start && t.name + t.name_len <= r->p,
           "a tag's element type does not lie inside the run the scan consumed — it is a borrowed slice and "
           "not a copy, so a slice outside means it was measured against something else");
    *out = t;
    return XML_TAG_OK;

fail:
    xml_tag_free(&t);
    tag_rewind(r, &start, e);
    return e;
}

XmlTagError xml_tag_scan_etag(XmlCharReader *r, const char **name, size_t *name_len)
{
    XmlCharReader start;
    const char *n = NULL;
    size_t n_len = 0;
    XmlTagError e;

    DCHECK(r != NULL && name != NULL && name_len != NULL,
           "an end-tag scan was asked for with no reader or nowhere to put the element type");
    DCHECK(xml_tag_at_etag(r),
           "an end-tag scan was handed a reader that does not stand at a [42] ETag — it begins with '</' and "
           "xml_tag_at_etag is the question this scan's caller owes");
    start = *r;
    eat(r, TAG_OPEN);
    eat(r, TAG_SLASH);
    if (!scan_name(r, &n, &n_len)) {
        e = r->fatal != XML_CHAR_OK ? XML_TAG_ERR_CHARACTER : XML_TAG_ERR_NAME;
        goto fail;
    }
    eat_s(r);                                        /* [42]'s `S?` */
    if (r->fatal != XML_CHAR_OK) { e = XML_TAG_ERR_CHARACTER; goto fail; }
    /* [42] is `'</' Name S? '>'` and nothing else. Two different author mistakes end up here and they are
       reported apart, because "the entity ends before this tag does" is a FALSE sentence about `</a b>`: the
       entity ending is XML_TAG_ERR_UNTERMINATED, and anything else standing after the Name and its S? is a
       character that is neither [4a] NameChar nor [3] S nor the close — which in practice is an author who
       wrote a start-tag's attribute list on an end-tag. */
    if (peek(r) == TAG_NO_BYTE) { e = XML_TAG_ERR_UNTERMINATED; goto fail; }
    if (peek(r) != TAG_CLOSE) { e = XML_TAG_ERR_ETAG_ATTRIBUTE; goto fail; }
    eat(r, TAG_CLOSE);

    DCHECK(r->p > start.p && r->fatal == XML_CHAR_OK,
           "an end-tag scan succeeded without consuming anything, so a caller walking [43] content would never "
           "advance");
    DCHECK(n >= start.start && n + n_len <= r->p,
           "an end-tag's element type does not lie inside the run the scan consumed — it is a borrowed slice "
           "and not a copy, so a slice outside means it was measured against something else");
    *name = n;
    *name_len = n_len;
    return XML_TAG_OK;

fail:
    tag_rewind(r, &start, e);
    return e;
}
