/* HTML §8.4.1 "Opening the input stream" + §8.4.2 "Closing the input stream" steps 3-6 — see document_open.h
   for why the stream's lifetime is its own component and what it still owes core/frame/navigable.c. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/document_current_script.h"   /* §8.4.1 step 5's "active parser whose script nesting level > 0" */
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/frame/window_proxy.h"
#include "core/html/document_open.h"
#include "core/html/html_parser_snapshot.h"   /* a fork arm's parser state, put back before it writes */
#include "core/html/html_parse.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"
#include "solver/world.h"

/* §8.4.2 step 3's "script-created parser", as a property under a Symbol nothing publishes — document_open.h
   states why the record is a property (the COW delta captures it, so it is per flow) rather than a C field.
   `g_ready` and not a test of the key, for core/events/event_target.c's reason: a static JSValue is
   zero-initialised and zero is not JS_UNDEFINED. */
static JSValue g_key;
/* The parser state this flow's last write left (core/html/html_parser_snapshot.h), beside the record and
   time-travelling with it. */
static JSValue g_snap_key;
static bool    g_ready;

void document_open_init(JSContext *ctx)
{
    DCHECK(!g_ready, "document_open_init ran twice — the key is the AGENT's and is minted once in it");
    g_key = JS_NewSymbol(ctx, "scriptCreatedParser", false);
    CHECK(!JS_IsException(g_key),
          "§8.4.2 step 3's script-created-parser key could not be allocated — without it `close()` cannot tell "
          "a parser §8.4.1 minted from one a document load left open, and would end the wrong parse");
    g_snap_key = JS_NewSymbol(ctx, "scriptCreatedParserState", false);
    CHECK(!JS_IsException(g_snap_key), "§8.4.1's parser-state key could not be allocated");
    g_ready = true;
}

void document_open_free(JSRuntime *rt)
{
    DCHECK(g_ready, "document_open_free ran in an agent that never minted the key");
    JS_FreeValueRT(rt, g_key);
    JS_FreeValueRT(rt, g_snap_key);
    g_key = JS_UNDEFINED;
    g_snap_key = JS_UNDEFINED;
    g_ready = false;
}

/* The record, read and written as an OWN slot — never a property lookup, for the reason event_target.c's
   listener map states: a miss on a page-reachable object is the solver's absent-state seam and would mint a
   concolic where an internal slot belongs. It is this flow's statement that it opened the stream, as the
   parser's write_epoch at its last write into it (a number), or absent/false for no stream: §8.4.1's own initial
   state, a Document with no parser until an algorithm creates one. Being a JS slot it time-travels, so after a
   fork each arm holds the epoch it last wrote. */
static JSValue open_record_get(JSContext *ctx, JSValueConst doc_obj)
{
    JSAtom k;
    JSValue v;

    DCHECK(g_ready, "§8.4.2 step 3's record was asked for before the key existed");
    DCHECK(JS_IsObject(doc_obj),
           "§8.4.1's record was asked of a Document with no wrapper — the record lives on the object the member "
           "was CALLED on, so a caller without one has not come through a member");
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_GetOwnSlot(ctx, &v, doc_obj, k) <= 0) v = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return v;
}

static bool open_record_read(JSContext *ctx, JSValueConst doc_obj)
{
    JSValue v = open_record_get(ctx, doc_obj);
    bool on = JS_IsNumber(v);

    JS_FreeValue(ctx, v);
    return on;
}

/* `epoch` is the parser's write_epoch this flow last saw, or 0 for no open stream. */
static void open_record_write(JSContext *ctx, JSValueConst doc_obj, uint64_t epoch)
{
    JSAtom k;

    DCHECK(g_ready, "§8.4.2 step 3's record was written before the key existed");
    k = JS_ValueToAtom(ctx, g_key);
    JS_SetProperty(ctx, (JSValue)doc_obj, k, epoch ? JS_NewFloat64(ctx, (double)epoch) : JS_FALSE);
    JS_FreeAtom(ctx, k);
}

