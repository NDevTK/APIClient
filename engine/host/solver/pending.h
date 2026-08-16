/* THE FLOW'S PENDING REGISTER — what the host still owes ONE flow, held as a JS ARRAY OF PLAIN OBJECTS.
 *
 * WHY IT IS A JS VALUE. CLAUDE.md §State-isolation states the rule in as many words: "PLATFORM DATA A FLOW
 * QUEUES IS A JS VALUE, never malloc'd C … each must PARK to the IDB cold tier and RESUME, and each must fork
 * per-flow." This register was the last malloc'd list on a Flow — a `FlowPending *` grown by realloc, each
 * entry holding four strdup'd strings and a strdup'd array of header name/value pairs beside two real
 * JSValues. Three things followed from that and all three are why this file exists:
 *   - IT WAS INVISIBLE. `heapKiB` is quickjs's allocator; plain `malloc` lands in the @HEAP line's
 *     `unattributed` residual with nothing naming an owner, and the runtime's own leak walk (the gc_obj_list
 *     pass in JS_FreeRuntime, which is what the test262 gate counts leaks with) cannot see a `char *` at all.
 *   - IT COULD NOT PARK. A cold flow must serialize its snapshot and resume byte-identically; a list of raw
 *     pointers crosses neither a session nor a WASM instance. JS strings, numbers and plain objects do.
 *   - THE FORK PAID FOR EVERY BYTE. Inheriting one entry strdup'd its URL, its method, its body and every
 *     header name and value; a JS string is immutable and refcounted, so the same inheritance is now a
 *     reference each. The copy is O(entries), not O(bytes).
 * XMLHttpRequest's two header lists are the worked example this follows: JS Arrays of [name, value] for
 * exactly the same reason, in a component whose synchronous send() parks through engine_host_request.
 *
 * WHY NO DELTA MAY EVER CAPTURE IT. The register is the SCHEDULER's record ABOUT a flow, not state the page
 * can observe, and the host reads EVERY flow's register from outside any flow's delta — engine_provide fills
 * whichever flows parked on a URL, engine_host_requests joins what is outstanding across all of them, and the
 * preempt hook asks whether the running flow is blocked. A COW capture would make an entry's contents depend
 * on which delta happens to be applied: an entry appended after a fork would be TRUNCATED away the moment a
 * sibling switched in, and the reply would be delivered into a slot that no longer exists. So every mutation
 * here runs inside cow_engine_write_begin/end — one place, because every mutation is in this file.
 *
 * WHAT AN ENTRY IS. A null-prototype object carrying EVERY field of PENDING_FIELDS below, always — the fork
 * copies through that one list and asserts the source's own-property count against it, so a field added to the
 * record without a line in the list CRASHES AT THE CLONE instead of arriving in the sibling as `undefined`,
 * which is a real value belonging to the parent's request. The prototype is null because these are the
 * engine's own records: a page's `Object.prototype.url` must not answer a question the scheduler asks. */
#ifndef ENGINE_HOST_SOLVER_PENDING_H
#define ENGINE_HOST_SOLVER_PENDING_H

#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

/* WHAT THIS FLOW OWES ITSELF once the host supplies `url`. A fetch RESOLVES its promise with the reply; an
   injected <script src> has no promise at all — its reply is more of this flow's PROGRAM, queued as another
   script the one BFS runs. The kind is on the entry because it is the entry's business: the register, the
   dedup and the stall accounting are identical either way, and only the delivery differs. */
#define FLOW_PENDING_RESOLVE   0   /* fetch(): call `resolve` with the reply RECORD, which becomes a Response */
#define FLOW_PENDING_SCRIPT    1   /* injected <script src>: queue the reply as this flow's next program */
#define FLOW_PENDING_DOCSCRIPT 2   /* the document's OWN <script src>: the reply fills script slot `scriptI` */
/* A SYNCHRONOUS REQUEST ONLY THE HOST CAN ANSWER, and the one kind the flow cannot proceed past. A
   cross-document read (`iframe.contentWindow.document.body`) must answer at its own call site, and across
   instances the answer is not available in this turn — so the flow SUSPENDS there exactly as it suspends at an
   await or a loop back-edge, siblings run, and it resumes with the value. The other kinds are ASYNC: a fetch
   hands the page a promise and the flow keeps running. This one blocks, which is why flow_blocked exists and
   why the rendezvous is a REQUEST ID rather than a URL — two flows asking the same question in different
   worlds must get different answers, so answers are never shared the way a fetched body is. */
