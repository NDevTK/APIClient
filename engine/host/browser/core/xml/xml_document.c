/* See xml_document.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_decl.h"
#include "core/xml/xml_document.h"
#include "core/xml/xml_element.h"
#include "core/xml/xml_markup.h"
#include "core/xml/xml_tag.h"
#include "solver/flow.h"

/* §2.8's [28] `doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S? ('[' intSubset ']' S?)? '>'`, AS ITS
   OPENING DELIMITER AND NOWHERE ELSE IN THIS TREE. §1.2 Terminology's `match` performs no case folding, so
   `<!doctype` is not this string and is answered §2.8's own "matches none of [22]'s constructs" instead. The
   byte compare is exact for core/xml/xml_markup.h's reason: every character of it is ASCII, so none can occur
   as a continuation byte of some other code point. It stands here rather than in a component of its own
   because the crash that names it stands here — see xml_document.h — and one site is what makes building [28]
   move the delimiter and the crash together. */
#define DOC_DOCTYPE     "<!DOCTYPE"
#define DOC_DOCTYPE_LEN 9

#define DOC_NO_BYTE 0x100u
#define DOC_OPEN    '<'
#define DOC_BANG    '!'

/* WHERE IN [1] THE WALK STANDS. Three states because [1] has three parts and each admits a DIFFERENT set of
   constructs — a single loop with flags would be the same three rules with nothing naming which one is in
   force, and the sentence a report quotes depends on exactly that. */
typedef enum { DOC_PROLOG, DOC_ELEMENT, DOC_TRAILING } XmlDocumentState;

struct XmlDocumentWalk {
    XmlElementWalk  *element;    /* §3's [39], delegated whole — see xml_document.h */
    XmlDecl          decl;       /* §2.8's [23], when there was one */
    bool             has_decl;
    bool             prolog_entered;
    XmlDocumentState state;
    bool             ended;      /* [1] matched to the last byte of the entity */
    bool             stopped;    /* ended, or §1.2's fatal error was reported */
    Flow            *owner;      /* the flow that created it; see xml_document.h */
};

static void assert_owner(const XmlDocumentWalk *w)
{
    DCHECK(w != NULL, "an XML document walk operation ran on no walk");
    DCHECK(w->owner == flow_running(),
           "an XML document walk was reached by a flow other than the one that created it — it is flow-private "
           "C memory that no COW delta captures, so two flows standing on one of them are two timelines "
           "reading one [1] document. core/xml/xml_element.c states what a fork owes and stands at the same "
           "wall for the element stack underneath this one");
}

const char *xml_document_error_message(XmlDocumentError err)
{
    switch (err) {
    case XML_DOCUMENT_OK:
        return "no document-level well-formedness constraint was violated";
    case XML_DOCUMENT_ERR_NO_ELEMENT:
        return "fatal error (§2.1 Well-Formed XML Documents): [1] document ::= prolog element Misc* — the "
               "element is not optional, and §2.1 states what matching the production implies: \"It contains "
               "one or more elements\", exactly one of which is the root";
    case XML_DOCUMENT_ERR_PROLOG:
        return "fatal error (§2.8 Prolog and Document Type Declaration): [22] prolog ::= XMLDecl? Misc* "
               "(doctypedecl Misc*)? and [27] Misc ::= Comment | PI | S — what stands before the document "
               "element is none of those, and §2.4 Character Data and Markup puts character data inside the "
               "document element and nowhere else";
    case XML_DOCUMENT_ERR_TRAILING:
        return "fatal error (§2.1 Well-Formed XML Documents): [1] document ::= prolog element Misc* admits "
               "only [27] Misc after the root element — \"There is exactly one element, called the root, or "
               "document element, no part of which appears in the content of any other element\", so a second "
               "one at the top level is not a sibling this grammar has";
    case XML_DOCUMENT_ERR_DECL:
        return "fatal error (§2.8 Prolog and Document Type Declaration's [23] XMLDecl): ask "
               "xml_decl_error_message(detail.decl), whose sentence this is";
    case XML_DOCUMENT_ERR_MISC:
        return "fatal error (§2.5 Comments or §2.6 Processing Instructions, reached from [27] Misc): ask "
               "xml_markup_error_message(detail.misc), whose sentence this is";
    case XML_DOCUMENT_ERR_ELEMENT:
        return "fatal error (§3 Logical Structures' [39] element): ask "
               "xml_element_error_message(detail.element), whose sentence this is";
    case XML_DOCUMENT_ERR_CHARACTER:
        return "fatal error (§2.2 Characters or §4.3.3 Character Encoding in Entities): ask "
               "xml_char_error_message(r->fatal), whose sentence this is";
    }
    DFAIL("xml_document_error_message was handed a value that is not an XmlDocumentError — the enum is the "
          "whole list of sentences this component can report and a value outside it names no constraint");
    return "";
}

