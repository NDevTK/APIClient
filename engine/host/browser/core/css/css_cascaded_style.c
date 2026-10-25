/* One element's cascaded values — see core/css/css_cascaded_style.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_cascade.h"
#include "core/css/css_cascaded_style.h"
#include "core/css/css_layer_order.h"
#include "core/css/css_logical.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

typedef struct {
    char               *name;         /* owned; a longhand */
    char               *value;        /* owned; never NULL */
    const CssLayerNode *layer;        /* borrowed from the layer order */
    uint32_t            specificity;
    uint32_t            seq;
    uint32_t            decl_order;
    CssOrigin           origin;
    bool                important;
    bool                element_attached;
} CsdDecl;

/* A declaration's place in the by-name order: its name, then its position in the collection. */
typedef struct {
    const char *name;
    uint32_t    at;
} CsdRef;

/* One resolved property. `name` is borrowed from a declaration or from css-logical-1's static partner names. */
typedef struct {
    const char *name;     /* NULL is empty */
    char       *value;    /* owned; NULL is css-cascade-5 §4.2's empty output */
} CsdValue;

typedef enum {
    CSD_COLLECTING = 0,
    CSD_UNGROUPED,      /* phase one running: the properties in no logical group */
    CSD_UNGROUPED_DONE, /* phase one's values readable; phase two waits for the first grouped ask */
    CSD_GROUPED,        /* phase two running: the group pairs */
    CSD_RESOLVED
} CsdPhase;

struct CssCascadedStyle {
    lxb_dom_element_t *el;
    CssLayerOrder     *layers;
    CsdDecl           *v;
    uint32_t           n, cap;
    CsdRef            *by_name;       /* `n` long once collection closes */
    CsdValue          *tab;           /* power-of-two table, at most half full */
    uint32_t           tab_cap, tab_used;
    CsdPhase           phase;
};

CssCascadedStyle *css_cascaded_style_create(lxb_dom_element_t *el, CssLayerOrder *layers)
{
    CssCascadedStyle *s;

    DCHECK(el != NULL && layers != NULL, "a cascaded style was opened for no element or under no layer order");
    s = calloc(1, sizeof *s);
    CHECK(s != NULL, "css-cascaded-style-oom: an element's cascaded style could not be allocated");
    s->el = el;
    s->layers = layers;
    return s;
}

void css_cascaded_style_free(CssCascadedStyle *s)
{
    uint32_t i;

    DCHECK(s != NULL, "a cascaded style was freed through nothing");
    DCHECK(s->phase == CSD_RESOLVED || s->phase == CSD_UNGROUPED_DONE || s->phase == CSD_COLLECTING,
           "a cascaded style was released in the middle of its resolution — the record goes with its job, and "
           "an element's resolution finishes inside the hold of the ask that started it");
    for (i = 0; i < s->n; i++) {
        free(s->v[i].name);
        free(s->v[i].value);
    }
    for (i = 0; i < s->tab_cap; i++) free(s->tab[i].value);
    free(s->v);
    free(s->by_name);
    free(s->tab);
    free(s);
}

lxb_dom_element_t *css_cascaded_style_element(const CssCascadedStyle *s)
{
    return s->el;
}

static char *csd_strdup(const char *t)
{
    char *out = strdup(t);

    CHECK(out != NULL, "css-cascaded-style-oom: a declaration could not be copied into an element's cascade");
    return out;
}

void css_cascaded_style_add(CssCascadedStyle *s, const char *name, CssOrigin origin, bool important,
                            bool element_attached, const CssLayerNode *layer, uint32_t specificity, uint32_t seq,
                            uint32_t decl_order, const char *value)
{
    CsdDecl *d;

    DCHECK(s != NULL && s->phase == CSD_COLLECTING, "a declaration was added to a cascaded style that is closed");
    DCHECK(name != NULL && value != NULL, "a declaration was added to a cascaded style with no name or value");
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 32;
        CsdDecl *grown = lexbor_realloc(s->v, (size_t)cap * sizeof *grown);

        CHECK(grown != NULL, "css-cascaded-style-oom: an element's declarations could not be grown");
        s->v = grown;
        s->cap = cap;
    }
    d = &s->v[s->n++];
    d->name = csd_strdup(name);
    d->value = csd_strdup(value);
    d->layer = layer;
    d->specificity = specificity;
    d->seq = seq;
    d->decl_order = decl_order;
    d->origin = origin;
    d->important = important;
    d->element_attached = element_attached;
}

