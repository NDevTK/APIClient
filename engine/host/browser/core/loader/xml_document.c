/* HTML §7.5.3 "Loading XML documents", and nothing else. See core/loader/xml_document.h.
 *
 * WHY §14.2's SCRIPT STEP IS ASKED HERE AND NOT IN THE PARSER. HTML §14.2 "Parsing XML documents" is the
 * section that says what an XML parser does about a `script` element, and it says it does something only for a
 * parser invoked with XML SCRIPTING SUPPORT ENABLED. Of the three entries that parse XML in this engine, two
 * run with that support DISABLED and say so in their own standards (HTML §8.5.1 "DOMParser" creates its parser
 * "with XML scripting support disabled"; XMLHttpRequest §3.6.6 "set a document response" runs no script), so
 * the step is only ever owed HERE. A scripting FLAG in core/xml/ would give a grammar walk an opinion about a
 * document's scripting state that it has no way to be right about, and a `script` test there would make that
 * walk decide which elements matter. What core/xml/xml_tree.h reports instead is the bare boundary — the
 * element whose end tag the last step parsed — which is a fact about XML §3's [39] `element` and about nothing
 * else. So the parse is one question and this is another, asked by the one caller that has to ask it.
 *
 * WHAT §14.2 SAYS, IN ITS OWN WORDS, AND WHICH HALF LANDS WHERE. "When an XML parser with XML scripting
 * support enabled creates a script element, it must have its parser document set and its force async set to
 * false. If the parser was created as part of the XML fragment parsing algorithm, then the element's already
 * started must be set to true. When the element's end tag is subsequently parsed, the user agent must perform
 * a microtask checkpoint, and then prepare the script element."
 *   `force async` IS ALREADY WRITTEN AND NOT BY THIS FILE — HTML §4.12.1.1 "Processing model" states that flag
 * of BOTH parsers in one sentence ("It is set to false by the HTML parser and the XML parser on script
 * elements they insert"), and core/html/html_script.h's `html_script_parsed` is the one walk that applies it,
 * run from `document_install` over the finished tree of every document this engine installs, XML included and
 * with a realm in hand. A second writer here would be one fact answered from two places.
 *   `parser document` IS EXPRESSED AND NOT STORED, which core/html/html_script.h argues at length: the flag has
 * exactly two readers and each of them IS one of the two ways an element can arrive, so it is a PARAMETER at
 * the preparation rather than a slot. This route states it TRUE, because this route is a parser.
 *   THE FRAGMENT ARM HAS NO PARSER TO STAMP: §14.4 "Parsing XML fragments" is not built in this engine, and
 * §7.5.3 is a DOCUMENT parse, so the arm is not narrowed here — it is absent one component over.
 *   THE END TAG IS THIS FILE'S, and it is the substantive difference from HTML §13.2's parser. There is no
 * raw-text tokenizer state in XML, so a `script` body is ordinary XML §3.1's [43] `content` and a `<![CDATA[`
 * inside one is §2.7's [18] `CDSect` and NOT program text — core/xml/ already reads it that way, and the
 * element's script text is the character data of its children.
 *
 * AND THE PREPARATION IS THE ONE THAT ALREADY EXISTS. HTML §13.2.6.4.8 'The "text" insertion mode' and §14.2
 * state the SAME action for the SAME reason — a parser reached a `script` element's end tag, so prepare it,
 * parser-inserted — so `html_script_parser_inserted` serves both and there is no second script-running path
 * beside it. Two sections, one body: an XML document's script joins the flow's program sequence through the
 * identical door an HTML document's does, which is what keeps §4.12.1's type steps, its `already started` and
 * its five destinations from having to be right twice.
 *
 * WHAT REPLACED THE CRASH THAT STOOD HERE, AND WHAT THAT CRASH GOT WRONG. It named two things to build and one
 * of them was already built: it instructed the next reader to unset `force async` at the parse, which
 * `document_install` has been doing for XML trees along with HTML ones. That is CLAUDE.md §DFAIL's own failure
 * mode — text that stays accurate about the SPEC and goes wrong about THIS TREE — and it is why the rule is to
 * grep the entry a crash names before writing the code it asks for. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/html/html_script.h"   /* §4.12.1's `script` test and the ONE preparation both parsers reach */
#include "core/loader/document_load_type.h"
#include "core/loader/xml_document.h"
#include "core/mime/mime_type.h"
#include "core/xml/xml_parse.h"
#include "solver/rest_unit.h"   /* how many items each of this loader's phases rests after */
#include "check.h"

