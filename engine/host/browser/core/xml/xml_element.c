/* See xml_element.h. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/core/array_obj.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_element.h"
#include "core/xml/xml_markup.h"
#include "core/xml/xml_ref.h"
#include "core/xml/xml_tag.h"
#include "solver/flow.h"

/* THE THREE BYTES §2.4 SUBTRACTS FROM [14] `CharData`, AND THE TWO THAT END IT. Each is ASCII, so a byte test
   for one is exact: no continuation byte of a multi-byte UTF-8 sequence is ever below 0x80, which is the same
   argument core/xml/xml_markup.c makes for [21] CDEnd and core/xml/xml_tag.c for its delimiters. */
#define EL_NO_BYTE 0x100u                        /* above every byte, so no peek can be confused with one */
#define EL_OPEN    '<'
#define EL_AMP     '&'
#define EL_BRACKET ']'
#define EL_CLOSE   '>'

/* ONE OPEN ELEMENT. The element type BORROWS the entity, which is exact rather than merely convenient: §2.11
   End-of-Line Handling only ever rewrites #xD, and #xD is in neither §2.3's [4] NameStartChar nor its [4a]
   NameChar, so no character of a Name can differ from the bytes it was decoded from — core/xml/xml_tag.h makes
   the same argument for the slice it hands out, and this is that slice kept. */
typedef struct { const char *name; size_t name_len; } XmlOpenElement;

struct XmlElementWalk {
    lexbor_array_obj_t *open;     /* XmlOpenElement — the stack §C-stack requires instead of C recursion */
    XmlTag              tag;      /* the last XML_CONTENT_ELEMENT_START's tag; owned here, one free site */
    bool                empty_end;/* the item before this one was a [44] START, so its END is owed and free */
    bool                stopped;  /* [39] finished, or §1.2's fatal error was reported */
    Flow               *owner;    /* the flow that created it; see xml_element.h's last paragraph */
};

/* §Offensive-programming's "assert every invariant eagerly at its origin", for the one invariant this
   component makes about ITSELF rather than about its input. */
static void assert_owner(const XmlElementWalk *w)
{
    DCHECK(w != NULL, "an XML element walk operation ran on no walk");
    DCHECK(w->owner == flow_running(),
           "an XML element walk was reached by a flow other than the one that created it — the stack is "
           "flow-private C memory that no COW delta captures, so two flows standing on one of them are two "
           "timelines pushing one set of open elements, and the [WFC: Element Type Match] each of them decides "
           "is about the other's tree. What has to be built is this state's JSStepVisit declaration in the "
           "step machine that holds the parse, and it is not a byte copy: every name on this stack is an "
           "interior pointer into the ENTITY the reader was initialised over. core/xml/xml_ns.c stands at the "
           "same wall for the namespace scope stack and core/html/fragment_parser.c's fragment_parse_unforkable is what stands in "
           "until a state declares one");
}

