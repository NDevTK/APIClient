/* THE REQUEST INTERFACE — WHATWG Fetch §5.4 "Request class". See request.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_REQUEST_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_REQUEST_H
#include <stdbool.h>

#include "quickjs.h"
#include "core/idl_args.h"

void request_init(JSContext *ctx);
/* Fetch §5.4 "Request class"' prototype AND its interface object, for ONE realm — Web IDL §3.8 "Platform
   objects implementing interfaces" is given a realm and names no Document, and §5.4 is
   `[Exposed=(Window,Worker)]`, so the name is owed by a realm that reaches no per-document install. */
void request_install_proto(JSContext *ctx);
/* THE AGENT'S half of §5.4, run from core/platform.c's RELEASE COLUMN — it takes no JSContext because it
   reads none: the prototype and the interface object are the REALMS' and go with their contexts. */
void request_free(void);

/* IS THIS VALUE A Request — the BRAND, which is what Web IDL's `RequestInfo = Request or USVString` union
   resolves on. A union member that is an interface type matches a PLATFORM OBJECT OF THAT INTERFACE and
   nothing else; every other value goes to the USVString member and is converted. `JS_IsObject` is not that
   test and the difference is not pedantic — see the FETCH_INPUT_URL stage in fetch.c, where it sent a CONCOLIC
   URL down the Request arm and made every attacker-shaped endpoint report one property name too deep. The
   class id is this file's, so the answer lives here rather than being re-derived by each caller. */
bool request_is(JSValueConst v);

/* §5.4's captured blob URL entry, or JS_UNDEFINED — the Blob a Request built from a `blob:` URL holds, so
   revoking the URL afterwards does not stop that request. Borrowed. */
JSValueConst request_blob_entry(JSValueConst v);

/* §5.4 step 25 — "if method is not a method or is a forbidden method, throw a TypeError", then "normalize
   method" — as ONE operation, because `fetch(input, init)` performs §5.4 inline and needs the same answer.
   Returns the normalized method, which the caller releases with js_free, or NULL with a TypeError live. */
char *request_method_check(JSContext *ctx, const char *m);

/* ---- Fetch §2.2.5 "Requests"' REQUEST, as the record BOTH of §5's entry points build ---------------------
 *
 * WHY THIS IS A RECORD AND NOT NINE FIELDS ON ONE INTERFACE'S PRIVATE STRUCT. Fetch §5.6 "Fetch methods"'
 * `fetch(input, init)` step 2 is "Let requestObject be the result of invoking the initial value of Request as
 * constructor with input and init as arguments", so the two entry points do not merely resemble each other —
 * one IS the other, and every member of §5.4's `RequestInit` reaches the network through the same forty-two
 * steps. `fetch()` performed those steps INLINE over three hand-read properties (`body`, `headers`, `method`)
 * and DROPPED the other thirteen the dictionary declares, so `fetch(u, {credentials: "include"})` built a
 * request with credentials mode "same-origin", `{mode: "navigate"}` built one where a browser throws, and
 * `{cache: "only-if-cached"}` built one §5.4 step 21 refuses. Those are WRONG ANSWERS and not missing
 * features, and the reason they could exist at all is that the second copy of §5.4 had no way to be told the
 * first had grown a step.
 *
 * SO THE MEMBERS ARE APPLIED ONCE, HERE, AND THE RECORD IS WHAT BOTH CALLERS HOLD. `request_init_apply` is
 * §5.4 steps 10-27 and nothing else: the URL (steps 5-6), the header list (steps 31-33), the body
 * (steps 34-42) and the signal (steps 26/29-30) are each their own thing and each caller states its own,
 * because the two differ there and the difference is real — see request_init_apply.
 *
 * WHAT IS NOT ON IT: the request's URL. §2.2.5 gives a request one, and this engine cannot yet put it here —
 * see request_init_apply's residual. */
/* THE NINE ARE JSValues AND NOT `char *`, WHICH IS AN OWNERSHIP FACT BEFORE IT IS A CONCOLIC ONE.
 * A step machine's state is BYTE-COPIED at a deep fork (`tramp_step_state_clone`: `memcpy` and then the
 * declared slots taken a second time through `visit`), so a heap pointer on a record a machine carries BY
 * VALUE is held by two arms and freed twice, while a JSValue the machine's `visit` names is RE-TAKEN. That is
 * the whole of why core/fetch/fetch.c's `js_fetch_unforkable` had to REFUSE a fork while this record was
 * filled, and refusing a fork is a flow dropped. THAT REFUSAL IS DELETED and the function name greps to
 * nothing — this record's nine fields were the FIRST of its three terms to be declared, and the last (§5.4
 * step 33's header list) is declared through quickjs-step.h's `tree` operation; the block above
 * `js_fetch_decl` carries all three. The ownership fact this paragraph is about is unchanged, which is why it
 * is rewritten rather than dropped: it is why these nine are JSValues. There is exactly one list of what a state owns — the
 * declaration — and `request_record_visit` puts these nine on it.
 * AND IT IS WHAT §5.4 STEP 25 NEEDS: "Set request's method to method" over a method that is UNKNOWN EXTERNAL
 * INPUT has nowhere to go in a `char *`, because the ToString boundary owes C real bytes. A JSValue field is
 * NECESSARY for that and is not SUFFICIENT — step 25.2's "is not a method or is a forbidden method" is a
 * PREDICATE over those bytes, which an unknown cannot answer, so the step still owes a fork; see
 * `request_init_apply`'s own block on it. This conversion is the storage half and says so. */
