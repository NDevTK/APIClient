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
#include <stdlib.h>

#include "core/loader/document_load.h"
#include "core/loader/document_load_type.h"
#include "core/loader/html_document.h"
#include "core/loader/text_document.h"
#include "core/loader/xml_document.h"
#include "core/mime/mime_type.h"
#include "solver/quantum.h"
#include "check.h"

/* THE ARM AND THE ARM'S OWN LOAD, and nothing else. The three §7.5 subsections hold entirely different state
   between two items — a position in a byte stream, a position in a byte stream with a different prologue in
   front of it, a phase and a tree-build handle — so each owns its own, and this holds only which of them is
   running. A union rather than three pointers: a load is exactly one arm for its whole life, and three
   nullable fields would be three ways to say the same thing and one way to disagree. */
struct DocumentLoad {
    DocumentLoadType arm;
    union {
        HtmlDocumentLoad *html;
        TextDocumentLoad *text;
        XmlDocumentLoad  *xml;
    } u;
};

DocumentLoad *document_load_begin(lxb_html_document_t *document, DomParseRootKind root_kind,
                                  HtmlScriptingMode scripting,
                                  const MimeType *type, int encoding,
                                  const lxb_char_t *text, size_t size)
{
    DocumentLoad    *l;
    DocumentLoadType arm;

    DCHECK(document != NULL && text != NULL,
           "HTML §7.4.5's load-a-document was reached without both halves of what it loads — a Document to "
           "load into and the characters of the response it loads; a caller with only one of them is one that "
           "has no response, and §7.4's initial about:blank is not asked this question at all");
    /* THE OTHER HALF OF WHAT THESE BYTES ARE, AND THE CLOSURE ON THE ENTRY THAT SKIPS IT. §7.4.5's WHICH-
       DOCUMENT dispatch is answered above by a component every entry reaches; §13.2.3.2's WHICH-DECODER
       dispatch is answered before this call, and an entry that never asked it has no id to hand over. It
       cannot stay silent about that — the id is a parameter — so the state this refuses is a caller that
       reached a parser with bytes NOBODY DECODED, which is what two of the three entries used to do: the
       response's own bytes went to a tokenizer that reads UTF-8, and a document served in any other encoding
       came back a real tree full of U+FFFD with a `characterSet` of UTF-8 and nothing to say so.
       WHY THIS IS THE PLACE. §13.2.3.1 Parsing with a known character encoding makes every arm below operate
       on a KNOWN encoding whether or not one was determined, so the last moment the question can be asked at
       all is the one before the arm opens — and this router is the only thing in the engine ever handed a
       response to ask it about. A route added later that reaches a §7.5 loader with an encoding nobody
       determined FIRES HERE rather than widening the old wrong answer in silence. */
    DCHECK(encoding >= 0,
           "an HTML §7.4.5 load-a-document was opened for a response whose ENCODING nobody determined — HTML "
           "§13.2.3.2 Determining the character encoding is what states otherwise than DOM §4.5's utf-8 "
           "default, and §13.2.3.1 Parsing with a known character encoding then has this parse operate on that "
           "encoding with certain confidence. Run core/loader/document_load_decode.h over the RESPONSE (its "
           "header list and its bytes) at the entry that fetched it, parse what it decoded, and hand its "
           "answer here; the bytes a response arrived in are not the characters a parser takes");
    arm = document_load_type_of(type);
    switch (arm) {
    case DOC_LOAD_HTML:
    case DOC_LOAD_TEXT:
    case DOC_LOAD_XML:
        break;
    case DOC_LOAD_MULTIPART:
    case DOC_LOAD_MEDIA:
    case DOC_LOAD_EXTERNAL:
        /* NOT A `default:` ARM — the switch is exhaustive over the enum, so every arm §7.4.5 defines is named
           and adding one to core/loader/document_load_type.h makes THIS not compile rather than fall into a
           generic crash. What reaches here is an arm with no loader, and the §7.5 subsection to build is the
           message. CLAUDE.md §Offensive programming: a capability that does not exist is honestly ABSENT and
           the crash is what names it — and the alternative here is not "no support for this arm", it is the
           silent wrong tree the file comment describes. */
        DFAIL(document_load_type_section(arm));
        /* THE RELEASE HALF. `DFAIL` is compiled out at APICLIENT_DEV=0 and the caller's own always-fatal CHECK
           on the status `document_load` returns is what stops the response there — in release the same error
           is still correct, because the capability is not supportable outside dev either. */
        return NULL;
    }

    l = malloc(sizeof(*l));
    CHECK(l != NULL,
          "OOM opening an HTML §7.4.5 load-a-document — the DOM this parse produces is what every flow of "
          "this instance reads, so a load that cannot even begin is not a document with no endpoints");
    l->arm = arm;
    switch (arm) {
    /* §7.5.2, §7.5.3 and §7.5.4 each re-ask the dispatch about the type they were handed. That is not a second
       answer to this switch — it is the closure at the consumer, which catches a route that reaches a loader
       WITHOUT coming through here, and this switch cannot. */
    case DOC_LOAD_HTML:
        l->u.html = html_document_load_begin(document, root_kind, scripting, type, text, size); break;
    case DOC_LOAD_TEXT:
        l->u.text = text_document_load_begin(document, root_kind, scripting, type, text, size); break;
    case DOC_LOAD_XML:
        l->u.xml  = xml_document_load_begin(document, root_kind, scripting, type, text, size);  break;
    case DOC_LOAD_MULTIPART:
    case DOC_LOAD_MEDIA:
    case DOC_LOAD_EXTERNAL:
        DFAIL("an arm with no loader reached the second switch of load-a-document — the first one returned "
              "for exactly these three, so the two switches disagree about which arms this build serves");
        break;
    }
    return l;
}

