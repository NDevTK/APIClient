/* A QUEUED PROGRAM'S SOURCE TEXT — one immutable buffer, shared by every flow whose sequence holds it.
 *
 * WHAT IT IS FOR. A flow's program sequence (solver/flow.h's `dyn` table) is per-flow, and its ROWS are: which
 * programs this timeline holds, in what order, and where its cursor is. The BYTES of a program are not. A
 * script's source text is fixed the moment it is decoded — the only write to the body column is
 * flow_deliver_one_reply replacing one row's POINTER when an external script's reply arrives, never a write
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

#include "quickjs.h"   /* a HELD PARSE is a JSValue; see dyn_body_parse_hold below */

/* OPAQUE, so that the text cannot be reached without going through the accessor and the refcount cannot be
   reached at all. It is also what makes the conversion enforceable: `free(f->dyn[k])` on a column of these is
   a type error rather than a heap corruption that only a fork would ever surface. */
typedef struct DynBody DynBody;

/* A PROGRAM IS `len` BYTES AND NOT "UP TO THE FIRST NUL", AND THAT IS THE SPEC'S ANSWER RATHER THAN THIS
   FILE'S CONVENIENCE. ECMAScript §11.1 "Source Text" says it outright — "All Unicode code point values from
   U+0000 to U+10FFFF, including surrogate code points, may occur in ECMAScript source text where permitted by
   the ECMAScript grammars" — so a U+0000 inside a string literal is a program a page may legitimately ship,
   and a bundle carrying one is not a corrupt bundle. Real ones do: measured over a 30-site mirror, one site
   shipped 125 of them in a single script and another 2309.
   WHAT THAT COSTS IF THE LENGTH IS DROPPED IS NOT A PARSE ERROR, IT IS SILENCE. Reading the text to its first
   NUL hands the compiler a PREFIX of the page's program, so every endpoint, every sink and every branch after
   that byte is unreachable — a run that learns less and reports nothing missing. The pair is therefore the
   type: every entry here takes a length, and `dyn_body_len` is what every reader asks rather than `strlen`.

   One body over a COPY of `text[0 .. len)`, with one reference. The copy is NUL-terminated at `len` because
   the compiler entries (`JS_FlowNew` / `JS_FlowEvalModule`) take the pair AND lexbor's arena scans want a
   sentinel, but that terminator is a guard and never the length. NULL only if the allocation failed — the
   callers CHECK, because a program that cannot be stored is a program the sequence silently would not run. */
DynBody *dyn_body_new(const char *text, size_t len);

/* One body over `text` ITSELF, which this call owns from here on: `len` is its length and `text[len]` must be
   the NUL guard. `text` MAY contain embedded NULs — see the paragraph above — and that is the one thing this
   entry's assertion no longer says otherwise. Consumes `text` on every path — on failure it is freed and NULL
   is answered — so the decode paths that already hold a malloc'd source text hand it over without a second
   copy of a megabyte. */
DynBody *dyn_body_adopt(char *text, size_t len);

DynBody *dyn_body_ref(DynBody *b);
void     dyn_body_unref(DynBody *b);

/* The program, for as long as the caller holds a reference. There is a NUL at `dyn_body_len(b)` and there may
   be NULs BEFORE it, so this is never read on its own: a caller takes it WITH `dyn_body_len` or it is reading
   a prefix of somebody's bundle. */
const char *dyn_body_text(const DynBody *b);
/* Its length — the number this file exists to carry. It is NOT `strlen` of the above (that is exactly the read
   this pair replaces), and it is kept rather than recomputed for a second reason: the census walks every row of
   every flow, and a `strlen` there is a pass over every byte of every bundle the frontier holds. */
size_t dyn_body_len(const DynBody *b);