typedef struct {
    JSValue method;          /* §5.4 step 25, normalized */
    JSValue mode;            /* "cors" | "no-cors" | "same-origin" | "navigate" */
    JSValue credentials;     /* "omit" | "same-origin" | "include" */
    JSValue cache;
    JSValue redirect;
    JSValue referrer;
    JSValue referrer_policy;
    JSValue integrity;
    JSValue destination;     /* §2.2.5: "" for a request a script constructed */
    int     keepalive;
} RequestRecord;

/* Every field JS_UNDEFINED and keepalive 0 — the state `request_init_apply` requires, and the state a failed
   apply leaves behind, so a free or a visit over a record that was never filled is a no-op.
   IT IS NINE ASSIGNMENTS AND NOT A `memset`, AND THAT IS THE ONE THING TO GET RIGHT HERE: a zeroed JSValue is
   the INTEGER 0 and not JS_UNDEFINED, so a record a `js_mallocz` produced reads as nine filled slots holding
   the number zero. core/fetch/fetch.c states the same rule twice over its own `captured` flag and its
   `body.stream` term for exactly this reason — "is this slot filled yet" asked of the slot is never the
   question. A HOLDER THAT ALLOCATES ITSELF ZEROED THEREFORE STILL CALLS THIS before its first failure path:
   a free over integer 0 is harmless and a READ of it is not, and `request_str_is` asserts the difference. */
void request_record_init(RequestRecord *rec);
/* THE DECLARATION — the nine slots a step machine carrying this record BY VALUE owns, handed to the one list
   quickjs-step.h's `visit` contract is about. A machine embedding a RequestRecord calls this from its own
   `visit` and then MUST NOT free the record: idl_args.c's release fold folds every declared slot's identity on
   each side of a member's `release` and aborts when one moved, because a second list beside the declaration
   leaks whatever the next field misses and double-frees whatever this one did not null. */
void request_record_visit(JSContext *ctx, RequestRecord *rec, JSStepVisit *v);
/* THE SAME NINE, FOR A HOLDER THAT IS A CLASS OPAQUE RATHER THAN A STEP STATE — the collector cannot see
   through an opaque, so a Request's own record is MARKED here, FREED by `request_record_free` in the finalizer
   and DUP'd by `request_record_copy` at clone. Those three sites are one obligation and are read together,
   exactly as the Request's `headers`/`blob_entry`/`signal` trio is.
   IT MARKS NOTHING WHILE ALL NINE ARE STRINGS, which is today: `JS_MarkValue` reaches the collector only for
   JS_TAG_OBJECT and JS_TAG_FUNCTION_BYTECODE. It is not therefore dead code to delete — the field that makes it
   fire is §5.4 step 25's method the day it can hold a CONCOLIC, which is a class-opaque OBJECT — and it is not
   a mechanism to rely on yet either. The caller's site states both halves. */
void request_record_mark(JSRuntime *rt, const RequestRecord *rec, JS_MarkFunc *mark_func);
/* Release every value the record owns and re-initialize it. Takes the RUNTIME because a finalizer has one and
   has no context.
   FOR A CLASS-OPAQUE HOLDER ONLY. A STEP MACHINE'S record is discharged by the driver through
   `request_record_visit`, so a machine's `release` that also called this would free each slot twice — which is
   not a leak to be found later but the assert named above, firing on the one place every member's teardown
   converges on. */
void request_record_free(JSRuntime *rt, RequestRecord *rec);
/* §5.4 step 12's carry-forward as a copy of the record — a second reference to each of the nine, since a
   JSValue string is immutable and the two records are two requests' answers to the same question rather than
   two buffers. VOID, because a dup allocates nothing: the `-1 on OOM` this used to return was real when every
   field was a `js_strdup`, and an int return no caller can act on is an invitation to write an `if` with no
   arm — so the signature states that there is no failure to handle rather than leaving a reader to find out. */
