/* THE `script` ELEMENT'S PARSE STATE AND HTML §4.12.1's "prepare the script element".
 *
 * WHY IT IS A COMPONENT AND NOT A STATIC IN element.c. §4.12.1 step 1 is "if el's already started is true,
 * then return", and `already started` is not a property of the element's markup — it is written by the PARSER
 * that built the element, read by the preparation that would run it, and copied by §4.12.1.1 "Processing
 * model"'s cloning steps.
 * Three call sites in three files over one boolean is a component; a static beside one of them is the same
 * boolean answered from wherever the reader happened to be.
 *
 * WHAT THE FLAG DECIDES, AND WHAT IT COST NOT TO HAVE IT. HTML §13.2.4.5 gives an HTML parser a SCRIPTING MODE
 * — one of Normal, Disabled, Inert, Fragment — and §13.2.6.4.4's `script` start tag says of the third: "if the
 * parser's scripting mode is Inert, then set the script element's already started to true (fragment case)".
 * §13.2.4.5 states what that buys in one line: "Inert: Scripts are enabled, however they are marked as already
 * started, essentially preventing them from executing. This is the default mode of the HTML fragment parsing
 * algorithm." So Inert is not a mode this engine chooses — §13.4 makes it the default of every fragment parse,
 * and all five of this engine's markup members (innerHTML, outerHTML, insertAdjacentHTML, setHTML,
 * setHTMLUnsafe) are §13.4 with no scriptingMode argument.
 * With no flag there was nothing for §4.12.1 step 1 to return on, and the fragment machine's placement puts
 * every parsed node through dom_cow_append_child, which runs §4.2.3's insertion steps, which prepare an
 * inserted `<script>`. So `el.innerHTML = "<script>…</script>"` EXECUTED that script and `<script src=…>`
 * RECORDED an endpoint no browser would ever fetch — a fidelity bug in both halves at once: the browser half
 * ran code a browser does not run, and the solver half reported a request the page cannot make. element.c said
 * in prose that "markup parsed into innerHTML does not execute its scripts" while its own code did.
 *
 * WHERE THE FLAG LIVES. On the element's WRAPPER, as an own slot under a Symbol this file minted and never
 * published — the same store DOM §4.9's custom element state uses (core/html/custom_elements.c), and for the
 * same two reasons: nothing outside can reach a key the page cannot mint, and the write is an ordinary
 * property write, so the heap COW captures it and one flow's marked script is not another flow's. ABSENT MEANS
 * FALSE *for `already started`*, which is §4.12.1's own initial value for that flag, so the reader never
 * allocates a wrapper to learn a default — an element the parser never marked is one nothing has written, and
 * node_wrap_peek answers for it without minting anything. The other flag's initial value is the other one, and
 * its reader answers absent accordingly; see below.
 *
 * `parser document` IS A SLOT NOW, AND IT IS A PARAMETER AS WELL — the two are not alternatives and the reason
 * is the one that used to argue against the slot. It said the fact has exactly two readers, the script HTML
 * element post-connection steps' step 1 ("if insertedNode is parser-inserted, then return") and §13.2.6.4.8's
 * `</script>` handling, and that each of them KNOWS the answer without asking because each IS one of the two
 * ways an element can get here — so a slot would be a fact written at one moment and read at another with no
 * moment in between that could change it. That argument was about the readers that existed. A THIRD reader
 * arrived and cannot ask its caller: Fetch §2.2.5's request PARSER METADATA, which HTML §4.12.1.1 computes
 * from this fact ("Let parser metadata be "parser-inserted" if el is parser-inserted, and "not-parser-inserted"
 * otherwise") and which CSP §6.7.1.1 step 1.3 then reads at the moment the request is CHECKED — which in this
 * engine is a park, arbitrarily later, with the preparation long off the stack; and a dynamic `import()` reads
 * it off the IMPORTING script's options (HTML §8.1.4.2's get-the-descendant-script-fetch-options step 1, "Let
 * newOptions be a copy of originalOptions", which copies parser metadata like every other member it does not
 * overwrite), which is an element from a different moment entirely. So the CALLER still states it — it is
 * either §13.2.6 tree construction, which is the thing that sets it, or page code, which cannot — and
 * `html_script_prepare` WRITES what it was told, so that a request built out of this element long afterwards
 * can be told the same thing. It was a hardcoded `false` while the parser half did not exist.
 *
 * ITS ABSENCE IS NOT ITS FALSE, WHICH IS WHY THE READER ANSWERS THREE THINGS AND NOT TWO. §4.12.1 makes a
 * `parser document` "initially null", so an element nothing has written is genuinely not parser-inserted for
 * the questions asked of the ELEMENT — but a REQUEST's parser metadata is set by §4.12.1.1, and an element
 * `html_script_prepare` never ran over has had that algorithm run over it never. Those two are different
 * claims and Fetch §2.2.5 spells them differently ("" against "not-parser-inserted"), so the reader below
 * reports UNSTATED rather than collapsing them; core/frame/policy_container.h says what each answers at the
 * check.
 *
 * `force async` IS HERE NOW, AND IT IS THE SECOND FLAG BECAUSE IT GAINED THE TWO READERS THE OTHER STILL LACKS.
 * §4.12.1.1: "A script element has a force async boolean, INITIALLY TRUE. It is set to false by the HTML parser
 * and the XML parser on script elements they insert, and when the element gets an async content attribute
 * added." Its readers are §4.12.1's destination branch — "if el has an async attribute or el's force async is
 * true", the test that puts an element in the `set of scripts that will execute as soon as possible` rather than
 * in the `list of scripts that will execute in order as soon as possible` — and the `async` IDL getter, whose
 * step 1 is "if this's force async is true, then return true". Without it `s = createElement('script'); s.async
 * = false; s.src = u` was an UNORDERED script: the setter is the whole of how a page asks for in-order lazy
 * loading, and it wrote nothing this engine read, so the two chunks a bundler emits in a fixed order ran in
 * whichever order the network answered. The flag's initial value is TRUE, so absence cannot mean false the way
 * `already started`'s does — an element nothing has written is one whose force async is true. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_SCRIPT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/report_exception.h"   /* §8.1.4.4 step 8's report — see "execute the script element" below */

