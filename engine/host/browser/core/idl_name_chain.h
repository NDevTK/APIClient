/* ONE LINK OF A NAME-KEYED ELIMINATION CHAIN — see idl_name_chain.c for the argument.
 *
 * NOT core/idl_index_arg.h AND NOT A REPLACEMENT FOR IT. That file is a whole ALGORITHM: Web IDL §3.2.4.6
 * `unsigned long`'s own question over a collection's positions, with the enumeration, the past-the-end world
 * and the two out-parameters that belong to it. This file is the one LINK such a chain is built out of, which
 * is the part that is the SAME in every chain and which had been written twice.
 *
 * WHAT A CALLER STILL OWNS, AND WHY IT IS NOT HERE. The ENUMERATION — which member the cursor stands at, and
 * what "the next one" means — differs at every asker and is a fact about the asker's own set:
 * core/idl_index_arg.c counts positions ascending from 0 to a length it was handed, while
 * core/timing/timer.c walks "the least §8.7 identifier this global's map still holds that is at or above the
 * cursor". So does the EXAMPLE COMPARISON, because the operand's type decides it (§3.2.4.6's ConvertToInt(V,
 * 32, "unsigned") and §3.2.4.5's ConvertToInt(V, 32, "signed") do not agree on a negative operand), and so
 * does the REMAINDER's answer, which core/idl_index_arg.c states at length must never be shared. Lifting the
 * LOOP would have taken all three of those into one file and decided them for everybody; lifting the LINK
 * takes exactly what they share.
 *
 * WHAT THEY SHARE IS THE NAMING RULE, WHICH IS THE ONE THING WORTH HAVING IN ONE PLACE. A link asks "is this
 * unknown the member called N" and files that question under a constraint key composed of the member's own
 * NAME. CLAUDE.md's §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED is why it is the name and never the
 * RANK: a position is a fact about the operand only where the set is the machine's own and fixed at its
 * definition, and over a set the page mutates a replayed rank silently renames its referent — every arm still
 * in range, every assert on the path satisfied, and the flow acting on a member no answer of its own ever
 * named. core/timing/timer.c records that as a measured near-miss rather than a hypothetical. Two copies of
 * that rule is two places for it to stop being obeyed, and the copy that drifts is the one nobody reads. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_NAME_CHAIN_H
#define ENGINE_HOST_BROWSER_CORE_IDL_NAME_CHAIN_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* THE COMPOSED CONSTRAINT KEY'S STORAGE, AND WHY IT IS AN INLINE ARRAY RATHER THAN A POINTER.
 *
 * step_fork_run keeps a BORROWED pointer to the operation string on the header and the DRIVER reads it after
 * the machine has returned, so the string cannot be a C local — that dangles exactly where the key is built.
 * It lives on the machine's state instead, and a machine's state is what a fork COPIES: quickjs-step.h's
 * JS_StepVisitOwnedFingerprint states the rule for the other kind of storage — "a `buf` named by the visit is
 * the declaration's allocation exactly as a `val` is its reference: the fork copies it with js_malloc and the
 * discharge frees it with js_free". An INLINE array needs no declaration to survive that copy, which is why
 * every member using a chain can share ONE `visit` that names nothing.
 *
 * THE CONSEQUENCE IS A REAL LIMIT AND IT IS STATED HERE RATHER THAN DISCOVERED: a member whose name is
 * UNBOUNDED cannot be spelled into this. A §8.7 identifier and a collection position are both a uint32, so
 * their widest spelling is arithmetic; a name the PAGE supplies — HTML §9.5's BroadcastChannel `name`, the
 * console standard's §1.2/§1.4 `label` — has no width at all, and an asker holding one uses
 * IdlNameChainSuppliedKey below, whose storage its own `visit` declares as a `buf`. The compile-time check
 * below is what makes that a refusal at the call site instead of a truncation at run time.
 *
 * THE NUMBER ITSELF IS NOT LOAD-BEARING AND MUST NOT BE READ AS A BUDGET. It is large enough for every
 * predicate this tree spells plus the widest uint32, and IDL_NAME_CHAIN_SPELLS is what actually enforces that
 * per caller — so a caller that does not fit gets a COMPILE ERROR naming its own predicate, which is the
 * signal to raise this or to move that caller to IdlNameChainSuppliedKey, never to shorten a predicate until
 * it fits. */
#define IDL_NAME_CHAIN_OP_MAX 128

/* WHAT ONE MEMBER'S CHAIN KEEPS OF THIS COMPONENT: the composed key, and nothing else. A member EMBEDS this
   in its own step state (core/idl_index_arg.h's IdlIndexChain does, beside its cursor) rather than allocating
   one, for the reason above. It holds no JSValue, so it adds nothing to any member's `visit`. */
typedef struct {
    char op[IDL_NAME_CHAIN_OP_MAX];
} IdlNameChainKey;

