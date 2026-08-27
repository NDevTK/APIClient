/* HTML §7.4.5 "Populating a session history entry"'s LOAD A DOCUMENT — the routing half, and nothing else.
 *
 * core/loader/document_load_type.h answers WHICH arm a response takes. This runs the arm. They are separate
 * files because they are separate questions with separate dependencies: the classification is a fact about a
 * MIME type record and knows nothing about a parser, while this knows nothing about MIME groups and only ever
 * asks the other one.
 *
 * WHY A ROUTER RATHER THAN AN `if` AT EACH LOADER. CLAUDE.md §Browser half: a dispatch deciding WHAT A SET OF
 * BYTES IS must be asked at EVERY entry that builds the thing out of them, and an entry that skips it does not
 * report an absent capability — it reports an UNRELATED SUBSYSTEM failing on input that subsystem should never
 * have been shown. This tree has both halves of that on record. The entry that never asked handed an XML
 * response to the HTML tokenizer, where `<script><![CDATA[` is program text under HTML §13.2.6.4.4 The "in
 * head" insertion mode (which switches the tokenizer to §13.2.5.4 Script data state) rather than XML §2.7
 * "CDATA Sections", and the document failed as a JavaScript COMPILE error — true about the bytes, and naming
 * the wrong subsystem for a document that was never JavaScript. The entries that DID ask each carried their
 * own copy of the arm test and their own crash, which is the same defect one step later: three copies of a
 * rule are three rules, and the third one to be updated is the one that is wrong.
 *
 * AND THE ARM IS RUN HERE RATHER THAN RETURNED. An earlier shape had each entry ask the type, crash for the
 * arms it had no loader for, and then parse — so the classification and the parse sat in three places with
 * three chances to disagree about which is which. What a caller wants is a Document out of a response; the
 * §7.5 subsection is this component's business, and a caller that never names one cannot name the wrong one.
 */
#include "core/loader/document_load.h"
#include "core/loader/document_load_type.h"
#include "core/loader/html_document.h"
#include "core/loader/text_document.h"
#include "core/mime/mime_type.h"
#include "check.h"

lxb_status_t document_load(lxb_html_document_t *document, DomParseRootKind root_kind,
                           HtmlScriptingMode scripting,
                           const MimeType *type, const lxb_char_t *text, size_t size)
{
    DocumentLoadType arm;

    DCHECK(document != NULL && text != NULL,
           "HTML §7.4.5's load-a-document was reached without both halves of what it loads — a Document to "
           "load into and the characters of the response it loads; a caller with only one of them is one that "
           "has no response, and §7.4's initial about:blank is not asked this question at all");
    arm = document_load_type_of(type);
    switch (arm) {
    /* §7.5.2 and §7.5.4 each re-ask the dispatch about the type they were handed. That is not a second answer
       to this switch — it is the closure at the consumer, which catches a route that reaches a loader WITHOUT
       coming through here, and this switch cannot. */
    case DOC_LOAD_HTML:
        return html_document_load(document, root_kind, scripting, type, text, size);
    case DOC_LOAD_TEXT:
        return text_document_load(document, root_kind, scripting, type, text, size);
    case DOC_LOAD_XML:
    case DOC_LOAD_MULTIPART:
    case DOC_LOAD_MEDIA:
    case DOC_LOAD_EXTERNAL:
        break;
    }
    /* NOT A `default:` ARM — the switch above is exhaustive over the enum, so every arm §7.4.5 defines is
       named and adding one to core/loader/document_load_type.h makes THIS not compile rather than fall into a
       generic crash. What reaches here is an arm with no loader, and the §7.5 subsection to build is the
       message. CLAUDE.md §Offensive programming: a capability that does not exist is honestly ABSENT and the
       crash is what names it — and the alternative here is not "no XML support", it is the silent wrong tree
       the file comment describes.
       §7.5.3 IS THE ONE WHOSE LEXICAL LAYERS ARE ALREADY IN THE TREE — `core/xml/` holds XML §2.2 [2] `Char`,
       §2.3 [3] `S` and §2.11's end-of-line normalization as the reader every production reads its input
       through; §2.3 [5] `Name` with Namespaces in XML §3 [4] `NCName` and §4 [7] `QName`; §4.1 [66] `CharRef`
       and [68] `EntityRef` over §4.6's five predefined entities, which is the layer §3.3.3's normalization is
       written on; §2.5 [15] `Comment`, §2.6 [16] `PI` with [17] `PITarget` and §2.7 [18] `CDSect`, the three
       constructs §2.4 names as the places a literal `<` or `&` may stand; §2.8 [23] `XMLDecl` with §2.9 [32]
       `SDDecl` and §4.3.1 [77] `TextDecl`; §2.3 [11] `SystemLiteral`, [12] `PubidLiteral` and [13] `PubidChar`;
       and §6's namespace scope stack with every §3 and §5 namespace constraint as a returned error. Read that
       directory rather than a list here, which is a census and goes stale.
       WHAT IS MISSING IS THE GRAMMAR BETWEEN THEM: XML §2.8 [22] `prolog` around the declaration and around
       [28] `doctypedecl` with [28b] `intSubset`, §4.2 [70] `EntityDecl` with §4.5's replacement text,
       [39] `element` with [40] `STag` / [42] `ETag` / [44] `EmptyElemTag`, [43] `content`, and §3.3.3
       attribute-value normalization over [10] `AttValue` — producing a Lexbor tree through element_create_ns
       (core/dom/element.h, which does NOT case-fold, unlike lexbor's own entry) and dom_attr_write
       (core/dom/attr_list.h), and reporting well-formedness errors for §8.5.1's `parsererror`. Lexbor ships no
       `xml` module, so CLAUDE.md's bind-before-build order has nothing at the "existing Lexbor module" rung and
       this is a faithful spec port; `ls source/lexbor` in the vendored checkout is the whole of that check,
       which is why no version number is written here — one was, it named a release this build no longer uses,
       and a sentence that stays true about the standard while going wrong about this tree is the stale
       citation CLAUDE.md warns about.
       AND THE SCRIPTS IN THAT DOCUMENT ARE NOT HTML'S: HTML §14.2 "Parsing XML documents" runs an XML parser
       with XML scripting support enabled, which sets a `script` element's parser document and clears its force
       async, and then — at the element's END TAG, after a microtask checkpoint — prepares it. There is no
       raw-text tokenizer state, so a `<script>` body is ordinary [43] `content` and a `<![CDATA[` inside one
       is §2.7's CDSect rather than program text.
       core/html/domparser.c's XML arm of HTML §8.5.1 `parseFromString`, and core/xhr/xml_http_request.c's XML
       arm of XHR's "set a document response", stand at this same wall and say so at their own sites — checked
       before this comment claimed it. ONE component serves all three; when it lands, every one of those sites
       is deleted along with the prose that agreed with them. */
    DFAIL(document_load_type_section(arm));
    /* THE RELEASE HALF. `DFAIL` is compiled out at APICLIENT_DEV=0 and the caller's own always-fatal CHECK on
       this status is what stops the response there — CLAUDE.md §Offensive programming: in release the same
       error is still correct, because the capability is not supportable outside dev either. Returning OK here,
       or falling through to the HTML parser, is the silent wrong tree the file comment is about. */
    return LXB_STATUS_ERROR_NOT_EXISTS;
}