/* IS THIS NODE A `script` ELEMENT? The INTERNED TAG ID and the pair of namespaces a `script` can be in —
   HTML's `script` and SVG's are both script elements, and lexbor's own `lxb_html_tree_node_is` answers only
   for the first because it hardcodes the HTML namespace. It is exported because §4.12.1 is asked of an
   element by more than this file — core/loader/data_block.c has to know it is looking at a `script` before
   the section's type-string steps mean anything, and a private copy of the test there would be two answers to
   one question, differing on SVG the first time either changed. */
bool html_script_is(const lxb_dom_node_t *n);

/* The slot key. Once per runtime, beside the other per-element slot declarations, and released with them —
   the key is a Symbol and its atom is an interned reference, so an agent torn down without this leaves both
   for the runtime's own leak walk to count. */
void html_script_init(JSContext *ctx);
void html_script_free(JSRuntime *rt);

/* WHAT A PARSER STAMPS ON THE `script` ELEMENTS IT INSERTS, applied to the tree the parse produced. Two stamps
   and they are not the same population, which is why one walk takes the mode rather than two walking the tree:
     ALWAYS — §4.12.1.1's `force async` is "set to false by the HTML parser and the XML parser on script
   elements they insert". Every parse, document and fragment alike, and it is what makes a parsed `<script>`
   with no `async` attribute answer `async` FALSE — without it the boolean's initial true would answer true for
   every element in the page's markup.
     `inert` — §13.2.6.4.4's `script` start tag under §13.2.4.5's INERT scripting mode, which §13.4 makes the
   default of every FRAGMENT parse: "set the script element's already started to true (fragment case)". A
   DOCUMENT parse's scripts run, so it passes false.
   IT RUNS AT THE PARSE BOUNDARY rather than at each start tag because the tree builder is lexbor's and not
   this engine's — the same boundary, and for the same reason, as dom_attr_normalize_parsed's namespace
   correction, which is the statement immediately before it. That substitution is unobservable and is so
   because of something this engine asserts elsewhere: a parse runs NO page code, so nothing can look at a
   `script` element between the start tag that created it and the end of the parse that produced it. */