/* THE THREE THINGS A §7.5.3 LOAD CAN BE DOING, in the order it can reach them. Two of them are a LOOP over
   something the response controls, which is why each is a phase with a bounded step rather than a stretch of a
   completing function: the constructs of [1] `document`, and the top-level children a failed parse left
   behind. A phase list is what makes the loader's own state a thing a driver can stand in the middle of.
   EACH PHASE'S REST UNIT IS solver/rest_unit.h's AND NOT A `1` HELD HERE, and it is 1 for both loops for a
   reason worth reading there: both are counted in items whose SIZE the document picks — a [14] `CharData`
   run, a subtree — so a multiplier on either would multiply an unbounded quantity, which is a bound wearing a
   granularity's name. Nothing about that reasoning is decidable from inside this file, which is why the
   numbers are not in it.
   §14.2's SCRIPT STEP IS NOT A PHASE OF ITS OWN AND MUST NOT BECOME ONE. It happens AT AN END TAG, in the
   middle of [1] `document`, which is precisely what distinguishes it from HTML §13.2's `script` handling and
   is the whole reason a post-parse walk over the finished tree was the wrong shape: a walk gets the document
   ORDER right and the parse-then-execute ORDER wrong, and CLAUDE.md §ORDER-and-NARROW names parse-then-execute
   script order as the spec rather than a detail. So it lives inside XML_LOAD_PARSE, as a step the parse phase
   owes before it reads its next construct. */
enum { XML_LOAD_PARSE = 0,   /* REST_UNIT_XML_CONSTRUCTS items of core/xml/xml_parse.h's walk, and §14.2's
                                end-tag step for each `script` element that closes among them */
       XML_LOAD_DISCARD,     /* REST_UNIT_SUBTREE_REMOVALS children of the partial tree a failed parse left */
       XML_LOAD_ERRDOC,      /* §7.5.3's inline report, which is one element and one text node — no unit */
       XML_LOAD_DONE };

/* WHAT A §7.5.3 LOAD IS BETWEEN TWO ITEMS. `parse` is live exactly while the phase is XML_LOAD_PARSE and is
   destroyed by the finish that assembles `report`.
   `script_pending` IS §14.2's OWN STATE AND NOTHING ELSE'S: the section puts TWO actions at a `script`
   element's end tag — "the user agent must perform a microtask checkpoint, and then prepare the script
   element" — with the checkpoint FIRST, so between them the load is standing at a position that has to be
   expressible. It holds the element whose end tag the parse has just read and whose preparation is still owed.
   IT IS A BORROWED NODE OF THE TREE THIS LOAD IS BUILDING, which is sound for exactly one step: the step that
   sets it RETURNS immediately and the next step consumes it before anything else runs, so no phase that
   removes nodes (XML_LOAD_DISCARD) can be reached while it is non-NULL — asserted at both transitions rather
   than argued here. */
struct XmlDocumentLoad {
    lxb_html_document_t *document;
    lxb_dom_node_t      *root;
    DomParseRootKind     root_kind;
    HtmlScriptingMode    scripting;
    XmlParse            *parse;
    lxb_dom_node_t      *script_pending;
    XmlParseReport       report;
    uint8_t              phase;
};

XmlDocumentLoad *xml_document_load_begin(lxb_html_document_t *document, DomParseRootKind root_kind,
                                         HtmlScriptingMode scripting,
                                         const MimeType *type, const lxb_char_t *xml, size_t size)
{
    XmlDocumentLoad *l;

    DCHECK(document != NULL,
           "HTML §7.5.3 was reached with no Document — its own step creates one and hands it to the XML "
           "parser, so a loader with nothing to load into never ran that step");
    DCHECK(xml != NULL,
           "HTML §7.5.3 was handed a NULL pointer for the response's bytes — an empty XML entity is a zero "
           "SIZE over a real pointer, and XML §2.1's [1] `document` has an answer about one (there is no "
           "element, which is a well-formedness error and not an absent response)");
    DCHECK(document_load_type_of(type) == DOC_LOAD_XML,
           "XML's parser was reached with a response HTML §7.4.5's load-a-document sends to some other §7.5 "
           "subsection — the arm is a fact about the COMPUTED TYPE and this loader re-asks it, so this is a "
           "route into the XML parse that never asked what it fetched");

    l = malloc(sizeof(*l));
    CHECK(l != NULL,
          "OOM opening an HTML §7.5.3 document load — the DOM this parse produces is what every flow of this "
          "instance reads, so a load that cannot even begin is not a document with no endpoints");
    l->document  = document;
    l->root      = lxb_dom_interface_node(document);
    l->root_kind = root_kind;
    l->scripting = scripting;
    l->script_pending = NULL;
    l->phase     = XML_LOAD_PARSE;
    memset(&l->report, 0, sizeof(l->report));
    l->parse = xml_parse_begin(lxb_dom_interface_document(document), l->root, root_kind,
                               (const char *)xml, size);
    return l;
}

