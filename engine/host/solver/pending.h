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
#define FLOW_PENDING_RESOLVE   0   /* fetch(): call `resolve` with the reply */
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
#define PENDING_FIELDS(X)                    \
    X(RESOLVE,    "resolve",   PEND_SHARE)   \
    X(VALUE,      "value",     PEND_SHARE)   \
    X(URL,        "url",       PEND_SHARE)   \
    X(HAVE_VALUE, "haveValue", PEND_SHARE)   \
    X(KIND,       "kind",      PEND_SHARE)   \
    X(SCRIPT_I,   "scriptI",   PEND_SHARE)   \
    X(REQ,        "req",       PEND_SHARE)   \
    X(OP,         "op",        PEND_SHARE)   \
    X(METHOD,     "method",    PEND_SHARE)   \
    X(HEADERS,    "headers",   PEND_STRUCT)  \
    X(BODY,       "body",      PEND_SHARE)

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

/* Is any entry deliverable (its value has arrived)? */
int  pending_ready(JSValueConst reg);

/* APPEND an entry of `kind` with every field present at its default (no URL, no answer, scriptI -1, req 0).
   Creates the register if this is the flow's first. Returns the new entry, OWNED by the caller. */
JSValue pending_push(JSValue *reg, int kind);

/* Set a field. `v` is consumed. `pending_set_int` is the same for the numeric ones. */
void pending_set(JSValueConst e, int field, JSValue v);
void pending_set_int(JSValueConst e, int field, int64_t v);
/* A field whose value is BYTES — the request body. Held as an ArrayBuffer, which is the honest shape for
   bytes that may not be text and the one XMLHttpRequest already uses for the same data. */
void pending_set_bytes(JSValueConst e, int field, const void *p, size_t n);

/* A [name, value] LIST, for a request's headers: the same shape XMLHttpRequest's two header lists have. Built
   through these so this file needs no dependency on the browser half's HeaderList — the solver reaching into
   the browser for one struct is what made an earlier component's link drag the whole DOM in with it. */
JSValue pending_list_new(void);
void    pending_list_add_pair(JSValueConst list, const char *name, const char *value);

/* Remove entry `i` (swap-remove: the register is a set of outstanding requests, not an ordered queue). */
void pending_remove(JSValue *reg, int i);

/* Release the whole register. Idempotent; leaves *reg JS_UNDEFINED. */
void pending_free(JSContext *ctx, JSValue *reg);

/* THE SIBLING'S OWN REGISTER at a fork — a structural copy through PENDING_FIELDS. It is a COPY and not a
   shared reference because the host reads every flow's register from outside any flow's delta: two flows
   sharing one array would each see the other's outstanding requests, and neither could be answered
   independently. JS_UNDEFINED in gives JS_UNDEFINED out, which is the common case and costs nothing. */
JSValue pending_fork(JSValueConst reg);

/* THE CENSUS ROW (solver/cold.h): what this register would cost a pager. Now quickjs's bytes rather than the
   host allocator's, which is the point of the conversion — the strings are shared with everything else that
   names them. */
long pending_bytes(JSValueConst reg);

#endif