#define FLOW_PENDING_HOSTREQ   3
/* A DYNAMIC `import()`: call `resolve` with the reply's BODY, because what a module load is owed is SOURCE
   TEXT and not a Response — JS_ModuleLoadPending compiles whatever the promise settles with. It shared
   FLOW_PENDING_RESOLVE with fetch() while both were owed a bare body string; the moment a reply became a
   RECORD the two stopped wanting the same value, and one kind answering two questions would have handed the
   compiler `{status: 200, …}` to parse. Same register, same dedup, same stall accounting; only the delivery
   differs, which is what the kind is for. */
#define FLOW_PENDING_MODULE    4
/* A PUBLISHED API DESCRIPTION the engine itself asked for (solver/discovery.h). Same register, same dedup, same
   stall accounting; the delivery differs because there is nobody to deliver TO — a discovery flow has no
   promise and no program, so the drain hands the reply to the component that reads it and the flow is done.
   It is the one kind whose METHOD is not the request's to choose: this entry is minted by an entry point that
   takes a URL and nothing else, which is how §Attacker sources' "never fired to learn" is structural here. */
#define FLOW_PENDING_DISCOVERY 5

/* THE RECORD'S FIELDS, IN ONE PLACE, WITH WHAT THE FORK DOES WITH EACH.
 *   SHARE  — the sibling takes a REFERENCE. Right for an immutable value (a JS string, the request body's
 *            bytes, a number) and for the two values both arms genuinely observed: the resolve capability is
 *            shared deliberately (its already_resolved latch and the promise's settlement are per-flow state
 *            the COW delta captures, which is what lets both arms settle one capability), and an answer that
 *            arrived BEFORE the fork was computed in a world both arms were in.
 *   STRUCT — the sibling gets its OWN container, leaves shared. The request's header list is the flow's own
 *            record of what it asked for; two arms that diverge must not accumulate into one list.
 * A field added here is inherited by default. A field added to the record and NOT here is caught by the clone's
 * own-property-count DCHECK — the assert at the origin CLAUDE.md §Architecture names, rather than a comment. */
#define PEND_SHARE  0
#define PEND_STRUCT 1
/* `completion` IS THE ANSWER'S OTHER HALF — ECMA-262 6.2.4 says a completion is a TYPE and a VALUE, and a
   register with a slot for the value and none for the type delivers a peer's THROW as `undefined`. It rides
   the record beside the value it belongs to, is SHARED for the same reason the value is (an answer that
   arrived before a fork was computed in a world both arms were in), and parks with it. Its default is
   JS_UNDEFINED — "no answer yet, so no completion type" — and engine_host_answer writes the two together, so a
   record that has a value and no type crashes at the take rather than reading as a normal completion. */
/* `extra` IS EVERY OTHER TRUE ANSWER TO THIS ONE QUESTION, and it is a field rather than a second register
   because it belongs to the REQUEST. A peer's document state IS its flows, so a cross-agent operation is
   performed by every live timeline the peer has and each completes with its own answer: `otherW.length` has N
   answers for N peer timelines and all of them are true. The FIRST fills `value`/`completion` above; the rest
   land here, as [completion, value] PAIRS — the same shape `headers` has, so pend_list_fork already copies it —
   until the asking flow forks one arm per pair (engine.c's flow_answer_fork, which is the only reader).
   IT IS `STRUCT` AND THAT IS LOAD-BEARING: an arm DRAINS its own list, so two flows that share a record would
   drain each other's — the first to run would take an answer the other was going to fork over, and that peer
   timeline would then be explored by nobody. Each arm therefore owns its container; the ANSWERS in it are
   leaves and stay shared, because an answer is immutable once it has arrived.
   JS_NULL — not an empty Array — is "no second answer", which is every request that ever crosses this seam
   except the handful a forked peer answers, and it keeps the common case a tag test.
 *
 * `answerFixed` IS WHICH FLOW COLLECTS THEM, and without it the frontier doubles at every answer. An arm forked
 * over the peer's k-th answer holds the SAME request id — it has to, because the id lives in the step state
 * inside the frame it is a clone of — so a LATER answer would land on the arm's entry as well as on the
 * issuer's, and each would fork: three peer timelines would produce four flows, one of them a timeline that
 * answered B and then C at one call site, which no peer was ever in. The arm's answer is FIXED: it is the
 * timeline that took answer k, so a further answer is another peer timeline for the ISSUING flow to fork, never
 * for this one. It is SHARE because it belongs to the record: an arm's own later branch-siblings are fixed too
 * (they are that arm continued), and the issuer's branch-siblings are not (each of them asked and observed the
 * first answer, so each must fork over every other one). */
