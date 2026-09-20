/* THE LAYOUT QUESTION AND ITS OPEN CHAIN. See layout_question.h for why the tuple is the thing layout's
   recursion is over, for why the chain is intrusive and must stay intrusive until a resolver replaces the C
   frames it mirrors, and for why every formatter here answers with a SENTENCE rather than asserting. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "core/layout/box_subject.h"
#include "core/layout/layout_question.h"

/* Innermost first; NULL when no layout question is open. A FILE STATIC, which is sound only because layout
   cannot suspend — see the header's retirement condition. */
static LayoutQuestionOpen *g_layout_open;

static const char *const LAYOUT_QUESTION_LABELS[] = { LAYOUT_QUESTIONS(LAYOUT_QUESTION_LABEL) };

/* TOTAL, like everything else on this path: an out-of-range kind is reported as one rather than indexed. The
   kind is this engine's own enum, so an out-of-range value is a defect — and the site that can say so is the
   one that COMPOSED the question, not the one printing a message about a different defect entirely. */
static const char *layout_question_label(LayoutQuestionKind kind)
{
    if ((unsigned) kind >= (unsigned) LAYOUT_Q__COUNT) return "(a layout question of no declared kind)";
    return LAYOUT_QUESTION_LABELS[kind];
}

/* TWO QUESTIONS ARE THE SAME QUESTION WHEN ALL FOUR FIELDS AGREE, and `name` is compared by VALUE rather
   than by pointer because the asking frames hold their own copies of the property-name literals — two frames
   asking `margin-top` are asking one question whether or not the compiler pooled the two strings. */
static bool layout_question_same(LayoutQuestion a, LayoutQuestion b)
{
    if (a.kind != b.kind || a.element != b.element || a.code != b.code) return false;
    if (a.name == NULL || b.name == NULL) return a.name == b.name;
    return strcmp(a.name, b.name) == 0;
}

unsigned layout_question_repeat(LayoutQuestion q)
{
    const LayoutQuestionOpen *w;
    unsigned n = 0;

    /* A question with no element is not a question about a box and cannot repeat meaningfully. The entry that
       owns such a kind asserts its own operands — `uv_px_ask`'s first `DCHECK` is that one — and answering
       "cycle" for a pair of malformed asks would report THIS component's invariant in place of that entry's,
       which is the failure box_subject.h describes of a helper that asserts during message composition. */
    if (q.element == NULL) return 0;
    for (w = g_layout_open; w != NULL; w = w->below) {
        n++;
        if (layout_question_same(w->q, q)) return n;
    }
    return 0;
}

const char *layout_question_ask(LayoutQuestion q, char *buf, size_t cap)
{
    char sbuf[160];
    char pbuf[96];
    int n;

    if (buf == NULL || cap == 0) return "(nowhere to compose a layout question)";
    /* The PARAMETER renders as whichever of the two fields the kind uses — see the header. Both empty is a
       kind with no parameter and reads as the bare label, which is right: the element is then the whole of
       the question's identity. */
    if (q.name != NULL && q.code != 0)     n = snprintf(pbuf, sizeof pbuf, " `%s`#%u", q.name, q.code);
    else if (q.name != NULL)               n = snprintf(pbuf, sizeof pbuf, " `%s`", q.name);
    else if (q.code != 0)                  n = snprintf(pbuf, sizeof pbuf, " #%u", q.code);
    else                                   { pbuf[0] = '\0'; n = 0; }
    if (n < 0) pbuf[0] = '\0';
    n = snprintf(buf, cap, "%s%s of %s", layout_question_label(q.kind), pbuf,
                 box_subject(q.element, sbuf, sizeof sbuf));
    return n < 0 ? "(a layout question that could not be composed)" : buf;
}

const char *layout_question_chain(unsigned frames, char *buf, size_t cap, bool *cut)
{
    const LayoutQuestionOpen *p;
    char abuf[LAYOUT_QUESTION_ASK_CAP];
    size_t at = 0;
    unsigned i = 0;

    if (cut != NULL) *cut = false;
    if (buf == NULL || cap == 0) return "(nowhere to compose an open chain)";
    buf[0] = '\0';
    for (p = g_layout_open; p != NULL && i < frames; p = p->below, i++) {
        int k = snprintf(buf + at, cap - at, "%s%s", at == 0 ? "" : " <- ",
                         layout_question_ask(p->q, abuf, sizeof abuf));

        /* The chain is composed on the ABORTING frame and the buffer is what a reader can act on, so a chain
           that outruns it SAYS SO rather than arriving as prose nobody can tell was shortened. It is not a
           bound on the chain, which has no capacity at all. */
        if (k < 0 || (size_t) k >= cap - at) { if (cut != NULL) *cut = true; break; }
        at += (size_t) k;
    }
    return buf;
}

LayoutQuestionOpen *layout_question_top(void)
{
    return g_layout_open;
}

void layout_question_push(LayoutQuestionOpen *slot, LayoutQuestion q)
{
    if (slot == NULL) return;   /* TOTAL: nothing here may abort — see the header */
    slot->below = g_layout_open;
    slot->q = q;
    g_layout_open = slot;
}

/* THE RESTORE IS FROM THE SLOT'S OWN `below` AND NOT A DECREMENT, which is what makes the chain agree with
   the C stack by construction: whatever the frames above did, this frame puts back exactly the chain it
   found. Whether it was the innermost is the caller's `LAYOUT_QUESTION_CLOSE` assert, expanded at the site
   that pushed so it names that site and not this line. */
void layout_question_pop(LayoutQuestionOpen *slot)
{
    if (slot == NULL) return;
    g_layout_open = slot->below;
}