/* HAVE THESE BYTES ALREADY BEEN PARSED TO COMPLETION BY SOME FLOW OF THIS PROCESS — the direct answer to the
   question `classicCompiles` against a document's program count was being asked to infer, and it lives HERE
   because the body is the only thing in the engine that IS a program's source rather than a holder of one.
   WHY THE INFERENCE IT REPLACES IS UNSOUND, which is the whole reason this entry exists. solver/engine.h USED
   TO READ `classic_compiles` against "the programs a document reached" and call a figure far above that count a
   compile REPEATED per flow; it records that sentence in its own words rather than deleting it, because it is
   what two counters printed side by side invite and because a brief was written out of it and a lane dispatched
   on it. The numerator's population is wider than that denominator's on TWO independent
   axes at once: it counts every FLOW and every TIMELINE (a fork carries its parent's cursor and then parses
   every later program of the sequence itself; an @S candidate session and a cold-resumed replay re-run the
   document from the baseline and parse its own programs again), and it counts every APPENDED row as well as
   every seeded one — a lazy chunk, an injected `<script>`, a `javascript:` URL, a peer's operation, a value
   dump. A modern bundle's chunks are the bulk of that second axis and each one is DISTINCT BYTES, so a count
   many times a document's `<script>` count is what a healthy run of such a page MUST read. The tree already
   records the identical defect one row over, for `progStarts` read against `rootPrograms` (solver/flow.c): two
   counters differenced across populations of different width, agreeing by coincidence on one document.
   IT IS NOT A HASH OF THE SOURCE TEXT, which CLAUDE.md §ONE-global rules out by name because a minified bundle
   repeats one-line bodies dozens of times and a text hash therefore names a SET. It is the BODY — the one
   allocation those bytes arrived in — so two rows answer `already` for each other only when they are literally
   holding the same program: the seed table's body, which every flow of a document references, and a fork's
   inherited column, which references its parent's.
   IT IS A FLOOR AND THE ROW BESIDE IT IS WHAT SAYS BY HOW MUCH. A reply is decoded PER DELIVERY, so two arms
   parked on one external row each adopt their own buffer for the same chunk and a repeat between them is
   invisible here; `dyn_body_is_own_decode` is that population, so a reader has a floor and a bound instead of
   a floor and a sentence. It is also a floor across a park: a cold-resumed flow's rows are rebuilt, so its
   first parse of bytes some earlier session parsed reads as a first.
   MONOTONE, SET ONCE, AND READ BY NOTHING THAT DECIDES ANYTHING (CLAUDE.md §NO BOUNDS). No source is refused
   for having been parsed, no parse is skipped and no arm is chosen on it; the mark's only consumer is the
   census counter raised at the same event as `classic_compiles`. It is state on a C allocation and not on the
   JS heap, so the COW delta neither owes it a capture nor can unapply it — which is the same reason
   `dyn_body_total_bytes` is kept incrementally on this side rather than walked per flow.
   THE MARK IS MADE HERE AND NOT AT THE CALLER, so the answer and the write cannot come apart: a caller that
   read the bit and then forgot to set it would report every later parse of those bytes as a first. */
int dyn_body_note_parsed(DynBody *b);
/* WHETHER THIS BUFFER WAS DECODED FOR ONE FLOW'S OWN DELIVERY — the population in which the answer above
   CANNOT be observed, published so the floor has a bound. `dyn_body_adopt`'s two callers are both a reply
   arriving into ONE flow, which decodes the response for that timeline and adopts the result; a sibling parked
   on the same row takes its own delivery and adopts a SECOND buffer over the same bytes. So a repeat within
   this population is real and unobservable, and a count of parses of such bodies is how much of the total a
   reader must treat as unmeasured rather than as measured-zero. `dyn_body_new`'s bodies are a COPY the caller
   made and may be referenced by many rows, which is where the answer above has its force. */
int dyn_body_is_own_decode(const DynBody *b);

