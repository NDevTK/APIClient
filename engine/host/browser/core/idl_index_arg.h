/* A WEB IDL `unsigned long index` ARGUMENT, KNOWN AND UNKNOWN — see idl_index_arg.c.
 *
 * NOT core/idl_indexed.h, WHICH IS A DIFFERENT PROBLEM WITH A SIMILAR NAME. That file is Web IDL §3.9 Legacy
 * platform objects — §3.9.1 [[GetOwnProperty]] and its neighbours, the exotic behavior behind `list[3]`, whose
 * subject is a property KEY and whose declaration is the indexed property getter Web IDL §2.5.6 Special
 * operations defines. This file is the `unsigned long index` ARGUMENT of a declared operation —
 * `list.item(3)` — whose subject is an argument slot the declaration converted, or that unknown external
 * input crossed as itself. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_INDEX_ARG_H
#define ENGINE_HOST_BROWSER_CORE_IDL_INDEX_ARG_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* THE PREDICATE ONE LINK OF THE CHAIN ASKS, WHICH IS THE OPERATION HALF OF ITS CONSTRAINT KEY — and it names
 * the QUESTION and not the MEMBER, which is the whole of what makes one flow's answers agree with each other.
 *
 * A LINK ASKS `index == k` AND NOTHING ELSE. The operand half of the key is the value's own identity, so the
 * fact a link establishes is "the number this unknown denotes is exactly k" — a fact about the VALUE, with no
 * collection, no interface and no algorithm in it. Two members of this family reached over ONE unknown are
 * therefore asking ONE question twice, and CLAUDE.md's §Solver-half says what that has to mean: the constraint
 * is "keyed by the PREDICATE's own identity — operator and both operands — so the flow's record of one
 * predicate decides that predicate and never its neighbour."
 *
 * THE MEMBER'S NAME USED TO BE THE PREFIX OF THIS STRING, AND IT GAVE ONE PREDICATE AS MANY IDENTITIES AS
 * THERE ARE DISTINCT `algorithm` STRINGS AMONG THE CALLERS — not one per interface, since NodeList and
 * HTMLCollection already share theirs and the two `deleteRule`s share CSSOM §6.4's; the count is a fact about
 * the call sites and is worth reading there rather than trusting here.
 * `nl.item(i)` and `document.styleSheets.item(i)` over one `i` composed
 * `"DOM §4.2.10 … item(index) (index is 3)"` and `"CSSOM §6.2.2 … item(index) (index is 3)"`, which are two
 * keys for one fact — so the second member found NOTHING recorded, re-asked every position the first had
 * already answered, and minted a sibling for each. Those siblings are not extra exploration: each stands on a
 * decision vector that says `index == 0` AND `index == 3`, and the member hands it an element on the strength
 * of the second while the first is what the flow's own earlier link recorded. One value, two positions, one
 * world — a fabricated timeline, with every arm in range and every assert on the path satisfied. It is
 * core/timing/timer.c's own correction read from the other end: §8.7 states clearTimeout and clearInterval as
 * ONE algorithm and that file spells ONE operation for both, because "two names for the one question would be
 * two constraint keys for one fact".
 *
 * WHAT `solver_outcome`'S REFUSAL ACTUALLY PROTECTS is the OTHER direction and is untouched by this. It
 * rejects an operation that names NOTHING, because an unnamed one merges with every other operation over the
 * same operand — the collapse decide.c's own `decide_key` note measures (`x < 700` deciding `x < 300`). The
 * string below names its question exactly and in full; what it does not name is the SITE, which is a different
 * requirement with its own field (`algorithm`, the assert address) rather than a second use of this one.
 *
 * `%u` IS THE ONLY VARIABLE PART, so the buffer under it is sized from this text plus the widest decimal a
 * uint32 can be — truncation, which would file two positions under one key, is arithmetic here rather than a
 * fact about whichever member happened to be longest. */
#define IDL_INDEX_PREDICATE "Web IDL §3.2.4.6 unsigned long (index is"

