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
 * whichever flows parked on a REQUEST, engine_host_requests joins what is outstanding across all of them, and
 * the preempt hook asks whether the running flow is blocked. A COW capture would make an entry's contents depend
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

/* WHAT THIS REQUEST IS EVIDENCE OF — CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's three provenances, recorded
 * on the record at the PUSH because a park is a work item and its provenance is one of the inputs it has to
 * take with it (§scheduler: "anything it reads back off the object it acts on is read at the wrong TIME"). A
 * flow that parks a request and takes a contradicted arm afterwards built that request on the path it had
 * THEN; asking the flow when the join runs would file it under a path it reached later.
 *   OBSERVED — a real load of this document makes exactly this request. It is the conjunction of two facts and
 *              neither alone is it: HTML §4.12.1 "The script element"'s parser-inserted flag (this register's
 *              FLOW_PENDING_DOCSCRIPT, whose address came out of bytes the trusted zone itself fetched) AND a
 *              path that has stood on no contradicted arm. A `document.write` on a forced arm produces a
 *              parser-inserted row too, in that flow's own sequence and in no real load of anything.
 *   DERIVED  — the page's own code computed it from real inputs. No session sent it; it is still a fact about
 *              the app, and it is the surface forced execution exists to find.
 *   FORCED   — the path took an arm the concrete example contradicts, so a value in this request exists only
 *              because a gate was forced. A reply to it is evidence about what a server says to a request no
 *              client makes, and §@H forbids it ever being reported as OBSERVED.
 * THE ORDER IS THE JOIN'S, and it is why these are numbered rather than named-only: engine_pending_fetches
 * dedups over the (method, url) pair and has to state the fact that survives every member of the set, which is
 * the MOST OBSERVED of them — the same rule, and the same reasoning, as the initiator's.
 * IT IS `PEND_SHARE`, for `doc`'s reason exactly: an arm forked while the load is in flight is the same
 * request, built on the prefix both arms share and before either of them had taken the branch. A sibling that
 * recomputed it from its OWN path would re-date the request to the fork. */
#define PROV_OBSERVED 0
#define PROV_DERIVED  1
#define PROV_FORCED   2
/* THE COMPOSITION, IN ONE PLACE, FROM THE TWO FACTS AND THEIR TWO OWNERS. The KIND is the record's own (only
   this register knows what a park is), and the forced mark is the FLOW's (only the solver knows what a path
   stood on); a caller states the half it owns and this states the other. It is not a policy — the engine
   decides nothing about firing here, it names what the request IS, and the trusted zone decides. */
int pending_prov_compose(int kind, int path_forced);

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
/* `answerWorld` IS WHICH OF THE PEER'S TIMELINES COMPUTED THE ANSWER IN `value`, and it is written in the same
   bracket for the same reason `completion` is: an answer that does not say which timeline produced it is
   INDISTINGUISHABLE FROM A SECOND COPY OF ANOTHER ONE, and a seam that cannot tell those apart cannot assert
   about either. That is not a hypothetical — with the answers anonymous, the routing zone kept whichever
   arrived last in a one-slot map and dropped the rest, so one page's single expression
   `(typeof w.closed) + ":" + w.closed` was answered `true` at one read and `false` at the next, out of two
   CONTRADICTORY timelines of one peer document, with nothing anywhere able to name the disagreement.
   It is the name of the ANSWERING flow's world in world_serialize's own grammar (solver/world.h), so its head
   is a peer timeline's identity that this agent can compare, park beside and — next — PIN a subsequent
   operation to. JS_NULL is "no answer yet, or an answer the trusted zone computed itself", which is a positive
   statement and not a hole: a zone-computed answer has exactly one producer and no timeline, which is what
   ENGINE_ANSWER_HOST says.
   SHARE, for `value`'s reason exactly: an answer that arrived before a fork was computed in a world both arms
   were in, and the timeline that computed it is the same fact about the same answer. */