static int csd_ref_cmp(const void *a, const void *b)
{
    const CsdRef *x = a, *y = b;
    int c = strcmp(x->name, y->name);

    if (c != 0) return c;
    return x->at < y->at ? -1 : x->at > y->at;
}

static uint32_t csd_hash(const char *name)
{
    uint32_t h = 0x811c9dc5u;
    const unsigned char *p;

    for (p = (const unsigned char *)name; *p != '\0'; p++) {
        h ^= *p;
        h *= 0x01000193u;
    }
    return h;
}

/* The slot holding `name` or the first empty one, for the read and the write alike. */
static uint32_t csd_probe(const CsdValue *tab, uint32_t cap, const char *name)
{
    uint32_t i = csd_hash(name) & (cap - 1);

    while (tab[i].name != NULL && strcmp(tab[i].name, name) != 0) i = (i + 1) & (cap - 1);
    return i;
}

static void csd_store(CssCascadedStyle *s, const char *name, char *value)
{
    uint32_t i = csd_probe(s->tab, s->tab_cap, name);

    DCHECKF(s->tab[i].name == NULL, "`%s` was resolved twice on one element — each property is resolved once, "
                                    "with its group partner when it has one", name);
    DCHECK(s->tab_used * 2 < s->tab_cap, "an element's cascaded values outgrew the table sized for them");
    s->tab[i].name = name;
    s->tab[i].value = value;
    s->tab_used++;
}

