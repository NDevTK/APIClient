/* WHICH ENTRY OF A CONSOLE MAP A `label` NAMES — the Console Standard's §1.2/§1.4 membership question, asked
 * of a label that may be unknown external input. See console_label.c for the argument.
 *
 * FIVE MEMBERS ASK IT AND EVERY ONE OF THEM ASKS IT FIRST. §1.2.1 count(label) and §1.2.2 countReset(label)
 * ask it of the count map ("Each console namespace object has an associated count map, which is a map of
 * strings to numbers, initially empty" — §1.2); §1.4.1 time(label), §1.4.2 timeLog(label, ...data) and §1.4.3
 * timeEnd(label) ask it of the timer table (§1.4's own sentence, with "times" for "numbers"). §1.2.1's step is
 * "If map[label] exists", §1.4.1's is "If the associated timer table contains an entry with key label", and
 * §1.4.2/.3 read `timerTable[label]` — four spellings of ONE question about a map and a key.
 *
 * WITH A KNOWN LABEL IT IS A PROPERTY LOOKUP AND THERE IS NOTHING TO DECIDE. With an UNKNOWN one it is the
 * question this component exists for, and the answer that had been standing in its place was a COERCION: an
 * unknown key reaching ECMAScript §7.1.21 ToPropertyKey ( arg ) denotes its own display shape as a real
 * string (quickjs's JS_ToPropertyKeyInternal states the model), so `console.time(location.hash)` filed a timer
 * under the slot `{location.hash}` and every one of those four steps answered NO — always, with no fork, and
 * with nothing anywhere to say a question had been asked. That is §Solver-half's
 * collapse exactly: a value the flow does not know decided a branch, the other world was deleted, and the run
 * looked clean. THE SHAPE-AS-KEY MODEL IS NOT THE DEFECT AND IS NOT REMOVED — it is the right answer for the
 * ONE world in which the label names no existing entry, which is where this component still uses it.
 *
 * WHAT THIS FILE OWNS AND WHAT IT DOES NOT. It owns the ENUMERATION (the map's own string keys, snapshotted)
 * and the EXAMPLE COMPARISON (§7.1.21 over the operand's own example, run rather than predicted), which all
 * five members share because their maps have one shape and their labels one type. It does NOT own the
 * REMAINDER's answer — what §1.2.1 does when the label names no entry is not what §1.4.1 does — and that is
 * why it answers with a KEY and never with a value, a throw or a flag of its own. Each member reads its own
 * algorithm off the key it is handed, which is what it already did with the label it converted.
 * The LINK each ask is built out of is core/idl_name_chain.h's, which owns the composition and the naming
 * rule; this file owns the loop, for the reason that header gives — the enumeration, the example comparison
 * and the remainder are the caller's, and here the first two are genuinely shared by five callers. */
#ifndef ENGINE_HOST_BROWSER_CORE_CONSOLE_CONSOLE_LABEL_H
#define ENGINE_HOST_BROWSER_CORE_CONSOLE_CONSOLE_LABEL_H
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_name_chain.h"

/* THE PREDICATE ONE LINK ASKS, WHICH IS THE OPERATION HALF OF ITS CONSTRAINT KEY — and it names neither the
 * MEMBER of the console namespace that is asking nor the MAP it is asking about, which is the same call
 * core/idl_index_arg.h's IDL_INDEX_PREDICATE makes and the opposite of core/timing/timer.c's.
 *
 * A LINK ASKS `label === N` AND NOTHING ELSE. §1.2's count map and §1.4's timer table are maps of STRINGS, and
 * a lookup in either is ECMAScript §7.1.21 ToPropertyKey ( arg ) over the label followed by an ordinary
 * property test — so the fact a link establishes is that the string this unknown denotes is exactly N, a fact
 * about the VALUE with no map and no member in it. `console.count(x)` and `console.time(x)` over one `x`
 * asking whether it is the label `session` are asking ONE question twice, and CLAUDE.md's §Solver-half
 * requires a constraint to be keyed by the PREDICATE's own identity — the operator and both operands — so that
 * a flow's record of one predicate decides that predicate and never its neighbour (core/idl_index_arg.h quotes
 * the sentence in full, at the chain that reached this conclusion first). Putting either name in would give
 * one predicate two identities: the second member would find NOTHING recorded, re-ask every label the first
 * eliminated, and mint a sibling for each — siblings standing on a vector that says `x === "a"` AND
 * `x !== "a"`, which is a fabricated timeline with every arm in range and every assert on the path satisfied.
 * core/timing/timer.c's question is different in KIND and that is why it names its section: a §8.7 identifier
 * is drawn from one global's map, so which map it came from is part of what is being asked. A label is not —
 * "session" is the string "session" in both of this realm's maps and in every other realm's.
 *
 * IT HAS NO WIDTH AND THAT IS WHY THERE IS NO IDL_NAME_CHAIN_SPELLS BESIDE IT. The two uint32 askers declare
 * their widest name at compile time because a decimal's width is arithmetic; a label is a page-supplied
 * DOMString of unbounded length, so there is nothing for a static assertion to check and a cap would be a
 * §NO BOUNDS violation that silently merged two members' questions at the limit. IdlNameChainSuppliedKey
 * grows to the composition instead, which is the guarantee by construction that replaces the refusal. */