bool xml_document_load_ended(const XmlDocumentLoad *load)
{
    DCHECK(load != NULL, "xml_document_load_ended was asked of no load");
    return load->phase == XML_LOAD_DONE;
}

void xml_document_load_step(XmlDocumentLoad *load)
{
    size_t n;

    DCHECK(load != NULL, "xml_document_load_step was asked of no load");
    DCHECK(load->phase != XML_LOAD_DONE,
           "an HTML §7.5.3 load was stepped after it had nothing left to do — xml_document_load_ended is what "
           "a driver's loop tests, and asking past it is a driver that did not");

    switch (load->phase) {
    case XML_LOAD_PARSE:
        /* §14.2's END-TAG STEP, SECOND ACTION: "the user agent must perform a microtask checkpoint, and THEN
           prepare the script element." The checkpoint is the FIRST action and it has already happened — it is
           the RETURN that ended the previous step and handed the thread back to the one WFQ, which is what a
           microtask checkpoint IS in this engine: CLAUDE.md §Every-runtime-job makes every queued reaction a
           first-class flow and states outright that there is no `while(JS_ExecutePendingJob)` loop, so a drain
           written here would be the second scheduler §THERE-IS-NO-GRIND forbids. That is why the two actions
           are two steps and not one: expressing "checkpoint, then prepare" needs a position between them.
           NAMED RESIDUAL — the checkpoint OFFERS the queued reaction flows a turn and does not GUARANTEE they
           take one before this load resumes, because ordering between flows is the WFQ's and this load holds
           no priority over it. WHAT IS NOT COVERED: a reaction queued by an earlier `script` of this same
           parse, which the standard has settled before the next `script` is prepared. WHAT THE NEXT DIFF
           BUILDS: a run-these-before-that ordering the scheduler owns (never a drain here). HOW ITS ABSENCE
           SHOWS: two `script` elements in one XML document where the second observes a promise the first
           settled — the second reads it unsettled, and only there. It is unreachable while the preparation
           below returns at §4.12.1 step 18 (see it), because then no script of this parse runs to queue one:
           the two become live together, which is why they are one residual and not two. */
        if (load->script_pending != NULL) {
            lxb_dom_node_t *script = load->script_pending;

            load->script_pending = NULL;
            /* HTML §13.2.6.4.8 'The "text" insertion mode' AND §14.2 REACH THE SAME BODY, because they state
               the same action: a parser is at a `script` element's end tag, so §4.12.1 "The script element"'s
               "prepare the script element" runs with `parser document` non-null. One capability, one
               implementation — core/html/html_script.h owns which of §4.12.1's five destinations the element
               takes, and an XML document's script must not answer that question in a second place.
               IT RETURNS AT §4.12.1 STEP 18 FOR EVERY LOAD THIS ENGINE PERFORMS TODAY, and that is the
               standard's own step rather than a gap: "if scripting is disabled for el, then return", which
               §8.1.3.4 "Enabling and disabling scripting" defines over the node document's browsing context —
               and §7.5.1 "Shared document creation infrastructure" creates the Document and parses into it,
               while this engine parses and THEN builds the realm, so during a load there is no browsing
               context to be enabled. The scripts of a finished tree are inventoried by
               core/loader/document_scripts.h instead, which is exactly what §13.2.6.4.8's route does for an
               HTML document — so the two parsers agree, and they will still agree the day that order is
               corrected, with nothing here to change. */
            html_script_parser_inserted(script);
            return;
        }
        if (!xml_parse_ended(load->parse)) {
            /* THE UNIT IS ASKED FOR EVEN WHERE THE ANSWER IS ONE. Writing the `1` here instead would put a
               granularity decision back inside the component that rests, which is where nobody can see it
               beside the others — and the day core/xml/'s walk gains a pull of its own for [14] `CharData`
               (whose unbounded length is the reason this answer is 1) the number to move is in one file rather
               than in this loop. */
            n = rest_unit_items(REST_UNIT_XML_CONSTRUCTS);
            rest_unit_begin(REST_UNIT_XML_CONSTRUCTS);
            while (n-- > 0 && !xml_parse_ended(load->parse)) {
                lxb_dom_node_t *closed;

                xml_parse_step(load->parse);
                /* §14.2 ASKED IN ITS OWN ORDER: the scripting mode is the parser's ("when an XML parser with
                   XML scripting support enabled…"), the element's kind is §4.12.1's, and the boundary is the
                   grammar's. This is the only place the first of those three is known — see the head comment
                   on why core/xml/ is not told it. */
                if (load->scripting != HTML_SCRIPTING_ENABLED) continue;
                closed = xml_parse_closed_element(load->parse);
                if (closed == NULL || !html_script_is(closed)) continue;
                /* THE BATCH ENDS HERE, which is the microtask checkpoint being taken rather than a rest point
                   being offered. §14.2 puts the checkpoint BEFORE the preparation, so the parse may not read
                   another construct first — that is an ordering the standard states, not a granularity this
                   file may tune, which is why it breaks the loop instead of asking solver/rest_unit.h. */
                load->script_pending = closed;
                break;
            }
            rest_unit_end();
            return;
        }
        DCHECK(load->script_pending == NULL,
               "an XML parse reached the end of [1] `document` with §14.2's preparation still owed for a "
               "`script` element — the step that records one RETURNS immediately and the next step consumes "
               "it, so the two can never be outstanding at once unless a driver stepped the parse from "
               "somewhere other than the branch above");
        if (xml_parse_finish(load->parse, &load->report)) {
            load->parse = NULL;
            /* A WELL-FORMED PARSE, so the tree stands and §14.2's steps have already run at the end tags they
               belong to — there is nothing left for this load to do. */
            load->phase = XML_LOAD_DONE;
            return;
        }
        load->parse = NULL;
        load->phase = XML_LOAD_DISCARD;
        return;

    case XML_LOAD_DISCARD:
        /* §7.5.3: "Error messages from the parse process (e.g., XML namespace well-formedness errors) may be
           reported inline by mutating the Document." The partial tree the parse built is what a browser
           discards before doing so — a REST UNIT's worth of top-level children per step, because [1]
           `document`'s `Misc*` is as long as the response chooses to make it. That unit is one child today and
           solver/rest_unit.h says why it stays one: removing a child removes its subtree, so a multiplier here
           would multiply a size the document picked rather than a fixed cost. */
        if (load->root->first_child != NULL) {
            n = rest_unit_items(REST_UNIT_SUBTREE_REMOVALS);
            rest_unit_begin(REST_UNIT_SUBTREE_REMOVALS);
            while (n-- > 0 && load->root->first_child != NULL)
                dom_cow_remove_child(load->root->first_child);
            rest_unit_end();
            return;
        }
        load->phase = XML_LOAD_ERRDOC;
        return;

    case XML_LOAD_ERRDOC:
        /* ONE ELEMENT AND ONE TEXT NODE — the description is the layer's own sentence, see
           core/xml/xml_parse.h — so this phase is a single step and never a loop. */
        xml_parse_error_document(lxb_dom_interface_document(load->document), load->root, load->root_kind,
                                 &load->report);
        load->phase = XML_LOAD_DONE;
        return;
    }
    /* NOT A `default:` ARM — the switch is exhaustive over the phases above, so a phase added without a body
       does not compile rather than falling into a generic crash. XML_LOAD_DONE is refused by the DCHECK at the
       top, which is where a step past the end belongs. */
    DFAIL("an HTML §7.5.3 load stepped in a phase this loader does not define — the phase enum and this "
          "switch are one list and something wrote a value that is in neither");
}