void html_script_parsed(JSContext *ctx, lxb_dom_node_t *root, bool inert);

/* ---- HTML §4.12.1.1 "Processing model" STEP 36 — REPORTED BY `prepare`, PERFORMED BY A STEP MACHINE --------
 *
 * Step 36 is the LAST of "prepare the script element"'s thirty-six steps and it ends: "Otherwise, immediately
 * execute the script element el, EVEN IF OTHER SCRIPTS ARE ALREADY EXECUTING." It is the one destination
 * §4.12.1 has that is NOT A POSITION IN A SEQUENCE — the other four are a list, a set, a pending slot and the
 * parser's own resumption, and a row in the flow's program sequence expresses each of them exactly — because it
 * is a NESTED RUN inside the operation that reached these steps. Expressed as the nearest slot the sequence
 * has (the one after the running program) it puts the REST OF THE CAUSING PROGRAM IN FRONT of it, so
 * `body.appendChild(s); f()` ran `f()` before `s`'s code where a browser runs it after — a timeline no browser
 * produces, on every page that injects an inline script.
 *
 * SO `prepare` REPORTS IT AND NEVER PERFORMS IT, and that is a statement about where `prepare` is CALLED FROM
 * rather than a division of labour. Performing step 36 is running the page's code, and this algorithm is
 * reached from inside a DOM mutation, from inside an attribute change and from inside a parse — three C bodies
 * with no flow base under them, where a nested run is the drive-to-completion this engine aborts on. The party
 * that can perform it is the one standing on a STEP MACHINE, so the report travels OUT to that party and the
 * machine makes the request. A caller with no machine under it therefore cannot silently do the wrong thing:
 * it is holding a record it has to answer for.
 *
 * `text` IS STEP 5'S SOURCE TEXT, NUL-TERMINATED AND OWNED (`free`, or hand the record to
 * html_script_immediate_free). It is a COPY and not lexbor's own buffer, because lexbor's element form
 * allocates `length + 1` and its concatenating walk never writes the last byte, while every compiler entry in
 * this engine requires `input[input_len] == '\0'` — a program compiled off that buffer would end in whatever
 * the arena last held there.
 * `text_n` IS THE LENGTH AND NOT A `strlen`: this element's text is the one inline source that can hold a
 * U+0000, because it was ASSIGNED by page code (`s.textContent = …`) and so never went through HTML §13.2.5.4
 * "Script data state", whose U+0000 NULL row emits a U+FFFD REPLACEMENT CHARACTER. ECMAScript §11.1 "Source
 * Text" admits every code point from U+0000 up.
 * `el` IS THE ELEMENT "execute the script element" is given — its step 6 classic arm sets this document's
 * §3.1.7 `currentScript` to it for the run.
 * AN EMPTY RECORD IS A POSITIVE STATEMENT AND IS THE COMMON ONE: every other destination has taken the element
 * by the time `prepare` returns, so `text == NULL` says "this preparation owes no nested run", never "the
 * report was not filled in". `text` and `el` are null together and there is no third state. */
typedef struct {
    char              *text;
    size_t             text_n;
    lxb_dom_element_t *el;
} ScriptImmediate;

/* Release what a report owns and leave it empty. Idempotent, so a caller that has already handed the text to
   the execution below may still call it. */
void html_script_immediate_free(ScriptImmediate *imm);

