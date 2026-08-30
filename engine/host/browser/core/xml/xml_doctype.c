/* See core/xml/xml_doctype.h. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_doctype.h"
#include "core/xml/xml_external_id.h"
#include "core/xml/xml_name.h"

/* §2.8's [28] `doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S? ('[' intSubset ']' S?)? '>'`, AS ITS
   OPENING DELIMITER AND NOWHERE ELSE IN THIS TREE — see core/xml/xml_doctype.h on why it stands here and not
   where the crash that used to name it stood. */
#define DT_DOCTYPE     "<!DOCTYPE"
#define DT_DOCTYPE_LEN 9

#define DT_NO_BYTE   0x100u
#define DT_SUBSET    '['
#define DT_CLOSE     '>'

const char *xml_doctype_error_message(XmlDoctypeError err)
{
    switch (err) {
    case XML_DOCTYPE_OK:
        return "no document-type-declaration well-formedness constraint was violated";
    case XML_DOCTYPE_ERR_SPACE:
        return "fatal error (§2.8 Prolog and Document Type Declaration): [28] doctypedecl ::= '<!DOCTYPE' S "
               "Name (S ExternalID)? S? ('[' intSubset ']' S?)? '>' — the S between the delimiter and the "
               "Name is written S and not S?, and [3] S is one or more white space characters";
    case XML_DOCTYPE_ERR_NAME:
        return "fatal error (§2.3 Common Syntactic Constructs, reached from §2.8's [28] doctypedecl): [5] "
               "Name ::= NameStartChar (NameChar)* — what stands after the space is not a Name, and §2.8's "
               "[VC: Root Element Type] makes this the element type of the root element";
    case XML_DOCTYPE_ERR_EXTERNAL_ID:
        return "fatal error (§4.2.2 External Entities' [75] ExternalID): ask "
               "xml_external_id_error_message(detail.external), whose sentence this is";
    case XML_DOCTYPE_ERR_COMPONENT:
        return "fatal error (§2.8 Prolog and Document Type Declaration): after [28] doctypedecl's Name and "
               "its optional [75] ExternalID the production admits only the '[' that opens an internal "
               "subset or the '>' that closes the declaration, and what stands here is neither";
    case XML_DOCTYPE_ERR_UNTERMINATED:
        return "fatal error (§2.8 Prolog and Document Type Declaration): [28] doctypedecl closes with '>' and "
               "the entity ends before this declaration does";
    case XML_DOCTYPE_ERR_CHARACTER:
        return "fatal error (§2.2 Characters or §4.3.3 Character Encoding in Entities): ask "
               "xml_char_error_message(r->fatal), whose sentence this is";
    }
    DFAIL("xml_doctype_error_message was handed a value that is not an XmlDoctypeError — the enum is the "
          "whole list of sentences this component can report and a value outside it names no constraint");
    return "";
}

static unsigned peek(const XmlCharReader *r)
{
    DCHECK(r->start <= r->p && r->p <= r->end,
           "a document-type-declaration peek was taken from a reader whose cursor is outside its own entity");
    return r->p == r->end ? DT_NO_BYTE : (unsigned)(unsigned char)*r->p;
}

/* §2.3's [3] `S`, as the RUN [28] spells `S` in one position and `S?` in two others. Answers how many
   characters it consumed, which is what tells those two apart. A latched character error is the CALLER's to
   notice, exactly as it is in every other scan in this family. */
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

/* CONSUME THE DELIMITER THE CALLER HAS ALREADY PEEKED, through the READER rather than by advancing the cursor,
   so `line` and `column` count it — the position a `parsererror` quotes for anything after it is measured from
   there. The reads cannot fail: the peek matched the bytes and every character of `<!DOCTYPE` is ASCII. */
