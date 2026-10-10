/* css-cascade-5 §6.2 "Cascading Origins"' author origin as one root's sheets present it — see
   core/css/css_author_sheets.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_author_sheets.h"
#include "core/css/css_layer_order.h"
#include "core/css/css_rule.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "core/dom/document.h"

struct CssAuthorSheets {
    lxb_dom_node_t *root;
    CssLayerOrder  *order;    /* owned; sealed by the build */
    CssAuthorSheet *v;        /* owned, `n` long, in the sheet list's order */
    uint32_t        n, cap;
};

static void cas_push(CssAuthorSheets *s, uint32_t si, CssRuleCascadeSheet view)
{
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 8;
        CssAuthorSheet *grown = realloc(s->v, (size_t)cap * sizeof *grown);

        CHECK(grown != NULL, "cssom: OOM recording an applying sheet of the author origin");
        s->v = grown;
        s->cap = cap;
    }
    s->v[s->n].si = si;
    s->v[s->n].view = view;
    s->n++;
}

CssAuthorSheets *css_author_sheets_build(lxb_dom_node_t *root)
{
    /* The sheets hang off the root's wrapper, which belongs to the realm whose record owns the document. */
    JSContext *ctx = document_realm_of(root);
    CssAuthorSheets *s;
    JSValue sheets;
    uint32_t ns = 0, si;

    DCHECK(root != NULL && root->parent == NULL, "the author origin was built for a node that is not a root");
    DCHECK(ctx != NULL,
           "the author origin was built for a root whose document has no realm record — CSSOM §6.2's sheet "
           "list lives on that document's root wrapper, so give the caller's document a record");
    s = calloc(1, sizeof *s);
    CHECK(s != NULL, "cssom: OOM building the author origin's sheet set");
    s->root = root;
    s->order = css_layer_order_create();
    sheets = style_sheet_list_of(ctx, root);
    if (JS_IsArray(sheets)) {
        JSValue len = JS_GetPropertyStr(ctx, sheets, "length");

        JS_ToUint32(ctx, &ns, len);
        JS_FreeValue(ctx, len);
    }
    for (si = 0; si < ns; si++) {
        JSValue sheet = JS_GetPropertyUint32(ctx, sheets, si);
        CssRuleCascadeSheet view = { NULL, NULL, 0 };
        bool applies, emitted = false;

        DCHECK(css_style_sheet_is(sheet),
               "CSSOM §6.2's list holds something that is not a CSS style sheet — its add is the one thing "
               "that ever puts one in");
        /* CSSOM §6.1's disabled flag, then HTML §4.2.4.1 "Processing the media attribute"'s outer gate; a
           conditional group rule inside the sheet is the emission's own question. */
        applies = !css_style_sheet_disabled(sheet) && css_style_sheet_media_matches(ctx, sheet);
        if (applies) {
            JSValue rules = css_style_sheet_rules(ctx, sheet);

            emitted = css_rule_cascade_sheet(ctx, rules, s->order, &view);
            JS_FreeValue(ctx, rules);
        }
        JS_FreeValue(ctx, sheet);
        if (!emitted) continue;
        /* A sheet of `@layer a, b;` alone emits no rule and has already declared its layers. */
        if (view.text == NULL) { css_rule_cascade_sheet_free(&view); continue; }
        cas_push(s, si, view);
    }
    JS_FreeValue(ctx, sheets);
    css_layer_order_seal(s->order);
    return s;
}

void css_author_sheets_free(CssAuthorSheets *s)
{
    uint32_t i;

    DCHECK(s != NULL, "the author origin's sheet set was freed through nothing");
    for (i = 0; i < s->n; i++) css_rule_cascade_sheet_free(&s->v[i].view);
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
    DCHECK(s != NULL, "the author origin's layer order was read through no sheet set");
    return s->order;
}

uint32_t css_author_sheets_count(const CssAuthorSheets *s)
{
    DCHECK(s != NULL, "the author origin's sheet count was read through no sheet set");
    return s->n;
}

const CssAuthorSheet *css_author_sheets_at(const CssAuthorSheets *s, uint32_t i)
{
    DCHECKF(s != NULL && i < s->n, "the author origin's sheet %u was read from a set of %u", i, s ? s->n : 0);
    return &s->v[i];
}
