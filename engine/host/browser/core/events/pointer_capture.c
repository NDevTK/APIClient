/* POINTER CAPTURE — Pointer Events 4 §8 "Pointer capture". The header states which of Pointer Events 4 §4
 * "Extensions to the Element interface"' three members is installed here, why the other two are not, and what
 * the choice was measured to cost; this file is the member. */
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/element.h"
#include "core/events/pointer_capture.h"
#include "core/agent_state.h"
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
 *   Pointer Events 4 §8.3 "Releasing pointer capture" step 3, which CLEARS and never sets, behind a refusal
 *     at its own step 1 that is NARROWER than §8.2's rather than "the same": it throws only when the
 *     pointerId matches no active pointer "and these steps are not being invoked as a result of the implicit
 *     release of pointer capture", so the §8.5 invocations below reach step 3 with no live pointer at all.
 *     A reader who builds §8.3 from §8.2's step 1 refuses the implicit release, which is the only way a
 *     capture is given up — the arm count is in the header and it is two here and FOUR in §8.2.
 *   Pointer Events 4 §8.4 "Implicit pointer capture", whose trigger is "just before the invocation of any
 *     pointerdown listeners" — those are fired by Pointer Events 4 §3.2.9 "maybe send pointerdown event",
 *     which is not built.
 *   Pointer Events 4 §8.5 "Implicit release of pointer capture", which clears after a pointerup or
 *     pointercancel this agent likewise does not fire — and ALSO on two triggers that are not pointer events
 *     at all, which is where the state's hygiene at REMOVAL lives rather than in a section of its own: "When
 *     the pending pointer capture target override is no longer connected [DOM], the pending pointer capture
 *     target override node SHOULD be cleared", and a successful pointer lock, which §8.5 specifies by
 *     running §8.3's steps. Both are SHOULD rather than MUST, and the first is the reason a set-then-remove
 *     sequence leaves no dangling override; neither is reachable while the override is unset for every key.
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
 * 4 §8.2 step 2 takes its pointer FROM — and Pointer Events 4 §8.2 WITH ALL FOUR OF ITS REFUSAL ARMS,
 * Pointer Events 4 §8.3 WITH BOTH OF ITS, Pointer Events 4 §3.1.3.2 and the keyed map land WITH it and never
 * before it, since each of the other three is unreachable while that set is empty. The arm counts are in this
 * component's header, enumerated from the fetched steps, because a clause naming ONE arm of a multi-arm
 * refusal asks for a WRONG answer rather than a partial one.
 * HOW ITS ABSENCE WOULD SHOW: a run in which a page's `pointerdown` handler calls
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
    /* AND THE ID IS AGENT STATE, WHICH THE ASSERT ABOVE ASSUMES AND NOTHING WAS MAKING TRUE. A file-scope
       static survives an agent; `idl_method_id` returns an INDEX INTO core/idl_args.c's member pool, and
       `idl_args_pool_free` puts that pool's count back at 0 — so a carried index names a member of a pool that
       no longer exists, and once the next agent has declared far enough it names a DIFFERENT member, which
       pointer_capture_install would then install under `hasPointerCapture`. Registering the slot is what puts
       it back at -1 when the agent is released, so the `< 0` precondition is true for the SECOND agent by
       construction rather than by there only ever having been one.
       AND THIS COMMENT NAMED `idl_args_free` UNTIL THE COUNT WAS GREPED, WHICH IS RECORDED HERE BECAUSE THE
       WRONG NAME IS THE INTUITIVE ONE AND HAS NOW BEEN WRITTEN FOUR TIMES. `g_n = 0` is in
       `idl_args_pool_free` and in nothing else; `idl_args_free` gives back what the pool INTERNED and runs
       BEFORE `JS_FreeRuntime`, where `idl_args_pool_free` runs after — so a reader who takes the shorter name
       places the danger window on the wrong side of the runtime's death, which is the one ordering fact this
       paragraph exists to state. The confusion had already been caught ONCE, at core/idl_async_iter.h, and
       recorded only there; it then recurred at core/dom/dom_implementation.c, core/events/broadcast_channel.c,
       core/fetch/body.c and HERE, in commit 07c7ae92861851d43bc221fa0b4b9a4e00e2c144, which is this file's own
       registration. A record filed only where it was fixed does not reach the sibling that spells the same
       question the same wrong way.
       MEASURED: without this line a build's own two-agent and cold-park/cold-resume stages aborted at that
       assert — four stage processes, one identity — and the assert was RIGHT: `pointer_capture_init` really
       had run twice with the first agent's index still in the static. The defect was never the crash. */
    /* AND THE ROW IS `element`, NOT THIS FILE'S OWN NAME, WHICH IS THE SPELLING A READER RE-DERIVES AND THE
       ONE THAT ABORTED A BUILD. Pointer Events 4 §4 is a `partial interface Element`, and this component is
       reached from ONE place in each direction: `pointer_capture_init` and `pointer_capture_install` have
       exactly one caller each and both are core/dom/element.c. So element's release column is whose release
       this slot's reset is reached from, which is precisely what core/platform.c's row walk asks about --
       `element` is a row carrying a release and `pointer_capture` is not a row at all. It is the same idiom
       core/dom/selection.c uses declaring under `document` and core/crypto/subtle_crypto.c under `crypto`:
       A SUB-COMPONENT NAMES THE ROW THAT RELEASES IT, NEVER ITS OWN FILE.
       THIS LINE READ `"pointer_capture"` AND THE WRONG SPELLING IS THE INTUITIVE ONE -- a file names its slot
       after itself, the name reads as correct at this site, and nothing local disagrees. What it cost is not
       a weaker check but a check that was NEVER RUN: the row pairing can only ask "does anybody RELEASE
       this?" about a name a row carries, so this slot was never asked, while `element`'s row reported
       "declared no agent state" in the exact words a component that declared nothing produces. One answer,
       three states -- the defect core/agent_state.h exists to refuse, arriving through the NAME. */
    agent_state_id("element", &g_id_has_capture,
                   "Pointer Events 4 §4's `hasPointerCapture` pool entry");
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

