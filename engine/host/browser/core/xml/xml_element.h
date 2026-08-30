/* XML 1.0 (Fifth Edition) §3 Logical Structures' [39] `element` over §3.1 Start-Tags, End-Tags, and
 * Empty-Element Tags' [43] `content` — THE ELEMENT STACK, AND THE ONE PLACE [WFC: Element Type Match] IS
 * DECIDED.
 *
 * WHAT IT IS. Two productions and one well-formedness constraint. §3's [39] `element ::= EmptyElemTag | STag
 * content ETag` and §3.1's [43] `content ::= CharData? ((element | Reference | CDSect | PI | Comment)
 * CharData?)*`, with §2.4 Character Data and Markup's [14] `CharData ::= [^<&]* - ([^<&]* ']]>' [^<&]*)`, and
 * with §3's "Well-formedness constraint: Element Type Match — The Name in an element's end-tag MUST match the
 * element type in the start-tag." It sits on core/xml/xml_tag.h for [40]/[42]/[44], on core/xml/xml_markup.h
 * for [15]/[16]/[18], on core/xml/xml_ref.h for [67], and on core/xml/xml_char.h underneath all three — and on
 * NOTHING ELSE. It builds no tree: what a Lexbor node is made of is the CONSUMER's problem, and mixing the two
 * would put DOM ownership rules inside a grammar walk.
 *
 * [WFC: Element Type Match] IS THIS FILE'S BECAUSE OF WHERE THE STANDARD WRITES IT. The constraint hangs off
 * [39] in §3, not off [42] in §3.1, and the difference is not editorial: it is a statement about a PAIR of
 * tags, so deciding it needs the OPEN element's name, which is a fact only something holding a stack has.
 * core/xml/xml_tag.h says the same from its side and deliberately does not check it — a version of it asserted
 * there would have needed the scan told a name it has no business being told.
 *
 * IT IS AN EXPLICIT HEAP STACK AND NOT C RECURSION, AND THAT IS CLAUDE.md's §C-stack RULE RATHER THAN A STYLE.
 * [43] contains [39], so the obvious spelling is a function that calls itself once per `<`, and its depth is
 * then whatever the DOCUMENT says — a page serving `<a>` a million times over would overflow the C stack of a
 * security tool from markup it controls. "All calls trampoline onto the HEAP stack ... overflow impossible by
 * construction" is the engine's answer everywhere else and it is this component's here: the nesting lives in a
 * `lexbor_array_obj_t` that grows with the document, so the C stack of a walk is the same three frames at depth
 * one and at depth one million. It is also what makes the walk PULL rather than push — a callback-driven parse
 * holds its position in C locals, and a position in C locals is one no flow can park.
 *
 * WHAT COMES BACK IS ONE ITEM PER CONSTRUCT, AND AN EMPTY-ELEMENT TAG IS TWO OF THEM. §3.1: "The
 * representation of an empty element is either a start-tag immediately followed by an end-tag, or an
 * empty-element tag" — the two spellings are ONE element, and the DOM has no member that tells them apart. So
 * [44] reports XML_CONTENT_ELEMENT_START and then XML_CONTENT_ELEMENT_END for the same element type, which is
 * what makes a consumer's push/pop one shape instead of two. That second item is the ONE answer that consumes
 * no input, because the construct it closes was already consumed by the item before it, and the scan asserts
 * exactly that rather than leaving "does the reader always advance" a rule with an unstated exception.
 *
 * A ZERO-LENGTH [14] `CharData` IS NOT AN ITEM, WHICH IS [43]'s OWN `?`. The production is optional in both of
 * its positions, so a `<` standing immediately after a `>` has NO CharData between them — reporting an empty
 * one would put an empty Text node in every tree this engine builds, and no XML parser produces one. An
 * XML_CONTENT_CHARDATA item therefore always carries at least one byte, and that is asserted.
 *
 * ADJACENT RUNS ARE NOT COALESCED HERE, AND THAT IS THE DIVISION RATHER THAN AN OMISSION. `a&amp;b` is three
 * items — CharData, Reference, CharData — because [43] lists `CharData` and `Reference` as different
 * alternatives, and one Text node holding `a&b` is what the DOM's own rule about adjacent text produces from
 * them. A walk that merged them would be answering the consumer's question with the producer's information and
 * would have to allocate to do it; a consumer that appends into the node it is already building gets the same
 * answer for free.
 *
 * §4.1's [WFC: Entity Declared] IS DECIDED HERE AND ALSO IN core/xml/xml_tag.c, AND THAT IS TWO DECISIONS OF
 * ONE SENTENCE RATHER THAN TWO SPELLINGS OF IT. core/xml/xml_ref.h hands a [68] `EntityRef` back as a borrowed
 * Name precisely because whether it resolves depends on facts only a caller has, and the two callers are about
 * to DIVERGE: §4.4 Entity Type Table gives "Reference in Content" and "Reference in Attribute Value" different
 * rows, and §4.4.4 Forbidden's third bullet makes "a reference to an external entity in an attribute value" a
 * fatal error while the same reference in CONTENT is included. So one site cannot answer for both. What each
 * site answers is the same one for now: the Name must be one of §4.6 Predefined Entities' five or no
 * declaration this parse read matches it.
 *   WHETHER THAT ANSWER IS THE CONSTRAINT'S OR MERELY THIS PARSE'S IS NOT DECIDED HERE, AND MUST NOT BE.
 * [WFC: Entity Declared] applies to "a document without any DTD, a document with only an internal DTD subset
 * which contains no parameter entity references, or a document with `standalone='yes'`", so a document whose
 * §2.8's [28] `doctypedecl` points at an EXTERNAL SUBSET this non-validating processor did not read is
 * outside all three clauses and the constraint says nothing about it. That is a fact about the DOCUMENT —
 * [28]'s [75] `ExternalID` conjoined with §2.9's [32] `SDDecl` — and neither is visible from inside [39], so
 * this walk reports what it found and core/xml/xml_document.h, which holds both, decides what it means.
 *
 * EVERY ERROR IS FATAL AND IS RETURNED RATHER THAN ASSERTED, AND A FAILED CALL CONSUMES NOTHING — for
 * core/xml/xml_markup.h's reasons, which are that a malformed document is a page's INPUT and that the position
 * a report quotes must name the CONSTRUCT rather than some place inside the one that failed. THE UNIT OF THAT
 * PROMISE IS THE ITEM, not the element: a walk that failed on the third construct leaves its reader at the
 * third construct's first byte, which is the position a `parsererror` has to quote. THE ONE CARVE-OUT IS THE
 * FAMILY'S: an error the character layer latched is NOT rewound, because restoring a saved reader would
 * restore its §1.2 latch to XML_CHAR_OK and silently un-report the fatal error that layer just detected.
 *
 * THE STACK IS FLOW-PRIVATE STATE AND IS NEVER COW-CAPTURED, on core/xml/xml_ns.h's argument and with its
 * assertion. §State-isolation: the delta captures only shared baseline state, and an object created by the
 * running flow can never be observed by another; a walk is created by one parse, named by nothing outside it,
 * and destroyed before that parse returns. THE FORK HALF IS OPEN IN EXACTLY THE SAME WAY and is checked rather
 * than asserted in prose: the walk records the flow that created it and every operation DCHECKs that the
 * running flow is still that one, so a fork that forgot to copy it fires at its first item. What a fork owes
 * is a JSStepVisit declaration for this state in whatever step machine holds the parse, and a byte copy is the
 * wrong answer here for the same reason it is there — every name on this stack is an interior pointer into the
 * ENTITY the reader was initialised over, so a copy that also re-points the entity would name the original
 * arm's bytes. */