/* `extra` IS EVERY OTHER TRUE ANSWER TO THIS ONE QUESTION, and it is a field rather than a second register
   because it belongs to the REQUEST. A peer's document state IS its flows, so a cross-agent operation is
   performed by every live timeline the peer has and each completes with its own answer: `otherW.length` has N
   answers for N peer timelines and all of them are true. The FIRST fills `value`/`completion`/`answerWorld`
   above; the rest land here, as [completion, value, world] TRIPLES — the same shape `headers` has one field
   wider, so pend_list_fork (which copies a tuple by its ARITY) already copies it — until the asking flow forks
   one arm per triple (engine.c's flow_answer_fork, which is the only reader). The world is on the triple and
   not beside it because it is that answer's, and an arm forked over answer k has to be able to say so.
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
/* `scriptEl` IS THE `script` ELEMENT WHOSE PROGRAM THE REPLY WILL BE, for that same kind and for the same
 * reason one field down: HTML §4.12.1.1 "Processing model"'s "execute the script element" is a switch on EL,
 * and its "classic" arm sets that document's §3.1.7 `currentScript` to it for the whole of the run. A park is
 * where the element would otherwise be lost — the flow leaves html_script_prepare with the node in hand and
 * comes back to a URL and a reply — so the element rides the record to the delivery, which puts it on the row.
 * IT IS THE ELEMENT'S WRAPPER AND NOT A POINTER, which is what makes it storable at all: this record is a JS
 * value, so a raw `lxb_dom_element_t *` in it would be a pointer inside a structure the runtime walks and the
 * cold tier measures. The wrapper is the node's ONE identity object (core/dom/node.h), so naming the node
 * through it costs nothing and keeps the record made of JS values. JS_UNDEFINED for a park no element caused.
 * SHARE, because it belongs to the record for the reason `doc` does: an arm forked while the load is in flight
 * is loading the same script for the same element. */
/* `declined` IS THE TRUSTED ZONE REFUSING TO ASK, WHICH IS AN ANSWER AND IS NOT A REPLY — the one thing this
 * record could not say and the reason a whole class of flow had nowhere to go.
 *
 * A refusal a REAL BROWSER also makes (a blocked scheme, a CORS failure, a CORB-blocked body) is Fetch §5.6
 * "Fetch methods"' network error and arrives here as one: `value` is JS_NULL, `haveValue` is set, and the flow
 * resumes down its failure path having been told the truth. A refusal only THIS TOOL makes — the firing policy
 * declining to spend an act at an unwidened origin, the destructive deny list refusing to send a session-ending
 * GET, a method the chokepoint cannot issue — has no browser behind it, so there is no reply to write and
 * `haveValue` stays clear: the flow stays PARKED, which is what §@S requires of a search not yet solved.
 * IT IS A SEPARATE FIELD FROM THE ANSWER BECAUSE IT IS A DIFFERENT FACT AND THE PAIR IS ASSERTED. `haveValue`
 * says a reply exists; this says the question will not be asked. A record carrying both would be a flow told
 * the server answered AND that nobody asked it, and the two send it in opposite directions.
 * SHARE, for `value`'s reason exactly: the refusal is a fact about the REQUEST — the address, its method, its
 * provenance and this session's per-origin policy — so it is true of every arm parked on it, including arms
 * forked before the refusal arrived. It is written ONCE, by engine_decline, and the host that writes it is the
 * only party that knows the rule that fired (extension/lib/safe-fetch.js grades its own refusal).
 * ITS REASON IS THE ZONE'S OWN WORDS AND IS THE ONLY ACCOUNT ANYBODY GETS of a request nobody made, which is
 * why it is the field's VALUE rather than a boolean beside a reason kept elsewhere: JS_NULL is "this zone has
 * refused nothing", and a string is the refusal with the sentence that explains it. */
/* `declineTaken` IS WHETHER *THIS REGISTER'S* NAMING OF THAT RECORD HAS ALREADY FORKED ITS FAILURE ARM, and
 * without it the decline is a SPIN rather than a fork.
 *
 * A declined request has TWO feasible outcomes and this engine must explore both (CLAUDE.md §Solver-half:
 * where the domain permits both, BOTH arms run). One arm goes on WAITING — it is the success arm, and it is
 * un-fabricated: it holds no reply, invents no status and no bytes, and it fires the day the origin is widened.
 * The other takes §5.6's network error and runs the page's `.catch`, which is where bundles keep their fallback
 * endpoints, their retry hosts and their degraded-mode configuration. flow_decline_fork builds that pair, and
 * having built it once for a register it must never build it again: a second arm would be a byte-identical
 * timeline of the same flow, which is duplication and not exploration.
 * IT IS PER REGISTER AND NOT PER RECORD, WHICH IS WHY THE FORK UNSHARES BOTH SIDES. pending_fork SHARES a
 * record between an arm and its parent, so N flows can be parked on ONE record — and each of them is a
 * DIFFERENT timeline that diverged after the park, so each owes its own failure arm. A mark written on the
 * shared record would give the first flow to step its arm and silently deny every other one, which is
 * §scheduler's razor exactly ("drops, starves, skips … ANY flow"). So each side of the fork takes a private
 * copy of the record and marks it, the shared original loses those two namings, and when the last register has
 * taken its decline the record leaves the frontier's outstanding set on its own — which is also what stops the
 * host being shown, and re-declining, a request it has already refused.
 * SHARE, for `answerFixed`'s reason exactly: an arm forked AFTER the decline was taken is that timeline
 * continued, and its own failure path is explored by the failure arm's own branch-siblings rather than by a
 * second arm of the same decline. */
