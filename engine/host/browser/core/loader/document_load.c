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
#include "core/loader/xml_document.h"
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
        return xml_document_load(document, root_kind, scripting, type, text, size);
    case DOC_LOAD_MULTIPART:
    case DOC_LOAD_MEDIA:
    case DOC_LOAD_EXTERNAL:
        break;
    }
    /* NOT A `default:` ARM — the switch above is exhaustive over the enum, so every arm §7.4.5 defines is
       named and adding one to core/loader/document_load_type.h makes THIS not compile rather than fall into a
       generic crash. What reaches here is an arm with no loader, and the §7.5 subsection to build is the
       message. CLAUDE.md §Offensive programming: a capability that does not exist is honestly ABSENT and the
       crash is what names it — and the alternative here is not "no support for this arm", it is the silent
       wrong tree the file comment describes. */
    DFAIL(document_load_type_section(arm));
    /* THE RELEASE HALF. `DFAIL` is compiled out at APICLIENT_DEV=0 and the caller's own always-fatal CHECK on
       this status is what stops the response there — CLAUDE.md §Offensive programming: in release the same
       error is still correct, because the capability is not supportable outside dev either. Returning OK here,
       or falling through to the HTML parser, is the silent wrong tree the file comment is about. */
    return LXB_STATUS_ERROR_NOT_EXISTS;
}
