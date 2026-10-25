/* css-cascade-5 §6.2 "Cascading Origins"' author origin as one root's sheets present it — see
   core/css/css_author_sheets.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_author_sheets.h"
#include "core/css/css_layer_order.h"
#include "core/css/media_query.h"   /* the memo of `@media` answers one run's emission reads */
#include "core/css/css_rule.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "core/dom/document.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */
#include "solver/reclaim.h"

struct CssAuthorSheets {
    lxb_dom_node_t     *root;
    CssLayerOrder      *order;    /* owned; sealed by the build's last run */
    CssAuthorSheet     *v;        /* owned, `n` long, in the sheet list's order */
    uint32_t            n, cap;
    CssPropertyRegistrationEntry *regs;   /* every applying sheet's `@property` registrations, in order, owned */
    uint32_t            nregs, regs_cap;
    uint32_t            at;       /* the sheet list position the build reads next */
    CssRuleCascadeWalk *walk;     /* owned: the walk over the sheet at `at`, NULL between sheets */
    bool                sealed;
};

static void cas_push(CssAuthorSheets *s, uint32_t si, CssRuleCascadeSheet view)
{
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 8;
        CssAuthorSheet *grown = lexbor_realloc(s->v, (size_t)cap * sizeof *grown);

        CHECK(grown != NULL, "cssom: OOM recording an applying sheet of the author origin");
        s->v = grown;
        s->cap = cap;
    }
    s->v[s->n].si = si;
    s->v[s->n].view = view;
    s->n++;
}

/* `view`'s registrations moved onto the set's, after every earlier sheet's (document order across sheets). */
static void cas_take_registrations(CssAuthorSheets *s, CssRuleCascadeSheet *view)
{
    uint32_t i;

    if (view->nregs == 0) return;
    if (s->nregs + view->nregs > s->regs_cap) {
        uint32_t cap = s->regs_cap ? s->regs_cap : 8;
        CssPropertyRegistrationEntry *grown;

        while (cap < s->nregs + view->nregs) cap *= 2;
        grown = lexbor_realloc(s->regs, (size_t)cap * sizeof *grown);
        CHECK(grown != NULL, "cssom: OOM recording the author origin's `@property` registrations");
        s->regs = grown;
        s->regs_cap = cap;
    }
    for (i = 0; i < view->nregs; i++) s->regs[s->nregs++] = view->regs[i];
    free(view->regs);
    view->regs = NULL;
    view->nregs = 0;
}

/* The sheets hang off the root's wrapper, which belongs to the realm whose record owns the document. */
static JSContext *cas_realm(const CssAuthorSheets *s)
{
    JSContext *ctx = document_realm_of(s->root);

    DCHECK(ctx != NULL,
           "the author origin was built for a root whose document has no realm record — CSSOM §6.2's sheet "
           "list lives on that document's root wrapper, so give the caller's document a record");
    return ctx;
}

CssAuthorSheets *css_author_sheets_open(lxb_dom_node_t *root)
{
    CssAuthorSheets *s;

    DCHECK(root != NULL && root->parent == NULL, "the author origin was built for a node that is not a root");
    s = lexbor_calloc(1, sizeof *s);
    CHECK(s != NULL, "cssom: OOM building the author origin's sheet set");
    s->root = root;
    s->order = css_layer_order_create();
    return s;
}