#ifndef APICLIENT_XML_ELEMENT_H
#define APICLIENT_XML_ELEMENT_H

#include <stdbool.h>
#include <stddef.h>

#include "core/xml/xml_char.h"
#include "core/xml/xml_doctype.h"
#include "core/xml/xml_markup.h"
#include "core/xml/xml_ref.h"
#include "core/xml/xml_tag.h"

/* WHICH SENTENCE OF THE STANDARD THIS CONTENT VIOLATED. One value per sentence, for core/xml/xml_ns.h's
   reason: an end-tag naming the wrong element, an entity that ends with elements still open, a `]]>` written
   in content and a `<!` form the grammar has no place for are four different mistakes an author has to be told
   apart. The three that name a LAYER carry that layer's own answer in the detail beside them rather than
   transcribing its sentences into this enum, which is the one way two spellings of one rule could drift apart.
   Zero is OK so a caller may write `if (err)`. */
typedef enum {
    XML_ELEMENT_OK = 0,
    XML_ELEMENT_ERR_ELEMENT_TYPE_MATCH,  /* §3 [WFC: Element Type Match] */
    XML_ELEMENT_ERR_UNCLOSED,            /* [39]: the entity ends with an element still open */
    XML_ELEMENT_ERR_CDATA_SECTION_CLOSE, /* §2.4's [14] CharData subtracts `]]>` from its own run */
    XML_ELEMENT_ERR_CONTENT,             /* [43]: a `<!` form that is neither [15] Comment nor [18] CDSect */
    XML_ELEMENT_ERR_ENTITY_UNDECLARED,   /* §4.1 [WFC: Entity Declared] — see the head comment */
    XML_ELEMENT_ERR_TAG,                 /* §3.1's layer reported one — `detail.tag` names which */
    XML_ELEMENT_ERR_MARKUP,              /* §2.5/§2.6/§2.7's layer reported one — `detail.markup` names which */
    XML_ELEMENT_ERR_REFERENCE,           /* §4.1's layer reported one — `detail.ref` names which */
    XML_ELEMENT_ERR_CHARACTER            /* the character layer latched one — ask xml_char_error_message(r->fatal) */
} XmlElementError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_ELEMENT_OK has a
   message too, and a caller that formats it has asked the wrong question. The three layer-naming values say to
   ask that layer, because its sentences are ITS to word. */