const char *xml_element_error_message(XmlElementError err)
{
    switch (err) {
    case XML_ELEMENT_OK:
        return "no content-level well-formedness constraint was violated";
    case XML_ELEMENT_ERR_ELEMENT_TYPE_MATCH:
        return "fatal error (§3 Logical Structures) [WFC: Element Type Match]: \"The Name in an element's "
               "end-tag MUST match the element type in the start-tag\" — this end-tag names a different "
               "element than the one it closes, so [39] element matched neither of its alternatives";
    case XML_ELEMENT_ERR_UNCLOSED:
        return "fatal error (§3 Logical Structures): [39] element ::= EmptyElemTag | STag content ETag — the "
               "entity ends with an element still open, so the [42] ETag its [40] STag requires is not there";
    case XML_ELEMENT_ERR_CDATA_SECTION_CLOSE:
        return "fatal error (§2.4 Character Data and Markup): [14] CharData ::= [^<&]* - ([^<&]* ']]>' "
               "[^<&]*) — the string \"]]>\" is subtracted from character data, and §2.4 requires the `>` to "
               "be escaped as \"&gt;\" or a character reference where that string appears in content without "
               "closing a CDATA section";
    case XML_ELEMENT_ERR_CONTENT:
        return "fatal error (§3.1 Start-Tags, End-Tags, and Empty-Element Tags): [43] content admits an "
               "element, a [67] Reference, a [18] CDSect, a [16] PI and a [15] Comment, and this `<!` opens "
               "none of them — §2.8 puts [28] doctypedecl in [22] prolog, before the first element, and the "
               "markup declarations it contains stand only inside it";
    case XML_ELEMENT_ERR_ENTITY_UNDECLARED:
        return "fatal error (§4.1 Character and Entity References) [WFC: Entity Declared]: \"the Name given "
               "in the entity reference MUST match that in an entity declaration ... except that well-formed "
               "documents need not declare any of the following entities: amp, lt, gt, apos, quot\" — no "
               "declaration this parse read matches this Name and it is none of §4.6 Predefined Entities' "
               "five";
    case XML_ELEMENT_ERR_TAG:
        return "fatal error (§3.1 Start-Tags, End-Tags, and Empty-Element Tags): ask "
               "xml_tag_error_message(detail.tag), whose sentence this is";
    case XML_ELEMENT_ERR_MARKUP:
        return "fatal error (§2.5 Comments, §2.6 Processing Instructions or §2.7 CDATA Sections): ask "
               "xml_markup_error_message(detail.markup), whose sentence this is";
    case XML_ELEMENT_ERR_REFERENCE:
        return "fatal error (§4.1 Character and Entity References): ask xml_ref_error_message(detail.ref), "
               "whose sentence this is";
    case XML_ELEMENT_ERR_CHARACTER:
        return "fatal error (§2.2 Characters or §4.3.3 Character Encoding in Entities): ask "
               "xml_char_error_message(r->fatal), whose sentence this is";
    }
    DFAIL("xml_element_error_message was handed a value that is not an XmlElementError — the enum is the whole "
          "list of sentences this component can report and a value outside it names no constraint");
    return "";
}

XmlElementWalk *xml_element_walk_create(void)
{
    XmlElementWalk *w = calloc(1, sizeof *w);

    CHECK(w != NULL, "the XML element walk could not be allocated");
    w->open = lexbor_array_obj_create();
    CHECK(w->open != NULL, "the XML element stack could not be allocated");
    CHECK(lexbor_array_obj_init(w->open, 16, sizeof(XmlOpenElement)) == LXB_STATUS_OK,
          "the XML element stack could not be initialised");
    w->owner = flow_running();
    return w;
}

/* THE TEARDOWN, WHICH THE TWO ANSWERS BELOW SHARE. They differ in what they ASSERT and in nothing else, so
   `open` and the last start-tag's §3.3.3 values still have exactly one free site each. */
static void element_walk_free(XmlElementWalk *w)
{
    lexbor_array_obj_destroy(w->open, true);
    /* The last start-tag's §3.3.3 values, whose one free site this is (xml_element.h on the item's lifetime).
       A record no scan filled is zeroed, and xml_tag_free on one is a no-op. */
    xml_tag_free(&w->tag);
    free(w);
}

void xml_element_walk_destroy(XmlElementWalk *w)
{
    if (!w) return;
    assert_owner(w);
    DCHECK(lexbor_array_obj_length(w->open) == 0,
           "an XML element walk was DESTROYED with elements still open — the stack empties exactly when [39] "
           "element ends, so a residue here is a caller that stopped reading items in the middle of one and a "
           "tree missing every node below that point. THE WALK'S OWN `stopped` FLAG CANNOT ANSWER THIS AND "
           "MUST NOT BE ADMITTED AS A DISJUNCT HERE: it records a fatal error THIS walk reported and nothing "
           "else, while Namespaces in XML §6.3 Uniqueness of Attributes' [NSC: Attributes Unique] is decided "
           "one layer up over a start-tag item this walk answered successfully — so `stopped ||` admits one "
           "kind of unfinished [39] and crashes on the others, which is a fact about the OPERATION read off "
           "the OBJECT. xml_element_walk_abandon is that fact stated by the caller that holds it");
    element_walk_free(w);
}

