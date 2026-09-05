/* HTML §6.6.7 — THE AUTOFOCUS ATTRIBUTE. See autofocus.c.
 *
 * §6.6.7 is a Document's own state — an AUTOFOCUS CANDIDATES list and an AUTOFOCUS PROCESSED FLAG — and the two
 * algorithms stated over it: the insertion steps that fill the list, and FLUSH AUTOFOCUS CANDIDATES, which
 * HTML §8.1.7.3 update the rendering step 7 runs once per rendering opportunity. It is not part of core/html/
 * focus.c because it is not the focus model: it is a queue that DECIDES, once per document load, which of the
 * elements the markup marked should be handed to §6.6.4's focusing steps. Every decision it makes is one of
 * §6.6's own algorithms, reached through the four entry points focus.h declares. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_AUTOFOCUS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_AUTOFOCUS_H
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* THE AGENT'S HALF: the realm slot §6.6.7's two pieces of state live in. Reached from document_init, beside
   §6.6's focused area and for the reason stated there — the list belongs to a DOCUMENT, and document.c is the
   one component every host that has a Document goes through, so pairing the declaration with the per-realm
   install is what keeps it off each host's hand-copied init list. */
void autofocus_init(JSContext *ctx);
/* Reached from document_agent_free — §6.6.7 is declared by document_init, so it is released by its declarer. */
void autofocus_free(void);

/* THIS REALM'S candidates list and processed flag, built WITH the realm so they belong to the pre-boot
   BASELINE — a record made on first touch would be made inside whichever flow happened to insert first, and
   would then be that flow's list rather than the one every flow forks from. */
void autofocus_install_document(JSContext *ctx);

/* §6.6.7's INSERTION STEPS, "when an element with the autofocus attribute specified is inserted into a
   document". `el` is that element; the algorithm's own step 1 tests the attribute, so a caller hands over every
   inserted element rather than filtering first. Reached from DOM §4.2.3's insertion steps in
   core/dom/element.c, which is where every scripted insertion converges.
   THEY ARE A REQUEST, because their step 5 runs §6.6.6's ALLOW FOCUS STEPS, whose second clause is §6.4.1's
   transient activation — unknown external state, so the answer FORKS. `h` is the driving step machine's header
   and `ua_phase` a byte that machine owns on its own state, exactly as user_activation.h's questions take one;
   both belong to the CALLER because the fork's snapshot is taken there and the re-entry comes back to this
   call site. Returns JS_STEP_FORK (the caller returns it) or 0 when the steps have finished. */
int autofocus_element_inserted(lxb_dom_element_t *el, JSStepHdr *h, uint8_t *ua_phase);

/* THE SAME STEPS FOR THE ELEMENTS THE PARSER INSERTED. A browser runs the insertion steps during tree
   construction, so `<input autofocus>` in the page's own markup is a candidate before the first script runs;
   no document load in this engine reaches those steps (HTML §7.5.2's Lexbor parse never reaches the DOM
   chokepoint; §7.5.3's XML parse reaches it and is refused at the record by core/dom/element.c's
   tree_steps_can_run, the realm not existing until after the parse), so the parsed tree gets its steps here,
   once, when the document is installed — exactly as §4.8.5's child navigables do.
   It is NOT a request: document install runs at the pre-boot COW baseline, where there is no flow to snapshot,
   so it hands the steps no header and the allow focus steps assert against the one document whose parsed tree
   would need the fork (a cross-origin-embedded one) rather than picking an arm for it. */
void autofocus_document_parsed(JSContext *ctx);

/* THE LAST FOUR STEPS OF HTML §6.12 The popover attribute's POPOVER FOCUSING STEPS AND OF HTML §4.11.4 The
 * dialog element's DIALOG FOCUSING STEPS, which are the same four in both — steps 7-10 of each: resolve
 * `control`'s node navigable's top-level traversable's active document, return if `control`'s node document is
 * not same origin with it, then EMPTY that document's autofocus candidates and set its autofocus processed
 * flag. Both algorithms are ten steps and both end here.
 * It is §6.6.7's state, so it is this component's to write, and it is ONE door rather than two because the
 * list and the flag are written together (see af_record's note in autofocus.c).
 * `ctlctx` is the CONTROL's node document's realm — steps 7 and 8 name the control and not the subject.
 * It runs NO page code and asks nothing unknown, so it is a plain call and not a request. */
void autofocus_focusing_steps_tail(JSContext *ctlctx);

/* HTML §8.1.7.3 update the rendering STEP 7 — "For each doc of docs, flush autofocus candidates for doc if its
 * node navigable is a top-level traversable."
 *
 * IT IS A REQUEST, because §6.6.7's step 5.11.3 runs §6.6.4's focusing steps, which fire `blur`, `focusout`,
 * `focus` and `focusin` at the page's own listeners: the calling machine parks on it and resumes with the
 * algorithm finished, exactly as it parks on a fire. `phase` and `cb` belong to the CALLING machine — it visits
 * them, so a fork copies them and a suspension inside a `focus` listener resumes in the same stage — and `cb` is
 * passed through STEP_CB so its capacity travels with it.
 *
 * IT IS ALSO WHERE THE WALK OF THE LIST LIVES, and that is the point of making it a machine of its own rather
 * than a loop the rendering stage runs: §6.6.7's step 5 is a `while` over the candidates that can remove several
 * before it reaches one worth focusing, so the walk must be resumable MID-LIST. It is, and with no cursor to
 * park: every iteration of step 5 either removes candidates[0] or returns, so the list IS the cursor and a
 * resume simply looks at its head again. `docctx` is the realm whose active document is topDocument.
 *   JS_STEP_CALL = return it, 0 = the flush has finished. */
/* step_call_run's operand shape is [this, func, args…] and the flush takes no arguments. */
#define AUTOFOCUS_FLUSH_CB_SLOTS (2 + 0)
int autofocus_flush_run(JSContext *docctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValue in,
                        JSValue **out_cb, int *out_argc);

#endif
