/* HTML §3.1.7 "DOM tree accessors" — `document.currentScript` — AND THE BRACKET IN §4.12.1.1 THAT WRITES IT.
 *
 * §3.1.1 "The Document object" declares it:
 *
 *     readonly attribute HTMLOrSVGScriptElement? currentScript; // classic scripts in a document tree only
 *
 * and §3.1.7 states the whole of the getter: "The currentScript attribute, on getting, must return the value to
 * which it was most recently set. When the Document is created, the currentScript must be initialized to null."
 * So the member is a SLOT, and everything interesting about it is who writes it and when.
 *
 * THE WRITER IS §4.12.1.1 "Processing model"'s "execute the script element", whose "classic" arm is four steps:
 *
 *     Let oldCurrentScript be the value to which document's currentScript object was most recently set.
 *     If el's root is not a shadow root, then set document's currentScript attribute to el. Otherwise, set it
 *       to null.
 *     Run the classic script given by el's result.
 *     Set document's currentScript attribute to oldCurrentScript.
 *
 * and whose "module" arm opens with the standard's own assertion — "Assert: document's currentScript attribute
 * is null" — which is BEHAVIOUR here and not an exemption: it is a DCHECK at the module compile.
 *
 * WHY THIS IS NOT A C SAVE/RESTORE BRACKET, WHICH IS THE ONLY HARD THING ABOUT THE MEMBER. "Run the classic
 * script" is not a call in this engine, it is a WORK ITEM: the classic arm's third step is `JS_FlowNew`, and the
 * program then runs across an unbounded number of scheduler steps, parking and resuming and being outranked by
 * siblings in between. A C bracket around that call would set the slot for whichever flow happened to be running
 * when the next flow's script started, and it would appear to work in any fixture that runs one script.
 * CLAUDE.md §scheduler states the rule this is an instance of: an operation that becomes a work item takes its
 * inputs with it, and anything read back off the object it acts on is read at the wrong TIME.
 *
 * SO THE SLOT IS PER-FLOW STATE THAT TIME-TRAVELS, and it is held the way §3.1.5's readiness is (core/dom/
 * document.c): in this realm's own BASELINE RECORD, an object built with the realm and unreachable from the
 * page, whose one field is an ORDINARY PROPERTY WRITE. That is the whole mechanism — the heap COW delta already
 * captures a property write on a baseline object, so:
 *
 *   - two script flows interleaving over one document each read their own `currentScript`, because the write
 *     lives in the writing flow's delta and a context switch unapplies it;
 *   - a flow PARKED mid-script and resumed — hot or from the cold tier — comes back to the value it set,
 *     byte-identical, because the delta is what the snapshot is made of;
 *   - a flow that FINISHES or is released takes its write with it, restoring the baseline null.
 *
 * IT IS DELIBERATELY *NOT* `cow_capture_host_record`. That primitive exists for state kept behind a CLASS
 * OPAQUE, where no property hook and no engine hook can see the write, and it costs an owned-JSValue layout
 * that the finalizer and the gc_mark must iterate in lockstep — a list this member would get wrong exactly
 * once, and `currentScript` is a live element reference, so getting it wrong is a UAF or a leak. A property on
 * a baseline object needs none of that: the capture is the one the delta already performs, and there is no
 * second statement of what the record owns to fall out of step with the first.
 *
 * WHAT `oldCurrentScript` IS HERE. A flow holds at most ONE live program frame (solver/flow.h's `frame`, and
 * solver/engine.c compiles only under `!f->frame`), so §4.12.1.1's classic arm can never be re-entered on one
 * flow's timeline: the restore of the previous script has already run by the time the next one starts. That
 * makes oldCurrentScript provably null rather than a value to carry across the work item — and it is ASSERTED
 * rather than assumed, at the set, because a non-null value there is precisely the symptom of the C-bracket bug
 * this file exists to not have: either a restore was skipped, or two flows' writes have leaked across the delta.
 *
 * A DOCUMENT WITH NO BROWSING CONTEXT ANSWERS NULL, and that is §3.1.7's initialization value rather than a
 * fallback: `createHTMLDocument`, a DOMParser parse and XHR's `responseXML` each build a Document that never
 * executes a script element, so nothing ever sets theirs. The getter asks core/dom/document.h's
 * `document_active_realm_of` exactly as `readyState` does, for the same reason and with the same answer shape. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_CURRENT_SCRIPT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_CURRENT_SCRIPT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT, from document_init — the realm-value slot this member lives in. */
void document_current_script_init(JSContext *ctx);
/* Installed PER REALM, from document_install_proto: §3.1.1's accessor on Document.prototype, and this realm's
   BASELINE record holding null. Built with the realm and never on first touch, so the record every flow forks
   from belongs to the baseline rather than to whichever flow read it first. */
void document_current_script_install(JSContext *ctx, JSValueConst proto);
/* Given back by document_agent_free, in the reverse of the declaration order. */
void document_current_script_free(void);

/* §4.12.1.1's classic arm, steps 1-2: assert oldCurrentScript is null (see the header comment) and set the slot
   to `el`, or to null when el's root IS a shadow root. `ctx` is the realm of the document whose program is
   about to run — the document §4.12.1.1 calls `el's node document`, which is the realm the program compiles in.
   Called by solver/engine.c at the one place a classic script's frame is created. */
void document_current_script_set(JSContext *ctx, lxb_dom_element_t *el);
/* …and step 4, "Set document's currentScript attribute to oldCurrentScript", which is null here. `el` is the
   element step 2 was given, and it is passed back so the restore can ASSERT that what it is clearing is what
   this program set: anything else is another flow's element in this flow's slot. */
void document_current_script_restore(JSContext *ctx, lxb_dom_element_t *el);
/* Is this realm's slot null? The standard's own assertion in §4.12.1.1's MODULE arm, asked where that arm runs. */
bool document_current_script_is_null(JSContext *ctx);
/* Is the classic script this realm is executing one whose source came FROM AN EXTERNAL FILE — §4.12.1.1's own
   condition for raising §8.4.3's ignore-destructive-writes counter, asked of the element this slot holds. Its
   caller is core/html/document_write.c's step 9.1; the body states which half of §4.12.1.1's condition it
   answers and which half needs the counter's real producer. */
bool document_current_script_is_from_external_file(JSContext *ctx);
/* Is the classic script this realm is executing one the PARSER is standing inside — HTML §8.4.1 "Opening the
   input stream" step 5's "an active parser whose script nesting level is greater than 0", answered from the
   element's §4.12.1 SCHEDULE. Its caller is core/html/document_open.c; the body states why the schedule is the
   question and the document's readiness is not. */
bool document_current_script_is_parser_executed(JSContext *ctx);

#endif
