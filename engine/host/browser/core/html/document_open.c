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
#include "core/html/html_parse.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"
#include "solver/world.h"

/* §8.4.2 step 3's "script-created parser", as a property under a Symbol nothing publishes — document_open.h
   states why the record is a property (the COW delta captures it, so it is per flow) rather than a C field.
   `g_ready` and not a test of the key, for core/events/event_target.c's reason: a static JSValue is
   zero-initialised and zero is not JS_UNDEFINED. */
static JSValue g_key;
static bool    g_ready;

void document_open_init(JSContext *ctx)
{
    DCHECK(!g_ready, "document_open_init ran twice — the key is the AGENT's and is minted once in it");
    g_key = JS_NewSymbol(ctx, "scriptCreatedParser", false);
    CHECK(!JS_IsException(g_key),
          "§8.4.2 step 3's script-created-parser key could not be allocated — without it `close()` cannot tell "
          "a parser §8.4.1 minted from one a document load left open, and would end the wrong parse");
    g_ready = true;
}

void document_open_free(JSRuntime *rt)
{
    DCHECK(g_ready, "document_open_free ran in an agent that never minted the key");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_ready = false;
}

/* The record, read and written as an OWN slot — never a property lookup, for the reason event_target.c's
   listener map states: a miss on a page-reachable object is the solver's absent-state seam and would mint a
   concolic where an internal slot belongs. ABSENT IS FALSE and that is §8.4.1's own initial state: a Document
   has no parser until an algorithm creates one. */
static bool open_record_read(JSContext *ctx, JSValueConst doc_obj)
{
    JSAtom k;
    JSValue v;
    bool on;

    DCHECK(g_ready, "§8.4.2 step 3's record was asked for before the key existed");
    DCHECK(JS_IsObject(doc_obj),
           "§8.4.1's record was asked of a Document with no wrapper — the record lives on the object the member "
           "was CALLED on, so a caller without one has not come through a member");
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_GetOwnSlot(ctx, &v, doc_obj, k) <= 0) v = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    on = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    return on;
}

static void open_record_write(JSContext *ctx, JSValueConst doc_obj, bool on)
{
    JSAtom k;

    DCHECK(g_ready, "§8.4.2 step 3's record was written before the key existed");
    k = JS_ValueToAtom(ctx, g_key);
    JS_SetProperty(ctx, (JSValue)doc_obj, k, JS_NewBool(ctx, on));
    JS_FreeAtom(ctx, k);
}