bool document_load_ended(const DocumentLoad *load)
{
    DCHECK(load != NULL, "document_load_ended was asked of no load");
    switch (load->arm) {
    case DOC_LOAD_HTML: return html_document_load_ended(load->u.html);
    case DOC_LOAD_TEXT: return text_document_load_ended(load->u.text);
    case DOC_LOAD_XML:  return xml_document_load_ended(load->u.xml);
    case DOC_LOAD_MULTIPART:
    case DOC_LOAD_MEDIA:
    case DOC_LOAD_EXTERNAL:
        break;
    }
    DFAIL("a load-a-document handle names an arm this build has no loader for — document_load_begin returns "
          "NULL for those, so a handle carrying one was not built by it");
    return true;
}

void document_load_step(DocumentLoad *load)
{
    DCHECK(load != NULL, "document_load_step was asked of no load");
    DCHECK(!document_load_ended(load),
           "an HTML §7.4.5 load-a-document was stepped after its §7.5 subsection had nothing left to do — "
           "document_load_ended is what a driver's loop tests, and asking past it is a driver that did not");
    switch (load->arm) {
    case DOC_LOAD_HTML: html_document_load_step(load->u.html); return;
    case DOC_LOAD_TEXT: text_document_load_step(load->u.text); return;
    case DOC_LOAD_XML:  xml_document_load_step(load->u.xml);   return;
    case DOC_LOAD_MULTIPART:
    case DOC_LOAD_MEDIA:
    case DOC_LOAD_EXTERNAL:
        break;
    }
    DFAIL("a load-a-document handle names an arm this build has no loader for — document_load_begin returns "
          "NULL for those, so a handle carrying one was not built by it");
}

