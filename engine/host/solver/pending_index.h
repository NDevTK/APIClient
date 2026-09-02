/* THE FRONTIER'S OUTSTANDING REQUESTS, AS A SET THE HOST CAN LOOK ONE UP IN — the frontier-wide version of the
 * sentence solver/pending.h already makes about ONE register ("the register is a set of outstanding requests,
 * not an ordered queue"), which is exactly what the two host doors were both asking for and neither had.
 *
 * WHY IT EXISTS. `engine_pending_fetches` is asked once per SLICE and `engine_provide` once per REPLY, and both
 * answered by walking EVERY flow's register and EVERY entry in it, materialising each entry's URL and method as
 * C strings BEFORE deciding whether the entry was even a candidate. The set never shrinks in aggregate — a flow
 * keeps an ANSWERED entry until IT delivers, and an @S candidate re-fire re-runs the document and re-issues its
 * fetches — so the two walks are over the total number of requests the session has ever made, while the answer
 * both of them want is over the ones still OUTSTANDING. Measured on a live document at a stall: 147616 entries
 * across 3422 flows, of which the outstanding set was two orders of magnitude smaller; a payment round with
 * ZERO replies to pay cost 10.2 ms at one point in the run and 14.7 ms later, which is a pure walk whose price
 * is set by everything already answered. Two thirds of the process's CPU was inside those two functions.
 *
 * ONE RECORD IS ONE MEMBER HOWEVER MANY FLOWS NAME IT. A fork SHARES records (pending.h's PEND_SHARE), so the
 * outstanding SET is a set of records and not of (flow, entry) pairs — which is also why the join's O(join
 * length) rescan for a duplicate line stops being the mechanism that dedups the host's list: the sharing is
 * already the dedup, one level down.
 *
 * WHY IT IS C AND NOT A JS VALUE, WHICH IS THE FIRST QUESTION §State-isolation ASKS. That rule is about
 * PLATFORM DATA A FLOW QUEUES — a port's message queue, a pending task's payload — and its two obligations are
 * that the data fork per flow and park to the cold tier. This is neither: it is HOST-SIDE SCHEDULER STATE
 * DERIVED FROM the registers, in the same category as engine.c's `join` buffer, and the records it names are
 * themselves JS values which do fork and do park. Nothing here outlives the frontier it indexes; it is rebuilt
 * by the same pushes that rebuild the registers and reset at teardown.
 *
 * AND NO COW DELTA MAY CAPTURE IT, for the reason pending.h gives for the register itself and one step
 * stronger: the host reads this from OUTSIDE any flow's delta, so a capture would make membership depend on
 * which delta happened to be applied, and a context switch would revert the set while the records stayed
 * reachable from the registers — a reply delivered into a set that no longer names it. Every mutation here is
 * reached from solver/pending.c, inside the brackets that file already owns.
 *
 * IT IS NOT A BOUND AND IT DROPS NOTHING (§NO BOUNDS). Membership is the SAME predicate the walks computed for
 * themselves — an entry that carries an address, is not a synchronous host request, and has no value yet — and
 * a record leaves only when that predicate stops holding: it is answered, or the last register naming it is
 * gone. Nothing is deduped by identity, nothing is capped, nothing is forgotten; this changes how the set is
 * LOOKED UP and never what is in it.
 *
 * THE SYNCHRONOUS REQUEST KIND IS NOT IN IT AT ALL. FLOW_PENDING_HOSTREQ is keyed by a REQUEST ID and not by a
 * pair — pending.h: "two flows asking the same question in different worlds must get different answers" — so it
 * has no key here, and a pair-keyed set that held one would answer one seam's question through the other. It is
 * refused at the track rather than skipped at each read. */
#ifndef ENGINE_HOST_SOLVER_PENDING_INDEX_H
#define ENGINE_HOST_SOLVER_PENDING_INDEX_H

#include "quickjs.h"

typedef struct PendIndexNode PendIndexNode;

/* ---- membership, driven entirely from solver/pending.c --------------------------------------------------- */

/* TRACK a record from its PUSH. Tracking is what holds the count of registers naming it, and it has to begin
   before the record can be shared, which is why it is the push and not the moment the address arrives. */
void pending_index_track(JSValueConst rec);
/* IS THIS RECORD TRACKED — false for a synchronous host request (never tracked) and for one already answered
   or already dropped by every register. The two calls below are no-ops on an untracked record, deliberately:
   a fork copies a register whose entries are a mix of all four states. */
int  pending_index_tracked(JSValueConst rec);
/* HAS IT AN ADDRESS THE HOST CAN BE SHOWN — tracked AND keyed under a (method, url) pair. */
int  pending_index_keyed(JSValueConst rec);

/* ANOTHER REGISTER NAMES IT — one call per shared record at a fork. */
void pending_index_ref(JSValueConst rec);
/* ONE FEWER. At zero the record has been dropped by every register, and it leaves the set: without this a
   flow SOLD to the cold tier while still owed a reply would leave its request in the join for ever, and the
   host would fetch an address no flow is parked on. That is the resurrection hazard, closed at its origin. */
void pending_index_unref(JSValueConst rec);