/* ---- §4.12.1.1's "execute the script element", DRIVEN --------------------------------------------------------
 *
 * The algorithm is eight steps and step 6 is a switch on el's type; its CLASSIC arm is four:
 *
 *     Let oldCurrentScript be the value to which document's currentScript object was most recently set.
 *     If el's root is not a shadow root, then set document's currentScript attribute to el. Otherwise, set it
 *       to null.
 *     Run the classic script given by el's result.
 *     Set document's currentScript attribute to oldCurrentScript.
 *
 * and "run the classic script" is HTML §8.1.4.4 "Calling scripts"'s eleven-step algorithm, whose step 8 —
 * reached with `rethrow errors` false, which is what §4.12.1.1 invokes it with — REPORTS an abrupt completion
 * rather than propagating it. That is why the state below holds a ReportExceptionWork: a nested program's throw
 * is not the mutating member's completion and must never become one, or `body.appendChild(s)` would throw
 * whatever the injected script threw AND leave §3.1.7's slot pointing at the element for the rest of the
 * session.
 *
 * IT IS A REQUEST AND THE PROGRAM IS A FIRST-CLASS FLOW. The compile asks for a TRAMPOLINABLE CLOSURE
 * (JS_EVAL_FLAG_TRAMP_CLOSURE) and the run is `step_call_run`, so the nested program's body executes on the
 * calling flow's own trampoline chain: preemptible per opcode, parkable at any loop back-edge or `await`,
 * riding the flow's COW delta, and forkable at a concolic branch inside it. A `JS_Call` from here would be the
 * drive-to-completion the engine aborts on, and `JS_FlowNew` would be a SECOND program frame on a flow that
 * already holds one.
 *
 * `oldCurrentScript` IS CARRIED AND NOT ASSERTED NULL. document_current_script.h used to prove it null from "a
 * flow holds at most one live program frame", and step 36 is exactly the case that falsifies it: the causing
 * program's frame is live, and its own element is in the slot, while the nested one runs.
 *
 * THE STATE IS THE DRIVING MACHINE'S, exactly as focus.h's `phase`/`cb` and abort.h's work record are: the
 * machine declares it in its own `visit` so a fork copies it, and calls the release below for the half no
 * declaration can carry (§8.1.4.6 step 6.1's error-reporting-mode flag). */
#define SCRIPT_EXEC_CB_SLOTS (2 + 0)   /* step_call_run's [this, program] — a program takes no arguments */

typedef struct {
    /* The report, ADOPTED. `text` is consumed by the compile in the FIRST leg and is NULL at every rest point
       this state has — which is why no visit operation names it: a plain C allocation crossing a fork would be
       one buffer named by two arms and the second free would be a double free. `el` OUTLIVES the compile
       because step 6's bracket needs it at both ends, and it is a bare node pointer named by the flow's own
       tree through the DOM COW delta, exactly as the walk's staticNodeList entries are. */
    char               *text;
    size_t              text_n;
    lxb_dom_element_t  *el;
    uint8_t             stage;   /* SX_* in html_script.c — which step of "execute the script element" */
    uint8_t             phase;   /* step_call_run's own cursor */
    JSValue             cb[SCRIPT_EXEC_CB_SLOTS];
    JSValue             old;     /* step 6's oldCurrentScript, held across the run */
    JSValue             exc;     /* §8.1.4.4 step 8's evaluationStatus.[[Value]], held across the report */
    ReportExceptionWork rx;      /* §8.1.4.6, which fires `error` at the global and so parks on the page's code */
} ScriptExec;

/* An idle state: nothing owed, nothing owned. Called where the driving machine's buffer is built. */
void html_script_exec_init(ScriptExec *x);
/* Is a nested run in flight — i.e. must the driving machine keep stepping this before it moves on? */
bool html_script_exec_owed(const ScriptExec *x);
/* Adopt a non-empty report. `imm` is MOVED (left empty); the state is asserted idle, because two nested runs on
   one machine at one time is one of them being dropped. */
void html_script_exec_begin(ScriptExec *x, ScriptImmediate *imm);
/* The ONE list of what this state owns, forwarded from the driving machine's own `visit`. */
void html_script_exec_visit(JSContext *ctx, ScriptExec *x, JSStepVisit *v);
/* The half the list cannot carry: §8.1.4.6 step 6.1's flag, given back if the run was abandoned holding it.
   Frees nothing the visit names — see quickjs-step.h on why that split is folded to a number and checked. */
void html_script_exec_release(JSContext *ctx, ScriptExec *x);
/* THE ALGORITHM, DRIVEN. `ctx` is `el`'s node document's realm — §4.12.1.1 step 32's settings object, step 34's
   base URL and the global the program closes over, and never whichever realm performed the mutation.
     JS_STEP_CALL = return it (the run has parked on the page's code, or on the `error` listeners of the report
   its throw owes), 0 = "execute the script element" has finished. */
int html_script_exec_run(JSContext *ctx, ScriptExec *x, JSValue in, JSValue **out_cb, int *out_argc);