const char *xml_element_error_message(XmlElementError err);

/* EVERY LAYER'S OWN ANSWER, WRITTEN ON EVERY CALL. An OK here is a POSITIVE statement that the layer found
   nothing to report — never a field a caller defaults past — and it is what makes `XmlElementError` a name for
   WHICH layer's sentence the report is rather than a lossy merge of four enums.
   EXACTLY ONE COMBINATION HAS TWO NON-OK FIELDS AND IT IS THE STANDARD'S DOING: §3.1's [10] `AttValue` holds
   [67] `Reference`s, so a tag scan carries §4.1's answer out with it and XML_ELEMENT_ERR_TAG can arrive with
   `tag` XML_TAG_ERR_REFERENCE and `ref` naming the reference's own mistake. The report is still the TAG's,
   because that is the construct the position names. Every other answer leaves at most one field set, and the
   scan asserts it. */
typedef struct {
    XmlTagError    tag;
    XmlMarkupError markup;
    XmlRefError    ref;
} XmlElementDetail;

/* WHICH OF [43]'s ALTERNATIVES THIS ITEM IS, plus the two halves of [39] itself, plus the ONE construct of
   [22] `prolog` that becomes a node. The element pair is here and not in [43]'s list because [43]'s `element`
   alternative is a WHOLE subtree, and a pull walk reports its boundaries rather than handing back a thing it
   did not build.
     §2.8's [28] `doctypedecl` IS IN THIS ENUM AND IS NOT IN [43], which is not a blurring of the two
   productions but the consequence of what an ITEM is: an item is a construct the tree builder turns into a
   node, and [28] becomes a DOM §4.6 `DocumentType`. [27] `Misc`'s `Comment` and `PI` are already in this list
   for exactly that reason and are equally not [43]'s alone. What is NOT here is [23] `XMLDecl` — a
   declaration ABOUT the entity, which becomes no node and is asked for by name (core/xml/xml_document.h). The
   walk that may PRODUCE a doctype item is the document walk and never the [39] walk, which asserts it. */
typedef enum {
    XML_CONTENT_ELEMENT_START,   /* [40] STag, or [44] EmptyElemTag's opening half */
    XML_CONTENT_ELEMENT_END,     /* [42] ETag, or the close [44] stands for */
    XML_CONTENT_CHARDATA,        /* §2.4's [14] CharData */
    XML_CONTENT_REFERENCE,       /* §4.1's [67] Reference, already resolved to its character */
    XML_CONTENT_CDSECT,          /* §2.7's [18] CDSect */
    XML_CONTENT_PI,              /* §2.6's [16] PI */
    XML_CONTENT_COMMENT,         /* §2.5's [15] Comment */
    XML_CONTENT_DOCTYPE          /* §2.8's [28] doctypedecl — [22] prolog's, never [43] content's */
} XmlContentKind;

