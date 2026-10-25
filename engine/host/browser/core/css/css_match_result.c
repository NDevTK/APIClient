/* The author origin's declarations that apply to one element — see core/css/css_match_result.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_author_sheets.h"
#include "core/css/css_layer_order.h"
#include "core/css/css_match_result.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */
#include "solver/reclaim.h"

struct CssMatchResult {
    lxb_dom_element_t     *el;
    const CssAuthorSheets *sheets;
    CssMatchedDeclaration *v;        /* owned, `n` long, in the set's emission order */
    uint32_t               n, cap;
    uint32_t               span;
    bool                   finished;
    CssMatchPosition       at;         /* the build's position while open */
};

CssMatchResult *css_match_result_create(lxb_dom_element_t *el, const CssAuthorSheets *sheets)
{
    CssMatchResult *m;

    DCHECK(el != NULL && sheets != NULL, "a match result was opened for no element or under no sheet set");
    m = lexbor_calloc(1, sizeof *m);
    CHECK(m != NULL, "css-match-result-oom: an element's match result could not be allocated");
    m->el = el;
    m->sheets = sheets;
    return m;
}

void css_match_result_free(CssMatchResult *m)
{
    uint32_t i;

    DCHECK(m != NULL, "a match result was freed through nothing");
    for (i = 0; i < m->n; i++) {
        free(m->v[i].name);
        free(m->v[i].value);
    }
    free(m->v);
    free(m);
}

void css_match_result_add(CssMatchResult *m, const char *name, const char *value, bool important,
                          const CssLayerNode *layer, uint32_t specificity, uint32_t seq, uint32_t decl_order)
{
    CssMatchedDeclaration *d;

    DCHECK(m != NULL && !m->finished, "a declaration was added to a match result that is closed or absent");
    DCHECK(name != NULL && value != NULL, "a matched declaration was added with no name or no value");
    DCHECKF(m->n == 0 || m->v[m->n - 1].seq <= seq,
            "a matched declaration's rule position %u precedes the previous one's %u — the builder walks the "
            "set's emission in order, so the result is in css-cascade-5 §6.1's Order of Appearance",
            seq, m->n ? m->v[m->n - 1].seq : 0);
    if (m->n == m->cap) {
        uint32_t cap = m->cap ? m->cap * 2 : 16;
        CssMatchedDeclaration *grown = lexbor_realloc(m->v, (size_t)cap * sizeof *grown);

        CHECK(grown != NULL, "css-match-result-oom: an element's matched declarations could not be grown");
        m->v = grown;
        m->cap = cap;
    }
    d = &m->v[m->n];
    d->name = reclaim_strdup(name);
    d->value = reclaim_strdup(value);
    CHECK(d->name != NULL && d->value != NULL,
          "css-match-result-oom: a matched declaration could not be copied into the element's result");
    d->important = important;
    d->layer = layer;
    d->specificity = specificity;
    d->seq = seq;
    d->decl_order = decl_order;
    m->n++;
}

void css_match_result_finish(CssMatchResult *m, uint32_t span)
{
    DCHECK(m != NULL && !m->finished, "a match result was closed twice or through nothing");
    DCHECKF(m->n == 0 || m->v[m->n - 1].seq < span,
            "a match result was closed with span %u below its last rule position %u — the span is how many "
            "rules the set emitted, so every matched rule's position is below it",
            span, m->n ? m->v[m->n - 1].seq : 0);
    m->span = span;
    m->finished = true;
}

bool css_match_result_closed(const CssMatchResult *m)
{
    DCHECK(m != NULL, "a match result's state was read through nothing");
    return m->finished;
}

CssMatchPosition *css_match_result_position(CssMatchResult *m)
{
    DCHECK(m != NULL && !m->finished, "a match result's build position was asked of a closed result");
    return &m->at;
}

lxb_dom_element_t *css_match_result_element(const CssMatchResult *m)
{
    DCHECK(m != NULL, "a match result's element was read through nothing");
    return m->el;
}

const CssAuthorSheets *css_match_result_sheets(const CssMatchResult *m)
{
    DCHECK(m != NULL, "a match result's sheet set was read through nothing");
    return m->sheets;
}

uint32_t css_match_result_span(const CssMatchResult *m)
{
    DCHECK(m != NULL && m->finished, "a match result's span was read before the result was closed");
    return m->span;
}

uint32_t css_match_result_count(const CssMatchResult *m)
{
    DCHECK(m != NULL && m->finished, "a match result was read before it was closed");
    return m->n;
}

const CssMatchedDeclaration *css_match_result_at(const CssMatchResult *m, uint32_t i)
{
    DCHECKF(m != NULL && m->finished && i < m->n,
            "matched declaration %u was read from a result of %u that is absent or not closed",
            i, m ? m->n : 0);
    return &m->v[i];
}