/* HTML §4.12.1 "The script element"'s "prepare the script element", reached from DOM §4.2.3's insertion steps
   and from §4.12.1.1's post-connection and attribute change steps. `el` is any inserted element; one that is
   not a `script` returns having done nothing, because the caller is a walk over every node of an inserted
   subtree and the tag test is that walk's filter rather than a step of the algorithm.
   `parser_inserted` IS STEP 2's `parser document` BEING NON-NULL, stated by the CALLER, and RECORDED on the
   element here (see above): the caller is the only party that can answer — it is either §13.2.6 tree
   construction, which is the thing that sets it, or page code, which cannot — and the recording is what lets a
   request this element causes be judged after the caller's frame is gone. It decides steps 4 and 14's `force
   async` round trip and, through it, which of §4.12.1's five destinations the element takes — so it was not a
   formality while it was hardcoded false, it was a parsed `<script src>` being filed in a list it does not
   belong to.
   `imm` IS STEP 36'S ANSWER AND IT IS MANDATORY — see the record below. It is written on EVERY path and is
   empty on all but one, so no caller can forget to look and no caller can decide for itself what to do with
   the one destination §4.12.1.1 has that is not a position in a sequence. */
void html_script_prepare(JSContext *ctx, lxb_dom_element_t *el, bool parser_inserted, ScriptImmediate *imm);

/* HTML §4.12.1.1 "Processing model"'s PARSER METADATA FOR THIS ELEMENT — "Let parser metadata be
 * "parser-inserted" if el is parser-inserted, and "not-parser-inserted" otherwise" — as the request field
 * Fetch §2.2.5 "Requests" makes it, read back from what `html_script_prepare` was told.
 *
 * THREE ANSWERS, BECAUSE UNSTATED IS NOT NOT-PARSER-INSERTED. §4.12.1 makes `parser document` "initially
 * null", so an element nothing marked answers §4.12.1's own ELEMENT question "not parser-inserted"; but the
 * REQUEST field is a thing §4.12.1.1 sets, and an element that algorithm never ran over has no answer of its
 * own to report. §2.2.5's empty string is exactly that state, and the caller is what turns UNSTATED into it —
 * see core/frame/policy_container.h's CspParserMetadata, which spells the same three.
 *
 * NO REALM PARAMETER, for `html_script_parser_inserted`'s reason: the slot is an own property of the element's
 * wrapper and any realm that can reach the node will do, so this asks the element's node document rather than
 * making every caller carry one. A node no realm has ever reached has no wrapper and therefore no slot, which
 * is UNSTATED and not a branch of its own.
 * `el` may be NULL — a program no `<script>` produced (a lazy chunk, an @S candidate, a `javascript:` URL) is
 * a real caller and its answer is UNSTATED, which is why no assert stands here. */
typedef enum {
    HTML_SCRIPT_PARSER_METADATA_UNSTATED = 0,
    HTML_SCRIPT_PARSER_METADATA_PARSER_INSERTED,
    HTML_SCRIPT_PARSER_METADATA_NOT_PARSER_INSERTED,
} HtmlScriptParserMetadata;
HtmlScriptParserMetadata html_script_parser_metadata(const lxb_dom_element_t *el);