/* THE RELEASE IS THE CLAIM AND NOT THE RESET, WHICH IS WHY IT EXISTS WITH NOTHING TO FREE. The one slot this
   file holds is declared under `element`, so element_free's last line is what puts it back; a reset written
   here as well would be the SECOND resetter core/agent_state.h's undo exists to stop being kept by hand. What
   only THIS file can say is that the cascade reached it — agent_state_undo refuses to put back a slot declared
   in a file that has not spoken, because once the slot is at its pre-init value a release that never ran and
   one that did are the same bytes.
   IT IS THE OTHER HALF OF THE ROW MOVE RATHER THAN A SEPARATE REPAIR, and that is the whole reason it was
   owed. Declaring under `element` is what makes element_free the release that reaches this file, which is
   exactly what core/platform.c's row walk asks for — and the SAME act puts this file inside
   agent_state_undo("element")'s per-file question, which the old spelling was outside. A row move therefore
   carries a cascade edit with it; the declaration alone trades one abort for another.
   DERIVED RATHER THAN CHOSEN, AND THE DENOMINATOR IS NOT THE DECLARING SET: THIRTY-FOUR files declare under
   `element`, and ONE of them is core/dom/element.c, which CALLS the undo and is exempt by the undo's own
   `strcmp(file)` arm. Of the THIRTY-THREE that remain, THIRTY-TWO already ended their release in
   agent_state_reached("element") and this file was the one that did not — so the population was exactly one
   and this diff closes it at thirty-three of thirty-three.
   ORDER IS FREE HERE, uniquely among element_free's members: this file holds no reference, no atom and no
   allocation — one `int` pool entry — so nothing later in that cascade can read a handle this gives back. */
void pointer_capture_free(void)
{
    /* AND THE CASCADE REACHED THIS FILE — the claim that entitles element_free's last line to put this file's
       slot back. See core/agent_state.h's agent_state_reached. */
    agent_state_reached("element");
}