lxb_status_t document_load_finish(DocumentLoad *load)
{
    lxb_status_t st = LXB_STATUS_ERROR_NOT_EXISTS;

    DCHECK(load != NULL, "document_load_finish was asked of no load");
    DCHECK(document_load_ended(load),
           "an HTML §7.4.5 load-a-document was finished with items left — every §7.5 subsection's close is the "
           "END of a parse, and closing one that has not been given the whole response builds a Document out "
           "of a prefix of it");
    switch (load->arm) {
    case DOC_LOAD_HTML: st = html_document_load_finish(load->u.html); break;
    case DOC_LOAD_TEXT: st = text_document_load_finish(load->u.text); break;
    case DOC_LOAD_XML:  st = xml_document_load_finish(load->u.xml);   break;
    case DOC_LOAD_MULTIPART:
    case DOC_LOAD_MEDIA:
    case DOC_LOAD_EXTERNAL:
        DFAIL("a load-a-document handle names an arm this build has no loader for — document_load_begin "
              "returns NULL for those, so a handle carrying one was not built by it");
        break;
    }
    free(load);
    return st;
}

void document_load_abort(DocumentLoad *load)
{
    DCHECK(load != NULL, "document_load_abort was asked of no load");
    switch (load->arm) {
    case DOC_LOAD_HTML: html_document_load_abort(load->u.html); break;
    case DOC_LOAD_TEXT: text_document_load_abort(load->u.text); break;
    case DOC_LOAD_XML:  xml_document_load_abort(load->u.xml);   break;
    case DOC_LOAD_MULTIPART:
    case DOC_LOAD_MEDIA:
    case DOC_LOAD_EXTERNAL:
        DFAIL("a load-a-document handle names an arm this build has no loader for — document_load_begin "
              "returns NULL for those, so a handle carrying one was not built by it");
        break;
    }
    free(load);
}

