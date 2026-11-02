/* Time-travel points before a source read: a held snapshot of a flow standing at its first read of a declared
 * attacker source, which an @S candidate of that source forks from instead of re-running the document from the
 * pre-boot baseline.
 *
 * The cut-point rule is the delivery rule read backwards: a candidate's world answers every read of its source
 * with one delivered value, and an answer that has flowed into state cannot be re-answered, so a snapshot serves a
 * source only if it precedes that source's first read on its path. The reading machine parks before such a read
 * (source_cut_ask), flow_step forks a sibling at the same call site and holds it (flow_set_held), and the parent
 * reads and runs on; every flow forked from it afterwards references the cut.
 *
 * `refs` counts the lineage members carrying a cut and the searches holding it; at zero the standby is shed at the
 * next pick. A standby is re-derivable (its path replayed from the baseline is the same state), so RAM pressure, a
 * park and an exhausted frontier shed it unwritten, and a search holding a shed cut seeds from the baseline. */
#ifndef ENGINE_HOST_SOLVER_SOURCE_CUT_H
#define ENGINE_HOST_SOLVER_SOURCE_CUT_H

#include "solver/flow.h"

typedef struct SourceCut SourceCut;

/* The running flow has read declared source `src` (concolic_source_wrap's door): its history now holds a read a
   later delivery could not re-answer. No-op with no flow running or a flow already holding the read. */
void source_read_note(const char *src);
/* Whether `f`'s history read `src` before this instant. */
int  source_read_has(const Flow *f, const char *src);

/* Asked by a reading machine before it reads `srcs`: 1 when the running flow must park so a snapshot can be taken
   first (the machine returns JS_STEP_YIELD and asks again on re-entry), 0 to read now. A flow that cannot open a
   search (a candidate, a drive, a snapshot) or that holds work a held member must not carry is answered 0, as is a
   source already read or already cut on this path. A re-entry that finds its own ask still standing was not
   parked, and is answered 0 with the ask withdrawn (counted in source_cut_census). */
int  source_cut_ask(const char *const *srcs, int n);

/* The sources the running flow is parked to cut before, read by flow_step's ladder. */
int  source_cut_due(const Flow *f);
/* flow_step's half, after it assembled `standby` at `f`'s call site: make the cut, hold the standby, give `f` its
   reference and clear the ask. */
void source_cut_leave(Flow *f, Flow *standby);

/* A fork's half (engine_sibling_assemble): `sib` inherits `parent`'s read sources, and its references to the
   lineage's cuts when it can still open a search. */
void source_cut_fork(Flow *sib, const Flow *parent);
/* flow_release's half: the read sources, the references, an outstanding ask, and a standby's place in its cut. */
void source_cut_flow_release(Flow *f);

/* The cut on `f`'s path that precedes every read of `src`, or NULL. */
SourceCut *source_cut_of(const Flow *f, const char *src);
void source_cut_hold(SourceCut *c);
void source_cut_drop(SourceCut *c);
/* The held member, or NULL once shed. */
Flow *source_cut_standby(const SourceCut *c);

/* A standby to shed other than `keep`: one no lineage member or search refers to, or with `all` any. NULL when
   none is left. The caller releases it (engine.c), which detaches it from its cut. */
Flow *source_cut_next_shed(int all, const Flow *keep);

/* An arm was forked off a standby for a candidate (engine_cut_arm). */
void source_cut_count_arm(void);
/* Lifetime counts: cuts taken, asks withdrawn because the read was re-entered unparked, standbys shed, and
   candidate arms forked off a standby. */
typedef struct { long taken, unparked, shed, arms; } SourceCutCensus;
SourceCutCensus source_cut_census(void);

#endif