void xml_element_walk_abandon(XmlElementWalk *w)
{
    if (!w) return;
    assert_owner(w);
    /* NO RESIDUE ASSERTION, AND ITS ABSENCE IS THE POSITIVE STATEMENT — open elements ARE what abandonment
       means, and this entry exists so that `destroy` above can be the pure one. Nor may the MIRROR be
       asserted: a parse is abandoned between two items, and the item before the teardown can perfectly well
       have been the root's end-tag, so "something must still be open" is false of a legitimate abandonment. */
    element_walk_free(w);
}

size_t xml_element_depth(const XmlElementWalk *w)
{
    assert_owner(w);
    return lexbor_array_obj_length(w->open);
}

/* THE ITEM IS TOTAL FOR ITS KIND, ASSERTED IN BOTH DIRECTIONS. Every field the kind does not write holds a
   value no predicate accepts, so `kind` is the only thing that decides what to read — and a kind that forgot
   to write its own field would otherwise hand a consumer a NULL that reads as "absent" for a construct the
   document contains. */
static void assert_item(const XmlContentItem *it)
{
    bool start = it->kind == XML_CONTENT_ELEMENT_START;
    bool end = it->kind == XML_CONTENT_ELEMENT_END;
    bool text = it->kind == XML_CONTENT_CHARDATA || it->kind == XML_CONTENT_CDSECT
                || it->kind == XML_CONTENT_COMMENT;
    bool pi = it->kind == XML_CONTENT_PI;
    bool ref = it->kind == XML_CONTENT_REFERENCE;

    DCHECK(start == (it->tag.name != NULL),
           "a content item's [40]/[44] tag and its kind disagree — a start is the only kind that carries one");
    DCHECK((it->tag.atts != NULL) == (it->tag.att_n != 0),
           "a start-tag item's attribute array and its count disagree about whether there are any");
    DCHECK(end == (it->name != NULL),
           "a content item's element type and its kind disagree — an end is the only kind that carries one");
    DCHECK(!end || it->name_len > 0,
           "an end item names a zero-length element type, and [5] Name matches no empty string");
    DCHECK(text == (it->text.raw != NULL),
           "a content item's borrowed run and its kind disagree — §2.4's [14] CharData, §2.7's [20] CData and "
           "§2.5's comment body are the three that carry one");
    DCHECK(it->kind != XML_CONTENT_CHARDATA || it->text.raw_len > 0,
           "a zero-length [14] CharData was reported as an item — [43] content writes `CharData?` in both of "
           "its positions, so an absent run is the grammar's own answer and an empty Text node is a node no "
           "XML parser produces");
    DCHECK(pi == (it->pi.target != NULL),
           "a content item's [17] PITarget and its kind disagree — §2.6's instruction is the only kind that "
           "carries one");
    DCHECK(ref == (it->ref.cp != XML_CHAR_EOF),
           "a content item's resolved character and its kind disagree — XML_CHAR_EOF is above [2] Char's "
           "ceiling precisely so that a consumer reading the wrong field fails xml_char_is_char rather than "
           "emitting U+0000 as a character the document contained");
    DCHECK(!ref || it->ref.kind != XML_REF_ENTITY,
           "an unresolved [68] EntityRef reached a content item — §4.1's [WFC: Entity Declared] is decided "
           "before the item is built, so a kind that survives it has a character");
    DCHECK(it->kind != XML_CONTENT_DOCTYPE && it->doctype.name == NULL,
           "§2.8's [28] doctypedecl reached an item this walk produced — [28] stands in [22] prolog, before "
           "the first element, and [43] content has no alternative it could match, so the only walk that may "
           "produce one is core/xml/xml_document.h's and a doctype here means the two have been confused");
}