/* THE SAME KEY WHEN THE PAGE SUPPLIES THE MEMBER'S NAME — the other storage, and the reason it is a second
 * TYPE rather than a second mode of the first.
 *
 * THE TWO ARE CHOSEN BY WHICH STRUCT A CALLER EMBEDS, AT COMPILE TIME, AND THERE IS NO PREDICATE ANYWHERE.
 * CLAUDE.md's §C-stack bans a run-time question that selects between two implementations of one thing; this is
 * not one. Neither of these is a fallback for the other: an asker whose member names are its own arithmetic
 * has a width, so IDL_NAME_CHAIN_SPELLS can refuse a build in which the key would truncate, and that refusal
 * is worth more than an allocation per link; an asker whose member names come from the PAGE has no width to
 * declare, so there is nothing for a static assertion to check and the only honest guarantee is that the
 * buffer is GROWN to whatever the composition needs. Two facts about a name, two storages, and the choice is
 * made once at the call site by declaring a field.
 *
 * TRUNCATION IS IMPOSSIBLE HERE BY CONSTRUCTION RATHER THAN REFUSED BY A CAP. idl_name_chain_ask_supplied
 * MEASURES the composition with snprintf's would-have-written count and grows `op` to it before writing, so
 * there is no length at which a member name stops fitting — which is what §NO BOUNDS requires of a label a
 * page may spell as long as it likes. The defect the inline key's compile-time check exists to prevent — a
 * key one byte short filing two members' questions under one name, so one link's recorded answer decides
 * another's — cannot arise, because the buffer is never shorter than the string.
 *
 * `op` IS OWNED AND `cap` IS ITS ALLOCATION, WHICH IS THE PAIR THE `visit` COPIES. A machine embedding this
 * MUST name it in its own `visit` through idl_name_chain_supplied_visit — a `buf` the declaration does not
 * name is not copied by a fork, and the sibling would then read the parent's freed buffer. `cap` only ever
 * grows, so the two consumers of the visit (the fork's copy and the discharge's free) agree about the size
 * without either of them having to be told twice. The zeroed state a machine arrives in — NULL and 0 — is the
 * valid "no question asked yet" shape and needs no initialiser. */
typedef struct {
    char  *op;
    size_t cap;
} IdlNameChainSuppliedKey;

/* NAME THE SUPPLIED KEY'S STORAGE IN THE EMBEDDING MACHINE'S OWN `visit`. One line at the machine, so the
   allocation is a fork-copied, discharge-freed field like any other and the machine never writes the
   `v->buf` call itself — the cap and the pointer are one fact and stating it twice is how they come apart. */
void idl_name_chain_supplied_visit(JSContext *ctx, IdlNameChainSuppliedKey *key, JSStepVisit *v);

/* THE WIDEST DECIMAL A uint32 MEMBER NAME CAN BE SPELLED AS — 4294967295, ten digits. Derived rather than
   typed, and named so the two callers that spell a uint32 identifier state the same fact once each instead of
   writing a magic 10 or, as both of them used to, sizing a buffer against a literal and asserting the result
   at run time. */
#define IDL_NAME_CHAIN_U32_BYTES 10

/* DECLARE, AT COMPILE TIME, THAT THIS CALLER'S QUESTION FITS — the half of the truncation problem that can be
 * made IMPOSSIBLE instead of merely detected.
 *
 * A TRUNCATED KEY IS THE DEFECT THE WHOLE NAMING SCHEME EXISTS TO AVOID, ARRIVING THROUGH THE BACK DOOR. The
 * member's name is the LAST thing in the composed string, so a buffer one byte short files two different
 * members' questions under ONE key and lets one link's recorded answer decide another's — the same fabricated
 * timeline a rank would produce, reached by a different road. Both existing chains caught it with a run-time
 * assert over snprintf's would-have-written count. That assert stays (see below), because a caller can be
 * wrong about its own widest name; but where the predicate is a literal and the name's width is arithmetic,
 * the compiler can settle it and no run has to.
 *
 * `predicate` must be a STRING LITERAL — `sizeof` and the diagnostic's concatenation both require it, and a
 * caller passing a `const char *` fails to compile, which is the correct answer rather than a limitation: a
 * predicate that is not fixed at compile time is one whose width nothing here can bound.
 * `max_name_bytes` is the widest spelling of a member name this caller can ever be asked about
 * (IDL_NAME_CHAIN_U32_BYTES for the two that spell a uint32). The `+ 2` is the separator space and the closing
 * paren; `sizeof` already counts the terminator. Place it at FILE SCOPE beside the predicate's own
 * definition. */
#define IDL_NAME_CHAIN_SPELLS(predicate, max_name_bytes)                                                      \
    _Static_assert(sizeof (predicate) + (max_name_bytes) + 2 <= IDL_NAME_CHAIN_OP_MAX,                        \
                   predicate " plus the widest member name this asker can be handed does not fit "            \
                   "IdlNameChainKey::op — a truncated key names a DIFFERENT member's question and the flow "  \
                   "answers it with this one's record, so raise IDL_NAME_CHAIN_OP_MAX, or move this asker to " \
                   "IdlNameChainSuppliedKey if its member names have no width at all because the page "       \
                   "supplies them")

