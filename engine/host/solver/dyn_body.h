/* A QUEUED PROGRAM'S SOURCE TEXT — one immutable buffer, shared by every flow whose sequence holds it.
 *
 * WHAT IT IS FOR. A flow's program sequence (solver/flow.h's `dyn` table) is per-flow, and its ROWS are: which
 * programs this timeline holds, in what order, and where its cursor is. The BYTES of a program are not. A
 * script's source text is fixed the moment it is decoded — the only write to the body column is
 * flow_drain_pending replacing one row's POINTER when an external script's reply arrives, never a write
 * THROUGH one — so the text is shared baseline state in exactly CLAUDE.md §State-isolation's sense, and a
 * per-flow copy of it is the defect that section names one layer out of the delta.
 *
 * WHY IT EXISTS AT ALL, measured rather than reasoned: the fork copied it. `sib->dyn[i] = strdup(parent->dyn[i])`
 * made a fork cost O(TOTAL SCRIPT BYTES) instead of O(rows), so a real single-page app — a 2.1 MB module bundle
 * is an ordinary size for one — paid 2.1 MB per arm, and forced multi-path execution forks per branch. It ends
 * where a ceiling always ends: `CHECK(sib->dyn[i], "engine: OOM fork dyn body")`, which is the allocator
 * refusing, on a page whose whole learned API surface is then nothing.
 *
 * THIS IS THE SAME CONVERSION solver/pending.h RECORDS FOR THE REGISTER BESIDE IT — "THE FORK PAID FOR EVERY
 * BYTE … a JS string is immutable and refcounted, so the same inheritance is now a reference each. The copy is
 * O(entries), not O(bytes)." One column of the flow was left holding raw bytes; this is that column.
 *
 * WHY NOT A JS STRING, which is what that rule reaches for first. Two facts about THIS column and not that
 * one: (1) every reader hands the text to `JS_FlowNew`/`JS_FlowEvalModule` as a `const char *`, so a JSString
 * would be converted back with `JS_ToCString` — a full copy at EVERY compile, which is the cost this file
 * exists to remove, moved rather than deleted; (2) a row never crosses a park, because the cold tier stores a
 * RECIPE and a resumed flow re-queues its rows from the replayed document (solver/flow.h), so the one thing a
 * JS value buys that a C allocation cannot is a thing this column does not need. What the JS heap WOULD have
 * bought is visibility, and `dyn_body_total_bytes` gives that back to the census directly instead of leaving
 * the bytes in @HEAP's unattributed residual.
 *
 * BLINK CALLS THIS `ScriptSourceCode` over a refcounted `String`: one immutable source shared by every context
 * that runs the script, and the same reason — the text is not a property of the runner.
 *
 * WHY THE ALLOCATIONS HERE ARE PLAIN AND NOT solver/reclaim.h's. That file's rule is "an allocation the engine
 * makes FOR A RUNNING FLOW must be able to shrink the frontier before it fails", and it applied squarely to the
 * `strdup` this file replaces: a fork's copy was one flow's, so selling the tail would have paid for it. A body
 * is not one flow's. It is made once when a program ARRIVES and it is released only when the last timeline
 * holding that program is gone, so selling the worst flow frees it only in the case where that flow was its
 * sole holder — which is exactly the case where the sale would have to run to completion before this
 * allocation could tell whether it had been funded. WHAT WOULD CHANGE THIS is an audit nobody has done: which
 * of `dyn_body_new`'s callers pass a string a SALE could free while the copy is in progress. Until that is
 * answered, converting this would be trading a crash for a use-after-free that only reproduces on the retry.
 *
 * THE OWNERSHIP CONTRACT, in one sentence each:
 *   - `dyn_body_new`  makes a body from text it COPIES; the caller owns the one reference it gets back.
 *   - `dyn_body_adopt` makes a body from a malloc'd buffer it TAKES; it consumes that buffer on every path,
 *     including failure, so a caller never frees what it handed over.
 *   - `dyn_body_ref`   takes one more reference and answers the same body.
 *   - `dyn_body_unref` gives one back; the text is freed when the last one is.
 * A holder of a reference may read the text for as long as it holds it, and may never write it. */
#ifndef ENGINE_HOST_SOLVER_DYN_BODY_H
#define ENGINE_HOST_SOLVER_DYN_BODY_H

#include <stddef.h>

/* OPAQUE, so that the text cannot be reached without going through the accessor and the refcount cannot be
   reached at all. It is also what makes the conversion enforceable: `free(f->dyn[k])` on a column of these is
   a type error rather than a heap corruption that only a fork would ever surface. */
typedef struct DynBody DynBody;

/* One body over a COPY of `text`, with one reference. NULL only if the allocation failed — the callers CHECK,
   because a program that cannot be stored is a program the sequence silently would not run. */
DynBody *dyn_body_new(const char *text);

/* One body over `text` ITSELF, which this call owns from here on: `len` is its length and `text[len]` must be
   the NUL every reader compiles it through. Consumes `text` on every path — on failure it is freed and NULL is
   answered — so the decode paths that already hold a malloc'd source text hand it over without a second copy
   of a megabyte. */
DynBody *dyn_body_adopt(char *text, size_t len);

DynBody *dyn_body_ref(DynBody *b);
void     dyn_body_unref(DynBody *b);

/* The program, NUL-terminated, for as long as the caller holds a reference. */
const char *dyn_body_text(const DynBody *b);
/* Its length, which is `strlen` of the above and is kept rather than recomputed: the census walks every row of
   every flow, and a `strlen` there is a pass over every byte of every bundle the frontier holds. */
size_t dyn_body_len(const DynBody *b);

/* WHAT THE PROGRAM TEXT COSTS THE INSTANCE, ONCE — the sum of `len + 1` over every body alive right now.
   It is a SHARED row of the cold census (solver/cold.h) and not a per-flow one, for the reason that file
   already states about frozen segments: a shared buffer added into each holder's own total reports the sharing
   as if it did not exist, which is exactly the mistake a pager makes. */
long dyn_body_total_bytes(void);
/* …and how many distinct bodies that is, because the pair is what says whether a growing number is more
   programs or bigger ones. */
long dyn_body_live_count(void);

#endif /* ENGINE_HOST_SOLVER_DYN_BODY_H */