/* A PARSER REACHED A `script` ELEMENT'S END TAG — the one action TWO sections state, which is why this is one
 * body with two callers rather than two bodies that must not disagree.
 *   HTML §13.2.6.4.8 'The "text" insertion mode' — "An end tag whose tag name is `script`": let script be the
 * current node, pop it, restore the insertion mode and the insertion point, and "prepare the script element
 * script". `script` is that node, taken by core/html/html_parse.c's token-done wrapper BEFORE tree construction
 * consumes the token, because the pop is what the section performs first.
 *   HTML §14.2 "Parsing XML documents" — "When the element's end tag is subsequently parsed, the user agent
 * must perform a microtask checkpoint, and then prepare the script element", for a parser invoked with XML
 * scripting support enabled. core/loader/xml_document.c is that caller and owns the checkpoint, the scripting
 * mode and the end-tag boundary; what it must NOT own is a second preparation, because §4.12.1's type steps,
 * its `already started` and its five destinations would then be right in one file and drifting in the other.
 * The XML side is not a copy of the HTML side in ANY other respect — there is no raw-text tokenizer state in
 * XML, so a `script` body is ordinary XML §3.1's [43] `content` — and this is the one thing the two share.
 *
 * WHY THIS EXISTS AT ALL, GIVEN THAT §8.4.3 EXPLICITLY PERMITS THE OPPOSITE. "User agents are explicitly
 * allowed to avoid executing script elements inserted via this method" is a real permission in §8.4.3
 * "document.write()", so a written `<script>` that never ran earned no crash, failed no conformance test and
 * showed up in no column — which is exactly why the loss was invisible. CLAUDE.md's §Boot names the case by
 * name: "Code-loading async ALWAYS executes (`await import(x)`, a chunk `fetch().then(eval)`, a
 * `document.write`'d `<script>`)". A written script is CONDITIONALLY-LOADED JS in its purest form — code that
 * exists only if a branch reached the write — and the solver half is the half that must not decline it.
 *
 * AN INERT PARSE NEVER REACHES IT, and the test is at the CALL rather than in here — see the paragraph at
 * core/html/html_parse.c's token-done wrapper for why §13.2.4.5's Inert mode is answered by whether the parse
 * is a §13.4 fragment parse, and why `html_script_parsed` stays the ONE writer of the flag those scripts get.
 *
 * THERE IS NO REALM PARAMETER BECAUSE §4.12.1 DERIVES IT: step 32 is "let settings object be el's node
 * document's relevant settings object" and step 34's base URL is that document's. §13.2.6 runs inside the
 * vendored parser and there is no realm on that path to hand over, so asking the element's document is not the
 * loose way to get this — it is the only place the standard says the answer is.
 *
 * THE SCRIPTING-DISABLED QUESTION IS NOT ASKED HERE, AND THAT IS AN ORDER FACT RATHER THAN A DELEGATION.
 * A Document that is no navigable's ACTIVE document has no browsing context, which is §8.1.3.4 "Enabling and
 * disabling scripting"'s own condition — "Scripting is disabled for a platform object object if … the object
 * implements Node, and object's node document's browsing context is null" — and §4.12.1.1 step 18 returns for
 * exactly that. But step 18 is step EIGHTEEN: steps 1 through 17 run first, and step 15 is "Set el's already
 * started to true". This entry asked step 18 at its door, so those fourteen steps were skipped for every
 * document with no browsing context and the flag was never set — which is what lets a `<script>` be parsed in
 * a scripting-disabled document and RUN when it is moved into a live one, the launder that §4.12.1.1's own
 * ordering forbids. §13.2.6.4.8 'The "text" insertion mode' states this door's step with no scripting
 * condition on it at all ("If the active speculative HTML parser is null, then prepare the script element
 * script"), so the door prepares and `html_script_prepare` reaches step 18 in its place.
 *
 * NAMED RESIDUAL. WHAT IS NOT COVERED: a Document this engine LOADS is parsed BEFORE it is given its realm
 * (core/dom/element.c's tree_steps_can_run states the same inversion for the insertion steps), so at the
 * moment this door runs there is no realm to write the flag through at all and it returns above — a loaded
 * document's markup `<script>` elements therefore still leave the parse with `already started` FALSE, and
 * core/loader/document_scripts.c's inventory is what runs them. WHAT THE NEXT DIFF BUILDS: HTML §7.5.1
 * "Shared document creation infrastructure" step 9's Document, and step 7.5's realm and Window, existing
 * BEFORE §7.5.2 "Loading HTML documents" step 3 creates the parser and associates it — after which this door
 * has a realm, step 15 fires for the markup's own scripts, step 18 answers ENABLED, and the destinations
 * below become the load's script runner, at which point that inventory has no caller left. HOW ITS ABSENCE
 * SHOWS: any second reach at a loaded document's own markup `<script>` re-prepares and re-runs it, because
 * step 1 has nothing to read — a connected insertion before a reference child re-runs every script after it,
 * and a `<script>` a page moves within its own document runs twice. */
void html_script_parser_inserted(lxb_dom_node_t *script);

