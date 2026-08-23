/* THE EVENT LOOP'S OWN STATE — HTML §8.1.7, and it TIME-TRAVELS.
 *
 * WHY THIS IS A COMPONENT AND NOT TWO STATICS. An event loop has state of its own that no task source owns: in
 * this engine the VIRTUAL CLOCK (there is no wall clock to wait on, so the clock is the moment the loop has
 * advanced to), §8.1.7.1's LAST RENDER OPPORTUNITY TIME, and the INSERTION ORDER a task source breaks its own
 * ties by. Each of those lived as a `static double` — the clock in core/timing/timer.c, the render opportunity
 * in core/rendering/rendering.c — and each said in its comment that it belonged to the event loop rather than
 * to the file it was in. A fact stated in two files is answered in two files.
 *
 * IT IS PER-FLOW, and that is the whole reason it moved into the heap. A code flow is a complete timeline of
 * this event loop: it runs its own tasks, in its own order, and parks and resumes byte-identically. Virtual
 * time is DERIVED from that order — it is a function of which tasks this flow has run — so a clock shared with
 * the flow next door is a clock another timeline moved. Concretely, with one static: flow A fires a
 * `setTimeout(f, 10000)` and the agent's clock jumps to 10000; flow B, whose own tasks have reached moment 0,
 * then sets `setTimeout(g, 0)` and gets an expiry of 10000, and a flow parked before either resumes into a
 * clock it never advanced. §Time-travel-resume's razor calls that a CAP: a resume that is not byte-identical.
 *
 * SO THE RECORD IS A HEAP OBJECT, whose property writes the per-flow COW delta already captures — the same
 * mechanism §8.12 Animation frames's map of animation frame callbacks and §9.4.2's port message queue use, for the same reason
 * CLAUDE.md gives: platform data a flow queues is a JS value, never malloc'd C. It is built at agent init,
 * which is pre-boot, so it is BASELINE and every flow's writes to it are captured rather than shared.
 *
 * IT IS PER-AGENT AND NOT PER-REALM, which is the other half of the answer. §8.1.7 gives one event loop to a
 * similar-origin window agent, and a document and its same-origin iframe are ordered by that ONE loop: a
 * per-realm clock would order the parent's timers against the child's by nothing at all. §8.7 Timers's map of active
 * timers is the opposite — HTML puts one on every global — so the two live in different places on purpose. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_EVENT_LOOP_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_EVENT_LOOP_H

#include "quickjs.h"

/* Declared ONCE PER AGENT, before any page script runs — the record must be in the pre-boot baseline. */
void event_loop_init(JSContext *ctx);
void event_loop_free(JSContext *ctx);

/* The VIRTUAL clock, in ms since the agent started — the one clock every task source is ordered by, and the
   one an Event's `timeStamp` and a file's modification time are stamped from. */
double event_loop_now(JSContext *ctx);

/* MOVE THE CLOCK to the moment a task source becomes due. `due` is the earliest moment ANOTHER source is
   already due at, or -1 when none is — the caller has it, because it is what decided this move, and passing it
   is what keeps the assertion side-effect-free (asking the timer source costs an allocation, which a DCHECK
   condition may not do). Both invariants are asserted here rather than trusted to the caller: time may not run
   backwards, and the loop may not step OVER a source that becomes due first. */
void event_loop_advance_to(JSContext *ctx, double when, double due);

/* §8.1.7.1's LAST RENDER OPPORTUNITY TIME — the moment the rendering task source last became due. */
double event_loop_last_render(JSContext *ctx);
void   event_loop_set_last_render(JSContext *ctx, double when);

/* THE INSERTION ORDER OF A TASK, allocated by the loop rather than by a source, because it is what orders two
   sources' tasks that become due at the SAME moment — and because a per-source counter cannot: §8.7 Timers's map of
   active timers is per-global, so two same-origin documents each hand out handle 1 and a tie between them
   would be decided by nothing. `_peek` answers the next number without allocating it, which is what makes "an
   entry this flow can see was queued on this flow's own timeline" an assertable statement. */
double event_loop_task_seq(JSContext *ctx);
double event_loop_task_seq_peek(JSContext *ctx);

#endif
