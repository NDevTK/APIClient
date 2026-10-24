/* PERFORMING A CROSS-AGENT OPERATION — the peer's half of remote_object.c, in the instance that HOLDS the
 * object.
 *
 * remote_object.c is the ASKING half: it encodes an operation, parks the flow on it, and raises the peer's
 * completion at the call site that asked. This file is what the other end does with that record, and until now
 * it existed once, inside a host's serve loop (wpt_runner.c), where the production entry could not reach it —
 * so the shipped ABI had a sender with no receiver. A second copy written for the second host is the dual
 * system CLAUDE.md forbids: the record's field layout, its operand encoding and the program that performs it
 * are one grammar, and two spellings of a grammar are two grammars.
 *
 * A PEER ANSWERS BY RUNNING A PROGRAM, never by reading a property from C. Every one of these operations is
 * the page's own code — an IDL getter for `otherW.length`, a page's setter for a [[Set]], a page's function
 * for a [[Call]] — and a C activation has no flow base under it, so a loop inside one would drive to
 * completion instead of parking. That is why this file hands back a PROGRAM and its operands rather than a
 * value: what runs it is the receiving flow, on the one frontier, preemptible and parkable at any depth.
 *
 * EVERY OPERAND REACHES THE PROGRAM THROUGH A SLOT, the property name included. Splicing a name into the
 * program's text inside quotes is a property name READ AS CODE the first time one carries a quote or a
 * backslash.
 *
 * AND THE TWO OPERATIONS THAT NEED AN INTRINSIC TAKE THIS REALM'S. `Reflect` is a global the page may replace,
 * so a peer performing a cross-agent [[Set]] through the page's own function would report a write that never
 * happened and would run the page's code where the spec puts an internal method. Three of the five have a
 * pure-syntax form nothing can intercept — `o[k]` IS 10.1.8, a SLOPPY-mode `delete o[k]` IS 10.1.10 and
 * yields exactly the boolean 10.5.10 step 8 asks the trap for, and `k in o` IS 10.1.7 by 13.10.1's own step
 * ("Return ? HasProperty(rightValue, ? ToPropertyKey(leftValue))") — and the two that do not (an assignment
 * expression discards the boolean 10.1.9 completes with; a call needs its argument list spread) reach the
 * operation through %Reflect.set% / %Reflect.apply%, captured PER REALM before that realm's scripts run. Per
 * realm because §3.7 gives every realm its own, and a module static would answer every document's operation
 * out of whichever realm happened to be built first. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_OP_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_REMOTE_OP_H

#include "quickjs.h"

/* Declared once per agent: the two intrinsic slots, and the per-realm capture that fills them. */
void remote_op_init(JSContext *ctx);
/* The AGENT's half, undone — core/platform.h's release column. NOT `remote_op_free`, which is further down and
   is a different lifetime entirely: that one frees ONE parsed record, once per operation performed. */
void remote_op_agent_free(void);

/* ONE RECORD, PARSED ONCE. The field COUNT and the field LAYOUT are both facts about the operation and are
   stated beside it, because a record shorter than its operation used to be SKIPPED — which parks the asking
   flow on an answer that never comes and reports nothing at all. */
typedef struct RemoteOp RemoteOp;

/* `record` is the text the asking instance emitted, verbatim, tab-separated:
     windowproxy.get   <doc> <world+ancestry> <marks> <addressee> <member>
     windowproxy.close <doc> <world+ancestry> <marks> <addressee>
     object.get        <doc> <world+ancestry> <marks> <addressee> <generation>:<id> <key>   (and .delete, .has)
     object.set        <doc> <world+ancestry> <marks> <addressee> <generation>:<id> <key> <value>
     object.apply      <doc> <world+ancestry> <marks> <addressee> <generation>:<id> <thisArg> <arg>*
   An export id indexes one session's table (remote_object.h) and is in range in every session of the
   document, so the object is named by (generation, id). The first four fields are the transport's: which
   instance, whose timeline asks, the asking flow's path marks (solver/engine.h's engine_record_head), and which
   of the receiver's timelines the asker is already in; the first three are a routed delivery's, so one router
   carries both. The addressee sits at index 4, after the marks and before every operand, because
   engine/trusted.mjs and extension/bridge.js read [1] and engine/route.mjs reads
   [1] and [2] positionally, and they run from the tree while this C runs only after a build; index 4 is also
   the only fixed slot every verb has (`object.apply`'s tail is variadic). Crashes on a verb this agent does
   not perform: an unanswered record parks the asking flow for ever. */
