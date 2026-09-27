/* POINTER CAPTURE — Pointer Events 4 §8 "Pointer capture". The header states which of Pointer Events 4 §4
 * "Extensions to the Element interface"' three members is installed here, why the other two are not, and what
 * the choice was measured to cost; this file is the member. */
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/element.h"
#include "core/events/pointer_capture.h"
#include "core/idl_args.h"

/* Pointer Events 4 §4's `boolean hasPointerCapture ( long pointerId )` — ONE declaration, once per agent. */
static int g_id_has_capture = -1;

/* Pointer Events 4 §4's hasPointerCapture, whose whole definition is "returns true if the pending pointer
 * capture target override for pointerId is set to the element on which this method is invoked, and false
 * otherwise". Unlike its two siblings it has NO throw clause, which is why it is the member that lands.
 *
 * THE OVERRIDE IS NOT SET FOR ANY pointerId, AND THAT IS DERIVED BY ENUMERATING THE STANDARD'S OWN WRITERS OF
 * IT RATHER THAN ASSUMED — a claim about a fetchable document, which is what makes it checkable rather than a
 * fact about this tree that would rot. There are four and there are no others:
 *   Pointer Events 4 §8.2 "Setting pointer capture" step 6 — "For the specified pointerId, set the pending
 *     pointer capture target override to the Element on which this method was invoked" — which its own step 1
 *     cannot reach: "If the pointerId provided as the method's argument does not match any of the active
 *     pointers, then throw a "NotFoundError" DOMException". core/html/user_activation.h states from its own
 *     side that this agent "dispatches no trusted `keydown`/`mousedown`/`pointerdown`/`pointerup`/`touchend`",
 *     so nothing has ever produced a pointer event and the active pointer set is empty.
 *   Pointer Events 4 §8.3 "Releasing pointer capture" step 3, which CLEARS and never sets, behind the same
 *     refusal at its own step 1.
 *   Pointer Events 4 §8.4 "Implicit pointer capture", whose trigger is "just before the invocation of any
 *     pointerdown listeners" — those are fired by Pointer Events 4 §3.2.9 "maybe send pointerdown event",
 *     which is not built.
 *   Pointer Events 4 §8.5 "Implicit release of pointer capture", which clears after a pointerup or
 *     pointercancel this agent likewise does not fire.
 * So false is Pointer Events 4 §4's own answer evaluated against the state this engine HAS — the same answer
 * a real browser gives for an element that has never captured a pointer — and it holds for EVERY key at once,
 * which is a statement about the ENGINE rather than about a pointerId. The member therefore never reads its
 * argument's VALUE, and an unknown `pointerId` is not concretized in order to answer.
 *
 * A MAP WITH NO WRITER WAS DECLINED, AND IT IS WRITTEN DOWN SO THE NEXT READER DOES NOT RE-DERIVE IT AS A GOOD
 * IDEA. core/html/user_activation.h's banner is the standard this file is trying to meet — it replaced "a
 * CONSTANT WITH AN EXCUSE ATTACHED" with real state whose reads are "computed rather than asserted" — and it
 * could, because §6.4.1's state has structure (two timestamps, an ordering, a consumption) AND a live writer
 * in core/file/file_picker.c. Pointer capture has neither: keyed by pointerId over an empty active pointer
 * set there is nothing to key, and the only writers are the four algorithms above. Allocating the map anyway
 * would not remove the constant, it would move it into the map's emptiness where a reader cannot see it, and
 * add state whose sole reader is this line. The enumeration above is the derivation, and it is in the one
 * place a reader of this answer will be standing.
 *
 * AND THERE IS NO CAPTURE-STATE DCHECK, WHICH IS STATED RATHER THAN LEFT TO BE NOTICED. Every candidate — that
 * the override is unset, that an override names a connected element per Pointer Events 4 §8.2 step 3, that an
 * override's pointerId is one Pointer Events 4 §8.2 step 2 could have taken — has two sides that cannot
 * disagree in any run of TODAY's program, because nothing writes the state either side reads. An assert no
 * value of the program can fail is a non-check with a reassuring transcript, so the two below are the ones
 * that CAN fail and the invariant that cannot yet be violated is carried by the residual instead.
 *
 * NAMED RESIDUAL — THE ANSWER IS KEY-INDEPENDENT WHERE THE STANDARD KEYS IT. WHAT IS NOT COVERED: Pointer
 * Events 4 §8.2 step 6 keys the override BY pointerId and Pointer Events 4 §3.1.3.2 "Process pending pointer
 * capture" promotes it to the pointer capture target override; this answers for the empty map rather than
 * from one, so it cannot distinguish two pointerIds. WHAT THE NEXT DIFF BUILDS: Pointer Events 4 §3.2.9
 * "maybe send pointerdown event", which is what puts a pointer in the active pointer set that Pointer Events
 * 4 §8.2 step 2 takes its pointer FROM — and Pointer Events 4 §8.2, Pointer Events 4 §8.3, Pointer Events 4
 * §3.1.3.2 and the keyed map land WITH it and never before it, since each of the other three is unreachable
 * while that set is empty. HOW ITS ABSENCE WOULD SHOW: a run in which a page's `pointerdown` handler calls
 * `setPointerCapture` and a subsequent `hasPointerCapture` on the same element answers false — which is
 * exactly the observation Pointer Events 4 §4's own Note names, "This method will return true immediately
 * after a call to setPointerCapture(), even though that element will not yet have received a
 * gotpointercapture event". */