/* The run of declarations named `name` in the by-name order: its first index, and its length. */
static uint32_t csd_run(const CssCascadedStyle *s, const char *name, uint32_t *len)
{
    uint32_t lo = 0, hi = s->n, start;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (strcmp(s->by_name[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    start = lo;
    while (lo < s->n && strcmp(s->by_name[lo].name, name) == 0) lo++;
    *len = lo - start;
    return start;
}

/* Add a run to `c`, asserting the hint and UA origins declare the property at most once. Answers how many of
   the run's declarations are each of those two origins', so a pair's caller can assert the same across both
   members. */
static void csd_add_run(const CssCascadedStyle *s, CssCascade *c, uint32_t start, uint32_t len,
                        unsigned *hints, unsigned *uas)
{
    uint32_t k;

    for (k = start; k < start + len; k++) {
        const CsdDecl *d = &s->v[s->by_name[k].at];

        if (d->origin == CSS_ORIGIN_PRESENTATIONAL_HINT) ++*hints;
        if (d->origin == CSS_ORIGIN_UA) ++*uas;
        css_cascade_add(c, d->origin, d->important, d->element_attached, d->layer, d->specificity, d->seq,
                        d->decl_order, d->value);
    }
}

/* The cascade over one run, or over a group pair's two runs together (css-logical-1 §4: "This shared value is
   determined by cascading the declarations of both properties together as one"). Owned. */
static char *csd_cascade(const CssCascadedStyle *s, const char *name, uint32_t start, uint32_t len,
                         uint32_t pstart, uint32_t plen)
{
    CssCascade *c = css_cascade_create(s->layers);
    unsigned hints = 0, uas = 0;
    char *out;

    csd_add_run(s, c, start, len, &hints, &uas);
    csd_add_run(s, c, pstart, plen, &hints, &uas);
    /* The hint origin and the UA origin are each one position in css-cascade-5 §6.1's Order of Appearance,
       and HTML §15 states each margin, padding and dimension in one spelling per element, so neither origin
       declares both members of a pair; two would need an order between rows these tables do not record. */
    DCHECKF(hints <= 1 && uas <= 1,
            "`%s`: the presentational hint origin declares it %u times and the UA origin %u times across its "
            "css-logical-1 §4 group pair on one element. Each origin is one Order of Appearance position, so "
            "give the origin's table an order over its rows and pass it as the declaration position",
            name, hints, uas);
    out = css_cascade_value(c);
    css_cascade_free(c);
    return out;
}

void css_cascaded_style_resolve(CssCascadedStyle *s)
{
    uint32_t i, len, cap = 16;

    DCHECK(s != NULL && s->phase == CSD_COLLECTING, "a cascaded style was resolved twice");
    s->by_name = calloc(s->n ? s->n : 1, sizeof *s->by_name);
    CHECK(s->by_name != NULL, "css-cascaded-style-oom: an element's declaration order could not be built");
    for (i = 0; i < s->n; i++) {
        s->by_name[i].name = s->v[i].name;
        s->by_name[i].at = i;
    }
    qsort(s->by_name, s->n, sizeof *s->by_name, csd_ref_cmp);
    /* Every declared name, and a partner for each grouped one, at most half full. */
    while (cap < 4u * s->n + 4u) cap *= 2;
    s->tab = calloc(cap, sizeof *s->tab);
    CHECK(s->tab != NULL, "css-cascaded-style-oom: an element's cascaded values could not be allocated");
    s->tab_cap = cap;

    s->phase = CSD_UNGROUPED;
    for (i = 0; i < s->n; i += len) {
        const char *name = s->by_name[i].name;
        bool physical;

        csd_run(s, name, &len);
        if (css_logical_group_of(name, &physical) != CSS_LOGICAL_GROUP_NONE) continue;
        csd_store(s, name, csd_cascade(s, name, i, len, 0, 0));
    }
    s->phase = CSD_UNGROUPED_DONE;
}

/* Phase two, at the first ask of a grouped property: by then this element's `writing-mode` and `direction`
   can be computed without re-entering a derivation that is waiting on phase one. */
static void csd_resolve_grouped(CssCascadedStyle *s)
{
    uint32_t i, len;

    s->phase = CSD_GROUPED;
    for (i = 0; i < s->n; i += len) {
        const char *name = s->by_name[i].name;
        const char *partner;
        uint32_t pstart, plen;
        bool physical;
        char *value;

        csd_run(s, name, &len);
        if (css_logical_group_of(name, &physical) == CSS_LOGICAL_GROUP_NONE) continue;
        if (s->tab[csd_probe(s->tab, s->tab_cap, name)].name != NULL) continue;  /* a partner already resolved */
        partner = css_logical_partner_of(s->el, name);
        DCHECKF(partner != NULL && strcmp(partner, name) != 0,
                "`%s` is in a css-logical-1 §4 group and the pairing answered %s for it — the two members have "
                "different mapping logic, so a grouped property always has a partner and is never its own",
                name, partner != NULL ? "itself" : "no partner");
        pstart = csd_run(s, partner, &plen);
        value = csd_cascade(s, name, i, len, pstart, plen);
        csd_store(s, name, value);
        csd_store(s, partner, value != NULL ? csd_strdup(value) : NULL);
    }
    s->phase = CSD_RESOLVED;
}

const char *css_cascaded_style_value(CssCascadedStyle *s, const char *name)
{
    bool physical, grouped;
    uint32_t i;

    DCHECK(s != NULL && name != NULL, "a cascaded value was asked of no record or for no property");
    DCHECKF(s->phase != CSD_COLLECTING && s->phase != CSD_UNGROUPED,
            "`%s`'s cascaded value was asked while its element's declarations are still being collected or its "
            "ungrouped properties resolved — neither step asks a cascaded value", name);
    grouped = css_logical_group_of(name, &physical) != CSS_LOGICAL_GROUP_NONE;
    DCHECKF(!grouped || s->phase != CSD_GROUPED,
            "`%s`'s cascaded value was asked while its element's logical groups are being paired. css-logical-1 "
            "§4 asks only `writing-mode`, `direction` and `text-orientation` mid-pairing, and those are in no "
            "group, so this is a pairing step that reads its own element's unfinished answers", name);
    if (grouped && s->phase == CSD_UNGROUPED_DONE) csd_resolve_grouped(s);
    i = csd_probe(s->tab, s->tab_cap, name);
    return s->tab[i].name != NULL ? s->tab[i].value : NULL;
}
