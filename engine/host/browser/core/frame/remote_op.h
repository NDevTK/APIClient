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

/* `record` is the text the asking instance emitted, VERBATIM:
     windowproxy.get <doc> <world+ancestry> <addressee> <member>
     object.get      <doc> <world+ancestry> <addressee> <generation>:<id> <key>
     object.set      <doc> <world+ancestry> <addressee> <generation>:<id> <key> <value>
     object.delete   <doc> <world+ancestry> <addressee> <generation>:<id> <key>
     object.apply    <doc> <world+ancestry> <addressee> <generation>:<id> <thisArg> <arg>*
     object.has      <doc> <world+ancestry> <addressee> <generation>:<id> <key>
   The object is named by (generation, id) because an id is an index into ONE session's export table
   (remote_object.h): the document name is stable across a park by requirement, so an id alone resolves in
   range in every session of that document and names a different object in each.
   The first THREE fields are the TRANSPORT'S — which instance, whose timeline asks, and which of the
   RECEIVER'S timelines the asker is already in — and the first two are exactly a routed delivery's, which is
   what lets one router carry both. Crashes on a verb this agent does not perform: an unanswered record parks
   the asking flow forever, so the operation has to be built rather than ignored.
   THE ADDRESSEE SITS AFTER THE WORLD AND BEFORE EVERY OPERAND, AND ITS POSITION IS NOT A MATTER OF TASTE.
   Three trusted-zone files read this grammar POSITIONALLY while relaying the record whole — engine/route.mjs
   takes `[1]` for the holder and `[2]` for the asking world, and engine/trusted.mjs and extension/bridge.js
   each take `[1]` — so a field at index 2 would silently re-point route.mjs's second read at a world nobody
   asked from. That zone's JavaScript is INTERPRETED FROM THE TREE and therefore live on WRITE (CLAUDE.md
   §A-CROSS-BOUNDARY-DIFF) while this C is live only after a build, so a grammar change those three can see is
   a diff that deploys itself asymmetrically. At index 3 all three are untouched and the pin lands as C only.
   Index 3 is also the only fixed slot EVERY verb has: `object.apply`'s tail is variadic. */
RemoteOp *remote_op_parse(const char *record);
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
const char *remote_op_addressee(const RemoteOp *op);
/* THE PROGRAM THAT PERFORMS IT. Installs every operand on `ctx`'s global as a slot and returns the source,
   BORROWED (static text). Running it is the caller's, and it must be run as a flow. */
const char *remote_op_program(JSContext *ctx, const RemoteOp *op);
void remote_op_free(RemoteOp *op);

#endif
