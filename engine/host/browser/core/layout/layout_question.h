/* A LAYOUT QUESTION — the (element, kind, parameter) tuple layout's recursion is over, as a VALUE.
 *
 * WHY THIS IS A COMPONENT AND WHY THE TUPLE IS THE THING. Layout in this engine is one strongly connected
 * component: re-derive it rather than trusting a number here —
 *
 *     a call-graph SCC over core/layout and core/css, cut one node at a time
 *
 * and the shape that comes back is a single cluster spanning eleven files whose members are reached BOTH
 * descending (a box asks its children) and ascending (a box asks the containing block it is inside). No
 * ordering over the box tree and no ordering over the property set makes it a DAG, because the recursion is
 * not over either: it is over (ELEMENT, QUESTION) PAIRS, and the same function is a node of that graph once
 * per element it is asked about. That is why DEPTH is not the invariant and REPETITION is — an arbitrarily
 * deep box tree is a legitimate document, and one pair asked while the identical pair is still open is a rule
 * reading a term it is a term of, with no fixed point to reach and nothing between the two occurrences that
 * made progress.
 *
 * NOTHING IN THIS ENGINE NAMED THAT PAIR BEFORE THIS FILE. The recursion's question was implicit in WHICH C
 * FUNCTION you were standing in and what was in its argument registers, which is exactly why it could only
 * ever be a C-stack recursion: a question you cannot NAME is one you cannot push, compare, park, or hand to a
 * resolver. §C-stack's answer for JavaScript was the trampoline — every call pushes a HEAP frame and the
 * driver loops — and layout needs the same shape, with this tuple as the frame's identity. So the type here
 * is the work stack's ELEMENT, landed with the one consumer it already has.
 *
 * THE CHAIN IS INTRUSIVE AND MUST STAY INTRUSIVE UNTIL THE RESOLVER EXISTS, which is the one design decision
 * in this file that is easy to get backwards. Each node lives on the C frame that is ASKING, so the chain has
 * exactly the recursion's depth, has no capacity of its own, and CANNOT DISAGREE WITH THE C STACK. A
 * heap-owned array that mirrored those frames would be a SECOND COPY of a live structure — it can be left
 * un-popped by an early return and it can outlive the frame whose question it names, and neither failure is
 * visible in its own output — while shortening the C stack by exactly nothing, since the frames it mirrors are
 * all still there. A heap stack earns its place at the moment it REPLACES those frames: when a question's body
 * is staged and the driver POPS rather than returning. Relocating this chain ahead of that is the dual system
 * §A-superseded-system-is-DELETED forbids, with the worse of the two copies load-bearing. A FIXED array is
 * worse again and for a different reason: past its Nth entry it stops comparing, and a frame nobody compared
 * reads exactly like a frame with no repeat in it, so it is a depth cap wearing a buffer.
 *
 * THE RECORD IS A FILE STATIC AND THAT IS SOUND ONLY BECAUSE LAYOUT CANNOT SUSPEND. It is the shape of the
 * current C activation chain, which no flow switch can interleave today, because every one of these questions
 * runs to completion on the C stack. RETIREMENT: this static goes when a layout question can yield — at that
 * point the chain belongs to the FLOW, and a static one would be another flow's questions, which is the defect
 * core/layout/block_flow.h's layout-is-per-flow-state note describes.
 *
 * THE FORMATTERS ARE TOTAL AND NOTHING HERE MAY ABORT ON THE FORMATTING PATH, for the reason box_subject.h
 * gives at length: a helper a `DFAILF` calls that asserts does not report a SECOND defect, it REPLACES the
 * first, and the reader loses the chain and the remedy at once. Every arm below answers with a SENTENCE
 * instead of refusing. box_subject.c enforces that STRUCTURALLY by not including `check.h`, and that mechanism
 * is unavailable here because the transition macro below must reach `DCHECK` — the invariant it checks is over
 * a TRANSITION, so §AN-ASSERT-THAT-NAMES-A-REMEDY requires it to be a MACRO EXPANDED AT EACH ASKING SITE
 * rather than a function called from them, or every kind's cycle would report this file's line number for
 * every caller. RETIREMENT: the totality-by-discipline here goes when the chain stops being intrusive, because
 * the LIFO invariant the macro checks is then impossible to violate rather than merely asserted, and this
 * header no longer needs `check.h`. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LAYOUT_QUESTION_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LAYOUT_QUESTION_H
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "check.h"

/* THE KINDS, AND WHAT ADMITS ONE. A kind is an ENTRY THE CLUSTER RE-ENTERS — a question some other member of
   the SCC asks by name — and it is declared here so that the label, the enum and the count cannot come apart.
   The second field is what a chain node reads as; it is a DISPLAY LABEL in a `DFAIL` reason and never an
   identity anything matches on, which is why it may carry a citation at all (§AN-IDENTITY-STRING-CARRIES-A-
   STABLE-TOKEN-AND-NEVER-A-CITATION).
   ADDING ONE IS NOT FREE AND IS NOT A LIST TO KEEP TIDY: a kind declared here with no asking site is a
   declaration with no consumer, and a kind whose re-entry is LEGITIMATE turns a working engine into an abort.
   The test is the one the used-value kind's own site states — arrival must be an ASK that is its own input. */
