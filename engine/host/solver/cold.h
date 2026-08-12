/* THE COLD TIER — RAM holds the hot working set; the lowest-value SUSPENDED flows page out and resume on
 * demand, byte-identically. See CLAUDE.md §Time-travel-resume ("the cold low-value tail serializes to IDB as
 * suspended snapshots and resumes on demand"), §scheduler ("STARVE means deprioritize-and-page, NEVER
 * terminate"; "an engine self-parks its residue to the IDB cold tier under pressure") and engine.c's own
 * flow-compile OOM, which names this file as what carries past the RAM floor.
 *
 * ITS FIRST CONTRACT IS THE CENSUS, and that ordering is not a preamble to the work — it IS the first piece of
 * it. A pager cannot be written for a snapshot nobody can describe: what crosses the boundary decides the
 * encoder, and what is SHARED between siblings decides whether writing one flow's snapshot writes its
 * siblings' too. The two chains that already report themselves (cow.c's `heapSegs`, dom_cow.c's `domSegs`) say
 * the sharing works — they were flat at 14/727 and 18/5160 across 14003 flows — and everything else in a
 * snapshot had no number at all. This is that number, per part, so a part that turns out to be a per-flow COPY
 * of state its parent already holds is visible as one instead of being paged out N times.
 *
 * WHAT A SNAPSHOT IS, in the order §Time-travel-resume names it: the flow's per-flow COW delta (its heap AND
 * DOM writes) plus its suspended heap-frame chain, plus the rest of what the scheduler swaps with it — the
 * decision vector and its cursor, the path constraint, the queued jobs, the replies the host still owes, and
 * the flow's own byte state (its lazily-loaded chunk bodies, its candidate substitution, a routed record).
 * Each of those is a row here, and each row is either PER-FLOW or SHARED; the pair is the whole design
 * question, so the census answers it directly rather than reporting one total. */
#ifndef ENGINE_HOST_SOLVER_COLD_H
#define ENGINE_HOST_SOLVER_COLD_H

/* WHAT THE FRONTIER'S SNAPSHOTS ARE MADE OF. Every `*_bytes` is host `malloc` unless the row says otherwise:
   the runtime's own allocations (the frame chains, the step machines) are counted by quickjs and reported in
   the @HEAP line's `unattributed` residual, which is named here rather than restated. */
typedef struct {
    long flows;              /* live members of the frontier */
    long framed;             /* … suspended inside a live heap-frame chain (the part with no encoder yet) */
    long blocked;            /* … holding an unanswered host request */

    /* PER-FLOW rows — these multiply by the number of parked flows, so they are what a pager pays for. */
    long dec_entries;        /* decision-vector slots the flows STAND ON — a chain total, so this counts the
                                same shared prefix once per flow that references it and is deliberately NOT a
                                byte figure. It is the number that was quadratic when the vector was copied. */
    long dec_bytes;          /* …and what the flows themselves own for it: one blob header each */
    long head_entries;       /* heap COW delta HEADS (the writes each flow has made since its last fork) */
    long head_bytes;
    long dom_head_entries;   /* the DOM half of the same */
    long dom_head_bytes;
    long job_count;          /* queued microtasks/tasks */
    long pend_count;         /* replies the host still owes */
    long pend_bytes;
    long dyn_bytes;          /* this flow's own lazily-loaded chunk bodies */
    long misc_bytes;         /* the Flow struct, its candidate substitution, a routed record, the blob headers */

    /* SHARED rows — counted ONCE for the whole frontier, because a frozen segment is referenced by every flow
       that forked below it. A pager that wrote these per flow would multiply the sharing back out. */
    long seg_count, seg_entries, seg_bytes;              /* cow.c's frozen heap chain */
    long dom_seg_count, dom_seg_entries, dom_seg_bytes;  /* dom_cow.c's frozen document chain */
    long pin_seg_count, pin_seg_entries, pin_seg_bytes;  /* concolic.c's frozen path constraint */
    long dec_seg_count, dec_seg_entries, dec_seg_bytes;  /* decide.c's frozen decision vector */
} ColdCensus;

/* Walk the frontier and fill `out`. Pure measurement: it takes no reference, mutates nothing, and is safe to
   call between scheduler steps (which is where the progress stream calls it). */
void cold_census(ColdCensus *out);

#endif