/* ASK ONE LINK: is the unknown `over` the member named `member`?
 *
 * IT COMPOSES `"<predicate> <member>)"` AND THAT SPELLING IS FROZEN THE DAY A CALLER LANDS. A constraint key
 * is what a flow's recorded answers are FILED under, and §Time-travel-resume carries those answers across a
 * park and into the next session out of the IndexedDB cold tier — so changing a caller's spelling does not
 * rename a question, it ORPHANS every answer already recorded against it. A resumed flow would find nothing
 * recorded, re-ask every member it had already eliminated, and mint a sibling for each — worlds its own path
 * has already contradicted. Both existing callers were converted onto this with their strings BYTE-IDENTICAL
 * for exactly that reason, which is also why the composition is stated here as a format and not left to each
 * caller's snprintf.
 *
 * `predicate` NAMES THE QUESTION. What may be in it is decided per caller and the two disagree on purpose:
 * core/idl_index_arg.h's IDL_INDEX_PREDICATE deliberately holds NO member name, because `index == 3` is one
 * fact and eleven `item(index)` members asking it must share one key; core/timing/timer.c's does name §8.7,
 * because its question is "is `id` the timer with identifier H" and the answer depends on which map H was
 * drawn from. Those are two answers to the question "is this the same predicate", and each file argues its
 * own — this component composes what it is given and decides neither.
 *
 * `member` IS THE MEMBER'S OWN NAME AND NEVER ITS RANK, which is the rule this file exists to hold; see the
 * banner. `real` is which arm the operand's own EXAMPLE reaches, computed by RUNNING the comparison and not
 * by a rule predicting it, or JS_OUTCOME_REAL_UNSTATED for an unknown carrying no example — that is a POSITIVE
 * fact and never a value to fall back on. `algorithm` is the ADDRESS: a should-never-happen stamps the line it
 * is WRITTEN at, so every assert in here would otherwise report THIS file for every member that reaches it,
 * which is CLAUDE.md's §AN-ASSERT-THAT-NAMES-A-REMEDY. The site travels with the operation instead, and it is
 * the member's own spec identity rather than a file and a line, for the reason core/idl_index_arg.h gives:
 * that name is stable across an edition of this tree in a way a coordinate is not.
 *
 * OUTCOME 0 IS "NO". step_fork_run's one numbering rule is that outcome 0 is the completion a run with no
 * forking policy takes, and both callers need the same thing of it for different reasons they state at their
 * own sites — a mutating member must not have its removal on the arm an @S candidate re-fire walks, and a
 * reading member must not ASSERT a position nobody knows. `*pyes` is therefore false for arm 0 and true for
 * arm 1.
 *
 * RETURNS 0 with `*pyes` written, or a STEP CODE the caller must return UNCHANGED — the flow is parked at the
 * fork and the sibling's snapshot was taken there, so the caller's state must be complete at the call. */
int idl_name_chain_ask(JSContext *ctx, JSStepHdr *hdr, IdlNameChainKey *key, JSValueConst over,
                       const char *predicate, const char *member, int real, const char *algorithm,
                       bool *pyes);

/* THE SAME LINK, ASKED WITH A MEMBER NAME THE PAGE SUPPLIED — see IdlNameChainSuppliedKey for why the storage
 * is the only difference and why that difference is a type rather than a flag.
 *
 * IT COMPOSES THE SAME `"<predicate> <member>)"` AS idl_name_chain_ask, FROM THE SAME FORMAT STATED ONCE in
 * idl_name_chain.c. That is not tidiness: a constraint key is what a parked flow's recorded answers are filed
 * under, out of the IndexedDB cold tier and into the next session, so two entry points composing two spellings
 * would file one question under two names and a flow that reached the same predicate through the other door
 * would find nothing recorded and re-ask every member it had already eliminated. One format, one question.
 *
 * `member` IS BORROWED for the length of the call and nothing here retains it — the composition copies its
 * bytes into `key->op`, which is what step_fork_run borrows, so a caller reading its member name out of an
 * atom or a JSString may release it the moment this returns. It MAY be the EMPTY STRING, which the inline
 * entry point refuses: a page-supplied name of zero length is an ordinary member (`console.count("")` stores
 * under it) rather than a name buffer nothing wrote, and the composition tells it apart from every other
 * member exactly as it tells any two apart.
 *
 * Every other parameter, the return protocol and the outcome numbering are idl_name_chain_ask's exactly. */
int idl_name_chain_ask_supplied(JSContext *ctx, JSStepHdr *hdr, IdlNameChainSuppliedKey *key, JSValueConst over,
                                const char *predicate, const char *member, int real, const char *algorithm,
                                bool *pyes);

#endif