static JSValue js_pc_has_capture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el;
    (void)argv;
    (void)magic;

    DCHECK(argc >= 1, "Pointer Events 4 §4 \"Extensions to the Element interface\" declares `boolean "
                      "hasPointerCapture ( long pointerId )` — one REQUIRED position — and Web IDL §3.6 "
                      "\"Overload resolution algorithm\" refuses a shorter call in the prologue, so this body "
                      "is only ever entered with it");
    el = element_of_value(this_val);
    DCHECK(el != NULL, "hasPointerCapture ran on a receiver `element_is` admitted and `element_of_value` does "
                       "not — one interface answered two ways about what an Element is, and the declaration's "
                       "own brand test is the half that was supposed to have settled it");
    (void)el;
    return JS_FALSE;
}

void pointer_capture_init(JSContext *ctx)
{
    /* Web IDL §3.2.4.5 "long" at the one position. The conversion belongs to the TYPE and not to this member:
       a page object with a `valueOf` runs at the boundary exactly as it does in a browser, and unknown
       external input crosses as itself — which is what lets the body answer without asking for a number. */
    static const IdlArgType PC_1LONG[1] = { IDL_LONG };

    DCHECK(g_id_has_capture < 0,
           "pointer_capture_init ran twice — Pointer Events 4 §4's member is declared once per AGENT, and a "
           "second declaration would leave two ids for one member with the first's pool entry unreachable");
    g_id_has_capture = idl_method_id(ctx, PC_1LONG, 1, js_pc_has_capture, 0);
    /* Web IDL §3.7.7 "Operations"' receiver test. Pointer Events 4 §4 is a `partial interface Element`, so the
       interface the receiver must implement is Element and the predicate is core/dom/element.h's own
       `element_is` — declared here rather than performed in the body, the way CSSOM VIEW §6's members are. */
    idl_this_iface(element_is, "Element");
}

void pointer_capture_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_has_capture >= 0,
           "Pointer Events 4 §4's member was installed in a realm before pointer_capture_init declared it — "
           "the component declares once per agent and installs from the cached id");
    idl_install_method(ctx, proto, "hasPointerCapture", g_id_has_capture);
    /* `setPointerCapture` and `releasePointerCapture` are DELIBERATELY NOT INSTALLED and the header says why:
       Pointer Events 4 §8.2 step 1 and Pointer Events 4 §8.3 step 1 both throw for every pointerId while the
       active pointer set is empty, and the corpus's presence tests are spelled on THOSE two names — so
       installing either flips a guard TRUE onto a branch that then raises, which is worse than the absence.
       They stay honestly absent, which is what engine/idlgen.mjs reports them as. */
}