static void eat_delimiter(XmlCharReader *r)
{
    size_t i;

    DCHECK((size_t)(r->end - r->p) >= DT_DOCTYPE_LEN && memcmp(r->p, DT_DOCTYPE, DT_DOCTYPE_LEN) == 0,
           "a document-type-declaration scan consumed a delimiter its peek had not matched");
    for (i = 0; i < DT_DOCTYPE_LEN; i++) {
        uint32_t cp = 0;
        XmlCharError e = xml_char_read(r, &cp);

        DCHECK(e == XML_CHAR_OK && cp == (uint32_t)(unsigned char)DT_DOCTYPE[i],
               "the `<!DOCTYPE` delimiter did not read back the characters its peek matched — the peek is a "
               "byte compare over ASCII and the reader produces those same bytes as characters, so a "
               "disagreement means the two spellings of that delimiter have drifted apart");
        (void)e;
    }
}

/* §2.3's [5] `Name`, scanned the way a tokenizer has to — [4] NameStartChar of the first character and [4a]
   NameChar of each one after, until one says no. The slice BORROWS the entity, which is exact by
   core/xml/xml_ref.h's argument: §2.11 only ever rewrites #xD, and #xD is in neither class, so no character
   of a Name can differ from the bytes it was decoded from. That is held to core/xml/xml_name.h's SLICE
   spelling of the same production on every successful scan rather than trusted to agree with it — this file
   is the FOURTH reader-side transcription of [5] in this component set, and the assert is what that
   arrangement rests on: each caller reports a different sentence when there is no Name here, so the scan is
   private and the PRODUCTION is shared.
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

bool xml_doctype_at(const XmlCharReader *r)
{
    DCHECK(r != NULL, "a document-type-declaration peek was asked for with no reader");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a document-type-declaration peek was taken from a reader that has already reported a fatal error "
           "— §1.2 Terminology: once a fatal error is detected the processor MUST NOT continue normal "
           "processing, so the caller owes a stop here and not another construct");
    return (size_t)(r->end - r->p) >= DT_DOCTYPE_LEN && memcmp(r->p, DT_DOCTYPE, DT_DOCTYPE_LEN) == 0;
}

XmlDoctypeError xml_doctype_scan(XmlCharReader *r, XmlDoctype *out, XmlDoctypeDetail *detail)
{
    XmlCharReader   start;
    XmlDoctype      dt;
    XmlDoctypeError e;
    size_t          n;

    DCHECK(r != NULL && out != NULL && detail != NULL,
           "a document-type-declaration scan was asked for with no reader, nowhere to put the declaration, or "
           "nowhere to put the layers' answers");
    DCHECK(xml_doctype_at(r),
           "a document-type-declaration scan ran on a reader that does not stand at `<!DOCTYPE` — the peek is "
           "the caller's, because WHERE a declaration may stand is [22] prolog's rule and not this "
           "component's");

    detail->external = XML_EXTERNAL_ID_OK;
    detail->literal  = XML_LITERAL_OK;
    memset(&dt, 0, sizeof dt);
    start = *r;
    eat_delimiter(r);

    /* [28]'s `S` — written `S` and not `S?`, so zero is never a legal answer here. The LATCH is tested before
       the count for core/xml/xml_external_id.c's reason: a run that stopped because §2.2's [2] Char was
       violated would otherwise be reported as a missing space, which is a plausible diagnosis of the wrong
       thing. */
    n = eat_s(r);
    if (r->fatal != XML_CHAR_OK) { e = XML_DOCTYPE_ERR_CHARACTER; goto fail; }
    if (n == 0)                  { e = XML_DOCTYPE_ERR_SPACE;     goto fail; }

    /* [28]'s `Name`, which §2.8's [VC: Root Element Type] makes the element type of the root element. That
       constraint is a VALIDITY constraint — §1.2 Terminology's `error`, not its `fatal error` — so it is not
       decided here and not decided anywhere in this build: §5.1 Validating and Non-Validating Processors puts
       it on a validating processor, which this is not. */
    if (!scan_name(r, &dt.name, &dt.name_len)) {
        e = r->fatal != XML_CHAR_OK ? XML_DOCTYPE_ERR_CHARACTER : XML_DOCTYPE_ERR_NAME;
        goto fail;
    }

    /* [28]'s `(S ExternalID)? S?` — ONE run of [3] S serves both when the identifier is absent, which is why
       this is one `eat_s` and not two. A [75] standing here implies the run was non-empty, because every
       character of both its keywords is a [4a] NameChar and the Name scan above would have absorbed them. */
    n = eat_s(r);
    if (r->fatal != XML_CHAR_OK) { e = XML_DOCTYPE_ERR_CHARACTER; goto fail; }
    if (xml_external_id_at(r)) {
        XmlExternalIdError xe;

        DCHECK(n > 0,
               "[75] ExternalID stands immediately after [28]'s Name with no [3] S between them — every "
               "character of `SYSTEM` and `PUBLIC` is a [4a] NameChar, so the Name scan would have consumed "
               "them and this reader could not be standing where it is");
        xe = xml_external_id_scan(r, &dt.external, &detail->literal);
        if (xe != XML_EXTERNAL_ID_OK) {
            detail->external = xe;
            e = xe == XML_EXTERNAL_ID_ERR_CHARACTER ? XML_DOCTYPE_ERR_CHARACTER : XML_DOCTYPE_ERR_EXTERNAL_ID;
            goto fail;
        }
        dt.has_external = true;
        /* [28]'s own `S?`, which only exists as a separate run once the identifier has been read. */
        (void)eat_s(r);
        if (r->fatal != XML_CHAR_OK) { e = XML_DOCTYPE_ERR_CHARACTER; goto fail; }
    }

    /* §2.8's [28b] `intSubset`, the one construct of [28] that is a MISSING CAPABILITY rather than a mistake.
       See core/xml/xml_doctype.h for the whole argument and for why this is a CHECK_FAIL rather than a DFAIL.
         WHAT WOULD MAKE SKIPPING IT INVISIBLE IS PRECISELY WHY IT MUST NOT BE SKIPPED. DOM's `DocumentType`
       has exactly three members — `name`, `publicId`, `systemId` — and no `internalSubset` among them, so a
       subset read and discarded would leave NOTHING in the tree to say it was dropped. Its effects are what a
       page sees: an [68] EntityRef to an entity declared here is INCLUDED (§4.4.2 Included) and would instead
       be reported undeclared, and §3.3.2 Attribute Defaults' declared defaults are attributes that must
       APPEAR on elements whose start-tags do not carry them. */
    if (peek(r) == DT_SUBSET)
        CHECK_FAIL("XML 1.0 (Fifth Edition) §2.8's [28] doctypedecl carries an INTERNAL SUBSET and this build "
                   "has no DTD subsystem. The declaration itself is read — `<!DOCTYPE html>` and `<!DOCTYPE "
                   "svg PUBLIC \"…\" \"…\">` build a DOM §4.6 DocumentType node — and what stops here is "
                   "[28b] intSubset ::= (markupdecl | DeclSep)*. WHAT MUST BE BUILT: [29] markupdecl's six "
                   "productions — §3.2's [45] elementdecl, §3.3's [52] AttlistDecl WITH §3.3.2 Attribute "
                   "Defaults (a declared default is an attribute that must APPEAR on an element whose "
                   "start-tag omits it) and §3.3.3 Attribute-Value Normalization's declared-type rule, §4.2's "
                   "[70] EntityDecl, §4.7's [82] NotationDecl, and [15] Comment and [16] PI, which "
                   "core/xml/xml_markup.h already scans; then [28a] DeclSep ::= PEReference | S under [WFC: "
                   "PEs in Internal Subset] (\"In the internal DTD subset, parameter-entity references MUST "
                   "NOT occur within markup declarations; they may occur where markup declarations can "
                   "occur\") and [WFC: PE Between Declarations]; and §4.5 Construction of Entity Replacement "
                   "Text, which is what makes §4.4.2 Included happen at every [68] EntityRef. THE EXPANSION "
                   "IS A FLOW AND NOT A BOUNDED LOOP: a replacement text may contain further references, so "
                   "its growth is the document's to choose, and CLAUDE.md §NO BOUNDS forbids the usual answer "
                   "— an expansion depth or size cap. It is parkable, preemptible and paged to the cold tier "
                   "like every other unbounded walk here, with the physical RAM→DISK floor as the only limit. "
                   "AND IT IS A SECURITY BOUNDARY IN THE SAME DIFF: §3.1's [WFC: No External Entity "
                   "References] (\"Attribute values MUST NOT contain direct or indirect entity references to "
                   "external entities\") and §4.4.4 Forbidden's third bullet make an external entity in an "
                   "attribute value a FATAL ERROR and not a configuration choice, and §4.4's table is why the "
                   "two entity sites stay two — `Reference in Content` and `Reference in Attribute Value` are "
                   "different rows and an external entity is Included in the first and Forbidden in the "
                   "second. §3.4 Conditional Sections are NOT part of this: §2.8 says the construct is \"not "
                   "allowed in the internal subset but is allowed in external parameter entities referenced "
                   "in the internal subset\", so [61] belongs with [30] extSubset and never with [28b]");

    if (peek(r) == DT_NO_BYTE) { e = XML_DOCTYPE_ERR_UNTERMINATED; goto fail; }
    if (peek(r) != DT_CLOSE)   { e = XML_DOCTYPE_ERR_COMPONENT;    goto fail; }
    {
        uint32_t cp = 0;
        XmlCharError ce = xml_char_read(r, &cp);

        DCHECK(ce == XML_CHAR_OK && cp == (uint32_t)DT_CLOSE,
               "[28]'s closing '>' did not read back the character its peek matched — the peek is a byte "
               "compare over ASCII and the reader produces that same byte as a character");
        (void)ce;
    }

    DCHECK(dt.name != NULL && dt.name_len > 0,
           "a successful [28] doctypedecl scan produced no Name — [28] writes `Name` without a quantifier, so "
           "there is no reading of the production with none");
    DCHECK(dt.has_external == (dt.external.system_id != NULL),
           "[28]'s `(S ExternalID)?` and the identifier beside it disagree about whether one was read — both "
           "alternatives of [75] end in a [11] SystemLiteral, so `has_external` and a non-NULL system "
           "identifier are one fact and a consumer told two different answers would report an absent external "
           "subset as present or the reverse");
    DCHECK(r->fatal == XML_CHAR_OK,
           "a successful [28] doctypedecl scan left the reader's §1.2 latch set — the latch is a FATAL error, "
           "after which §1.2 Terminology says a processor MUST NOT continue normal processing");
    *out = dt;
    return XML_DOCTYPE_OK;

fail:
    DCHECK((e == XML_DOCTYPE_ERR_EXTERNAL_ID) == (detail->external != XML_EXTERNAL_ID_OK
                                                  && e != XML_DOCTYPE_ERR_CHARACTER),
           "a [28] doctypedecl answer and its §4.2.2 detail disagree about whether that layer reported "
           "anything");
    /* PUT A FAILED SCAN'S READER BACK, with the family's one carve-out and for its reason — the guard is keyed
       on the character layer's §1.2 LATCH and never on the error value, because restoring a saved reader over
       a set latch would put it back to XML_CHAR_OK and silently un-report a fatal error. */
    if (r->fatal == XML_CHAR_OK) *r = start;
    else DCHECK(e == XML_DOCTYPE_ERR_CHARACTER,
                "a [28] doctypedecl scan left the character layer's §1.2 latch set while reporting a failure "
                "that does not name that layer — a latch was set on a path that does not say so, and the "
                "sentence a report quotes would be the wrong one");
    return e;
}
