/* POINTER CAPTURE — Pointer Events 4 §8 "Pointer capture", and Pointer Events 4 §4 "Extensions to the
 * Element interface", which is the three methods a page calls. See pointer_capture.c.
 *
 * EVERY NUMBER HERE CARRIES ITS STANDARD'S NAME, INCLUDING A REPEAT, and that is not a style. Pointer Events
 * is not one of the standards engine/specindex holds, so a BARE `§8.2` in this directory is resolved by the
 * FILE VOTE and judged against whichever indexed standard shares the number — core/events/pointer_event.c
 * records that exact accusation being made against its own verbatim quotations. The price of a checkable
 * citation is the repetition.
 *
 * WHICH OF THE THREE IS HERE AND WHY THE OTHER TWO ARE NOT. Pointer Events 4 §4 declares
 *     partial interface Element {
 *       undefined setPointerCapture ( long pointerId );
 *       undefined releasePointerCapture ( long pointerId );
 *       boolean hasPointerCapture ( long pointerId );
 *     };
 * and the three are NOT one landing, because only one of them is TOTAL. Pointer Events 4 §8.2 "Setting
 * pointer capture" step 1 is "If the pointerId provided as the method's argument does not match any of the
 * active pointers, then throw a "NotFoundError" DOMException", and Pointer Events 4 §8.3 "Releasing pointer
 * capture" step 1 throws the same exception for a NARROWER condition — see the arm count below, which is the
 * one thing in this header a next diff must not read short. This agent has no active pointers at all —
 * core/html/user_activation.h states from its own side that it "dispatches no trusted
 * `keydown`/`mousedown`/`pointerdown`/`pointerup`/`touchend`" — so those two throw on their FIRST STEP for
 * every call a page can make. `installing them ONLY RENAMES the exception a drag handler dies on` is what
 * this sentence read, and it is true at the MAJORITY of sites and understated at three — the ledger below
 * measures them: where a presence test's false arm runs today, the rename is a flow-ender appearing where
 * there was none. It is kept in its own words because a reader who re-derives it from a bare call will
 * re-derive it correctly; what it must not be read as is a reason the landing is HARMLESS, which is the
 * direction an understatement here invites.
 * Pointer Events 4 §4's hasPointerCapture has NO throw clause: its whole definition is "returns true if the
 * pending pointer capture target override for pointerId is set to the element on which this method is
 * invoked, and false otherwise". It is answerable against the state this engine has, so it is the landing and
 * the other two stay honestly absent.
 *
 * AND THE REFUSAL IS FOUR ARMS IN Pointer Events 4 §8.2 AND TWO IN Pointer Events 4 §8.3, NOT THE ONE OF EACH
 * THE PARAGRAPH ABOVE USED TO NAME — it read `Pointer Events 4 §8.3 "Releasing pointer capture" step 1 says
 * the same`, and §8.3's step 1 is a CONJUNCTION §8.2 has no counterpart for. Read off the fetched step lists:
 *   Pointer Events 4 §8.2 has SIX steps and THREE of them throw. Step 1's "NotFoundError"; step 3's
 *     "InvalidStateError" for an element that "is not connected"; step 4's "InvalidStateError" for a node
 *     document that "has a locked element"; step 5's "terminate these steps" for a pointer "not in the active
 *     buttons state", which Pointer Events 4 §4's own prose calls failing "silently"; and step 6, the WRITE.
 *   Pointer Events 4 §8.3 has THREE steps and ONE throw. Step 1's "NotFoundError", conditioned on "and these
 *     steps are not being invoked as a result of the implicit release of pointer capture"; step 2's "terminate
 *     these steps" when hasPointerCapture is false, which is THIS member and is therefore always the case
 *     here; and step 3, the CLEAR.
 * THAT SECOND CONJUNCT IS THE ARM THE RETIRED SENTENCE ERASED AND IT IS THE ONE A NEXT DIFF NEEDS. Pointer
 * Events 4 §8.5 "Implicit release of pointer capture" runs §8.3's steps after a pointerup or pointercancel,
 * and again when "a pointer lock [PointerLock] is successfully applied on an element" — neither of those is a
 * page call and neither carries a live-pointer requirement — so a §8.3 whose step 1 refused unconditionally
 * would refuse the implicit release, which is the only way a capture is ever given up. A refusal landed one
 * arm short is a different engine rather than a narrower one.
 * RETIREMENT: this record goes when Pointer Events 4 §8.2 and §8.3 are built and each arm above is a SITE, so
 * the count is read off the code rather than off this paragraph.
 *
 * THAT IS A NARROWER CLAIM THAN "THE MEMBERS NEED THE SOURCE FIRST", WHICH IS WHAT THIS PROJECT USED TO SAY,
 * AND THE DIFFERENCE IS THE WHOLE DIFF. pointer_event.c's residual reads that "every call of Pointer Events 4
 * §4's members throws on its FIRST STEP" and therefore that the landing unit is Pointer Events 4 §3.2.9
 * "maybe send pointerdown event". That is EXACT for setPointerCapture and releasePointerCapture and FALSE for
 * hasPointerCapture, which Pointer Events 4 §4 gives no first step to throw on — so the member that needs no
 * source is also, measurably, the one that GATES the two that do.
 *
 * WHAT INSTALLING IT COSTS AND BUYS, MEASURED RATHER THAN ARGUED. A corpus of real app bundles is fetched
 * rather than committed (testing/corpus/README.md is that decision), so the figures below are a fact about
 * one hour and the DERIVATION is what is handed over:
 *     NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs   # prints the corpus path
 * and then, over that path, a parse that classifies every occurrence of the three identifiers by POSITION and
 * every CALL by what lexically encloses it. ROW = one occurrence at a distinct (file, offset). Re-derive it;
 * what is durable is the SHAPE, which is that the dominant idiom pairs the two members in one expression:
 *     onPointerUp: e => { let t = e.target; t.hasPointerCapture(e.pointerId) && t.releasePointerCapture(e.pointerId) }
 * An absent MEMBER read is not what ends the flow — ECMAScript §10.1.8.1 "OrdinaryGet ( obj, propertyKey,
 * receiver )" answers undefined for an absent property, so the READ succeeds — the CALL is, through
 * ECMAScript §13.3.6.2 "EvaluateCall ( func , thisValueRef , argumentListNode , tailPosition )"'s "If func is
 * not an Object, throw a TypeError exception". So today that line raises at `hasPointerCapture`, and the
 * `&&` never runs. With hasPointerCapture installed it answers FALSE, the `&&` SHORT-CIRCUITS, and
 * `releasePointerCapture` is never reached — so ONE member removes BOTH flow-enders at every site of that
 * shape, without Pointer Events 4 §8.3 and without an active pointer.
 *
 * AND IT FLIPS NO GUARD, WHICH IS THE HALF THAT DECIDES WHETHER A PARTIAL LANDING IS A REGRESSION.
 * CLAUDE.md §NO-STUBS' hazard is that installing a member flips a page's presence test TRUE and takes the
 * page off a fallback arm that was working. The corpus's presence tests are all spelled on the OTHER two
 * names (`a.setPointerCapture && …`, `` `releasePointerCapture` in a && … ``); the single presence test of
 * hasPointerCapture is nested inside one of them, so it stays unreachable while releasePointerCapture is
 * absent. Re-derive that before adding either sibling — it is the reason the sibling landing is not free.
 *
 * AND IT WAS RE-DERIVED BEFORE THE SIBLINGS WERE DECLINED A SECOND TIME, WHICH TURNED UP A GUARD SPELLING NO
 * SWEEP FOR `&&` OR `in` CAN SEE. The corpus is fetched rather than committed, so the DERIVATION is what is
 * handed over — `NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs`, then for each of the
 * three identifiers every occurrence's enclosing expression READ rather than matched, since a question about
 * control flow is answered by reading control flow and a presence test has as many spellings as English. The
 * durable part is the SHAPE, and it is FOUR spellings rather than the two named above: `a.X && a.X(id)`,
 * `` `X` in a && a.X(id) ``, ``typeof a.X === `function` && a.X(id)``, and `a.X?.(id)` — the last an OPTIONAL
 * CALL, which is a guard by effect and matches no pattern a reader would write for one, and whose absent arm
 * CONTINUES exactly as a presence test's does.
 * AND THE LEDGER IS ASYMMETRIC IN THE DIRECTION THAT DECIDES IT. Installing either sibling buys at most ONE
 * fidelity gain — a `"setPointerCapture" in HTMLElement.prototype` ternary whose true arm a real browser takes
 * and this engine does not — against UNCAUGHT flow-enders at the sites whose guard's false arm runs TODAY: an
 * optional call standing as a `pointerdown` handler's last statement, an `&&` inside the test of an `if` whose
 * other comma operand is that handler's real work, and two `in` tests in front of the pointerup and
 * pointercancel registration of a press hook. Every other site is a bare call, where the change is a
 * TypeError renamed to a NotFoundError and the flow ends either way; one more is inside a `try`/`catch` and
 * one more is already gated by hasPointerCapture, which is this member answering false. NOTHING IMPROVES AND
 * THREE HANDLERS DIE, so the sibling landing is not merely un-free, it is negative.
 * AND WHAT MAKES THEM SURVIVABLE IS Pointer Events 4 §3.2.9 AND NOTHING SHORT OF IT, which is a reading of
 * those sites rather than a restatement of the residual: at every one of them the argument is a live
 * `e.pointerId` inside a pointer handler, so Pointer Events 4 §8.2's step 1 stops refusing exactly when that
 * pointer is in the active pointer set — which is what §3.2.9 puts it there. pointer_capture.c's residual
 * names that diff, and this reading CONFIRMS it rather than correcting it.
 * RETIREMENT: this record goes when the guard-shape question is answered for a MEMBER on an arbitrary receiver
 * by an instrument, the way engine/js_guard_shape.mjs answers it for a global (whose receiver set is the three
 * global spellings and cannot reach an element), so the spellings are a verdict list rather than a sentence.
 *
 * WHAT DOES NOT FOLLOW: THE ANSWER IS CONCRETE AND MUST NOT FORK. false is not a guess about a pointerId
 * this engine does not know; it is the answer for EVERY pointerId, because the pending pointer capture target
 * override is set by Pointer Events 4 §8.2 and Pointer Events 4 §8.4 "Implicit pointer capture" and by
 * nothing else, and neither exists here. So the member never reads its argument's value at all, an unknown
 * `pointerId` is never concretized to reach it, and forking would explore a world the engine cannot be in. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_POINTER_CAPTURE_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_POINTER_CAPTURE_H

#include "quickjs.h"

/* Pointer Events 4 §4's declarations, ONCE PER AGENT. Called from core/dom/element.c's element_init beside
 * element_view_init, because this is a `partial interface Element` exactly as CSSOM VIEW §6 is. */
void pointer_capture_init(JSContext *ctx);

/* Pointer Events 4 §4's members on Element.prototype, for ONE REALM — installed on the prototype
 * core/dom/element.c has just built, the way §4.9's other partial interfaces are. Per realm because a C
 * member runs in the realm that DEFINED it (core/dom/element.c states that for the same list). */
void pointer_capture_install(JSContext *ctx, JSValueConst proto);

/* Pointer Events 4 §4's AGENT half given back — called from core/dom/element.c's element_free, the release on
 * whose column `element` sits. It frees nothing: the one slot is declared under `element` and that cascade's
 * last line resets it from the registry. What this entry exists for is the CLAIM that the cascade reached this
 * file, which core/agent_state.h's agent_state_undo requires of every file declaring under a row it does not
 * itself undo — without it the undo refuses, naming this file's declaration. */
void pointer_capture_free(void);

#endif