/* THE DETAIL IS WRITTEN ON EVERY ANSWER AND IS THEN HELD TO THE ANSWER. xml_element.h states the one
   combination that carries two non-OK fields and why it is the standard's doing; every other answer sets at
   most one, and a detail that names a layer the report does not is a report that would send an author to the
   wrong sentence. */
static void assert_detail(XmlElementError e, const XmlElementDetail *d)
{
    DCHECK((e == XML_ELEMENT_ERR_TAG) == (d->tag != XML_TAG_OK),
           "an XML content answer and its §3.1 detail disagree about whether the tag layer reported anything");
    DCHECK((e == XML_ELEMENT_ERR_MARKUP) == (d->markup != XML_MARKUP_OK),
           "an XML content answer and its §2.5/§2.6/§2.7 detail disagree about whether that layer reported "
           "anything");
    DCHECK(d->ref == XML_REF_OK || e == XML_ELEMENT_ERR_REFERENCE
               || (e == XML_ELEMENT_ERR_TAG && d->tag == XML_TAG_ERR_REFERENCE),
           "an XML content answer carries a §4.1 detail for a report that is neither the reference layer's nor "
           "a tag scan that carried §4.1's answer out of an [10] AttValue — those are the only two ways a "
           "reference error reaches this level, so a third means the field was written by a path that does not "
           "say so");
}

static unsigned peek_at(const XmlCharReader *r, size_t n)
{
    DCHECK(r->start <= r->p && r->p <= r->end,
           "an XML content peek was taken from a reader whose cursor is outside its own entity");
    return (size_t)(r->end - r->p) > n ? (unsigned)(unsigned char)r->p[n] : EL_NO_BYTE;
}

/* §2.4's [14] `CharData ::= [^<&]* - ([^<&]* ']]>' [^<&]*)`, as the RUN [43] writes `CharData?` for.
 *
 * THE CHARACTERS ARE READ AND THE SLICE IS BYTES, which is core/xml/xml_markup.h's contract for every content
 * run in this family: reading is what applies §2.2's [2] Char and counts §2.11's lines, and the slice is what
 * a consumer materializes through §2.11 when it needs the characters rather than the bytes.
 *
 * THE `]]>` TEST IS OVER THE BYTES AND THAT IS EXACT, by the same argument the delimiters use: `]` and `>` are
 * ASCII, so neither can occur as a continuation byte of some other code point, and "does this run of
 * characters contain `]]>`" and "does this run of bytes contain it" are the same question. It is asked AFTER
 * the run is scanned rather than during it, because the run has to be [2] Char to be CharData at all. */
static XmlElementError scan_chardata(XmlCharReader *r, XmlMarkupText *out)
{
    const char *begin = r->p;
    const char *scan;

    for (;;) {
        uint32_t cp = 0;
        unsigned b = peek_at(r, 0);

        if (b == EL_NO_BYTE || b == EL_OPEN || b == EL_AMP) break;
        if (xml_char_read(r, &cp) != XML_CHAR_OK) return XML_ELEMENT_ERR_CHARACTER;
        DCHECK(cp != XML_CHAR_EOF,
               "a character-data scan read the end of the entity as a character after its own peek had said a "
               "byte was there");
    }
    for (scan = begin; (size_t)(r->p - scan) >= 3; scan++)
        if (scan[0] == EL_BRACKET && scan[1] == EL_BRACKET && scan[2] == EL_CLOSE)
            return XML_ELEMENT_ERR_CDATA_SECTION_CLOSE;
    out->raw = begin;
    out->raw_len = (size_t)(r->p - begin);
    return XML_ELEMENT_OK;
}

/* PUT A FAILED CALL'S READER BACK, with core/xml/xml_tag.c's carve-out and for its reason: the guard is keyed
 * on the character layer's §1.2 LATCH and never on the error value, because §3.1's and §4.1's layers both set
 * that latch while reporting an error of their OWN — a guard written as `e != XML_ELEMENT_ERR_CHARACTER` looks
 * complete, reads as the rule, and rewinds over exactly those cases, un-reporting a fatal error and handing
 * the caller a reader that will read the offending bytes again. The latch is the fact; the enum value is a
 * description of it, and only one of the two can be wrong. */
