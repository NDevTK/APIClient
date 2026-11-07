/* The pick ledger: one row per Level 2 dispatch, holding the picked member's ranking terms as the order read them,
 * its rank on each term among a sample of the frontier at that instant, and what its tenure produced (the work it
 * ran, the novel identities paid by kind, the code sites first served anywhere in the instance). It is the data a
 * scheduling heuristic is fitted to, written as `@PICKCOLS` (the column names, once) and `@PICK` (one row) lines on
 * the host's stdout. It observes only: no call moves a rank, allocates through the reclaim edge or reads a clock,
 * and engine/host/solver/engine.c asserts the first at the pick.
 *
 * A tenure opens at a dispatch (engine.c's flow_credit_pick site) and closes at the next dispatch or at the
 * session's close, so the rows partition the instance's dispatches and their work. Armed on a native host by
 * APICLIENT_PICK_LEDGER=1 in the environment; never armed in the wasm build. */
#ifndef ENGINE_HOST_SOLVER_PICK_LEDGER_H
#define ENGINE_HOST_SOLVER_PICK_LEDGER_H
#include <stdint.h>
#include "solver/flow.h"

/* Whether this process writes the ledger, read once. */
int pick_ledger_armed(void);

/* The scheduler is about to dispatch `best`, displacing an incumbent when `displaced`: close the open tenure at
   `work` (the instance's stepWork) and `sold` (its lifetime memory-pressure sales), then open `best`'s. */
void pick_ledger_pick(const Flow *best, int displaced, int64_t work, long sold);

/* The session is closing: close the open tenure, if any, at `work` and `sold`. */
void pick_ledger_close(int64_t work, long sold);

/* Code site (`kind`, `key`) of some family received its first service. Counted once per identity for the instance's
   life; a no-op when the ledger is not armed. */
void pick_ledger_site_served(int kind, uint64_t key);

#endif