#define CONSOLE_LABEL_PREDICATE "Console §1.2/§1.4 the console namespace object's label (label is"

/* THE ELIMINATION CHAIN A LABELLED MEMBER PARKS ON. A member EMBEDS this in its own step state and names it in
 * its own `visit` through console_label_chain_visit; the zeroed state a machine arrives in is the valid
 * "no question asked yet" shape, so there is no initialiser to forget.
 *
 * THE SET IS SNAPSHOTTED, WHICH IS WHAT MAKES THE CURSOR A SOUND NAME FOR A POSITION IN IT. CLAUDE.md's
 * §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED permits a rank only "where the set is the machine's own
 * and fixed at its definition" — a global the page mutates is exactly the case it forbids, and a console map
 * IS mutated by the page, one `console.time` at a time. So the keys are taken ONCE, at the chain's first link,
 * into storage the fork copies and the park carries: from that moment the set is this machine's own and fixed,
 * `next` names a member of it, and the sibling forked at link k re-asks about the same key rather than about
 * whichever key has slid into rank k. It is the same closure core/timing/timer.c reaches by carrying the
 * IDENTIFIER instead of the rank; a string set has no successor function to walk that way, and a snapshot is
 * the other way to make the set fixed rather than a weaker one.
 *
 * `keys` HOLDS ATOM REFERENCES and `nkeys` is how many — the pair the `props` visit copies and frees, which is
 * why they are written together and never separately. `taken` is what tells an EMPTY map (a legitimate
 * snapshot of nothing, `keys == NULL` and `nkeys == 0`) from a chain that has not started, which the pointer
 * alone cannot say. `key` is the composed constraint key and belongs to core/idl_name_chain.h. */
typedef struct {
    JSPropertyEnum         *keys;
    uint32_t                nkeys;
    uint32_t                next;
    uint8_t                 taken;
    IdlNameChainSuppliedKey key;
} ConsoleLabelChain;

/* THE OWNERSHIP DECLARATION FOR THE STATE ABOVE — the key snapshot and the composed key, which is everything
   it owns. A member embedding this calls it from its own `visit`; a machine that does not is one whose fork
   hands its sibling pointers into storage the parent's discharge frees. */
void console_label_chain_visit(JSContext *ctx, ConsoleLabelChain *c, JSStepVisit *v);

/* THE KEY `label` NAMES IN THIS FLOW'S WORLD — the whole of what a §1.2/§1.4 member needs before it can run
 * its own steps.
 *
 * `label` is the member's converted `optional DOMString label = "default"`, or unknown external input that
 * crossed Web IDL §3.2.10 DOMString's conversion as itself. `map` is §1.2's count map or §1.4's timer table, BORROWED. `algorithm`
 * is the asking member's own spec identity and is the ADDRESS every assert made on its behalf reports — a
 * should-never-happen stamps the line it is WRITTEN at, so a check inside a shared component would name THIS
 * file for all five members (CLAUDE.md's §AN-ASSERT-THAT-NAMES-A-REMEDY), and the cure is that the site
 * travels with the operation.
 *
 * RETURNS 0 with `*pkey` holding an atom the CALLER OWNS AND FREES, or a STEP CODE the caller must return
 * unchanged (the flow is parked at a fork and the sibling's snapshot was taken there, so the caller's state
 * must be complete at the call). The atom is what the member's own steps then use in place of the label:
 * §1.2.1's `map[label]`, §1.4.1's "contains an entry with key label", §1.4.3's "Remove timerTable[label]" and
 * the `label` of every one of their concatenations are all reads THROUGH it.
 *
 * THE ANSWER IS ONE OUT-PARAMETER BECAUSE NO CALLER NEEDS TWO. On the arm that names an existing entry the key
 * is that entry's, so the member's own `JS_HasProperty` is true; on the remainder arm it is a key this
 * component has ASSERTED the map does not hold, so the same test is false. Handing back a second "was it
 * present" flag would be a field with a writer and no reader, which is the shape CLAUDE.md calls a broken
 * contract from the other side. */
int console_label_run(JSContext *ctx, JSStepHdr *hdr, ConsoleLabelChain *c, JSValueConst label,
                      JSValueConst map, const char *algorithm, JSAtom *pkey);

#endif