/* This flow's write is done: the record takes the parser's new write count and the snapshot its state. */
static void open_stream_record_write(JSContext *ctx, JSValueConst doc_obj, lxb_html_parser_t *parser)
{
    JSAtom k = JS_ValueToAtom(ctx, g_snap_key);

    parser->write_epoch++;
    open_record_write(ctx, doc_obj, parser->write_epoch);
    JS_SetProperty(ctx, (JSValue)doc_obj, k, html_parser_snapshot_take(ctx, parser));
    JS_FreeAtom(ctx, k);
}

/* The script-created parser is one lxb_html_parser_t, not in any delta. Its write_epoch counts writes and this
   flow's record holds the count at its own last write, so the two differ exactly when a sibling arm of a fork
   has written since; this arm then puts back the state its last write left before it writes. So the live
   parser is only a cache of whichever arm wrote last, and no arm touches it without passing through here. */
static void open_stream_make_current(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom)
{
    lxb_html_parser_t *parser = (lxb_html_parser_t *)dom->parser;
    JSValue v = open_record_get(ctx, doc_obj), snap;
    double mine = 0.0;
    JSAtom k;

    DCHECK(parser != NULL && JS_IsNumber(v), "a script-created parser was written by a flow that did not open it");
    JS_ToFloat64(ctx, &mine, v);
    JS_FreeValue(ctx, v);
    if (mine == (double)parser->write_epoch) return;
    k = JS_ValueToAtom(ctx, g_snap_key);
    if (JS_GetOwnSlot(ctx, &snap, doc_obj, k) <= 0) snap = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsObject(snap), "a flow that wrote into a script-created parser holds no snapshot of it");
    html_parser_snapshot_restore(ctx, parser, snap);
    JS_FreeValue(ctx, snap);
}

void document_open_stream_write(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom,
                                const lxb_char_t *text, size_t size)
{
    open_stream_make_current(ctx, doc_obj, dom);
    html_parse_document_write(lxb_html_interface_document(dom), text, size);
    open_stream_record_write(ctx, doc_obj, (lxb_html_parser_t *)dom->parser);
}

bool document_open_stream_is_ours(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom)
{
    bool ours = open_record_read(ctx, doc_obj);

    /* The record says whether THIS flow opened the stream; §13.2.3.5's insertion point is a fact about the one
       lxb_html_parser_t the document carries. A flow whose record is open while the parser is closed is a fork
       arm whose sibling closed the stream: open_stream_make_current reopens it into this arm's own state before
       anything is written. The other disagreement is not built: a sibling's open stream visible to a flow that
       never opened one (`setTimeout(function () { document.write('<b>x</b>'); }, 0)` after an unclosed write
       is the shape, a stream left open across a flow boundary). */
    DCHECK(ours || !html_parse_insertion_point_defined(dom),
           "§8.4.2 step 3: another flow's script-created parser is open on this document and this flow never "
           "opened one. Lexbor's parser is one lxb_html_parser_t; per-arm state exists only for the flows that "
           "wrote into it (core/html/html_parser_snapshot.h). Build the case of a flow that did not open the "
           "stream: its own closed state, restored before its §8.4.1 open steps run");
    return ours;
}

/* §8.4.1 step 9's "erase all event listeners and handlers", over one node, and ONLY where there is something
   to erase. §2.7's listener list and §8.1.7.2's handler map are both stored on the node's WRAPPER (core/events/
   event_target.c), and a wrapper is minted by the first reach that needs one — so a node without one has never
   been reached by the page and holds neither. That is a positive statement about where the state lives, not an
   optimisation: minting a wrapper here to erase nothing would also make one per node of the tree being thrown
   away, in the realm of whichever flow happened to call `open()`. */
static void erase_listeners_of(JSContext *ctx, lxb_dom_node_t *n)
{
    JSValueConst w = node_wrap_peek(n);

    if (JS_IsObject(w)) event_target_erase_all(ctx, w);
}