#define LAYOUT_QUESTIONS(X) \
    X(LAYOUT_Q_USED_VALUE_PX, "CSS 2.1 §6.1.3 \"Used values\"' used box-model length")

#define LAYOUT_QUESTION_ENUM(id, label) id,
#define LAYOUT_QUESTION_LABEL(id, label) label,
typedef enum { LAYOUT_QUESTIONS(LAYOUT_QUESTION_ENUM) LAYOUT_Q__COUNT } LayoutQuestionKind;

/* THE PARAMETER IS TWO FIELDS AND EQUALITY IS OVER ALL FOUR, which is what keeps a kind from needing a table
   nobody can derive. A kind whose parameter is a STRING (a property name) leaves `code` at 0; a kind whose
   parameter is a small NUMBER (an axis, a box type) leaves `name` at NULL; a kind that needs both uses both;
   a kind with no parameter at all uses neither. `name` is compared by `strcmp` and is BORROWED from the asking
   frame — a node lives exactly as long as the frame it is on, so the pointer cannot outlive its subject. */
typedef struct {
    LayoutQuestionKind kind;
    lxb_dom_element_t *element;
    const char        *name;
    unsigned           code;
} LayoutQuestion;

/* THE ONE CONSTRUCTOR, so a question is composed in a single expression at the site that asks it and no
   caller needs a local to hold one — which is what keeps the release build's frame the size it was, since
   the transition macro below does not expand its argument at all at -DAPICLIENT_DEV=0. */
static inline LayoutQuestion layout_question(LayoutQuestionKind kind, lxb_dom_element_t *element,
                                             const char *name, unsigned code)
{
    LayoutQuestion q;

    q.kind = kind;
    q.element = element;
    q.name = name;
    q.code = code;
    return q;
}

/* One node of the open chain. It is the CALLER'S AUTOMATIC STORAGE — see the header banner for why that is
   the design and not a shortcut. */
typedef struct LayoutQuestionOpen {
    struct LayoutQuestionOpen *below;   /* the question the frame below is asking; NULL at the outermost */
    LayoutQuestion             q;
} LayoutQuestionOpen;

/* Big enough for a chain a reader can act on and bounded because it is composed on the ABORTING frame, where
   the stack is the resource that ran out in the shape this exists to report. A chain that does not fit says
   so; it is not a limit on the chain, which has no capacity at all. */
enum { LAYOUT_QUESTION_CHAIN_CAP = 512, LAYOUT_QUESTION_ASK_CAP = 224 };

/* HOW MANY FRAMES UP THE IDENTICAL QUESTION IS ALREADY OPEN, 1 for the immediately enclosing frame, 0 for
   none. A question with no element is not a question and can never repeat: the entry that owns such a kind
   asserts its own operands, and answering "cycle" for a pair of malformed asks would report this component's
   invariant in place of that entry's. */
unsigned layout_question_repeat(LayoutQuestion q);

/* ONE question, rendered. TOTAL: answers a sentence for a NULL buffer, a NULL element and an unknown kind. */
const char *layout_question_ask(LayoutQuestion q, char *buf, size_t cap);