lxb_status_t xml_document_load_finish(XmlDocumentLoad *load)
{
    DCHECK(load != NULL, "xml_document_load_finish was asked of no load");
    DCHECK(load->phase == XML_LOAD_DONE,
           "an HTML §7.5.3 load was finished with work left — the parse, the discard of a failed parse's "
           "partial tree and §7.5.3's inline error report are each a phase, and finishing inside one leaves a "
           "Document the caller will read as complete");
    DCHECK(load->parse == NULL,
           "an HTML §7.5.3 load reached its end still holding an open XML parse — xml_parse_finish is what "
           "destroys the build, so a live one here is a tree build leaked with the load");
    DCHECK(load->script_pending == NULL,
           "an HTML §7.5.3 load was finished with HTML §14.2 \"Parsing XML documents\"' preparation still owed "
           "for a `script` element — the element's end tag was parsed and the code the document shipped was "
           "never prepared, which is a page that loads with its own script silently not running");
    free(load);
    /* §7.5.3 HAS NO FAILURE STATUS. An ill-formed document is not a load that failed — it is a load whose
       Document is the inline report the standard permits, which the phases above have already built. */
    return LXB_STATUS_OK;
}

void xml_document_load_abort(XmlDocumentLoad *load)
{
    DCHECK(load != NULL, "xml_document_load_abort was asked of no load");
    /* THE PHASES AFTER THE PARSE HAVE NOTHING TO ABANDON — the discard and §7.5.3's inline report each act on
       the TREE, which this does not touch, so the only thing an abandoned §7.5.3 load still holds is an open
       tree build. §14.2's owed preparation is DROPPED WITH THE LOAD AND THAT IS THE CORRECT END: the flow that
       was driving this parse is gone, so there is no world for that script to be a program of — running it
       here would run a page's code on behalf of a flow that no longer exists. */
    if (load->parse) {
        xml_parse_abort(load->parse);
        load->parse = NULL;
    }
    free(load);
}