/* HTML §8.4.1 "Opening the input stream" STEP 13 — "Set document's is initial about:blank to false" — over the
   storage core/frame/window_proxy.h keeps §3.1.1 "The Document object"'s flag in: a byte on the NAVIGABLE
   holding the flag's negation, because a navigable's initial Document has no realm to hold one until something
   reaches through it.
   IT IS AN UNCONDITIONAL SET AND THAT IS THE WHOLE OF THE STEP. A DCHECK stood here asserting the flag was
   ALREADY false, which asserted the negation of the case the step is written for: §7.4 creates every navigable
   holding an initial `about:blank`, so a `document.write` into a fresh frame — the commonest arrival at these
   steps on the real web — reaches step 13 with the flag SET, by construction rather than by accident.
   A DOCUMENT WITH NO NAVIGABLE NEEDS NO WRITE, and that is §3.1.1's own initial value rather than a skipped
   step: the flag is "a boolean, initially false", only §7.4's create-a-new-navigable ever makes one true, and a
   Document that has no navigable was never handed one. A realm-less Document is the same statement one level
   further out — this engine keeps a Document's Window, and therefore its navigable, in the realm. */
static void open_clear_initial_about_blank(JSContext *realm)
{
    JSValueConst proxy;

    if (realm == NULL) return;
    proxy = document_window_proxy(realm);
    if (!window_proxy_is(proxy)) return;
    window_proxy_clear_initial_about_blank(proxy);
}