static void element_rewind(XmlCharReader *r, const XmlCharReader *start, XmlElementError e,
                           const XmlElementDetail *d)
{
    if (r->fatal != XML_CHAR_OK) {
        DCHECK(e == XML_ELEMENT_ERR_CHARACTER
                   || (e == XML_ELEMENT_ERR_TAG && d->tag == XML_TAG_ERR_CHARACTER)
                   || (e == XML_ELEMENT_ERR_TAG && d->tag == XML_TAG_ERR_REFERENCE)
                   || (e == XML_ELEMENT_ERR_MARKUP && d->markup == XML_MARKUP_ERR_CHARACTER)
                   || (e == XML_ELEMENT_ERR_REFERENCE && d->ref == XML_REF_ERR_CHARACTER),
               "an XML content scan left the character layer's §1.2 latch set while reporting a failure that "
               "no layer below it accounts for — those are the only answers this component has for a fatal "
               "error detected underneath, so another means a latch was set on a path that does not say so "
               "and the sentence a report quotes would be the wrong one");
        return;
    }
    DCHECK(e != XML_ELEMENT_ERR_CHARACTER,
           "an XML content scan reported that the character layer detected a fatal error while that layer's "
           "own §1.2 latch is clear — the latch is the fact and this answer is a description of it, so a "
           "disagreement means the description is wrong");
    *r = *start;
}