/* `doc` IS WHICH DOCUMENT OF THIS AGENT THE REPLY IS FOR, and it is on the record because for one kind the
 * reply is a PROGRAM: an injected <script src>'s body becomes a program of the flow that injected it, and a
 * program is compiled in the realm of the DOCUMENT it belongs to (solver/flow.h) — which for a script appended
 * into an iframe's tree is that frame's document and not the session's. It is SHARE because it belongs to the
 * record: an arm forked while the load is in flight is loading the same script into the same document. */
#define PENDING_FIELDS(X)                    \
    X(RESOLVE,    "resolve",   PEND_SHARE)   \
    X(VALUE,      "value",     PEND_SHARE)   \
    X(COMPLETION, "completion",PEND_SHARE)   \
    X(EXTRA,      "extra",     PEND_STRUCT)  \
    X(ANSWER_FIXED, "answerFixed", PEND_SHARE) \
    X(URL,        "url",       PEND_SHARE)   \
    X(HAVE_VALUE, "haveValue", PEND_SHARE)   \
    X(KIND,       "kind",      PEND_SHARE)   \
    X(SCRIPT_I,   "scriptI",   PEND_SHARE)   \
    X(REQ,        "req",       PEND_SHARE)   \
    X(OP,         "op",        PEND_SHARE)   \
    X(METHOD,     "method",    PEND_SHARE)   \
    X(HEADERS,    "headers",   PEND_STRUCT)  \
    X(BODY,       "body",      PEND_SHARE)   \
    X(DOC,        "doc",       PEND_SHARE)

enum {
#define PEND_ENUM(id, name, copy) PEND_##id,
    PENDING_FIELDS(PEND_ENUM)
#undef PEND_ENUM
    PEND_FIELD_COUNT
};

/* The session's context, for the reads and writes that have no call-site one — the preempt hook asks whether
   the running flow is blocked and has only the flow. The twins are cow_set_ctx and dom_cow_set_ctx, and this is
   named beside them for the same reason: a host that did not call it crashes at the first register touch. */
void pending_set_ctx(JSContext *ctx);
/* …and the context itself, for a caller that holds a register and no ctx of its own. It is NOT the scheduler
   session's: the WPT runner drives flows without ever opening a session, so a reader that reached for
   engine.c's `g_sess_ctx` there got NULL and freed a value through it — a SIGSEGV in 302 of 347 xhr runs, from
   the one line in this conversion that asked a second place for a fact this file already holds. */
JSContext *pending_ctx(void);
/* Release the atoms this file interned and forget that context — the frontier's teardown. The corpus hosts
   take a runtime down and bring another up per file, and an atom from the previous one is a handle into a
   freed table, so this is not tidiness: it is the same rule flow_registry_free keeps for the decision chain. */
void pending_free_ctx(JSContext *ctx);

/* HOW MANY ENTRIES. 0 for a register that has never held one, which is JS_UNDEFINED and answers with a tag
   test — the shape the per-opcode preempt hook needs, since most flows never park on anything. */
int  pending_count(JSValueConst reg);

/* Entry `i`, owned by the caller. */
JSValue pending_entry(JSValueConst reg, int i);

/* A field of an entry: the value (owned), or its integer coercion for the four numeric/boolean ones. */
JSValue pending_get(JSValueConst e, int field);
int64_t pending_get_int(JSValueConst e, int field);

/* IS THIS REGISTER HOLDING AN UNANSWERED SYNCHRONOUS REQUEST? The rendezvous scan, kept here rather than in
   flow.c because it is a question about the register's own contents and because the empty case has to stay a
   tag test — flow_blocked is asked at every suspend point the interpreter offers. */
int  pending_blocked(JSValueConst reg);

/* IS ANY ENTRY DELIVERABLE — and DELIVERABLE ASKS THE KIND, exactly as pending_blocked one line above does.
   The two predicates read the same register and only one of them was asking what an entry IS, and that
   asymmetry is the defect rather than an omission: a SYNCHRONOUS request's answer is TAKEN by the machine that
   asked (engine_host_take), never drained, so an answered HOSTREQ is not a reply anybody can deliver. Read
   without the kind it made the register "ready", flow_step called the fetch drain, and the drain swap-removed
   the answer and pushed it through a `resolve` capability the record does not have — the asking flow then
   parked at the call site that asked, forever, with its answer converted into somebody's fetch reply.
   THAT IS WHY THE SMOKE HOST PAYS ONLY AT A STALL. The shape is unreachable while the host is asked only when
   the whole frontier is blocked (the asking machine consumes its answer on the very next step), and it is
   reachable the moment the host pays per slice — which is the schedule §scheduler actually requires, and which
   run_scheduler had to leave switched off because of this line. */
