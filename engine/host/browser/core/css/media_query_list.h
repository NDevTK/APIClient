/* `MediaQueryList`, `MediaQueryListEvent` and `Window.matchMedia` — CSSOM VIEW §4.2, plus the "evaluate media
 * queries and report changes" steps that HTML §8.1.7.3's update-the-rendering runs as its STEP 10.
 *
 * WHAT THIS FILE IS FOR, AND WHY IT IS NOT media_query.c. The LANGUAGE — parsing a media query, serializing it,
 * evaluating it against the modelled viewport — is Media Queries Level 4 and lives next door. This file is the
 * CSSOM object a page gets back, its listener list, the document's collection of them and the algorithm that
 * reports a change. Two standards, two problems, two files.
 *
 * THE `matches` VALUE IS CONCOLIC AND ITS EXAMPLE IS REAL, which is CLAUDE.md's §Headless line stated for this
 * exact member: `matchMedia` resolves a default viewport for its `.matches` example yet stays CONCOLIC, so a
 * page that READS it gets a truthful value while a page that BRANCHES on it explores both worlds. Collapsing it
 * to the modelled boolean would delete the alternate-viewport arm — and with it every mobile router, every
 * `prefers-color-scheme` theme fetch and every reduced-motion code path a responsive bundle ships. Shrugging it
 * to opaque would be the other failure: `media_query_matches` computes a real answer from a real viewport, and
 * an opaque `.matches` makes update-the-rendering step 10 unable to decide whether anything changed at all.
 *
 * THE ENGINE's OWN READ CONCRETIZES ON THE FLOW's PIN. Step 10 is C and cannot fork, so it asks decide.c what
 * this flow already decided about this exact predicate and falls back to the modelled example — the identical
 * shape as page_visibility.c's, because it is the identical problem: one fact, a forking reader and a
 * non-forking one.
 *
 * THE STATE THAT TIME-TRAVELS IS THE LATCH. §4.2's algorithm fires only when the matches state has changed
 * "since the last time these steps were run", so each MediaQueryList carries the state it last reported — and
 * that is per-FLOW: the arm that explored the narrow viewport reported a different value from its sibling, and
 * one shared byte would make the sibling's next frame report a change it never saw. It is captured at the
 * record's accessor (CLAUDE.md §COW), so there is no write site left to miss. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_MEDIA_QUERY_LIST_H
#define ENGINE_HOST_BROWSER_CORE_CSS_MEDIA_QUERY_LIST_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* THE AGENT's HALF — the two interfaces and `matchMedia`, declared once. */
void media_query_list_init(JSContext *ctx);
/* THE REALM's HALF: §3.7 gives every realm its own two prototypes AND its own document collection, declared
   into realm.h's one list so a realm cannot come into existence without them. */
void media_query_list_install_proto(JSContext *ctx);
/* The Window member and the two interface objects, which are the host's per-document install. */
void media_query_list_install(JSContext *ctx, JSValueConst global);
/* THE AGENT'S HALF, UNDONE — a row on core/platform.h's release column, so it takes the RUNTIME: §4.2's two
   class ids, its realm-value slot and its four member declarations are registrations there, and the Symbol is
   given back with JS_FreeValueRT. */
void media_query_list_free(JSRuntime *rt);

/* CSSOM VIEW §4.2 "evaluate media queries and report changes" — HTML §8.1.7.3 update-the-rendering STEP 10.
 *
 * It is split into a COUNT and a per-target step because firing at one target runs the page's code, so the
 * walk must be able to rest between targets: a stage that spanned them would be a stage the scheduler cannot
 * preempt inside, which is exactly what CLAUDE.md forbids of a step containing author code.
 *
 * `media_query_list_count` is the SNAPSHOT §4.2's "in the order they were created, oldest first" walks — taken
 * once when the step begins, so a `matchMedia` called from a `change` listener does not extend the walk it is
 * inside. It cannot fire on this frame anyway (a list records its matches state at construction), which is why
 * the snapshot loses nothing.
 *
 * `media_query_list_change` performs the whole of §4.2 for target `i`: it evaluates the query, compares with
 * the state that target last reported, and — only when they differ — LATCHES the new state and mints the
 * `MediaQueryListEvent` to fire. It answers JS_UNDEFINED when nothing changed, and otherwise the event (OWNED)
 * with the target in `*ptarget` (OWNED). */
/* HTML §8.1.7.3 update-the-rendering STEP 4's test, for THIS document: has §4.2 anything to report? A
   MediaQueryList whose current matches state differs from the one it last reported is a visible effect pending,
   so a document holding one HAS a rendering opportunity — without this the frame that would deliver the
   `change` is never queued, and step 10 is written but unreachable. READS ONLY: the latch is step 10's to
   move, and moving it here would consume the change the frame is being queued to deliver. */
bool media_query_list_pending(JSContext *ctx);

uint32_t media_query_list_count(JSContext *ctx);
JSValue  media_query_list_change(JSContext *ctx, uint32_t i, JSValue *ptarget);

#endif