RemoteOp *remote_op_parse(const char *record);
/* Whether `record`'s verb is one of the operations above, read off the verb alone and never crashing: an asking
   instance's own request register holds these beside its document loads, and a refused one is told apart by
   it (solver/engine.c's flow_decline_fork). */
int remote_op_names_operation(const char *record);
/* WHOSE TIMELINE THE ANSWER IS TRUE IN — the asking flow's world and its ancestry, in world.h's wire form. The
   caller installs the segment before it runs the program; it is not this file's to install, because which
   delta an operation runs against is the scheduler's question and not the grammar's. */
const char *remote_op_worlds(const RemoteOp *op);
/* WHICH DOCUMENT of the receiving agent owns the object — the record's first operand. */
const char *remote_op_doc(const RemoteOp *op);
/* WHICH OF THIS AGENT'S OWN TIMELINES THE ASKER IS ALREADY IN — a world vector of THIS instance's forest,
   read back off the answer that flow took from one of our flows, or solver/engine.h's ENGINE_ADDRESSEE_NONE
   when it has taken none. BORROWED from the parsed record.
   THIS FILE DOES NOT JUDGE IT, and that is the split rather than an omission: what a world vector MEANS is a
   fact about the world forest and belongs to solver/world.h, which owns the one reader of that grammar. A
   second reader here would be a second grammar — the defect remote_op.h's own header paragraph names about
   the record — and this file holds no solver dependency at all. What it owes is the FIELD; what the operand
   means is asked where the flows are. */
/* THE ASKING FLOW'S PATH MARKS — the fourth field, as text (borrowed), parsed in solver/engine.c by the arrival
   word's one parser, because this file holds no solver dependency. */
const char *remote_op_marks_field(const RemoteOp *op);
const char *remote_op_addressee(const RemoteOp *op);
/* THE MEMBER A `windowproxy.get` NAMES — HTML §7.2.1.3.1 "CrossOriginProperties ( O )"' property name, which
   is the record's one operand — or NULL when the record is not a `windowproxy.get` at all.
   NULL IS AN ANSWER AND NOT AN ERROR, which is the one way this differs from the three fields above. Those are
   the TRANSPORT'S and every verb carries them, so asking for one says nothing about which operation arrived;
   a member is an operand of ONE verb, and the slot it occupies holds `<generation>:<id>` for the five
   `object.*` verbs — so an accessor that asserted its way past a non-`windowproxy.get` would hand its caller
   an object name under the name of a member. The caller is deciding WHICH operation this is, so the absence IS
   the fact it needs, and one call answers both halves of that question rather than a predicate and a reader
   that can disagree.
   THAT IT IS ONE OF §7.2.1.3.1's THIRTEEN IS ALREADY TRUE HERE — remote_op_parse CHECKs it where the record is
   born, which is the only place both callers of the parse reach, and a second check of one invariant is the
   copy that drifts. What this line owes is the FIELD. */
const char *remote_op_member(const RemoteOp *op);
/* THE PROGRAM THAT PERFORMS IT. Installs every operand on `ctx`'s global as a slot and returns the source,
   BORROWED (static text). Running it is the caller's, and it must be run as a flow. */
const char *remote_op_program(JSContext *ctx, const RemoteOp *op);
void remote_op_free(RemoteOp *op);

#endif