/* The open chain, INNERMOST FIRST, stopping after `frames` nodes — pass `layout_question_repeat`'s answer and
   the last node printed is the repeated one. `*cut` is set when the buffer ran out before that. TOTAL. */
const char *layout_question_chain(unsigned frames, char *buf, size_t cap, bool *cut);

/* The innermost open question's node, or NULL. Exported for the transition macro's LIFO check ONLY: it hands
   back a pointer to a live C frame, so a caller that stores one has stored a dangling pointer by construction.
   §Confident-change's contract for this component is the PAIR of macros below and not these three entries. */
LayoutQuestionOpen *layout_question_top(void);

void layout_question_push(LayoutQuestionOpen *slot, LayoutQuestion q);
void layout_question_pop(LayoutQuestionOpen *slot);

/* ---- THE TRANSITION, AS A MACRO AT EACH ASKING SITE ----------------------------------------------------
   `remedy` is the KIND'S OWN sentence: what the reader should do about a cycle in THIS question, which the
   entry knows and this component cannot. The generic half — that a question which is its own input has no
   fixed point, and that a depth cap answers nothing here — is stated once, below.
   At -DAPICLIENT_DEV=0 both expand to nothing, so the asking entry is a plain forwarder and no chain exists.
   THE DEV/RELEASE SPLIT LIVES HERE AND NOWHERE ELSE: layout_question.c is compiled in both builds and simply
   has no caller in release, because putting the same decision in two places gives it a copy that can drift. */
#if APICLIENT_DEV
#define LAYOUT_QUESTION_OPEN(slot, question, remedy) do {                                                    \
    LayoutQuestion lq_q_ = (question);                                                                       \
    unsigned lq_up_ = layout_question_repeat(lq_q_);                                                         \
                                                                                                             \
    if (lq_up_ != 0) {                                                                                       \
        char lq_ask_[LAYOUT_QUESTION_ASK_CAP];                                                               \
        char lq_chain_[LAYOUT_QUESTION_CHAIN_CAP];                                                           \
        bool lq_cut_ = false;                                                                                \
        const char *lq_at_ = layout_question_ask(lq_q_, lq_ask_, sizeof lq_ask_);                            \
        const char *lq_c_ = layout_question_chain(lq_up_, lq_chain_, sizeof lq_chain_, &lq_cut_);            \
                                                                                                             \
        DFAILF("%s IS ITS OWN INPUT — the identical question is already open %u frame(s) up and nothing "     \
               "between the two occurrences made progress, so there is no fixed point to reach. The open "   \
               "chain, innermost first: %s%s. %s A DEPTH CAP WOULD ANSWER NOTHING HERE and §NO BOUNDS "       \
               "forbids one: an arbitrarily deep box tree is a legitimate document and passes this check, "  \
               "so the REPETITION is the defect and not the depth",                                          \
               lq_at_, lq_up_, lq_c_, lq_cut_ ? " <- …" : "", (remedy));                                     \
    }                                                                                                        \
    layout_question_push((slot), lq_q_);                                                                     \
} while (0)

/* THE POP IS CHECKED AT THE SITE THAT PUSHED, because the chain's whole soundness is that a node is removed
   by the frame that added it, in order. A slot that is not the top means a frame returned without closing its
   question — the one failure an intrusive chain has, and the one a heap mirror would have silently. */
#define LAYOUT_QUESTION_CLOSE(slot) do {                                                                     \
    DCHECK(layout_question_top() == (slot),                                                                  \
           "a layout question is being closed by a frame that is not the innermost open one, so some frame " \
           "between them returned without closing its own. The chain is the shape of the C activation "      \
           "chain and is maintained by nothing else, so a missed close makes every later repeat test read "  \
           "against a question whose frame is gone — build the close into the same statement as the open "   \
           "at whichever entry took an early return");                                                       \
    layout_question_pop((slot));                                                                             \
} while (0)
#else
#define LAYOUT_QUESTION_OPEN(slot, question, remedy)  ((void) 0)
#define LAYOUT_QUESTION_CLOSE(slot)                   ((void) 0)
#endif

#endif