int  pending_ready(JSValueConst reg);

/* IS THE HOST STILL OWED ANYTHING ON THIS REGISTER — the exact question `flow_set_host_owed`'s mark is a claim
   about, and the one `pending_count(reg) > 0` cannot answer. A register holding one ANSWERED entry has a
   non-zero count while the host owes it nothing, so a flow stuck on such an entry passes a count test and is
   marked "waiting on the host" forever: the mark is cleared only by a host event, and no host event is coming.
   Counting outstanding entries is what makes that state a crash at the mark instead of a flow that silently
   leaves the pick and never comes back. */
int  pending_outstanding(JSValueConst reg);

/* APPEND an entry of `kind` with every field present at its default (no URL, no answer, scriptI -1, req 0).
   Creates the register if this is the flow's first. Returns the new entry, OWNED by the caller. */
JSValue pending_push(JSValue *reg, int kind);

/* Set a field. `v` is consumed. `pending_set_int` is the same for the numeric ones. */
void pending_set(JSValueConst e, int field, JSValue v);
void pending_set_int(JSValueConst e, int field, int64_t v);
/* A field whose value is BYTES — the request body. Held as an ArrayBuffer, which is the honest shape for
   bytes that may not be text and the one XMLHttpRequest already uses for the same data. */
void pending_set_bytes(JSValueConst e, int field, const void *p, size_t n);

/* ANOTHER TRUE ANSWER TO THE SAME REQUEST — see PEND_EXTRA above. `value` is consumed. Recorded rather than
   delivered, because the arm that will carry it cannot be forked where an answer ARRIVES: that runs between
   scheduler steps, where the running flow, the applied delta and the live DOM head all still belong to some
   other flow. */
void pending_extra_add(JSValueConst e, int completion, JSValue value);
/* How many answers beyond the first this request has been given — 0 for every request nothing forked. */
int  pending_extra_count(JSValueConst e);
/* TAKE one, with its completion TYPE as the return value and its value into `*pvalue` (owned). It leaves the
   list, because the arm about to be forked over it is where it lives from now on — a pair read and left behind
   would be forked over twice, which is two flows exploring one peer timeline. */
int  pending_extra_pop(JSValueConst e, JSValue *pvalue);

/* A [name, value] LIST, for a request's headers: the same shape XMLHttpRequest's two header lists have. Built
   through these so this file needs no dependency on the browser half's HeaderList — the solver reaching into
   the browser for one struct is what made an earlier component's link drag the whole DOM in with it. */
JSValue pending_list_new(void);
void    pending_list_add_pair(JSValueConst list, const char *name, const char *value);

/* Remove entry `i` (swap-remove: the register is a set of outstanding requests, not an ordered queue). */
void pending_remove(JSValue *reg, int i);

/* Release the whole register. Idempotent; leaves *reg JS_UNDEFINED. */
void pending_free(JSContext *ctx, JSValue *reg);

/* THE SIBLING'S REGISTER at a fork: a new ARRAY naming the parent's RECORDS.
   The array is per-flow and must be — each arm removes an entry when IT delivers, and the host walks every
   flow's register from outside any flow's delta, so two flows sharing one array would each see the other's
   outstanding requests. The RECORDS are shared, because a record never changes after it is pushed except for
   the answer, which every arm waiting on that request genuinely observes. JS_UNDEFINED in gives JS_UNDEFINED
   out, which is the common case and costs nothing. */
JSValue pending_fork(JSValueConst reg);

/* GIVE THIS REGISTER ITS OWN COPY OF ENTRY `i`, replacing the shared record in the slot. Returns the copy,
   owned. There is exactly one field that must DIFFER between two arms — the unanswered synchronous request's
   rendezvous id, because its answer is computed under the ASKING FLOW'S WORLD — and a shared record cannot
   hold two of them, so it stops being shared first. The copy goes through PENDING_FIELDS and asserts the
   source's own-property count against it. */
JSValue pending_unshare(JSValueConst reg, int i);

/* THE CENSUS ROW (solver/cold.h): what this register would cost a pager. Now quickjs's bytes rather than the
   host allocator's, which is the point of the conversion — the strings are shared with everything else that
   names them. */
long pending_bytes(JSValueConst reg);

#endif