/* HTML §13.2.6.4.8 'The "text" insertion mode' — "An end-of-file token: … If the current node is a script
   element, then set its already started to true." A `<script>` the input stream ended inside runs nothing, in
   this parse or in any later reach: the flag is what says so, and §4.12.1 step 1 is what reads it.
   IT IS SEPARATE FROM THE END-TAG ENTRY BECAUSE IT IS A DIFFERENT TOKEN'S STEP with a different outcome — that
   one prepares, this one refuses to — and a shared body with a "do not prepare" flag would be one function
   answering for two rows of one dispatch table. The realm is derived the same way and for the same reason. */
void html_script_end_of_file(lxb_dom_node_t *script);

/* HTML §4.12.1.1 "Processing model"'s CLONING STEPS — "the cloning steps for script elements given node, copy,
   and subtree are to set copy's already started to node's already started" — run from DOM §4.4 clone a
   node's step 3, which is where every one of a clone's nodes passes.
   THEY ARE NOT BOOKKEEPING. Without them `parent.innerHTML = "<script>…</script>"` produces an inert script
   whose CLONE is a live one, so `doc.body.appendChild(parent.firstChild.cloneNode(true))` runs exactly the
   code the Inert mode exists to stop. A flag is only worth writing if it survives the operations the standard
   says it survives. `src` and `copy` may be any node kind; a pair that is not two `script` elements is a
   no-op, for the same reason the preparation above tolerates one. */
void html_script_cloned(JSContext *ctx, lxb_dom_node_t *src, lxb_dom_node_t *copy);

/* HTML §4.12.1's `async` IDL attribute — the getter that reads `force async` and the setter that CLEARS it.
   It is not a [Reflect]ed boolean and was declared as one, which got both directions wrong at once: the getter
   answered the attribute's presence where the spec answers `force async || attribute present`, so a freshly
   created element read `false` while its force async was true; and the setter merely removed an absent
   attribute, so `s.async = false` — the one line whose entire purpose is to move the element into the ordered
   ASAP list — changed nothing at all. Handed the prototype by core/html/html_element.c for the same reason
   §4.2.6's `disabled` is: that file owns the table of which interface a tag wears, this one owns the state the
   member answers from. */
void html_script_install(JSContext *ctx, JSValueConst proto);

/* §4.12.1's "when an async attribute is added to a script element el, the user agent must set el's force async
   to false", as one of §4.9's ATTRIBUTE CHANGE STEPS — registered on core/dom/element.c's element_attr_changed
   beside media_element_attr_changed, and there rather than in the `async` setter for the same reason: `s.async =
   true`, `s.setAttribute('async','')` and `s.attributes.async.value = ''` are one write of one attribute, and a
   setter-side call answers for the first spelling only. `val` is the operation's input, because "ADDED" is a
   rule about the change: removing the attribute does not set the flag back. */
void html_script_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                              const char *val);

/* HTML §4.12.1.1 "Processing model", the src step: "Let url be the result of encoding-parsing a URL given src,
 * RELATIVE TO EL'S NODE DOCUMENT" — §2.4.2 "Parsing URLs"'s encoding-parse a URL takes a Document, and the
 * base it resolves against is then §2.4.3 "Document base URLs"'s document base URL, which for an element in a
 * child navigable's document is THAT document's and not the creator's. It is NOT §8.1.3.2 "Environment
 * settings objects"'s API base URL: that is a settings object's answer, and this step names a Document.
 * `ctx` is the node document's realm.
 *
 * NULL IS THE STANDARD'S OWN NEXT STEP and not an error code: "if url is failure, then queue an element task on
 * the DOM manipulation task source given el to fire an event named error at el, and RETURN" — so the element
 * runs no script AND the page is told. The second half is html_script_queue_error below, and a caller that
 * takes the NULL arm without it performs half of one step: the element goes quiet exactly as it should and the
 * `onerror` a bundle hangs its fallback host off never runs. A non-NULL answer is malloc'd and OWNED BY THE
 * CALLER.
 *
 * ONE COPY, HERE, because there are FOUR ways a `<script src>` reaches a loader in this engine — an element a
 * SCRIPT inserted (this file's html_script_prepare), a child navigable's parsed Document
 * (core/frame/navigable.c), a document joined to this agent (solver/engine.c's engine_join_document) and the
 * SESSION document's own inventory (solver/engine.c's engine_sched_begin, read by both the flow's fetch park
 * and the module compile's map key). Three of them had grown their own identical private copy of these lines,
 * which is three places for a step the standard states once to drift; the fourth had no copy at all, and that
 * is the defect this became a component to fix — the session document's sequence carried `src` as the RAW
 * ATTRIBUTE, so a page's own external module was named by its document and every real site shipping
 * `<script type=module src>` was unanalysable. */