XmlDocumentWalk *xml_document_walk_create(void)
{
    XmlDocumentWalk *w = calloc(1, sizeof *w);

    CHECK(w != NULL, "the XML document walk could not be allocated");
    w->element = xml_element_walk_create();
    w->state = DOC_PROLOG;
    w->owner = flow_running();
    return w;
}

void xml_document_walk_destroy(XmlDocumentWalk *w)
{
    if (!w) return;
    assert_owner(w);
    DCHECK(w->ended,
           "an XML document walk was DESTROYED before [1] document matched to the last byte of the entity — "
           "see xml_document.h on the two teardowns. `ended` and not `stopped` is the question, because a walk "
           "STOPPED by §1.2 Terminology's fatal error is one that will answer no more items and whose partial "
           "tree its caller discards, which is exactly what abandonment is");
    /* THE ELEMENT WALK BELOW IS DESTROYED RATHER THAN ABANDONED, AND THAT PAIRING IS AN ARGUMENT AND NOT AN
       ASSUMPTION: while [39] is open this walk's only route is xml_element_next, so every error it can report
       from DOC_ELEMENT is that walk's own and stops it — and in DOC_PROLOG and DOC_TRAILING the element stack
       is empty because the root has not opened or has already closed. So `w->ended` implies the stack below
       is empty, which is precisely what xml_element_walk_destroy asserts for itself. */
    xml_element_walk_destroy(w->element);
    free(w);
}

void xml_document_walk_abandon(XmlDocumentWalk *w)
{
    if (!w) return;
    assert_owner(w);
    xml_element_walk_abandon(w->element);
    free(w);
}

bool xml_document_ended(const XmlDocumentWalk *w)
{
    assert_owner(w);
    return w->ended;
}

const XmlDecl *xml_document_declaration(const XmlDocumentWalk *w)
{
    assert_owner(w);
    DCHECK(w->prolog_entered,
           "an XML document's [23] XMLDecl was asked for before the walk had read the prolog — [22] puts it at "
           "offset zero of the entity, so until the first item has been scanned \"there is no declaration\" and "
           "\"nobody has looked yet\" are the same NULL and a consumer cannot tell them apart");
    return w->has_decl ? &w->decl : NULL;
}

static unsigned peek_at(const XmlCharReader *r, size_t n)
{
    DCHECK(r->start <= r->p && r->p <= r->end,
           "an XML document peek was taken from a reader whose cursor is outside its own entity");
    return (size_t)(r->end - r->p) > n ? (unsigned)(unsigned char)r->p[n] : DOC_NO_BYTE;
}

static bool at_doctype(const XmlCharReader *r)
{
    return (size_t)(r->end - r->p) >= DOC_DOCTYPE_LEN && memcmp(r->p, DOC_DOCTYPE, DOC_DOCTYPE_LEN) == 0;
}

/* §2.3's [3] `S`, as the run [27] `Misc`'s third alternative is. Answers nothing, because whether a run was
   there is not a fact any consumer of [1] can see: white space outside the document element becomes no node.
   A latched character error is the CALLER's to notice, exactly as it is in every other scan in this family. */
static void eat_s(XmlCharReader *r)
{
    for (;;) {
        XmlCharReader at = *r;
        uint32_t cp = 0;

        if (xml_char_read(r, &cp) != XML_CHAR_OK) return;
        if (cp == XML_CHAR_EOF || !xml_char_is_s(cp)) { *r = at; return; }
    }
}

/* §2.5's [15] `Comment` and §2.6's [16] `PI` — the two alternatives of [27] `Misc` that BECOME NODES, and
   therefore the two that are items. Answers whether one was read; the reader is left where it stood when the
   answer is false. */
static bool at_misc_item(const XmlCharReader *r)
{
    return xml_markup_at_comment(r) || xml_markup_at_pi(r);
}

