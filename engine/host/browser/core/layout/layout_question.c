/* The layout question and its formatter — see layout_question.h. Nothing here may assert: a DFAILF message
   reaches it, and box_subject.h states why such a helper answers with a sentence. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "core/layout/box_subject.h"
#include "core/layout/layout_question.h"

static const char *const LAYOUT_QUESTION_LABELS[] = { LAYOUT_QUESTIONS(LAYOUT_QUESTION_LABEL) };

static const char *layout_question_label(LayoutQuestionKind kind)
{
    if ((unsigned) kind >= (unsigned) LAYOUT_Q__COUNT) return "(a layout question of no declared kind)";
    return LAYOUT_QUESTION_LABELS[kind];
}

/* `name` compares by value: two frames asking `margin-top` ask one question whether or not the compiler
   pooled the two literals. */
bool layout_question_same(LayoutQuestion a, LayoutQuestion b)
{
    if (a.kind != b.kind || a.element != b.element || a.code != b.code) return false;
    if (a.name == NULL || b.name == NULL) return a.name == b.name;
    return strcmp(a.name, b.name) == 0;
}

const char *layout_question_ask(LayoutQuestion q, char *buf, size_t cap)
{
    char sbuf[160];
    char pbuf[96];
    int n;

    if (buf == NULL || cap == 0) return "(nowhere to compose a layout question)";
    /* The parameter renders as whichever of the two fields the kind uses; both empty is the bare label. */
    if (q.name != NULL && q.code != 0)     n = snprintf(pbuf, sizeof pbuf, " `%s`#%u", q.name, q.code);
    else if (q.name != NULL)               n = snprintf(pbuf, sizeof pbuf, " `%s`", q.name);
    else if (q.code != 0)                  n = snprintf(pbuf, sizeof pbuf, " #%u", q.code);
    else                                   { pbuf[0] = '\0'; n = 0; }
    if (n < 0) pbuf[0] = '\0';
    n = snprintf(buf, cap, "%s%s of %s", layout_question_label(q.kind), pbuf,
                 box_subject(q.element, sbuf, sizeof sbuf));
    return n < 0 ? "(a layout question that could not be composed)" : buf;
}