bool css_author_sheets_run(CssAuthorSheets *s, bool rests)
{
    JSContext *ctx = cas_realm(s);
    MediaQueryMemo *memo;
    JSValue sheets;
    uint32_t ns = 0;

    DCHECK(!s->sealed, "the author origin's sheet set was built on after it was sealed");
    /* The `@media` answers this run reads, shed at its return so none is kept across a rest. */
    memo = media_query_memo_open(ctx);
    sheets = style_sheet_list_of(ctx, s->root);
    if (JS_IsArray(sheets)) {
        JSValue len = JS_GetPropertyStr(ctx, sheets, "length");

        JS_ToUint32(ctx, &ns, len);
        JS_FreeValue(ctx, len);
    }
    for (; s->at < ns; s->at++) {
        CssRuleCascadeSheet view = { NULL, NULL, 0, NULL, 0 };
        CssRuleCascadeStep step;

        if (s->walk == NULL) {
            JSValue sheet = JS_GetPropertyUint32(ctx, sheets, s->at), rules;
            bool applies;

            DCHECK(css_style_sheet_is(sheet),
                   "CSSOM §6.2's list holds something that is not a CSS style sheet — its add is the one thing "
                   "that ever puts one in");
            /* CSSOM §6.1's disabled flag, then HTML §4.2.4.1 "Processing the media attribute"'s outer gate; a
               conditional group rule inside the sheet is the emission's own question. */
            applies = !css_style_sheet_disabled(sheet) && css_style_sheet_media_matches(ctx, sheet);
            if (!applies) {
                JS_FreeValue(ctx, sheet);
                continue;
            }
            rules = css_style_sheet_rules(ctx, sheet);
            s->walk = css_rule_cascade_walk_open(ctx, rules, s->order);
            JS_FreeValue(ctx, rules);
            JS_FreeValue(ctx, sheet);
        }
        step = css_rule_cascade_walk_run(ctx, s->walk, rests, memo, &view);
        if (step == CSS_RULE_CASCADE_PARKED) {
            media_query_memo_close(memo);
            JS_FreeValue(ctx, sheets);
            return false;
        }
        css_rule_cascade_walk_free(ctx, s->walk);
        s->walk = NULL;
        if (step == CSS_RULE_CASCADE_FAILED) continue;
        cas_take_registrations(s, &view);
        /* A sheet of `@layer a, b;` alone emits no rule and has already declared its layers. */
        if (view.text == NULL) { css_rule_cascade_sheet_free(&view); continue; }
        cas_push(s, s->at, view);
    }
    media_query_memo_close(memo);
    JS_FreeValue(ctx, sheets);
    css_layer_order_seal(s->order);
    s->sealed = true;
    return true;
}

bool css_author_sheets_sealed(const CssAuthorSheets *s)
{
    DCHECK(s != NULL, "the author origin's sheet set's state was read through nothing");
    return s->sealed;
}

void css_author_sheets_free(CssAuthorSheets *s)
{
    uint32_t i;

    DCHECK(s != NULL, "the author origin's sheet set was freed through nothing");
    if (s->walk != NULL) css_rule_cascade_walk_free(cas_realm(s), s->walk);
    for (i = 0; i < s->n; i++) css_rule_cascade_sheet_free(&s->v[i].view);
    for (i = 0; i < s->nregs; i++) css_property_registration_entry_free(&s->regs[i]);
    free(s->regs);
    free(s->v);
    css_layer_order_free(s->order);
    free(s);
}

const lxb_dom_node_t *css_author_sheets_root(const CssAuthorSheets *s)
{
    DCHECK(s != NULL, "the author origin's root was read through no sheet set");
    return s->root;
}

CssLayerOrder *css_author_sheets_order(CssAuthorSheets *s)
{
    DCHECK(s != NULL && s->sealed, "the author origin's layer order was read through no sheet set or an open one");
    return s->order;
}

uint32_t css_author_sheets_count(const CssAuthorSheets *s)
{
    DCHECK(s != NULL && s->sealed, "the author origin's sheet count was read through no sheet set or an open one");
    return s->n;
}

bool css_author_sheets_registration(const CssAuthorSheets *s, const char *name, CssPropertyRegistration *out)
{
    uint32_t i;

    DCHECK(s != NULL && s->sealed && name != NULL && out != NULL,
           "a registration was read from no sheet set, an open one, for no name or with nowhere to put it");
    for (i = s->nregs; i-- > 0;) {
        const CssPropertyRegistrationEntry *x = &s->regs[i];

        if (strcmp(x->name, name) != 0) continue;
        out->syntax = reclaim_strdup(x->reg.syntax);
        out->inherits = x->reg.inherits;
        out->initial_value = x->reg.initial_value != NULL ? reclaim_strdup(x->reg.initial_value) : NULL;
        CHECK(out->syntax != NULL && (x->reg.initial_value == NULL || out->initial_value != NULL),
              "cssom: OOM copying a `@property` registration");
        return true;
    }
    return false;
}

const CssAuthorSheet *css_author_sheets_at(const CssAuthorSheets *s, uint32_t i)
{
    DCHECKF(s != NULL && s->sealed && i < s->n, "the author origin's sheet %u was read from a set of %u that is "
            "absent or open", i, s ? s->n : 0);
    return &s->v[i];
}