/* AND THE FOURTH COLUMN IS THE FIELD'S "NOTHING YET", WHICH IS WHY THIS TABLE IS FOUR COLUMNS AND NOT THREE.
 * Three of the four things a field obliges were already derived from this list — the id, the interned name, and
 * what the fork does with it — and the fourth, its value at the push, was a SEPARATE hand-written sequence of
 * `pend_put` calls in pending.c. A list beside a list is a list that goes out of step, and this one did: the
 * `scriptEl` field above was added to this table, to the copy and to the census, and NEVER to the push — so
 * every record ever pushed carried one field fewer than PENDING_FIELDS names, for many commits, and the assert
 * written to catch exactly that (pend_entry_copy's own-property count) said nothing because the only paths that
 * COPY a record are the two forks, and until a peer's second answer could be delivered at all neither of them
 * ran on a record of the affected kinds. A latent hole plus an unreachable check reads exactly like a healthy
 * subsystem.
 * SO THE DEFAULT IS DECLARED HERE, WITH THE FIELD, and pending_push expands this list instead of restating it.
 * The expressions are evaluated in pending_push's scope — `kind` and `path_forced` are its arguments and
 * `pend_ctx()` is the session's context — which is the one place this column is ever expanded with a value;
 * a field whose default is a fact only the CALLER holds becomes a parameter there rather than a second write
 * the push sites are each asked to remember, which is the same "no list beside a list" rule one line up; that coupling is the
 * price of there being no second list, and it is stated rather than left to be discovered. Each default is a
 * POSITIVE statement of "nothing yet" (§Architecture: a consumer never defaults a producer's field, so the
 * producer states the absence), never a hole a reader fills in. */
#define PENDING_FIELDS(X)                    \
    X(RESOLVE,    "resolve",   PEND_SHARE,  JS_UNDEFINED)                              \
    X(VALUE,      "value",     PEND_SHARE,  JS_UNDEFINED)                              \
    /* no answer yet, so no completion type and no answering timeline */               \
    X(COMPLETION, "completion",PEND_SHARE,  JS_UNDEFINED)                              \
    X(ANSWER_WORLD, "answerWorld", PEND_SHARE, JS_NULL)                                \
    /* …and no SECOND answer, which is the common case and stays a tag test */         \
    X(EXTRA,      "extra",     PEND_STRUCT, JS_NULL)                                   \
    /* this flow ISSUED the request, so it is the one that forks an arm per answer */  \
    X(ANSWER_FIXED, "answerFixed", PEND_SHARE, JS_FALSE)                               \
    X(URL,        "url",       PEND_SHARE,  JS_NULL)                                   \
    X(HAVE_VALUE, "haveValue", PEND_SHARE,  JS_FALSE)                                  \
    X(KIND,       "kind",      PEND_SHARE,  JS_NewInt32(pend_ctx(), kind))             \
    /* what this request is evidence of, composed from the kind and the pusher's path */ \
    X(PROV,       "prov",      PEND_SHARE,                                             \
      JS_NewInt32(pend_ctx(), pending_prov_compose(kind, path_forced)))                \
    X(SCRIPT_I,   "scriptI",   PEND_SHARE,  JS_NewInt32(pend_ctx(), -1))               \
    /* §4.12.1.1's NULL type: a park owing a PROGRAM with no type crashes at the delivery */ \
    X(SCRIPT_TYPE, "scriptType", PEND_SHARE, JS_NewInt32(pend_ctx(), SCRIPT_TYPE_NONE))  \
    X(REQ,        "req",       PEND_SHARE,  JS_NewInt64(pend_ctx(), 0))                \
    X(OP,         "op",        PEND_SHARE,  JS_NULL)                                   \
    X(METHOD,     "method",    PEND_SHARE,  JS_NULL)                                   \
    /* no DESTINATION STATED, which the join refuses to list rather than classifying */ \
    X(DESTINATION,"destination",PEND_SHARE, JS_NULL)                                   \
    X(HEADERS,    "headers",   PEND_STRUCT, JS_NULL)                                   \
    X(BODY,       "body",      PEND_SHARE,  JS_NULL)                                   \
    /* 0 is "this park's reply is not a program", which is every kind but the injected <script src> */ \
    X(DOC,        "doc",       PEND_SHARE,  JS_NewInt32(pend_ctx(), 0))                \
    X(SCRIPT_EL,  "scriptEl",  PEND_SHARE,  JS_UNDEFINED)                              \
    /* the zone has refused nothing about this request — see the two fields below */   \
    X(DECLINED,   "declined",  PEND_SHARE,  JS_NULL)                                   \
    X(DECLINE_TAKEN, "declineTaken", PEND_SHARE, JS_FALSE)