/* ONE CONSTRUCT OF [43] `content`, OR ONE BOUNDARY OF [39] `element`.
 *
 * THE FIELDS THAT DO NOT APPLY ARE SET TO VALUES NO PREDICATE ACCEPTS, never to plausible ones — which is
 * core/xml/xml_ref.h's discipline and is why `kind` can be trusted as the only thing that decides what to
 * read. `tag.name` and `name` and `text.raw` and `pi.target` and `doctype.name` are NULL where the kind does
 * not write them, and `ref.cp` is XML_CHAR_EOF, which is above §2.2's [2] Char ceiling so a consumer that
 * read it would fail xml_char_is_char rather than emit U+0000 as a character the document contained.
 *
 * EVERYTHING HERE IS VALID UNTIL THE NEXT CALL ON THE SAME WALK, AND `tag` IS THE REASON THAT MATTERS. §3.3.3
 * builds attribute values out of text the entity does not contain, so a start-tag OWNS an allocation — and the
 * walk holds it rather than handing it over, so that there is exactly ONE free site and a consumer's error
 * path cannot leak a value by returning early. Every other field is a BORROWED slice of the entity and would
 * outlive the walk anyway; they share the contract so that there is one rule and not two.
 *
 * `text` IS NOT §2.11-NORMALIZED, which is core/xml/xml_markup.h's contract carried through unchanged: a
 * content run is a borrowed slice of BYTES and §2.11 End-of-Line Handling stands between those bytes and the
 * characters the standard says the production matched. A consumer that materializes one asks
 * core/xml/xml_char.h for the rule over the slice. */
typedef struct {
    XmlContentKind kind;
    XmlTag         tag;        /* ELEMENT_START */
    const char    *name;       /* ELEMENT_END: the element type, borrowed */
    size_t         name_len;
    XmlMarkupText  text;       /* CHARDATA, CDSECT, COMMENT */
    XmlPi          pi;         /* PI */
    XmlRef         ref;        /* REFERENCE */
    XmlDoctype     doctype;    /* DOCTYPE */
} XmlContentItem;

typedef struct XmlElementWalk XmlElementWalk;

/* One walk per [39] `element`. Records the running flow — see the head comment's last paragraph. */
XmlElementWalk *xml_element_walk_create(void);

/* THE TWO WAYS A WALK'S LIFE ENDS, AND THE CALLER SAYS WHICH — never the walk, which cannot know.
   `destroy` is the walk that FINISHED: [39] element closed and its stack is empty, which is the only shape
   this component may be torn down in without somebody stating otherwise. It DCHECKs that.
   `abandon` is every other shape: no further item will be asked for and the partial tree below this point is
   discarded with the walk. THREE THINGS REACH IT and they are one fact, not three cases — (a) this walk
   reported XML §1.2 Terminology's fatal error, after which "a processor MUST NOT continue normal
   processing"; (b) a layer ABOVE it reported one over an item this walk answered successfully (Namespaces in
   XML §6.3 Uniqueness of Attributes' [NSC: Attributes Unique] is decided on the expanded names of a
   start-tag this walk had already pushed); (c) the flow driving the parse is gone. In all three nobody will
   ask for another item, which is the whole of what this component needs to be told.
   THE WORD IS `abandon` AND NOT `abort` BECAUSE (b) IS AN ORDINARY OUTCOME: an ill-formed document is a
   document with a `parsererror` in it, not a caller giving up. */
void            xml_element_walk_destroy(XmlElementWalk *w);
void            xml_element_walk_abandon(XmlElementWalk *w);

/* HOW MANY ELEMENTS ARE OPEN, WHICH IS ALSO HOW A CALLER KNOWS THE [39] IT ASKED FOR IS FINISHED. Zero before
   the first item and zero again after the last, and the walk asserts that nobody asks for an item after that —
   so "the element ended" and "the stack is empty" are ONE fact rather than two that could disagree. */
size_t xml_element_depth(const XmlElementWalk *w);

/* SCAN THE NEXT CONSTRUCT. On the FIRST call the reader MUST stand where `xml_tag_at_stag` answers true —
   [39] begins with [40] or [44] and a reader standing anywhere else is a caller that has not peeked, which is
   a DCHECK and not an error to report about the document.
   `*out` is written ONLY when XML_ELEMENT_OK is returned; `*detail` is written ALWAYS. The reader advances by
   exactly the construct that was read, except for the XML_CONTENT_ELEMENT_END that closes a [44]
   EmptyElemTag, which consumes nothing because its construct was consumed by the item before it.
   On any answer but XML_ELEMENT_OK the reader is byte-for-byte the one that was handed in — except when the
   character layer's §1.2 latch is set, see the head comment — the stack is untouched, and §1.2 Terminology
   makes this walk finished: asking for another item is a DCHECK, because a processor that has detected a fatal
   error "MUST NOT continue normal processing". */
XmlElementError xml_element_next(XmlElementWalk *w, XmlCharReader *r, XmlContentItem *out,
                                 XmlElementDetail *detail);

#endif
