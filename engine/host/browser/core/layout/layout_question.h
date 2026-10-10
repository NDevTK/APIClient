/* A layout question: the (kind, element, parameter) tuple layout's recursion is over, as a value.
 *
 * Layout is one strongly connected call graph across core/layout, entered both descending (a box asks its
 * children) and ascending (a box asks its containing block). Its recursion is over (element, question) pairs,
 * so repetition, not depth, is the invariant: an arbitrarily deep box tree is a legitimate document, while a
 * pair asked again while the identical pair is still open is a rule reading a term it is a term of.
 *
 * A question is what core/layout/layout_job.h stacks, memoizes and resumes. This header holds only the value
 * and its formatter. The formatter is total and must not assert: box_subject.h states why a helper reached
 * from a DFAILF message must answer with a sentence rather than abort. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LAYOUT_QUESTION_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LAYOUT_QUESTION_H
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* The kinds. A kind is an entry the cluster re-enters by name; a kind whose re-entry is legitimate would turn
   a working engine into an abort, so a kind is admitted only where arrival is an ask that is its own input.
   The label is display text for a DFAIL reason and never an identity anything matches on.

   Parameters, per kind:
   - LAYOUT_Q_USED_VALUE_PX: `name` is the property; `code` is 0.
   - LAYOUT_Q_UV_SIZED: `name` is the axis as the property naming it (`width` or `height`); `code` is the
     box type plus one. The box type is part of the question rather than derived from the element because
     css-flexbox-1 §9.4 "Cross Size Determination" step 7 lays a flex item out as a block-level box, which
     is a different question about the same element.
   - LAYOUT_Q_BF_BOX: `name` is NULL; `code` is the baseline pass plus one, so the commonest pass
     (`BF_BASELINE_NONE`, 0) still renders as a parameter. */
#define LAYOUT_QUESTIONS(X) \
    X(LAYOUT_Q_USED_VALUE_PX, "CSS 2.1 §6.1.3 \"Used values\"' used box-model length") \
    X(LAYOUT_Q_UV_SIZED, \
      "CSS 2.1 §10.4 \"Minimum and maximum widths: 'min-width' and 'max-width'\"'/§10.7's final pass") \
    X(LAYOUT_Q_BF_BOX, "CSS 2.1 §9.4.1 \"Block formatting contexts\"' stack contribution")

#define LAYOUT_QUESTION_ENUM(id, label) id,
#define LAYOUT_QUESTION_LABEL(id, label) label,
typedef enum { LAYOUT_QUESTIONS(LAYOUT_QUESTION_ENUM) LAYOUT_Q__COUNT } LayoutQuestionKind;

/* Equality is over all four fields; `name` compares by value. `name` is borrowed from the asking frame, which
   is sound because the only questions that outlive their asking frame (a suspended LAYOUT_Q_BF_BOX on a
   job's stack) carry no name. `code` encodes zero as "no numeric parameter", which is why a kind whose
   parameter can be zero encodes it one-based. */
typedef struct {
    LayoutQuestionKind kind;
    lxb_dom_element_t *element;
    const char        *name;
    unsigned           code;
} LayoutQuestion;

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

bool layout_question_same(LayoutQuestion a, LayoutQuestion b);

/* Buffer sizes for one rendered question and for a rendered chain. A chain that does not fit says so; this
   is a buffer on the aborting frame and not a limit on the chain. */
enum { LAYOUT_QUESTION_CHAIN_CAP = 512, LAYOUT_QUESTION_ASK_CAP = 224 };

/* One question, rendered into `buf`. Total: answers a sentence for a NULL buffer, a NULL element and an
   unknown kind. */
const char *layout_question_ask(LayoutQuestion q, char *buf, size_t cap);

#endif