XmlElementError xml_element_next(XmlElementWalk *w, XmlCharReader *r, XmlContentItem *out,
                                 XmlElementDetail *detail)
{
    XmlCharReader start;
    XmlContentItem it;
    XmlElementError e;
    XmlOpenElement *open;
    const char *name = NULL;
    size_t name_len = 0;
    bool owed = false;                               /* this call answers [44]'s owed close and reads nothing */

    assert_owner(w);
    DCHECK(r != NULL && out != NULL && detail != NULL,
           "an XML content scan was asked for with no reader, nowhere to put the item, or nowhere to put the "
           "layers' answers");
    DCHECK(!w->stopped,
           "an XML content scan was asked for another item after this walk was finished — [39] element ends "
           "when its stack empties and §1.2 Terminology ends it when a fatal error is reported, and in both "
           "cases the processor MUST NOT continue normal processing");
    DCHECK(r->fatal == XML_CHAR_OK,
           "an XML content scan was asked for an item from a reader that has already latched a fatal error — "
           "§1.2 Terminology: once one is detected the processor MUST NOT continue, so the caller owes a stop "
           "here and not another construct");

    detail->tag = XML_TAG_OK;
    detail->markup = XML_MARKUP_OK;
    detail->ref = XML_REF_OK;
    memset(&it, 0, sizeof it);
    it.ref.cp = XML_CHAR_EOF;                        /* the one absent field with a value rather than a NULL */
    start = *r;
    /* THE ONE FREE SITE for the previous start-tag's §3.3.3 values — see xml_element.h on the item's lifetime.
       It runs before anything that can fail, so a failed call frees the last item exactly as a successful one
       does and a caller never has to reason about which. */
    xml_tag_free(&w->tag);

    /* [44] EmptyElemTag's second half. It is answered before the reader is consulted at all, because the
       construct it closes was already consumed — §3.1 makes the empty-element tag the representation of an
       element whose content is empty, so the END is a fact about the item before it and not about the bytes
       standing here. */
    if (w->empty_end) {
        w->empty_end = false;
        owed = true;
        open = lexbor_array_obj_pop(w->open);
        DCHECK(open != NULL,
               "a [44] EmptyElemTag's close found no open element — its own start pushed one and nothing "
               "between the two items can pop");
        it.kind = XML_CONTENT_ELEMENT_END;
        it.name = open->name;
        it.name_len = open->name_len;
        goto done;
    }

    if (lexbor_array_obj_length(w->open) == 0) {
        /* [39] begins with [40] or [44] — the caller has peeked, and a reader standing anywhere else is a
           caller that has not. */
        DCHECK(xml_tag_at_stag(r),
               "an XML element walk was started on a reader that does not stand at a [40] STag or [44] "
               "EmptyElemTag — [39] element ::= EmptyElemTag | STag content ETag begins with one of those two "
               "and xml_tag_at_stag is the question this walk's caller owes");
    }

    if (peek_at(r, 0) == EL_NO_BYTE) {
        /* [43]'s content is unbounded and the entity is what bounds it; running out with elements open is
           [39] not matching, which is a different sentence from every construct-level one below. */
        DCHECK(lexbor_array_obj_length(w->open) > 0,
               "an XML element walk reached the end of the entity with no element open — the walk stops the "
               "moment its stack empties and its first call stands at a [40]/[44] `<`, so there is no item in "
               "which both can be true");
        e = XML_ELEMENT_ERR_UNCLOSED;
        goto fail;
    }

    if (peek_at(r, 0) != EL_OPEN && peek_at(r, 0) != EL_AMP) {   /* [14] CharData's own `[^<&]` class */
        e = scan_chardata(r, &it.text);
        if (e != XML_ELEMENT_OK) goto fail;
        DCHECK(it.text.raw_len > 0,
               "a character-data run scanned from a byte that is neither `<` nor `&` came back empty — [14] "
               "CharData's own class admits that byte, so an empty run means the scan and the peek disagree "
               "about what a character of content is");
        it.kind = XML_CONTENT_CHARDATA;
        goto done;
    }

    if (peek_at(r, 0) == EL_AMP) {
        /* [43]'s `Reference` alternative. The reader already stands on the `&`, which is
           core/xml/xml_ref.h's precondition — the peek is a byte test and consumes nothing. */
        XmlRef got;
        XmlRefError re = xml_ref_scan(r, &got);

        if (re != XML_REF_OK) { detail->ref = re; e = XML_ELEMENT_ERR_REFERENCE; goto fail; }
        if (got.kind == XML_REF_ENTITY) { e = XML_ELEMENT_ERR_ENTITY_UNDECLARED; goto fail; }
        it.kind = XML_CONTENT_REFERENCE;
        it.ref = got;
        goto done;
    }

    if (xml_tag_at_etag(r)) {
        XmlTagError te = xml_tag_scan_etag(r, &name, &name_len);

        if (te != XML_TAG_OK) { detail->tag = te; e = XML_ELEMENT_ERR_TAG; goto fail; }
        /* §3's [WFC: Element Type Match], decided HERE because here is the only place both names exist. The
           comparison is over the two Names' BYTES, which is exact: both are borrowed slices of the entity and
           §2.11 rewrites no character either one can hold. */
        open = lexbor_array_obj_last(w->open);
        DCHECK(open != NULL,
               "an end-tag was matched against an empty element stack — the walk finishes when the stack "
               "empties, so there is no item after that for an end-tag to arrive in");
        if (open->name_len != name_len || memcmp(open->name, name, name_len) != 0) {
            e = XML_ELEMENT_ERR_ELEMENT_TYPE_MATCH;
            goto fail;
        }
        (void)lexbor_array_obj_pop(w->open);
        it.kind = XML_CONTENT_ELEMENT_END;
        it.name = name;
        it.name_len = name_len;
        goto done;
    }

    if (xml_markup_at_comment(r)) {
        XmlMarkupError me = xml_markup_scan_comment(r, &it.text);

        if (me != XML_MARKUP_OK) { detail->markup = me; e = XML_ELEMENT_ERR_MARKUP; goto fail; }
        it.kind = XML_CONTENT_COMMENT;
        goto done;
    }

    if (xml_markup_at_cdsect(r)) {
        XmlMarkupError me = xml_markup_scan_cdsect(r, &it.text);

        if (me != XML_MARKUP_OK) { detail->markup = me; e = XML_ELEMENT_ERR_MARKUP; goto fail; }
        it.kind = XML_CONTENT_CDSECT;
        goto done;
    }

    if (xml_markup_at_pi(r)) {
        /* A `<?xml` standing in content reaches §2.6's scan and is answered [17]'s reserved-target error,
           which core/xml/xml_markup.h states is the correct report anywhere §2.8's [23] XMLDecl is not
           permitted — and [22] prolog is the only place it is. */
        XmlMarkupError me = xml_markup_scan_pi(r, &it.pi);

        if (me != XML_MARKUP_OK) { detail->markup = me; e = XML_ELEMENT_ERR_MARKUP; goto fail; }
        it.kind = XML_CONTENT_PI;
        goto done;
    }

    if (xml_tag_at_stag(r)) {
        XmlTagError te = xml_tag_scan_stag(r, &w->tag, &detail->ref);

        if (te != XML_TAG_OK) { detail->tag = te; e = XML_ELEMENT_ERR_TAG; goto fail; }
        open = lexbor_array_obj_push(w->open);
        CHECK(open != NULL, "the XML element stack could not be grown — [43] content nests as deeply as the "
                            "entity says and this is the physical memory floor, not a depth this component "
                            "chose");
        open->name = w->tag.name;
        open->name_len = w->tag.name_len;
        /* [44]'s close is owed and is free: §3.1 makes it the same element, so the pair is reported as one
           push and one pop exactly like [40] and [42]. */
        w->empty_end = w->tag.empty;
        it.kind = XML_CONTENT_ELEMENT_START;
        it.tag = w->tag;
        goto done;
    }

    /* NOTHING ELSE CAN STAND HERE, AND THAT IS THE ROUTING BEING TOTAL RATHER THAN A DEFAULT ARM. The byte is
       a `<`; `xml_tag_at_etag` took `</`, `xml_markup_at_pi` took every `<?`, and `xml_tag_at_stag` takes
       every `<` whose second byte is not `/`, `!` or `?` — including a `<` at the very end of the entity. So
       what is left is a `<!` that opened neither §2.5's comment nor §2.7's CDATA section. */
    DCHECK(peek_at(r, 0) == EL_OPEN && peek_at(r, 1) == '!',
           "an XML content scan reached its last arm on a byte that is not the `<!` its four peeks leave "
           "behind — the routing over [43]'s alternatives is total, so a fifth shape here means one of those "
           "peeks stopped answering about the delimiter it owns");
    e = XML_ELEMENT_ERR_CONTENT;
    goto fail;

done:
    assert_item(&it);
    assert_detail(XML_ELEMENT_OK, detail);
    DCHECK(r->fatal == XML_CHAR_OK, "a successful XML content scan left the reader's §1.2 latch set");
    /* The reader advanced by exactly one construct, except for the [44] close that consumes nothing — which
       is the walk's one stated exception and is asserted rather than left to the header alone. */
    DCHECK(owed ? r->p == start.p : r->p > start.p,
           "an XML content scan and the one exception to `every item consumes its construct` disagree — [44] "
           "EmptyElemTag's owed close reads nothing because the item before it read the whole tag, and every "
           "other item advances or a caller walking [43] content would never move");
    DCHECK(!owed || it.kind == XML_CONTENT_ELEMENT_END,
           "the item owed after a [44] EmptyElemTag is not that element's close");
    if (lexbor_array_obj_length(w->open) == 0) {
        DCHECK(it.kind == XML_CONTENT_ELEMENT_END,
               "an XML element walk emptied its stack on an item that is not an element's close — only [42] "
               "ETag and [44]'s owed close pop, so another kind here means the stack was unwound by something "
               "that is not the end of an element");
        w->stopped = true;
    }
    *out = it;
    return XML_ELEMENT_OK;

fail:
    assert_detail(e, detail);
    element_rewind(r, &start, e, detail);
    w->stopped = true;                               /* §1.2: a fatal error ends the parse */
    return e;
}