void request_record_copy(JSContext *ctx, RequestRecord *dst, const RequestRecord *src);
/* THE RECORD'S OWN BYTE COMPARISON — is this field's value the byte sequence `lit`.
 *
 * IT EXISTS BECAUSE THE NINE FIELDS BECAME VALUES AND §5.4 STILL COMPARES THEM TO SPEC CONSTANTS: step 17's
 * "navigate", step 21's "only-if-cached" against "same-origin", step 32.1's CORS-safelisted method set, step 35's
 * `GET`/`HEAD` and step 39.2's "same-origin"/"cors". Those were `strcmp` over a `char *` and the comparison is
 * the same comparison; what is new is that the operand can now BE unknown external input, and a byte test over
 * an unknown is the one answer §@H forbids — it would decide a gate by running a predicate over a string no
 * run ever computed. So this operation ASSERTS its operand is a real string and names the fork that is owed,
 * which is the refusal moving from "there is nowhere to store it" to "there is nobody to ask": the storage half
 * of §5.4 step 25 is this record and the DECIDING half is `request_init_apply`'s own residual.
 * NOT a general-purpose helper and not in a shared header: it is the record's question, over the record's
 * fields, and the assert is only sound because those nine are the ones whose provenance this file states. */
bool request_str_is(JSContext *ctx, JSValueConst v, const char *lit);
/* A Request's own record, or NULL for a value that is not one — §5.4 step 6's "Set request to input's
   request", as the read its two callers make on a `RequestInfo` that took the interface arm. Borrowed. */
const RequestRecord *request_record_of(JSValueConst v);

/* FETCH §5.4 new Request(input, init) STEPS 10-27, over the CONVERTED `RequestInit` — the ONE application of
 * that dictionary's members, reached by §5.4's constructor and by §5.6's `fetch(input, init)`.
 *
 * `init` is the dictionary the ARGUMENT MACHINE built (Web IDL §3.2.17 Dictionary types), never the object the
 * page passed, so this function runs none of the page's code: every member has already been read in
 * lexicographical order, coerced to its declared type and refused if §3.2.18 says so. That is the whole reason
 * a caller may run all eighteen steps in one stage.
 *
 * `from` is step 12's carry-forward — the input Request's record, or NULL for a string input, which is what
 * decides every default: §5.4 spells each of these "If init[member] exists, then set request's <field> to it"
 * over a request that already holds the input's value, so a constant in the default position is a statement
 * that the page asked for it and for a Request input that statement is false.
 *
 * Returns 0, or -1 with the TypeError §5.4 states live: step 10's `window`, step 17's "navigate" mode, step
 * 21's "only-if-cached", and step 25's method. `rec` is left safe to free either way.
 *
 * IT ALSO RETURNS -1 FOR A MEMBER THIS ENGINE CANNOT YET APPLY, which is a different thing from a step's own
 * refusal and is stated here because a caller cannot tell them apart and must not try: a member whose value is
 * UNKNOWN EXTERNAL INPUT aborts in a dev build at the step that reads it, naming what to build, and throws a
 * TypeError in release. §5.4's ORDER decides which member reports, exactly as it decides which TypeError a
 * page sees when two members are bad at once.
 *
 * NAMED RESIDUAL: the request's URL is not on this record, so §5.6 step 2's literal [[Construct]] — which
 * would delete `fetch()`'s remaining copy of steps 5-6 as this deletes its copy of 10-27 — cannot be built
 * yet. §5.4 step 5 parses the input STRING, and a `fetch('/api/u?uid=' + state.id)` URL is UNKNOWN EXTERNAL
 * INPUT: `JS_ToCStringLen` on one aborts in `js_force_tostring` by design, because that boundary owes C real
 * bytes and a concolic cannot ride a `const char *`. So `new Request(concolicUrl)` takes the whole flow down
 * today, and routing `fetch()` through the constructor would take the tool's headline surface with it. WHAT
 * THE NEXT DIFF BUILDS: §2.2.5's URL on this record as a JSValue rather than a `char *`, so it can BE the
 * concolic — with `Request.url`'s getter answering it (opaque-infectious, as `location.href` is) and the
 * serialized form derived at the one C consumer that needs bytes. ITS ABSENCE SHOWS as
 * `new Request(location.hash)` aborting the agent where `fetch(location.hash)` records an endpoint. */
int  request_init_apply(JSContext *ctx, JSValueConst init, const RequestRecord *from, RequestRecord *rec);

/* `dictionary RequestInit` ITSELF — the declaration table, so §5.6's `fetch(input, init)` declares the SAME
   one §5.4's constructor does. A second hand-written member list is exactly how thirteen of them went missing
   from one entry point and not the other, so there is one table and two declaration sites. `*n` is its
   length; the array outlives the declaration (it is a file static of request.c, which is what
   `idl_method_id_step` borrows). */
const IdlDictMember *request_init_members(int *n);

/* §5.1's "method": an RFC 7230 token. Public because the answer to "is this a method" and the answer to "is
   this a FORBIDDEN method" carry DIFFERENT errors in a second standard: XHR §3.5.1 open() throws a
   "SyntaxError" for the first and a "SecurityError" for the second, where §5.4 throws one TypeError for both.
   So the grammar is shared and the error stays each standard's own. */
bool request_method_is_token(const char *m);

#endif
