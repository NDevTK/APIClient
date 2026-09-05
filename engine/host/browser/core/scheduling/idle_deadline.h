/* IdleDeadline — Cooperative Scheduling of Background Tasks §4.3 The IdleDeadline interface.
 *
 * ITS OWN COMPONENT, BESIDE idle_callback.c, because they are two contracts. That file owns a Window's two
 * LISTS and the processing model that drains them; this owns ONE INTERFACE — a class, a per-realm prototype,
 * a brand check and two members over a two-field record. Exercising it takes one object and one call, which is
 * the test core/realm.h's neighbours are split until they pass.
 *
 * §4.3 GIVES AN IdleDeadline EXACTLY TWO ASSOCIATED CONCEPTS and this file holds both, in one internal-slot
 * record (core/idl_slots.h) hung off a private Symbol, so the per-flow COW delta captures them as ordinary
 * property writes:
 *   - a `get deadline time` algorithm, "which returns a DOMHighResTimeStamp representing the absolute time in
 *     milliseconds of the deadline. The deadline is initially set to return zero";
 *   - a `timeout`, "which is initially false", which §4.3's own note says the invoke idle callback timeout
 *     algorithm is what sets to true.
 *
 * THE DEADLINE IS HELD AS THE VALUE THAT ALGORITHM ANSWERS, and for an idle period this engine cannot state
 * one — see idle_deadline_new_for_idle_period, which is where the argument for that is written and where the
 * unknown is minted. It is minted at the DEADLINE and not inside timeRemaining() on purpose: §4.3 states one
 * algorithm and derives the member from it, so an unknown special-cased in the member would be a second answer
 * to a question the standard asks once. */
#ifndef ENGINE_HOST_BROWSER_CORE_SCHEDULING_IDLE_DEADLINE_H
#define ENGINE_HOST_BROWSER_CORE_SCHEDULING_IDLE_DEADLINE_H

#include "quickjs.h"

/* THE AGENT'S HALF — §4.3's class and its two member declarations, once per agent. Called from
   idle_callback_init, which is the one component core/platform.c has a row for: §4.3's interface exists to be
   handed to §5.2's callbacks and has no other producer, so a second row would be a second thing to remember. */
void idle_deadline_init(JSContext *ctx);

/* THE REALM'S HALF — this realm's IdleDeadline.prototype, through core/realm.h's one declared list. §3.7 of
   Web IDL gives every realm its own interface prototype objects, and in this engine that decides ANSWERS: a C
   member runs in the realm that DEFINED it, so a shared prototype would answer every document's
   `timeRemaining()` out of whichever realm happened to build it first. */
void idle_deadline_install_proto(JSContext *ctx);
/* The interface OBJECT on one realm's global. Called from idle_callback_install. */
void idle_deadline_install(JSContext *ctx, JSValueConst global);

void idle_deadline_free(JSRuntime *rt);

/* §5.2 step 3.2's "a new IdleDeadline whose get deadline time algorithm is getDeadline", for the idle period
   this engine runs. OWNED, or JS_EXCEPTION. */
JSValue idle_deadline_new_for_idle_period(JSContext *ctx);

#endif