static XmlDocumentError scan_misc_item(XmlCharReader *r, XmlContentItem *it, XmlDocumentDetail *d)
{
    XmlMarkupError me;

    if (xml_markup_at_comment(r)) {
        me = xml_markup_scan_comment(r, &it->text);
        if (me != XML_MARKUP_OK) { d->misc = me; return XML_DOCUMENT_ERR_MISC; }
        it->kind = XML_CONTENT_COMMENT;
        return XML_DOCUMENT_OK;
    }
    DCHECK(xml_markup_at_pi(r), "a [27] Misc item scan ran on a reader standing at neither of the two "
                                "alternatives its own peek admits");
    /* A `<?xml` that reaches here is NOT at offset zero — the prolog asks [23]'s question there and only
       there — so §2.6's [17] PITarget subtracts its target and the answer is the reserved-target error, which
       core/xml/xml_markup.h states is the correct report anywhere [23] is not permitted. */
    me = xml_markup_scan_pi(r, &it->pi);
    if (me != XML_MARKUP_OK) { d->misc = me; return XML_DOCUMENT_ERR_MISC; }
    it->kind = XML_CONTENT_PI;
    return XML_DOCUMENT_OK;
}

/* PUT A FAILED CALL'S READER BACK, with core/xml/xml_element.c's carve-out and for its reason — the guard is
 * keyed on the character layer's §1.2 LATCH and never on the error value, because every layer below this one
 * can set that latch while reporting an error of its own. */
static void document_rewind(XmlCharReader *r, const XmlCharReader *start, XmlDocumentError e)
{
    if (r->fatal != XML_CHAR_OK) {
        DCHECK(e == XML_DOCUMENT_ERR_CHARACTER || e == XML_DOCUMENT_ERR_DECL || e == XML_DOCUMENT_ERR_MISC
                   || e == XML_DOCUMENT_ERR_ELEMENT,
               "an XML document scan left the character layer's §1.2 latch set while reporting a failure that "
               "no layer below it accounts for — a latch was set on a path that does not say so, and the "
               "sentence a report quotes would be the wrong one");
        return;
    }
    DCHECK(e != XML_DOCUMENT_ERR_CHARACTER,
           "an XML document scan reported that the character layer detected a fatal error while that layer's "
           "own §1.2 latch is clear — the latch is the fact and this answer is a description of it");
    *r = *start;
}

/* [1]'s TRAILING `Misc*`, looked at ON A COPY so that "the document is over" is a fact this walk states
 * rather than one a caller infers from a peek it would have to spell itself. Called after every item once the
 * root element has closed.
 *   IT LOOKS AHEAD ON A COPY AND COMMITS ONLY ON SUCCESS, AND THAT IS NOT TIDINESS. §2.3's [3] S is read
 * through core/xml/xml_char.h, so reading it can LATCH §1.2's fatal error — and a lookahead that latched the
 * CALLER's reader would make the item this call just produced unreportable, discarding a construct the
 * document really contains because of a byte after it. On the copy, a latch simply means "not the end", the
 * item is delivered, and the next call reads the offending byte and reports it where it stands. The reader is
 * the POD position core/xml/xml_char.h documents, so the copy is free. */
static void note_end(XmlDocumentWalk *w, XmlCharReader *r)
{
    XmlCharReader at = *r;

    DCHECK(w->state == DOC_TRAILING, "the end of [1] document was looked for before its root element closed");
    eat_s(&at);
    if (at.fatal != XML_CHAR_OK || peek_at(&at, 0) != DOC_NO_BYTE) return;
    *r = at;
    w->ended = true;
    w->stopped = true;
}