bool document_open_steps(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom)
{
    lxb_dom_node_t *dnode = lxb_dom_interface_node(dom);
    JSContext *realm = document_active_realm_of(dnode);
    lxb_dom_node_t *n, *next;
    lxb_status_t st;

    /* STEPS 1 AND 2 — the XML throw and the throw-on-dynamic-markup-insertion counter — ARE THE CALLER'S here,
       and they are asserted rather than restated. Every entry into these steps in this engine is §8.4.3 step
       9.2 or the `open()` member, and both perform §8.4.3/§8.4.1's identical first two steps before they get
       here; a second copy would be two statements of one throw with a window between them for them to drift.
       The counter is DECIDED and not read, for the reason core/html/document_write.c states at its own step 7:
       §13.2.6.1 "Creating and inserting nodes" is the only algorithm that raises it, around a custom element
       constructor the PARSER runs, and this engine's tree construction runs none. */
    DCHECK(strcmp(document_content_type_of(dom), "text/html") == 0,
           "§8.4.1 step 1 — the document open steps were reached with an XML document. Both entries throw "
           "\"InvalidStateError\" for one before they call this, so an XML document arriving here is an entry "
           "that skipped its own step 1");

    /* STEPS 3 AND 4 — entryDocument, and the SecurityError for an entry document of another origin.
       THE ENTRY DOCUMENT IS NOT REACHABLE IN THIS ENGINE and this is the same substitution core/frame/
       location.c and core/timing/timer.c make with their own §7.2.4 and §8.7 entry settings objects: a C
       member runs in the realm that DEFINED it (`js_call_c_function` takes `p->u.cfunc.realm`), so what is
       reachable is the RELEVANT settings object. The answer to STEP 4 is a CONSTANT because of what an
       instance IS — an origin-keyed agent cluster (§Security), so every realm the entry could be is same
       origin with every Document this agent hosts — and the premise is what is checked here rather than the
       comparison. It is a real check: a Document a PEER instance holds fails it.
       WHERE THE SUBSTITUTION IS NOT FREE IS STEP 12, and it is stated there. */
    DCHECK(realm == NULL || world_doc_hosted(document_doc(realm)),
           "§8.4.1 step 4's same-origin check ran against a Document a PEER INSTANCE holds — an instance is an "
           "origin-keyed agent cluster, so a Document this agent hosts is same origin with every realm the "
           "entry global object could be, and that premise is the ONLY reason this check can be a constant. A "
           "Document that lives elsewhere needs the real comparison, and a \"SecurityError\" DOMException for "
           "the entry documents that fail it");

    /* STEP 5 — "If document has an active parser whose script nesting level is greater than 0, then return
     * document", whose own note says what it is for: "This basically causes document.open() to be ignored when
     * it's called in an inline script found during parsing, while still letting it have an effect when called
     * from a non-parser task such as a timer callback or event handler."
     *
     * THIS ENGINE CANNOT BE IN THAT STATE AND IS IN THE STATE IT STANDS FOR. A document's parse runs to
     * §13.2.7 "The end" before any of its scripts are seeded as flows, so while those scripts run the document
     * has NO active parser and an UNDEFINED insertion point — which is a browser's post-load state, reached
     * from a browser's mid-parse moment. Serving such a write out of these steps is not a near-miss: it would
     * ERASE the page the script is running in, in the one case the standard singles out for being left alone.
     * So it crashes, and the crash is what keeps the two halves of this algorithm distinguishable.
     * THE CONDITION IS THE STANDARD'S OWN AND IT IS ASKED OF THE ELEMENT, NOT OF THE READINESS. §4.12.1.1
     * parks the running classic script in `currentScript`, and its HTML §4.12.1.1 SCHEDULE says whether the parser is
     * standing inside it — core/dom/document_current_script.c states the whole argument, including the three
     * schedules whose writes ARE destructive in a browser and the cross-document `w.document.open()` that the
     * readiness alone would have refused. Asked of the TARGET document's realm, because step 5 is about
     * `document`'s parser and the caller may be another realm's script. */
    if (realm != NULL && !open_record_read(ctx, doc_obj) &&
        document_current_script_is_parser_executed(realm)) {
        DFAIL("§8.4.1 step 5 — the document open steps were reached for a Document that is still LOADING, "
              "which in HTML is a Document with an ACTIVE PARSER: §13.2.7 \"The end\" sets the insertion point "
              "to undefined and the readiness to \"interactive\" in consecutive steps, so a script running at "
              "\"loading\" is a script the parser is executing, and step 5 returns for it — a parse-time "
              "`document.write` APPENDS at §13.2.3.5's insertion point and must never replace the document. "
              "This engine reaches here because it parses a document to completion and seeds its scripts as "
              "flows afterwards, so the mid-parse state the standard is describing does not exist. What must "
              "be built is the document's own parse kept OPEN across script execution — its OWNERSHIP half is "
              "DONE (solver/dom_cow.c's cow_tc_create records every node §13.2.6 makes into the running flow's "
              "delta, so a live parser's nodes die with the flow that built them exactly as an appendChild's "
              "do). WHAT IS LEFT IS *WHEN* §13.2.7 \"The end\" RUNS, AND NOT WHICH ENTRY OPENS THE PARSE: "
              "core/loader/html_document.c ALREADY opens the §7.5.2 load with html_parse_document_open and "
              "fills it a rest unit at a time, so the parse is incremental and the stream is genuinely open "
              "while it runs. What completes it before any script of that document exists is the FINISH — "
              "core/loader/document_load.c's document_load_finish, reached from core/frame/navigable.c's "
              "nav_create_finish for a child navigable and from the host's own completing document_load for "
              "the root — and moving it is not a relocation of one call, because that finish is what the "
              "REALM is built around: nav_create_finish's own assert says the Document handed over must be "
              "PARSED, since document_policy_new walks the finished tree for CSP §3.3's `<meta>` policies. So "
              "the subproblem this crash names is an ORDER a browser gets by construction and this engine "
              "buys with the finish: §4.2.5.3 \"Pragma directives\" says \"when a meta element is inserted "
              "into the document, if its http-equiv attribute is present and represents one of the above "
              "states, then the user agent must run the algorithm appropriate for that state\", and "
              "§13.2.6.4.4 'The \"in head\" insertion mode' does that insertion AT THE TOKEN — so a browser "
              "has the policy in force before the next token is read, while this engine has it only because "
              "the whole tree exists before the realm does. That ordering, and not the opening, is what the "
              "next diff owes. Its hazard is html_parse.c's own: lexbor "
              "emits the EOF token in `chunk_end` and §13.2.6 builds html/head/body from it, so a document "
              "whose source produced none has a NULL documentElement until the close, and each reader that "
              "assumes otherwise is what must change. With that, a parse-time script runs with the insertion "
              "point DEFINED and never reaches §8.4.3 step 9 at all");
        /* STEP 5's OWN ACTION — "return document" — so that a release build, where the DFAIL above is compiled
           out, takes the standard's branch for the state it believes it is in rather than falling through into
           the destructive half of an algorithm the crash exists to keep it out of.
           AND THE CALLER HAS TO PERFORM THE SAME STEP OR THE RELEASE BUILD PERFORMS HALF OF ONE ALGORITHM.
           This arm returns a Document with NO INPUT STREAM, which is a state §8.4.3 step 10 cannot be reached
           in inside the standard — every §8.4.1 arm that returns without creating a parser has an insertion
           point anyway — so a caller that read this `true` as "a stream is open" inserted into a parser that is
           not in lexbor's PROCESS stage, and its WRONG_STAGE arrived at core/html/html_parse.c's always-fatal
           ALLOCATION check. core/html/document_write.c now asks §13.2.3.5's insertion point after this returns
           and takes step 9's own answer; the two halves of one release build agree, and neither of them is a
           softening of the dev crash above. */
        return true;
    }

    /* STEP 6 — "if document's unload counter is greater than 0, then return document", which its note explains
       as ignoring `open()` from a beforeunload, pagehide or unload handler. §7.5.9 "Unloading documents" is the
       only algorithm that raises that counter and this engine HAS those steps (core/frame/document_lifecycle.c
       runs them as a machine), so this is an absence with a producer rather than a decided constant: a
       `document.write` from an unload handler is performed here where a browser ignores it. The counter has to
       be raised and lowered by the unload machine's own stages — a C bracket around a work item reads the
       wrong flow's state, which is the defect core/dom/document_current_script.h records — and it is that
       machine's to add, not this algorithm's to guess.
       STEP 7 — "if document's active parser was aborted is true, then return document" — is DECIDED, for the
       reason core/html/document_write.c states at its own step 8: §8.4.1 declares the boolean with its initial
       value ("It is initially false") and what sets it is a navigation aborting a document MID-PARSE. This
       engine has no mid-parse state to abort. It becomes a real field in the diff that gives a document a
       parse that outlives its own C call — the same diff step 5 above names.
       NEITHER IS WRITTEN AS `if (0)`: a condition over a constant reads as a check and is not one. */

    /* STEP 8 — "If document's node navigable is non-null and document's node navigable's ongoing navigation is
       a navigation ID, then stop loading document's node navigable." THIS ENGINE HAS NO ENTRY FOR IT AND NO
       WAY TO ASK THE QUESTION: core/frame/navigable.c neither exposes an ongoing navigation nor a stop, so a
       `document.open()` that races a navigation of the same navigable leaves that navigation running and its
       document arrives on top of the one these steps just built. It is named here rather than approximated
       because the approximations available (treating a parked host request as the navigation, or asking
       whether this flow is the one navigating) are both about the FLOW and the question is about the
       NAVIGABLE. */

    /* STEPS 9 AND 10 — "For each shadow-including inclusive descendant node of document, erase all event
       listeners and handlers given node", then the same for the relevant global object.
       THE WINDOW IS NOT A DESCENDANT AND IS THE HALF THAT MATTERS. A `load` handler that survived the open
       runs again the moment §13.2.7 reaches its end for the newly written document, which for a handler that
       writes is not a divergence but a LOOP. */
    for (n = dnode; n; n = shadow_root_next_in_shadow_including(ctx, n, dnode))
        erase_listeners_of(ctx, n);
    if (realm != NULL) {
        JSValueConst win = document_window_of(dnode);
        DCHECK(JS_IsObject(win),
               "§8.4.1 step 10 — a Document with an active realm has no Window to erase the handlers of. The "
               "relevant global object is what a page registers `onload` on, and a document.open() that leaves "
               "it standing re-runs the handler that wrote the document as soon as the write is closed");
        event_target_erase_all(ctx, win);
    }

    /* STEP 11 — "Replace all with null within document", which for a Document node is the removal of every
       child (DOM §4.2.3's "replace all" adds nothing when the node is null). Through the per-flow chokepoint,
       so a forked arm that opened the document reads back its own empty tree and a sibling still reads the
       page. It is deliberately NOT the mutation-observer-firing form: §4.2.3's replace-all runs no mutation
       records that a document with no listeners left could deliver. */
    for (n = dnode->first_child; n; n = next) {
        next = n->next;
        dom_cow_remove_child(n);
    }
    /* …AND SELECTION API §2's SINGLETON, REPLACED WITH THE TREE IT POINTED INTO. §2's note states the one
       writer besides the install — "it gets replaced with a new object when Document.open() is called" — and
       HERE is where the object it describes stops being about anything: every node the old selection could
       have been anchored in has just left the tree. Placed after the removal rather than before it so the
       fresh object is never momentarily the selection of the OLD tree, which is the ordering the note's own
       sentence implies and the only one under which a reader of `getSelection()` between the two lines could
       not observe a mixture.
       THE RECORD OWNS THE SLOT AND PERFORMS THE SWAP — core/dom/document.c's `document_replace_selection`,
       which leaves a document with no browsing context without one rather than minting it a selection §4.1
       must answer null for. Until this line existed, core/dom/selection.c asserted at `getSelection()` that
       no producer for this replacement had been built; that assertion is retired by this call and deleted at
       its site, because a crash that outlives the absence it names is the failure mode this project rates
       worst. */
    document_replace_selection(ctx, doc_obj);
    /* …AND THE PARSER'S HEAD ELEMENT POINTER (HTML §13.2.4.4 "The element pointers"), which names a node that
       has just left the tree and which the new parser starts without: §13.2.4.1 "The insertion mode"'s
       "reset the insertion mode appropriately" reads it. It is the one tree-builder pointer lexbor keeps on
       the document; the doctype, the document element, `head` and `body` are read from the tree. Through the POD latch for the reason
       step 15 is: it is a field of the shared Lexbor document. */
    {
        lxb_html_document_t *html_doc = lxb_html_interface_document(dom);

        cow_capture_host_state(ctx, doc_obj, &html_doc->parser_head_pointer,
                               sizeof html_doc->parser_head_pointer);
        html_doc->parser_head_pointer = NULL;
    }

    /* STEP 12 — "If document is fully active: let newURL be a copy of entryDocument's URL; if entryDocument is
       not document, then set newURL's fragment to null; run the URL and history update steps with document and
       newURL."
       IT IS A NO-OP HERE AND THE REASON IS THE SUBSTITUTION AT STEPS 3-4, not an omission: with the RELEVANT
       document standing in for the ENTRY document, entryDocument IS document, so newURL is a copy of the
       document's own URL and §7.4.4 "Non-fragment synchronous \"navigations\""'s update writes the address it
       already has. WHAT THE SUBSTITUTION COSTS IS
       EXACTLY THIS STEP — a same-origin `otherFrame.contentDocument.open()` must move the OTHER document's
       address to the CALLER's, and this engine leaves it where it was. Closing it needs the entry global
       object, which is a fact about the running SCRIPT and not about any realm a C member can reach; the
       engine's own flow machinery is what knows which document's program is executing. */

    /* STEP 13 — "Set document's is initial about:blank to false."
       WHAT THE FLAG DECIDES ONCE IT IS FALSE is why this step is not bookkeeping: §7.4.4 "Non-fragment
       synchronous \"navigations\""'s URL and history update steps read it at their step 4 ("if document's is
       initial about:blank is true, then set historyHandling to \"replace\""), so a page that opens a window,
       writes into it and then navigates it gets the history entry the navigation should push instead of a
       replace for ever after; and §7.5.8 "Finishing the loading process" distinguishes at its step 3 the
       initial `about:blank` Document of a frame — whose container is still null there — from a Document a load
       produced, which is a `load` event fired or not fired. */
    open_clear_initial_about_blank(realm);

    /* STEP 14 — "If document's iframe load in progress flag is set, then set document's mute iframe load
       flag." Both flags are §4.8.5 "The iframe element"'s, raised around the iframe load event steps, and this
       engine keeps neither: core/html/html_iframe.c fires the container's `load` from §7.5.8 without the
       re-entrancy bracket the two flags form. The absence is that component's and shows up there first — a
       `document.open()` from inside an iframe's own `load` handler fires a second load — so it is named and
       not guessed at from here. */

    /* STEP 15 — "Set document to no-quirks mode." Through the POD latch, because `compat_mode` is a field on
       the shared Lexbor document and a flow that opened the document must not change what its siblings parse
       against: DOM §4.5's mode decides §13.2.6's quirks behaviour and CSSOM's comparisons. */
    cow_capture_host_state(ctx, doc_obj, &dom->compat_mode, sizeof dom->compat_mode);
    dom->compat_mode = LXB_DOM_DOCUMENT_CMODE_NO_QUIRKS;

    /* STEPS 16 AND 17 — create an HTML parser and associate it with document, and set the insertion point to
       just before the end of the (empty) input stream. ONE CALL, because in Lexbor the input stream is the
       tokenizer's own cursor rather than a buffer this engine holds (core/html/html_parse.h states it at the
       entry), so a parse opened over zero bytes IS a parser standing at the end of an empty stream.
       DOM_PARSE_ROOT_SHARED because the tree being built is the ACTIVE document's, which every flow reads —
       the declaration §13.2.6's writes are captured under. HTML_SCRIPTING_ENABLED because §13.2.4.5 fixes the
       scripting flag at the parser's creation from the Document it is associated with, and a Document reaching
       these steps is one whose scripts this engine runs.
       THE DOCUMENT HAS NO `documentElement` UNTIL SOMETHING IS WRITTEN, and that is a browser's answer too:
       §8.4.1 empties the tree at step 11 and §13.2.6 builds `html`/`head`/`body` out of the first token. */
    /* AND THE ONE PRECONDITION THE OPEN HAS, WHICH IS A DESTRUCTOR IN DISGUISE. Lexbor's
       `lxb_html_document_parse_chunk_begin` CLEANS the document first unless its `ready_state` is UNDEF or
       LOADING, and `lxb_html_document_clean` runs `lexbor_mraw_clean` over `mraw` and `text` — which
       core/dom/node_heap.h has made the AGENT'S, shared by every document in this instance. Opening a parse on
       a document whose load finished (lexbor's `lxb_html_tree_stop_parsing` leaves it COMPLETE) would hand
       back every chunk of every OTHER document while their trees still pointed into them.
       THE FIELD IS THE PARSER'S READY STATE AND NOT §3.1.5's READINESS, which is why setting it here is not a
       second statement of step 18: lexbor's has three values (UNDEF, LOADING, COMPLETE) and gates that clean,
       while §3.1.5's has "interactive" and is what `readyState` answers — core/dom/document.c holds that one in
       the realm's own record. What they agree on is the fact §8.4.1 is establishing at this moment: this
       document is being parsed again. Through the POD latch, like the other Lexbor fields this algorithm
       moves. */
    {
        lxb_html_document_t *html_doc = lxb_html_interface_document(dom);

        cow_capture_host_state(ctx, doc_obj, &html_doc->ready_state, sizeof html_doc->ready_state);
        html_doc->ready_state = LXB_HTML_DOCUMENT_READY_STATE_LOADING;
    }
    st = html_parse_document_open(lxb_html_interface_document(dom), DOM_PARSE_ROOT_SHARED,
                                  HTML_SCRIPTING_ENABLED, (const lxb_char_t *)"", 0);
    CHECK(st == LXB_STATUS_OK,
          "§8.4.1 step 16's parser could not be created — the document has already been emptied by step 11, so "
          "there is no state to return to and a page whose `document.write` silently did nothing would be "
          "reported on as a blank document it never built");
    open_stream_record_write(ctx, doc_obj, (lxb_html_parser_t *)dom->parser);   /* the open's own write */

    /* STEP 18 — "Update the current document readiness of document to 'loading'", whose note is why it is last:
       the `readystatechange` it fires "is actually unobservable to author code, because of the previous step
       which erased all event listeners and handlers that could observe it". A Document with no realm has
       nowhere to hold §3.1.5's readiness in this engine (core/dom/document.c keeps it in the realm's own
       baseline record) and answers "complete" from `document_readiness_of`; that is wrong for a Document that
       now has a parser, and it is the storage's gap rather than this step's. */
    if (realm != NULL) document_set_readiness_loading(realm);
    return true;                                                                  /* STEP 19 */
}