/* THIS RECORD IS NOW A COMPLETE OUTSTANDING REQUEST — key it under the pair. The strings are COPIED: the node
   outlives every record that named it (see `answered` below), so it cannot borrow a register's bytes. */
void pending_index_key(JSValueConst rec, const char *method, const char *url);
/* THE REPLY LANDED — it leaves the outstanding set for good, and its pair remembers that it did. */
void pending_index_answered(JSValueConst rec);

/* ---- the two host doors ---------------------------------------------------------------------------------- */

/* EVERY PAIR THE HOST IS STILL OWED A REPLY FOR, in one chain. `pending_index_node_count` is 0 for a pair whose
   every record has been answered — the node survives its members on purpose, see the next function. */
PendIndexNode *pending_index_first(void);
PendIndexNode *pending_index_next(PendIndexNode *n);
const char *pending_index_node_method(const PendIndexNode *n);
const char *pending_index_node_url(const PendIndexNode *n);
int          pending_index_node_count(const PendIndexNode *n);
/* Member `i`, OWNED by the caller. */
JSValue      pending_index_node_member(const PendIndexNode *n, int i);

/* THE PAIR A REPLY ANSWERS, or NULL for one this frontier never parked on. */
PendIndexNode *pending_index_find(const char *method, const char *url);
/* HOW MANY OF THIS PAIR'S RECORDS HAVE BEEN ANSWERED — which is the ONE fact that separates a reply the host
   sent TWICE from a reply for a request nobody ever made, and the reason a node outlives its members. Those
   are different failures with different owners (the second is excused by the pager's sale credit and the first
   is never excused), and CLAUDE.md's rule about a search that cannot tell two gaps apart is the same rule: an
   answer that collapses two states reports neither. */
long pending_index_node_answered(const PendIndexNode *n);

/* ---- the reply door's RATE ------------------------------------------------------------------------------- */

/* HOW MANY REQUESTS THIS INSTANCE HAS EVER PUT TO THE REPLY DOOR, AND HOW MANY REPLIES SETTLED ONE — the
 * counterpart of engine.h's `host_asked`/`host_answered`, which count the SYNCHRONOUS door and only it. Read
 * them for the reason that pair states: the census's `pend`, `owed` and `blocked` are LEVELS, and no single
 * reading of a level separates a host that is paying promptly from one that has never paid at all.
 *
 * THE UNIT IS THE RECORD, NOT THE ISSUED REQUEST. N arms share one record and the join dedups over the pair,
 * so one issued request may settle several records in one `engine_provide` — which is the same unit that
 * function returns and the same unit `pend` is a length of, so the three can be read together.
 *
 * `answered <= asked` HOLDS BY CONSTRUCTION: a record is keyed at most once (asserted at pending_index_key)
 * and untracked when answered, so it can contribute at most one to each and cannot be answered unkeyed.
 * result.c asserts it. Neither is reset at a session boundary; see pending_index_reset for why.
 *
 * AND A PAYMENT IS NOT YET A THING LEARNED — the value has reached the REGISTER, and the flow still has to
 * take it. Read `pending_index_answered_total` against the census's `deliver-one-reply` step-unit arm: the
 * first climbing with the second at zero is a document being paid and consuming nothing.
 *
 * THAT PAIRING IS NOT HYPOTHETICAL AND IT IS WHY THIS PAIR WAS BUILT. Measured on the wasm smoke at 74eb1d62
 * (build-full.log, 13 censuses, 5857 steps, the run killed at its CPU budget): `deliver-one-reply` NEVER RAN,
 * and neither did `await-fetch-record`, `await-owed-reply` or `scheme-fetch-answered`. Every reply-dependent
 * probe row read 0 — `fetch then-chain clone-body body-bytes body-iso`, the surface entire. The census said
 * `blocked 0`, `owed 0`, `payment: 0/0 asks paid` and `299306 owed repl(ies)`, and every one of those four
 * numbers was TRUE and none of them was about this door: the first three are the SYNCHRONOUS door's, and the
 * fourth is `pend`, the sum of every register's LENGTH. A reader took the whole set to mean the host is never
 * asked. It is asked, and it pays: test_forced.c's provider answers every line at every slice and engine.c's
 * run_scheduler asserts UNCONDITIONALLY after each payment that `engine_pending_fetches()` is EMPTY — an
 * assert armed in that build (`-DAPICLIENT_DEV=1`) and silent for the whole run. `engine_pending_fetches`
 * skips an entry on exactly three conditions — it carries no URL, it has a value, or it was declined — and a
 * park writes its address in the same C activation that pushed it, while the smoke's provider declines
 * nothing. So the residue those 299306 register slots are made of is ANSWERED REPLIES NO FLOW HAS EVER TAKEN,
 * at 67076 KiB: 55% of the frontier's entire per-flow memory.
 * A rate over THIS door is what says that in one reading, and until it existed the census could not say it at
 * all: not one of its rows separates "never asked" from "asked, paid, and never consumed". */
long pending_index_asked_total(void);
long pending_index_answered_total(void);

/* ---- teardown -------------------------------------------------------------------------------------------- */

/* Release the whole index. Named beside pending_free_ctx and called from it, because the atoms and this have
   the same lifetime and the same reason: a corpus host takes a runtime down and brings another up per file. */
void pending_index_reset(JSContext *ctx);

#endif