/* THE ELIMINATION CHAIN A MEMBER PARKS ON, AND THE WHOLE OF WHAT SUCH A MEMBER HAS TO KEEP.
 *
 * `next` is the position the chain will ask about when the flow is next entered — the cursor a park resumes
 * on. `op` is the operation half of the fork's constraint key; step_fork_run keeps a BORROWED pointer to it
 * and the DRIVER reads it after idl_index_chain_run has returned, so it lives on the machine's state and
 * never in a C local, which would dangle exactly where the key is built.
 *
 * ITS SIZE IS DERIVED AND NEVER TYPED: the prefix above, then ` 4294967295)` — a space, the widest decimal a
 * uint32 can take, and the closing paren — over `sizeof`, which already counts the terminator.
 *
 * IT HOLDS NO JSValue, WHICH IS WHY ONE `visit` SERVES EVERY MEMBER (idl_index_chain_visit below). A member
 * whose state is this and nothing else declares that function and is done; a member that needs more of its
 * own EMBEDS this as its first field and names the rest in its own visit. */
typedef struct {
    uint32_t next;
    char     op[sizeof IDL_INDEX_PREDICATE + 12];
} IdlIndexChain;

/* THE OWNERSHIP DECLARATION FOR THE STATE ABOVE, WHICH IS THAT IT OWNS NOTHING. It is a real function rather
   than a NULL in the member's IdlStepDecl because a machine with no `visit` cannot be FORKED and is refused at
   registration — and forking is the whole of what a member using this chain exists to do. It is declared HERE,
   once, because the state is this file's type: a per-member copy would be the second list core/idl_args.h's
   IdlStepDecl banner forbids, with nothing to catch the copy that goes wrong. */
void idl_index_chain_visit(JSContext *ctx, void *state, JSStepVisit *v);