void document_close_input_stream(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom)
{
    lxb_status_t st;

    /* STEP 3 — "If there is no script-created parser associated with this, then return." */
    if (!document_open_stream_is_ours(ctx, doc_obj, dom)) return;
    /* STEPS 4 AND 6 — the explicit "EOF" character at the end of the parser's input stream, and running the
       tokenizer to it. ONE call for the same reason step 16 above is one: the stream is the tokenizer's cursor,
       so the EOF and the run to it are `chunk_end`, which is also §13.2.7 "The end" — the insertion point
       becomes undefined and the `html`/`head`/`body` a zero-length write never produced are built here.
       STEP 5 — "if this's pending parsing-blocking script is not null, then return" — is DECIDED, AND THE
       ARGUMENT THAT USED TO DECIDE IT IS RETIRED RATHER THAN DELETED, because a reader who re-derives it will
       re-introduce it. The retired argument was that this engine's tree construction prepares no script at
       all, resting on §8.4.3's permission — "User agents are explicitly allowed to avoid executing script
       elements inserted via this method" — and it has not been true since core/html/html_parse.c's
       token-done wrapper began calling
       html_script_parser_inserted at every non-fragment `</script>`: a written `<script src>` IS prepared, and
       measurably reaches the same program-compile as a `<script src>` the markup carried.
       WHAT DECIDES IT NOW IS THAT THIS ENGINE HAS NO SUCH FIELD, and that is a positive statement rather than
       an absent one. HTML §4.12.1.1's `pending parsing-blocking script` exists so §13.2.6.4.8 'The "text" insertion
       mode' can BLOCK THE TOKENIZER on it; this engine's tokenizer does not block — core/html/html_script.c
       sends a parser-inserted external script to the flow's own sequence instead, and names the
       parser-suspension capability that would change it. So there is nothing for this step to read, and it
       becomes a real read in the diff that gives the tokenizer a way to stop. */
    open_stream_make_current(ctx, doc_obj, dom);
    st = html_parse_document_close(lxb_html_interface_document(dom));
    /* The close changes the parser's state too, so it counts as a write: a sibling arm still holding the
       stream open sees the parser moved past its last write and restores. */
    ((lxb_html_parser_t *)dom->parser)->write_epoch++;
    CHECK(st == LXB_STATUS_OK,
          "§8.4.2 step 6's tokenizer run did not complete — the document is left with an input stream that is "
          "neither open nor closed, and every later `document.write` would insert into a parser that will "
          "never emit its EOF");
    open_record_write(ctx, doc_obj, 0);
    {
        JSAtom k = JS_ValueToAtom(ctx, g_snap_key);

        JS_SetProperty(ctx, (JSValue)doc_obj, k, JS_UNDEFINED);   /* no stream, no state to put back */
        JS_FreeAtom(ctx, k);
    }
}