enum {
#define PEND_ENUM(id, name, copy, dflt) PEND_##id,
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

/* HOW MANY OF ONE KIND, and it is a KIND question because a departing flow takes TWO different debts with it
   and one number cannot say which. A sold flow's fetch replies are paired by (method, url) against the
   host's own list (engine_take_paged_owed), and its synchronous requests are routed BY ID to a call site — so
   a register counted whole spends a fetch's credit on a request's departure, and the pairing assert that
   exists to catch a host naming a request nobody parked on is then excused by a HOSTREQ that has nothing to
   do with it.
   IT COUNTS ANSWERED ENTRIES TOO, AND FOR THE REQUEST DOOR THAT IS THE RIGHT NUMBER — see the arity below. */
int  pending_count_kind(JSValueConst reg, int kind);

/* HOW MANY REPLIES THE HOST STILL OWES THIS REGISTER — the reply door's half of the same question, and the one
   the pager CREDITS (engine.c's g_paged_owed). It is not `pending_count_kind` with the HOSTREQ kinds taken
   off, because the two doors have different ARITY and the answered entries are exactly where they differ:
     - THE REPLY DOOR ANSWERS ONCE. engine_pending_fetches drops a request from the join the instant it carries
       a value, and engine_provide DFAILs on a host that answers one twice — so an ANSWERED fetch/script/module
       entry can never receive another reply, and crediting it tells the host a reply is owed for something
       already delivered. The debt is then larger than the number of replies that can arrive, and the surplus is
       spent by the next reply the host GENUINELY mispaired — silencing qjs_provide's pairing assert, which is
       the one thing standing between a flow parked forever and a reader who can see why.
     - THE REQUEST DOOR ANSWERS N TIMES. A peer's document state IS its flows, so one request id has one answer
       per peer timeline (PEND_EXTRA) and an ANSWERED HOSTREQ entry is still a rendezvous more answers may
       arrive for. That is why the request side stays a plain kind count above.
   An entry is owed exactly when it carries no value, which is the same predicate `pending_outstanding` and the
   join itself read — one condition, one place, so the credit and the list the host is shown cannot drift. */
int  pending_owed_replies(JSValueConst reg);

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
   asked (engine_host_take), never delivered, so an answered HOSTREQ is not a reply anybody can deliver. Read
   without the kind it made the register "ready", flow_step called the reply delivery, and it swap-removed
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

/* IS ANY ENTRY OF ONE KIND STILL OWED — the missing ARITY of the two questions above, and the one a caller
   that cares WHICH debt it is holding has to be able to ask. `pending_count_kind` asks the kind and counts
   ANSWERED entries too (which is right for the request door); `pending_outstanding` asks "owed" and not the
   kind. A caller that needs both was left to re-derive "owed" for itself, and pend_owed's own comment above it
   says why that must not happen: what differs between callers is which KINDS they ask about, never what
   "owed" means, and a second spelling of it is the drift that comment exists to prevent.
   THE CALLER THIS EXISTS FOR IS HTML §13.2.7 "The end" step 8 — "nothing that delays the load event" — which
   is a question about WHICH replies are outstanding and not how many: a `<script src>` still in the air delays
   a document's `load` (§4.12.1.1 "Processing model": "Whenever a script element el's delaying the load event is
   true, the user agent must delay the load event of el's preparation-time document") and a `fetch()` or a
   dynamic `import()` does not, so one number over the whole register cannot answer it. */
int  pending_outstanding_kind(JSValueConst reg, int kind);

/* HAS THE TRUSTED ZONE REFUSED THIS ENTRY — the `declined` field read through its own vocabulary, so no caller
   ever spells the JS_NULL-versus-string test for itself, and so the field's two legal shapes are asserted in
   ONE place rather than at each reader.
   IT IS ASKED OF AN ENTRY AND NOT OF A REGISTER, which is a statement about who needs it. `pending_outstanding`
   stays TRUE for a refused entry and must: the flow is parked at the line that asked, so a predicate answering
   "not outstanding" would let it read as FINISHED and tear its timeline down. What changes is only what the
   HOST can still be asked, and every one of those readers — the join, the reply debt, the fork — is standing at
   one entry when it asks. */
int  pending_entry_declined(JSValueConst e);

/* APPEND an entry of `kind` with every field present at its default (no URL, no answer, scriptI -1, req 0).
   Creates the register if this is the flow's first. Returns the new entry, OWNED by the caller.
   `path_forced` IS THE PUSHING FLOW'S OWN (flow_path_forced), and it is a PARAMETER rather than a lookup for
   two reasons that both matter: this file is below flow.c and must not reach up into it, and the provenance is
   a fact about the path AT THE PUSH — a parameter is what makes "read it now, not later" a thing the compiler
   enforces at every park site instead of a rule each one is asked to remember. */
JSValue pending_push(JSValue *reg, int kind, int path_forced);

/* Set a field. `v` is consumed. `pending_set_int` is the same for the numeric ones. */
void pending_set(JSValueConst e, int field, JSValue v);
void pending_set_int(JSValueConst e, int field, int64_t v);
/* A field whose value is BYTES — the request body. Held as an ArrayBuffer, which is the honest shape for
   bytes that may not be text and the one XMLHttpRequest already uses for the same data. */
void pending_set_bytes(JSValueConst e, int field, const void *p, size_t n);

/* ANOTHER TRUE ANSWER TO THE SAME REQUEST — see PEND_EXTRA above. `value` is consumed; `world` is the
   ANSWERING timeline (world_serialize's grammar) and is copied. Recorded rather than delivered, because the arm
   that will carry it cannot be forked where an answer ARRIVES: that runs between scheduler steps, where the
   running flow, the applied delta and the live DOM head all still belong to some other flow. */
void pending_extra_add(JSValueConst e, int completion, JSValue value, const char *world);
/* How many answers beyond the first this request has been given — 0 for every request nothing forked. */
int  pending_extra_count(JSValueConst e);
/* TAKE one, with its completion TYPE as the return value, its value into `*pvalue` and the timeline that
   computed it into `*pworld` (both owned). It leaves the list, because the arm about to be forked over it is
   where it lives from now on — a triple read and left behind would be forked over twice, which is two flows
   exploring one peer timeline. */
int  pending_extra_pop(JSValueConst e, JSValue *pvalue, JSValue *pworld);

/* HAS THIS PEER TIMELINE ALREADY ANSWERED THIS REQUEST — asked of the first answer's `answerWorld` and of every
   triple in `extra`. It is the question that separates the two things an arriving answer can be: another of the
   peer's N timelines, which is a fork the asking flow owes, and the SAME timeline's answer delivered twice,
   which is a relay that duplicated and would fork an arm into a timeline the peer was never in twice over. Only
   the deliverer can tell them apart and only with this, which is why the world rides the answer. */
int  pending_answer_world_seen(JSValueConst e, const char *world);

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
   source's own-property count against it.
   AND THE SECOND KIND OF RECORD THAT MUST STOP BEING SHARED IS A DECLINED ONE, for the same sentence about a
   different field. The rule this admits under is not "which kind" but WHAT THE HOST CAN STILL DO: the copy
   leaves the frontier's outstanding set while the original stays in it, so it is sound exactly when no reply
   is coming for the original — which is what a HOSTREQ record is (it is keyed by request id and was never in
   that set) and what a DECLINED record is (this zone has said it will not ask). A record of any other kind
   copied here would have the host answer the original while the arm holding the copy waited for the rest of
   the session with nothing anywhere to say so; a declined one waits for the rest of the session too, and its
   `declined` reason is the something that says so. */
JSValue pending_unshare(JSValueConst reg, int i);

/* THE CENSUS ROW (solver/cold.h): what this register would cost a pager. Now quickjs's bytes rather than the
   host allocator's, which is the point of the conversion — the strings are shared with everything else that
   names them. */
long pending_bytes(JSValueConst reg);

#endif