/* WEB IDL §3.2.4.6 `unsigned long`'S OWN QUESTION, ASKED OF AN UNKNOWN INDEX — which of this collection's
 * positions it is, or none of them — AS AN ELIMINATION CHAIN.
 *
 * WHY THERE IS A QUESTION AT ALL. §3.2's conversion is a BOUNDARY that unknown external input crosses AS
 * ITSELF (core/idl_args.h's `idl_number_of` states the rule and names the shape that breaks it: A BODY MAY NOT
 * CALL JS_ToFloat64 ON ITS OWN ARGUMENT), so `list.item(location.hash.length)` reaches its body still holding
 * the unknown, and a body owing C a `uint32_t` for it has no number to give. The coercion does not answer
 * coarsely — ToNumber hands a concolic straight back and the engine aborts INSIDE it, one frame below the
 * member, which is why checking a coercion's return is no defence: there is no return to check.
 *
 * IT IS ONE QUESTION AND THE CHAIN IS ITS DECOMPOSITION. §3.2.4.6's ConvertToInt(V, 32, "unsigned") is TOTAL
 * over [0, 2**32-1], so the type admits no value below 0 — which makes "is the index past the end" and "which
 * position is it" the SAME question: past-the-end is exactly "none of 0 ... npositions-1". The chain asks
 * `index == k` ascending; the arm that says YES pins the position, and EXHAUSTING the chain is the past-the-end
 * answer. There is no separate ask for the bound and no second key over one fact.
 *
 * `npositions` IS THE COUNT OF POSITIONS THE ALGORITHM ADMITS, WHICH IS NOT ALWAYS THE LENGTH. It is the
 * length for a member whose bound is `index >= length` — every `item(index)` in this engine, and CSSOM §6.4
 * CSS Rules' remove a CSS rule — and it is length + 1 for one whose bound is `index > length`, because
 * appending at the very end is a legal position there. Passing it rather than the length is what lets ONE
 * chain serve both, which is the difference css_rule.h used to name as the next diff's work.
 *
 * `npositions == 0` ASKS NOTHING and answers past-the-end at once. One feasible completion is not a fork, it
 * is the answer, and a seam handed it would be given a decision this chain had already made.
 *
 * EACH LINK'S KEY NAMES A NUMBER AND NEVER A RANK, AND NAMES NOTHING ELSE — see IDL_INDEX_PREDICATE above for
 * the second half of that, which is why the member is not in it. The first half is what makes the key survive
 * a park and a mutation.
 * CLAUDE.md's §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED is about a completion whose name is a
 * POSITION IN A SET THE PAGE MUTATES — answer "the entry at rank 0", shorten the set, and the recorded answer
 * names something else. Here the operand IS the number: `index == 3` is a fact about the value the page
 * computed, true in the same words after the collection grows, shrinks or is rebuilt in another session. That
 * is also why a chain drawn against a CHANGED length stays sound — a flow that answered NO at 0, 1 and 2 has
 * established `index >= 3`, so a later shorter collection exhausts at once and answers past-the-end, which is
 * what the algorithm's own bound says about that world.
 *
 * `algorithm` NAMES THE MEMBER, AND IT IS THE ADDRESS AND NOTHING ELSE — it does not reach the constraint key,
 * for the reason IDL_INDEX_PREDICATE gives. A should-never-happen stamps the line it is WRITTEN at, so a check
 * inside a shared chain would report THIS file for every member that reaches it — CLAUDE.md's
 * §AN-ASSERT-THAT-NAMES-A-REMEDY, whose cure is that the site travels with the operation. Here the site is
 * better than a file and a line: it is the member's own spec identity, which is stable across an edition of
 * this tree in a way a coordinate is not.
 *
 * IT USED TO BE BOTH, AND THAT IS THE §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS SHAPE EXACTLY. "Where did this
 * abort happen" and "which predicate is this" are two questions, they agree at every site until two members
 * ask one predicate, and at that point the stricter one — the address, which must differ per member — decided
 * the key and the looser one lost with nothing anywhere to say it had been asked. The cure is the one that
 * section names: two questions over the ONE fact, never two facts. The member's name is still stated once, in
 * one place, and each question now reads it for what it is.
 *
 * RETURNS 0 once the chain has ANSWERED, and the answer is in the two out-parameters: `*ppast_end` true means
 * the index is past the last position the algorithm admits and `*pindex` was not written, false means
 * `*pindex` is this world's position. Any other return is a STEP CODE the caller must return unchanged (the
 * flow is parked at the fork). `index_v` must be unknown external input — the known value is
 * idl_index_arg_known's and never comes here. */
int idl_index_chain_run(JSContext *ctx, JSStepHdr *hdr, IdlIndexChain *c, JSValueConst index_v,
                        uint32_t npositions, const char *algorithm, uint32_t *pindex, bool *ppast_end);

/* THE SAME ARGUMENT WHEN IT IS NOT UNKNOWN — the number a converted `unsigned long index` denotes.
 *
 * IT IS THE PAIR OF THE CHAIN ABOVE AND IT EXISTS SO THAT NEITHER HALF IS WRITTEN PER MEMBER. What every one
 * of these bodies used to write instead was a raw `JS_ToUint32(ctx, &i, argv[0])` with its return DISCARDED,
 * under a comment saying the declaration had already converted the value — the shape core/idl_args.h bans by
 * name. For an already-converted argument the sanctioned read (`idl_number_of`) is exactly the assert pair the
 * site would otherwise have to write itself: the operand is a Number, and reading it back cannot throw. So the
 * discarded return is not asserted dead, it is REMOVED, and what stands in its place asserts §3.2.4.6's own
 * postcondition — an integer in [0, 2**32-1], because §3.2.4.9 Abstract operations' ConvertToInt takes the
 * integer part modulo 2**32 and NaN and both infinities became +0 in the conversion.
 *
 * `algorithm` IS THE ADDRESS, for the reason given above. `index_v` must NOT be unknown external input. */
uint32_t idl_index_arg_known(JSContext *ctx, JSValueConst index_v, const char *algorithm);

#endif