char *script_src_absolute(JSContext *ctx, const char *src, size_t src_len);

/* HTML §4.12.1.1 "Processing model"'s ERROR ARM — "queue an element task on the DOM manipulation task source
 * given el to fire an event named error at el". The standard writes that sentence three times inside its `src`
 * branch (a `src` on an `importmap`/`speculationrules` element, a `src` that is the empty string, a `url` that
 * is failure).
 *
 * IT IS NOT THE FORM STEP 4 USES, AND THIS SENTENCE USED TO SAY IT WAS — a wrong claim about the standard,
 * sitting exactly where a reader would go to decide how a FAILED FETCH reports. "Execute the script element"
 * step 4 is a BARE fire: "If el's result is null, then fire an event named error at el, and return" — no task,
 * because `onComplete` already runs from the networking task that completed the fetch. The chain that reaches
 * it is real and is the only part that survives: §4.12.1.1's fetch `onComplete` is "Mark as ready el given
 * result", one of the five possibilities §4.12.1.1 lists for that `result` is "null (representing an error)",
 * and a load that did not happen marks it that.
 *
 * SO THIS IS STILL THE ARM A REFUSED OR FAILED LOAD OWES, and it is exported for that reason rather than kept
 * static — but it is this ENGINE's expression of step 4 and not a quotation of it, which is a difference the
 * caller states at its own site rather than one this file may hide. A bare fire is impossible from a C seam
 * here: the listener list is the PAGE's, so the dispatch runs the page's code and must have a flow base under
 * it, and a queued task is how a C seam gets one. The cost is one task hop of ORDER; solver/engine.c's
 * sequence arm carries the residual that names it and what would remove it.
 * The three sites above are `prepare`'s own and are called from this file; the fourth is a reply that never
 * came — a network error, or the trusted zone declining to make the request at all — and the party holding
 * that fact is the flow's pending register, not this file. What such a delivery must NOT do is settle the park
 * as if bytes had arrived: a `<script src>` and an injected script are owed a PROGRAM, and there is no value
 * that means "no program" which the compile path can be handed. It has an element (the park carries it) and it
 * has this.
 *
 * IT IS NOT THE ARM A DYNAMIC `import()` OWES, WHICH IS A DIFFERENT ALGORITHM AND MUST NOT BE ROUTED HERE.
 * `import()` has no element to fire at: HTML §8.1.6.7.3 "HostLoadImportedModule(referrer, moduleRequest,
 * loadState, payload)" ends in "Fetch a single imported module script given url, fetchClient, destination,
 * fetchOptions, settingsObject, fetchReferrer, moduleRequest, and onSingleFetchComplete as defined below",
 * whose first two steps are "Let completion be null." and "If moduleScript is null, then set completion to
 * ThrowCompletion(a new TypeError)." — the two sentences this paragraph used to quote with their initial
 * letters lowered, which is the cheapest tell that a quotation was written from memory rather than pasted.
 * The completion goes to ECMAScript §16.2.1.11 "FinishLoadingImportedModule ( referrer, moduleRequest,
 * payload, result )", whose dynamic-import arm is §13.3.10.3 "ContinueDynamicImport ( promiseCapability,
 * moduleCompletion )": "If moduleCompletion is an abrupt completion", "Perform ! Call(promiseCapability.
 * [[Reject]], undefined, « moduleCompletion.[[Value]] »)". An engine that answered a failed chunk load with
 * this arm would fire an `error` event at nothing and leave the importing flow parked for ever. */
void html_script_queue_error(JSContext *ctx, lxb_dom_element_t *el);

#endif
