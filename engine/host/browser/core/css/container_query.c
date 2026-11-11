/* CSS Conditional 5 §5.4's per-element condition. See container_query.h. */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lexbor/core/base.h>
#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/container_query.h"
#include "core/css/container_selector.h"
#include "core/css/css_computed_value.h"
#include "core/css/media_query.h"
#include "core/dom/document.h"
#include "core/layout/used_value.h"
#include "solver/reclaim.h"

_Static_assert(CONTAINER_AXIS_HORIZONTAL == MEDIA_QUERY_AXIS_HORIZONTAL &&
                   CONTAINER_AXIS_VERTICAL == MEDIA_QUERY_AXIS_VERTICAL &&
                   CONTAINER_AXIS_INLINE == MEDIA_QUERY_AXIS_INLINE && CONTAINER_AXIS_BLOCK == MEDIA_QUERY_AXIS_BLOCK,
               "a container query's axes are named once, by the feature table that reads them");

/* One `<container-condition>`. `query` NULL with `raw` non-empty is a query this table cannot parse (a
   `<general-enclosed>` function), which selects no container; `style_or_scroll` is one naming §6.2's or §6.3's
   features. */
typedef struct {
    char          *name;    /* serialized `<container-name>`, NULL where the condition declares none */
    char          *raw;     /* the `<container-query>` as written, "" where it declares none */
    MediaQuerySet *query;
    bool           style_or_scroll;
} CqCondition;

struct ContainerQuery {
    CqCondition          *v;
    unsigned              n;
    const ContainerQuery *up;
};

/* Whether `raw` names a function at a token boundary: §5.4's `style( <style-query> )` and
   `scroll-state( <scroll-state-query> )` arms. */
static bool cq_names_function(const char *raw, const char *fn)
{
    size_t n = strlen(fn);
    const char *p;

    for (p = raw; *p != '\0'; p++)
        if (strncasecmp(p, fn, n) == 0 && (p == raw || !(p[-1] == '-' || p[-1] == '_' || isalnum((unsigned char)p[-1]))))
            return true;
    return false;
}

ContainerQuery *container_query_new(const CssContainerConditions *conds, const ContainerQuery *up)
{
    ContainerQuery *q;
    unsigned i;

    DCHECK(conds != NULL && conds->n > 0, "an `@container` rule's query was built from no conditions, which the "
                                          "`#` multiplier cannot produce");
    q = lexbor_calloc(1, sizeof *q);
    CHECK(q != NULL, "container queries: OOM holding an `@container` rule's conditions");
    q->v = lexbor_calloc(conds->n, sizeof *q->v);
    CHECK(q->v != NULL, "container queries: OOM holding an `@container` rule's conditions");
    q->n = conds->n;
    q->up = up;
    for (i = 0; i < conds->n; i++) {
        CqCondition *c = &q->v[i];

        c->name = conds->v[i].name[0] != '\0' ? reclaim_strdup(conds->v[i].name) : NULL;
        c->raw = reclaim_strdup(conds->v[i].query);
        CHECK(c->raw != NULL && (c->name != NULL || conds->v[i].name[0] == '\0'),
              "container queries: OOM copying a `<container-condition>`");
        if (c->raw[0] == '\0') continue;
        c->style_or_scroll = cq_names_function(c->raw, "style(") || cq_names_function(c->raw, "scroll-state(");
        if (!c->style_or_scroll) c->query = media_query_parse_container(c->raw);
    }
    return q;
}

void container_query_free(ContainerQuery *q)
{
    unsigned i;

    if (q == NULL) return;
    for (i = 0; i < q->n; i++) {
        free(q->v[i].name);
        free(q->v[i].raw);
        if (q->v[i].query != NULL) media_query_free(q->v[i].query);
    }
    free(q->v);
    free(q);
}

/* §6.1: a dimension in the condition resolves on the query container's computed values. */
static bool cq_length(void *ctx, double n, const char *unit, double *px)
{
    CssPx out;

    if (!css_computed_length_on((lxb_dom_element_t *)ctx, n, unit, &out)) return false;
    *px = out.px;
    return true;
}

/* One condition for `el`: 1 true, 0 false, -1 unknown. The container's content box is read only on the axes
   the query reads, which selection made sure it accepts. */
static int cq_condition(const CqCondition *c, lxb_dom_element_t *el)
{
    MediaQueryContainer env;
    lxb_dom_element_t *k;
    unsigned axes = 0, phys;

    if (c->style_or_scroll) {
        DFAILF("`@container %s%s%s`: CSS Conditional 5 §6.2 \"Style Container Features\" and §6.3 \"Scroll State "
               "Container Features\" are unbuilt. BUILD §6.2's style() over the container's computed custom "
               "properties, and §6.3's scroll-state() over its scroll container state; until then the condition "
               "is answered unknown", c->name ? c->name : "", c->name ? " " : "", c->raw);
        return -1;
    }
    if (c->raw[0] == '\0') return container_selector_find(el, 0, c->name) != NULL ? 1 : -1;
    /* §5.4: "If the <container-query> contains unknown or unsupported container features, no query container
       will be selected for that <container-condition>", and "If no ancestor is an eligible query container,
       then the container query is unknown for that element." */
    if (c->query == NULL || !media_query_container_axes(c->query, &axes)) return -1;
    k = container_selector_find(el, axes, c->name);
    if (k == NULL) return -1;
    phys = axes & (CONTAINER_AXIS_HORIZONTAL | CONTAINER_AXIS_VERTICAL);
    if (axes & CONTAINER_AXIS_INLINE) phys |= container_selector_physical(k, true);
    if (axes & CONTAINER_AXIS_BLOCK) phys |= container_selector_physical(k, false);
    env.width = (phys & CONTAINER_AXIS_HORIZONTAL) ? used_value_content_px(k, false).px : NAN;
    env.height = (phys & CONTAINER_AXIS_VERTICAL) ? used_value_content_px(k, true).px : NAN;
    env.inline_size = container_selector_physical(k, true) == CONTAINER_AXIS_VERTICAL ? env.height : env.width;
    env.block_size = container_selector_physical(k, false) == CONTAINER_AXIS_VERTICAL ? env.height : env.width;
    env.length = cq_length;
    env.length_ctx = k;
    return media_query_container_matches(
        document_active_realm_of(lxb_dom_interface_node(lxb_dom_interface_node(el)->owner_document)), c->query,
        &env);
}

bool container_query_matches(const ContainerQuery *q, lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "a container query was asked of no element");
    for (; q != NULL; q = q->up) {
        bool any = false;
        unsigned i;

        for (i = 0; i < q->n && !any; i++) any = cq_condition(&q->v[i], el) == 1;
        if (!any) return false;
    }
    return true;
}