XmlDocumentError xml_document_next(XmlDocumentWalk *w, XmlCharReader *r, XmlContentItem *out,
                                   XmlDocumentDetail *detail)
{
    XmlCharReader start;
    XmlContentItem it;
    XmlDocumentError e;

    assert_owner(w);
    DCHECK(r != NULL && out != NULL && detail != NULL,
           "an XML document scan was asked for with no reader, nowhere to put the item, or nowhere to put the "
           "layers' answers");
    DCHECK(!w->stopped,
           "an XML document scan was asked for another item after this walk was finished — [1] document ends "
           "at the last byte of the entity and §1.2 Terminology ends it when a fatal error is reported, and in "
           "both cases the processor MUST NOT continue normal processing");
    DCHECK(r->fatal == XML_CHAR_OK,
           "an XML document scan was asked for an item from a reader that has already latched a fatal error — "
           "§1.2 Terminology: once one is detected the processor MUST NOT continue");

    detail->decl = XML_DECL_OK;
    detail->misc = XML_MARKUP_OK;
    detail->element = XML_ELEMENT_OK;
    detail->within.tag = XML_TAG_OK;
    detail->within.markup = XML_MARKUP_OK;
    detail->within.ref = XML_REF_OK;
    memset(&it, 0, sizeof it);
    it.ref.cp = XML_CHAR_EOF;
    start = *r;

    if (w->state == DOC_ELEMENT) {
        XmlElementError ee = xml_element_next(w->element, r, &it, &detail->within);

        if (ee != XML_ELEMENT_OK) { detail->element = ee; e = XML_DOCUMENT_ERR_ELEMENT; goto fail; }
        /* The element walk's stack emptying IS the end of [39], which is the end of [1]'s middle part. */
        if (xml_element_depth(w->element) == 0) { w->state = DOC_TRAILING; note_end(w, r); }
        goto done;
    }

    if (w->state == DOC_PROLOG) {
        /* [22]'s `XMLDecl?` IS AT OFFSET ZERO OR NOWHERE, and the reader's own cursor is that fact — a flag
           would be a second copy of it. core/xml/xml_decl.h states from its side that where a declaration may
           stand is the caller's rule, and this is the caller. */
        if (!w->prolog_entered) {
            w->prolog_entered = true;
            DCHECK(r->p == r->start,
                   "an XML document walk was started on a reader that does not stand at the first byte of the "
                   "entity — [22] prolog puts `XMLDecl?` at offset zero and §4.3.3's encoding signature is "
                   "consumed before a character is read, so a cursor anywhere else means this walk was handed "
                   "a slice of a document rather than one");
            if (xml_decl_at(r)) {
                XmlDeclError de = xml_decl_scan_xmldecl(r, &w->decl);

                if (de != XML_DECL_OK) { detail->decl = de; e = XML_DOCUMENT_ERR_DECL; goto fail; }
                w->has_decl = true;
            }
        }
        /* ONE PASS AND NOT A LOOP: [27]'s `S` is the only construct here that is not an item, so consuming
           it once is enough to reach the one this call answers with. A loop would suggest a second
           non-item construct exists in [22], and there is none. */
        eat_s(r);                                    /* [27] Misc's third alternative — never an item */
        if (r->fatal != XML_CHAR_OK) { e = XML_DOCUMENT_ERR_CHARACTER; goto fail; }
        if (peek_at(r, 0) == DOC_NO_BYTE) { e = XML_DOCUMENT_ERR_NO_ELEMENT; goto fail; }
        if (at_misc_item(r)) {
            e = scan_misc_item(r, &it, detail);
            if (e != XML_DOCUMENT_OK) goto fail;
            goto done;
        }
        /* §2.8's [28], the one construct here that is a MISSING CAPABILITY rather than a mistake — see
           xml_document.h for what must be built with it and why skipping it would be a security defect.
           IT IS A `CHECK_FAIL` AND NOT A `DFAIL`, WHICH IS THE ONE PLACE IN THIS COMPONENT SET THAT IS TRUE,
           and the reason is what a release build would otherwise SAY. A DFAIL compiles to nothing outside
           development, so the `<!DOCTYPE` would fall through to the arm below and be reported
           XML_DOCUMENT_ERR_PROLOG — "what stands before the document element matches none of [22]'s
           constructs" — about a document that matches [22] EXACTLY. That is a plausible diagnosis of the
           wrong thing, which is worse than silence, and it is a sentence a page author would be sent to
           §2.8 to check and find correct. The parse must fail in both builds (§Offensive-programming: a
           capability that is not supportable outside development fails rather than fabricating an answer),
           so it fails ONCE, through one mechanism, naming the real reason. It goes away when [28] is
           built — it is transition scaffolding whose only correct trajectory is to zero. */
        if (at_doctype(r))
            CHECK_FAIL("XML 1.0 (Fifth Edition) §2.8's [28] doctypedecl stands in this document's [22] prolog "
                       "and this build has no DTD subsystem. THIS IS A SECURITY BOUNDARY AND NOT A GAP TO "
                       "SKIP: nothing here reads a DTD, which is exactly why core/xml/xml_tag.c and "
                       "core/xml/xml_element.c may answer an [68] EntityRef outside §4.6's five with §4.1's "
                       "[WFC: Entity Declared] — for a document with no DTD that IS the standard's answer. "
                       "Reading past this declaration would leave both of those sites answering for a document "
                       "that HAS declarations. WHAT MUST BE BUILT, IN ONE DIFF: §4.2's [70] EntityDecl and "
                       "[71] GEDecl over §4.2.2's [75] ExternalID (core/xml/xml_literal.h already holds its "
                       "[11] SystemLiteral and [12] PubidLiteral), TOGETHER WITH §3.1's [WFC: No External "
                       "Entity References] (\"Attribute values MUST NOT contain direct or indirect entity "
                       "references to external entities\") and §4.4.4 Forbidden's third bullet (\"a reference "
                       "to an external entity in an attribute value\"), both of which make it a FATAL ERROR "
                       "and not a configuration choice. §4.4's Entity Type Table is why the two entity sites "
                       "must stay two: `Reference in Content` and `Reference in Attribute Value` are different "
                       "rows, and an external entity is Included in the first and Forbidden in the second. "
                       "§2.9's [32] SDDecl is already answered — xml_document_declaration hands back the [23] "
                       "XMLDecl this walk read — and it is the third alternative of [WFC: Entity Declared], so "
                       "the constraint becomes decidable in full the moment [28] is read");
        if (peek_at(r, 0) == DOC_OPEN && peek_at(r, 1) != DOC_BANG && !xml_tag_at_etag(r)) {
            /* [1]'s `element`. A `<` that opens neither a `<!` form nor an end-tag is [40]/[44]'s, which is
               core/xml/xml_tag.h's own peek — asked here through its two halves because a `</` standing
               before any element has been opened is a PROLOG mistake and not the element walk's to report:
               that walk's first call asserts it was handed a start-tag, so routing one there would crash the
               engine on a document's own error. */
            XmlElementError ee;

            DCHECK(xml_tag_at_stag(r),
                   "the prolog routed a `<` to [1]'s element that core/xml/xml_tag.h does not call a "
                   "start-tag — the two spellings of that delimiter have drifted apart");
            w->state = DOC_ELEMENT;
            ee = xml_element_next(w->element, r, &it, &detail->within);
            if (ee != XML_ELEMENT_OK) { detail->element = ee; e = XML_DOCUMENT_ERR_ELEMENT; goto fail; }
            DCHECK(it.kind == XML_CONTENT_ELEMENT_START,
                   "the first item of [1]'s element is not a start — [39] begins with [40] or [44] and both "
                   "report one, so the element walk answered with something that is not a boundary of the "
                   "element it was just started on");
            goto done;
        }
        e = XML_DOCUMENT_ERR_PROLOG;
        goto fail;
    }

    DCHECK(w->state == DOC_TRAILING, "an XML document walk is in no state [1] document has");
    eat_s(r);                                        /* [1]'s trailing `Misc*` */
    if (r->fatal != XML_CHAR_OK) { e = XML_DOCUMENT_ERR_CHARACTER; goto fail; }
    if (!at_misc_item(r)) { e = XML_DOCUMENT_ERR_TRAILING; goto fail; }
    e = scan_misc_item(r, &it, detail);
    if (e != XML_DOCUMENT_OK) goto fail;
    /* No latch test after this: `note_end` looks ahead on a COPY precisely so that it cannot put one on the
       caller's reader, and a test here would be a second site reasoning about a state that cannot arise. */
    note_end(w, r);

done:
    DCHECK(detail->decl == XML_DECL_OK && detail->misc == XML_MARKUP_OK
               && detail->element == XML_ELEMENT_OK,
           "a successful XML document item carries a layer's error report");
    DCHECK(r->fatal == XML_CHAR_OK, "a successful XML document scan left the reader's §1.2 latch set");
    DCHECK(r->p > start.p || it.kind == XML_CONTENT_ELEMENT_END,
           "an XML document scan produced an item without consuming anything — [44] EmptyElemTag's owed close "
           "is the one item that reads nothing, and it is the element walk's exception rather than a second "
           "one this level invents");
    DCHECK(!w->ended || w->state == DOC_TRAILING,
           "an XML document walk answered that [1] had matched to the end of the entity while standing "
           "somewhere other than its trailing Misc*");
    *out = it;
    return XML_DOCUMENT_OK;

fail:
    DCHECK((e == XML_DOCUMENT_ERR_DECL) == (detail->decl != XML_DECL_OK),
           "an XML document answer and its §2.8 detail disagree about whether the declaration layer reported "
           "anything");
    DCHECK((e == XML_DOCUMENT_ERR_MISC) == (detail->misc != XML_MARKUP_OK),
           "an XML document answer and its [27] Misc detail disagree about whether that layer reported "
           "anything");
    DCHECK((e == XML_DOCUMENT_ERR_ELEMENT) == (detail->element != XML_ELEMENT_OK),
           "an XML document answer and its §3 detail disagree about whether the element walk reported "
           "anything");
    document_rewind(r, &start, e);
    w->stopped = true;                               /* §1.2: a fatal error ends the parse */
    return e;
}
