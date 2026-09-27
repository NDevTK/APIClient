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
 * capture" step 1 says the same. This agent has no active pointers at all —
 * core/html/user_activation.h states from its own side that it "dispatches no trusted
 * `keydown`/`mousedown`/`pointerdown`/`pointerup`/`touchend`" — so those two throw on their FIRST STEP for
 * every call a page can make, and installing them only renames the exception a drag handler dies on.
 * Pointer Events 4 §4's hasPointerCapture has NO throw clause: its whole definition is "returns true if the
 * pending pointer capture target override for pointerId is set to the element on which this method is
 * invoked, and false otherwise". It is answerable against the state this engine has, so it is the landing and
 * the other two stay honestly absent.
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

#endif