lxb_status_t document_load(lxb_html_document_t *document, DomParseRootKind root_kind,
                           HtmlScriptingMode scripting,
                           const MimeType *type, int encoding,
                           const lxb_char_t *text, size_t size)
{
    DocumentLoad *load;

    /* THE ONE THING THAT MAKES A COMPLETING DRIVE LEGAL: THERE IS NOBODY TO YIELD TO. A document parse is
       O(document), so driving one to completion while a flow holds the thread is exactly the
       drive-to-completion CLAUDE.md §scheduler forbids — a higher-value sibling cannot preempt it, the
       cooperative quantum cannot take the thread back during it, and it cannot be parked to the IDB cold tier
       half-parsed. It is not forbidden on the HOST'S OWN TIME, and every caller of this entry is there: the
       production ABI's `qjs_init`, which parses before the first flow is seeded (it must return the document
       identity synchronously, which is why the frontier key is a Lexbor `<script>` scan and not something boot
       computes); the same ABI's `qjs_join`, which the trusted zone calls BETWEEN two `qjs_step` returns; and
       the WPT runner's top-level document, which drives flows under its own preempt policy with no frontier to
       be fair between.
       AND "HOST TIME" HAS EXACTLY ONE SPELLING IN THIS ENGINE, WHICH IS WHY THIS ASKS ABOUT THE SLICE AND NOT
       ABOUT THE FLOW STAMP. `flow_running() == NULL` stood here, and that register does not answer this
       question: solver/engine.c's engine_sched_step returns the cooperative-quantum yield WITHOUT switching the
       running flow out — §scheduler requires that flow to resume byte-identically on the frontier it left — and
       the stamp is deliberately left UP across the ABI boundary, because a yielded flow's COW delta is still
       APPLIED to the heap and putting the stamp down would be a claim about that heap which is not true.
       `qjs_step` puts the three marks down that CAN come down and asserts each on every exit (the slice, the
       flow generation, the capture route); `g_running` is not among them by design. So between two steps the
       stamp NAMES WHICHEVER FLOW WAS LAST SWITCHED IN, and an assert resting on it is false exactly when the
       engine is working.
       AND THE NARROWER PREDICATE IS SAFE RATHER THAN MERELY DEFENSIBLE, WHICH IS THE HALF THAT DECIDES THIS.
       The register a parse could actually be hurt by is not the stamp: it is the CAPTURE ROUTE, because a
       DOM_PARSE_ROOT_SHARED parse creates a whole tree, and a tree created into a yielded flow's COW head is
       DELETED by that flow's next unapply — a joined document with a realm and an empty tree, refcounts
       intact, nothing to say what happened. solver/engine.c's engine_sched_step suspends and restores all
       three marks around the slice, so on the host's own time the stamp is 0, `g_dom_capture` is 0 and
       `cow_set_current(NULL)` has taken the route down; only `g_running` is left up, and it is left up
       precisely because it is the one that would be a LIE about the heap. So the three registers that decide
       whether this parse can corrupt a flow are all down at every caller of this entry, and the one this
       assert used to read is the one that says nothing about that. `qjs_step` asserts the other two at the
       boundary; a tree that comes back empty with neither of them firing is those asserts being wrong, not
       this one.
       MEASURED, AND WHAT IT COSTS IS THE LEVEL-1 SCHEDULER RATHER THAN ONE DOCUMENT. `qjs_join` is host time
       by construction and hit this on 2 of 13 navigations of one real site. One hit is not one lost parse:
       extension/bridge.js latches `_hostDead` on the abort and never clears it, so every LATER document is
       answered by a refused kick — 12 of 15 rows in one measured batch were about a loop that was already
       dead. This is the same defect solver/engine.c records at engine_enqueue_job, one register and one file
       over, and the same shape: a SECOND spelling of host time disagreeing with the settled one, so continuous
       browsing survives exactly one navigation into a busy instance.
       WHAT THE ASSERT STILL FORCES IS UNCHANGED, which is why this is a truer predicate and not a deleted one.
       solver/quantum.h's slice is open exactly while the engine is EXECUTING — solver/engine.c's preempt_hook
       asserts the converse at the one point the scheduling policy is consulted — so a route that reaches this
       entry from inside a flow's slice crashes here and names the seam to build. The seam is not hypothetical
       and it is not this component's to hold: core/frame/navigable.c's create-a-new-child-navigable opens the
       load in `child_document`, steps it in `nav_create_step` and closes it in `nav_create_finish`, and this
       assert is what catches that regressing to a completing call.
       IT IS AN ASSERTION AND NEVER A BRANCH. solver/quantum.h says so at the predicate itself: a caller that
       BRANCHES on the slice is choosing between a scheduled path and an unscheduled one, which is the fallback
       CLAUDE.md §C-stack bans. Each caller statically calls one entry or the other — the step machine calls
       document_load_begin, the ABI calls this — and nothing anywhere selects between them. */
    DCHECK(!quantum_slice_open(),
           "HTML §7.4.5's load-a-document was driven to completion while a flow held the thread — a document "
           "parse is O(document) and this drive cannot be preempted, cannot give the thread back for a "
           "cooperative quantum and cannot be parked to the cold tier half-parsed, which is the "
           "drive-to-completion the scheduler forbids at any depth. The pull seam is built: "
           "document_load_begin / document_load_step / document_load_ended / document_load_finish in this same "
           "header, one REST UNIT per step (solver/rest_unit.h sizes it, and no step's cost grows with the "
           "response), with every byte of the load's state in the handle and none of it "
           "on the C stack. DRIVE IT FROM THE STEP MACHINE THIS CALL IS ALREADY INSIDE — a stage that opens "
           "the load, a stage that steps it and returns JS_STEP_YIELD while it has not ended, and a stage that "
           "finishes it — rather than from this entry, which exists for the ABI's own callers and reaches its "
           "parse on the host's own time, with no slice open and no driver to park in");

    load = document_load_begin(document, root_kind, scripting, type, encoding, text, size);
    if (load == NULL)
        return LXB_STATUS_ERROR_NOT_EXISTS;   /* the release half of the unbuilt-arm crash — see the header */
    while (!document_load_ended(load))
        document_load_step(load);
    return document_load_finish(load);
}