bool document_open_stream_is_ours(JSContext *ctx, JSValueConst doc_obj, lxb_dom_document_t *dom)
{
    bool ours = open_record_read(ctx, doc_obj);

    /* THE TWO-SIDED ASSERTION, AND THE ONE STATE IT REFUSES. This flow's record says whether IT opened the
       stream; §13.2.3.5's insertion point is a fact about the ONE `lxb_html_parser_t` the document carries,
       which is not in any delta. They agree for every flow that opened and closed its own stream, and they
       disagree in exactly one situation: a SIBLING flow opened this document's stream and parked. Resolving
       that to either answer is wrong in a way that cannot be seen afterwards — taking the parser's word makes
       this flow write into a sibling's tokenizer, taking the record's makes it re-open a stream the sibling is
       standing in — so it crashes at the flow that is about to do it, naming the capability that would make
       the two agree. */
    DCHECK(ours == html_parse_insertion_point_defined(dom),
           "§13.2.3.5's insertion point and this flow's §8.4.2 step 3 record DISAGREE about whether an input "
           "stream is open on this document — which is another flow's script-created parser being visible to "
           "this one. Lexbor's parser is a single `lxb_html_parser_t` on the Document and its state (the "
           "tokenizer's cursor, the stack of open elements, the insertion mode) is not captured by the per-flow "
           "COW delta, so §8.4.1's document open steps are the one operation in this engine whose effect a "
           "context switch does not carry. What must be built is the capture of that parser state, in "
           "core/html/html_parse.c beside the tree-construction routing dom_cow_install_tree_construction "
           "already performs — the tree writes are per flow and the parser that made them is not");
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

/* HTML §3.1.1 "The Document object"'s "is initial about:blank" — "Each Document has an is initial about:blank,
   which is a boolean, initially false" — read the way core/frame/window_proxy.h exposes it, as whether the
   navigable has EVER been navigated. §7.4.4 "Non-fragment synchronous \"navigations\"" is where it is CLEARED
   and not where it is declared, which is the number this comment carried until engine/citegen.mjs read the
   standard's own definitions and said so. Two steps of these want it and they want opposite things from it: step 5
   must not mistake an idle `about:blank` for a document mid-parse, and step 13 has to clear it and cannot. A
   realm whose Document has no navigable at all answers false, which is the same answer for both: there is no
   navigable holding an initial about:blank. */
static bool open_ever_navigated(JSContext *realm)
{
    JSValueConst proxy = document_window_proxy(realm);

    return window_proxy_is(proxy) && window_proxy_ever_navigated(proxy);
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
     * parks the running classic script in `currentScript`, and its §4.12.1 SCHEDULE says whether the parser is
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
              "do), and what is left is core/loader/document_load.c's: open the ACTIVE document's parse with "
              "html_parse_document_open instead of completing it with html_parse_document, and close it at "
              "§13.2.7 \"The end\"'s own moment — the lifecycle stage that moves the readiness to "
              "\"interactive\". Its hazard is html_parse.c's own: lexbor emits the EOF token in `chunk_end` "
              "and §13.2.6 builds html/head/body from it, so a document left open has a NULL documentElement "
              "until the close, and each reader that assumes otherwise is what must change. With that, a "
              "parse-time script runs with the insertion point DEFINED and never reaches §8.4.3 step 9 at all");
        /* STEP 5's OWN ACTION — "return document" — so that a release build, where the DFAIL above is compiled
           out, takes the standard's branch for the state it believes it is in rather than falling through into
           the destructive half of an algorithm the crash exists to keep it out of. */
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
    /* …AND THE FOUR POINTERS LEXBOR CACHES INTO THE TREE THAT JUST WENT, which are part of the same step and
       not bookkeeping after it. A Document node's children ARE its doctype and its document element, and
       lexbor additionally caches `head` and `body`; every one of them now names a node that is no longer in
       the tree. Two consumers read them and both would be wrong in a way that does not look like a stale
       pointer: §13.2.6.4.7 'The "in body" insertion mode' re-attributes a written `<body>` token's attributes
       onto `document->body`, so the next write's attributes would land on the REMOVED body; and
       core/dom/document.h's document_root_node hands out `dom_document.element` as the root a whole-tree walk
       starts from. The re-parse refills all four (§13.2.6.4.2 'The "before html" insertion mode' calls
       `lxb_dom_document_attach_element`), so this is the window between step 11 and the first token — which is
       exactly where a `document.open()` with no write yet leaves the page. Through the POD latch for the
       reason step 15 is: they are fields of the shared Lexbor document. */
    cow_capture_host_state(ctx, doc_obj, &dom->element, sizeof dom->element);
    cow_capture_host_state(ctx, doc_obj, &dom->doctype, sizeof dom->doctype);
    lxb_dom_document_attach_element(dom, NULL);
    lxb_dom_document_attach_doctype(dom, NULL);
    {
        lxb_html_document_t *html_doc = lxb_html_interface_document(dom);

        cow_capture_host_state(ctx, doc_obj, &html_doc->head, sizeof html_doc->head);
        cow_capture_host_state(ctx, doc_obj, &html_doc->body, sizeof html_doc->body);
        html_doc->head = NULL;
        html_doc->body = NULL;
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

    /* STEP 13 — "Set document's is initial about:blank to false." */
    if (realm != NULL) {
        DCHECK(open_ever_navigated(realm),
               "§8.4.1 step 13 — the document open steps ran on a navigable that still holds its INITIAL "
               "about:blank, and this engine cannot clear that flag: core/frame/navigable.c owns §3.1.1 \"The "
               "Document object\"'s \"is initial about:blank\" and window_proxy_ever_navigated is a getter "
               "with no setter beside it. "
               "Leaving it set makes §7.2.4's location navigate choose \"replace\" for ever after — a page that "
               "opens a window, writes into it and then navigates it loses the history entry the navigation "
               "should have pushed. The setter belongs beside window_proxy_navigate, which is the one site "
               "that clears it today");
    }

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
    open_record_write(ctx, doc_obj, true);

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
       STEP 5 — "if this's pending parsing-blocking script is not null, then return" — is DECIDED: a pending
       parsing-blocking script is set by §13.2.6.4.4's `script` handling of a PARSER-INSERTED external script,
       and this engine's tree construction prepares no script at all (§8.4.3's own definition permits it:
       "User agents are explicitly allowed to avoid executing script elements inserted via this method"). It
       becomes a real read in the diff that gives a written `script` its §4.12.1 preparation. */
    st = html_parse_document_close(lxb_html_interface_document(dom));
    CHECK(st == LXB_STATUS_OK,
          "§8.4.2 step 6's tokenizer run did not complete — the document is left with an input stream that is "
          "neither open nor closed, and every later `document.write` would insert into a parser that will "
          "never emit its EOF");
    open_record_write(ctx, doc_obj, false);
}