/* THE PARSE OF THESE BYTES, HELD SO THAT N TIMELINES CROSSING ONE DOCUMENT'S SCRIPT SEQUENCE PARSE IT ONCE
   RATHER THAN ONCE EACH — the CONSUMER of the repeat the row above measures, and it lives on the body for
   exactly the reason that row does: the body is the only thing in the engine that IS a program's source, so
   the cache and the census key on ONE identity and cannot drift into two answers about one program. A cache
   keyed on anything else — a document plus a script index, a hash of the text, a per-realm table — would be a
   second statement of "these are the same program" beside `dyn_body_note_parsed`'s, free to disagree with it,
   and CLAUDE.md §ONE-global rules the hash out by name because a minified bundle repeats one-line bodies and a
   hash therefore names a SET.
   WHY THE REPEAT EXISTS AT ALL, which is what says who the beneficiaries are: a fork inherits its parent's
   program column and its cursor (`sib->dyn[i] = dyn_body_ref(parent->dyn[i])`, `sib->last_compiled =
   parent->last_compiled`), so a sibling does not re-parse the program it branched INSIDE and parses every
   LATER program of that sequence itself — against its parent's own body. An @S candidate session and a
   cold-resumed replay re-run the document from the baseline and do the same. So the population is a
   document's own script sequence crossed by many timelines, which is every real page under forced
   multi-path execution.
   THE KEY IS `(realm, name, flags)` AND NOT THE BODY, AND EVERY ONE OF THE THREE IS BAKED INTO THE BYTECODE.
   `JS_CallInternal` takes the running realm from the BYTECODE (`ctx = b->realm`) and not from the frame, so a
   frame built over a closure compiled in another realm runs its body against that realm's global, class_proto
   and intrinsics — CLAUDE.md §A-PER-REALM-FACT, and quickjs's JS_FlowInstantiate asserts it on its own side.
   The NAME is the program's ScriptOrModule name: the module-map key and the base a relative `import()`
   resolves against. The FLAGS carry strictness and `JS_EVAL_FLAG_INLINE_SCRIPT`, which is what the solver
   reads to decide whether a missing member of a server-rendered record is unknown INPUT or `undefined`
   (solver/absent.h). A closure is therefore answered only to a caller whose three agree.
   A DISAGREEMENT IS A `DCHECK` AND NOT A MISS, which is a claim about this engine rather than a convenience.
   Every reference to one body names one realm by construction — a body is made where a program ARRIVES, and
   `engine_queue_into`'s callers each queue into rows of ONE document — and its name and flags are derived
   from the ROW, which a fork copies rather than recomputes. So two rows holding one body and disagreeing
   about any of the three is a defect somewhere else, and it is one this file can see and nothing else can.
   The RELEASE arm answers 0 and the caller parses the program itself, which is a defined wrong-nothing
   answer rather than an undefined one: it costs one parse and truncates nothing.
   THE OWNERSHIP CONTRACT, continuing the four sentences above:
     - `dyn_body_parse_hold` takes ONE MORE reference on the closure and records the key it was compiled
       under; the CALLER still owns the reference it passed.
     - `dyn_body_parse_ref`  answers ONE MORE reference, which the caller frees; the body keeps its own.
     - the body's LAST `dyn_body_unref` releases the closure, through the realm recorded beside it.
   WHY THAT FREE PATH NEEDS NO `JSContext` ARGUMENT AND CANNOT DANGLE, which is the question this slot was
   deferred on. The realm is stored because the KEY requires it, and a held closure keeps that realm ALIVE:
   `JSFunctionBytecode` takes a counted reference on its realm (`b->realm = JS_DupContext(ctx)`), released
   only when the bytecode is, so the pointer this file frees through is valid for exactly as long as there is
   something to free. What would be left is a body outliving the RUNTIME, and that is caught before any free
   can happen rather than argued about: a held closure is a live GC object, so `JS_FreeRuntime`'s own
   `gc_obj_list` walk reports it and aborts, naming the bytecode and its realm. `dyn_body_parses_held` is
   published so a host can assert the stronger statement at its teardown instead of waiting for that walk.
   IT IS NOT A CACHE WITH AN EVICTION POLICY AND MUST NOT GROW ONE. A held closure lives exactly as long as
   the body whose text it was compiled from, which is as long as some timeline holds that program — so the
   set is bounded by the frontier's own membership and by nothing this file decides. There is no registry of
   live bodies here and there must not be one (see `dyn_body_total_bytes`), so there is nothing to walk and
   nothing to choose between. What a pager MAY do is drop one: a parse is RE-DERIVABLE from the text the body
   is holding, which is CLAUDE.md §OOM's third category, so shedding it converts storage into recomputation
   and truncates nothing — and that is the only shape a release here may ever take.
   NOTHING HERE DECIDES ANY WORK (§NO BOUNDS). No source is refused for having been parsed, no arm is chosen
   on the answer, and no program is skipped: the two arms of `dyn_body_parse_ref` differ only in whether the
   caller spends the parse again. A FAILED parse holds nothing, because its result is an exception and an
   exception is one flow's completion rather than a fact about the bytes — so a program with a SyntaxError is
   re-parsed by every timeline that reaches it, and `dyn_body_note_parsed` goes on reporting each of those as
   a repeat, which is the honest reading and not a gap. */
int  dyn_body_parse_ref(const DynBody *b, JSContext *realm, const char *name, int flags, JSValue *pfn);
void dyn_body_parse_hold(DynBody *b, JSContext *realm, const char *name, int flags, JSValueConst fn);
/* HOW MANY BODIES ARE HOLDING ONE RIGHT NOW — a GAUGE and not a lifetime count, so it may not be differenced
   across two censuses and its sum over a partition is a live population rather than a total of events. It is
   published for the one comparison a reader of a shared-parse row can make without re-deriving the mechanism:
   a body may hold at most one parse and a parse is held only by a body that finished one, so this can never
   exceed the lifetime number of finished parses, which is what solver/engine.c asserts at its copy-out. */
long dyn_body_parses_held(void);

/* WHAT THE PROGRAM TEXT COSTS THE INSTANCE, ONCE — the sum of `len + 1` over every body alive right now.
   It is a SHARED row of the cold census (solver/cold.h) and not a per-flow one, for the reason that file
   already states about frozen segments: a shared buffer added into each holder's own total reports the sharing
   as if it did not exist, which is exactly the mistake a pager makes. */
long dyn_body_total_bytes(void);
/* …and how many distinct bodies that is, because the pair is what says whether a growing number is more
   programs or bigger ones. */
long dyn_body_live_count(void);

#endif /* ENGINE_HOST_SOLVER_DYN_BODY_H */
