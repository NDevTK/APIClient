/* A CSS property as an ID — see core/css/css_property_id.h. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <lexbor/css/css.h>

#include "check.h"
#include "core/css/css_property_id.h"

typedef struct {
    const char      *name;
    uintptr_t        lxb;
    CssComputedShape shape;
} CssPropertyRow;

static const CssPropertyRow CSS_PROPERTY_ROWS[CSS_PROPERTY_COUNT] = {
#define CSS_PROPERTY_ROW(id, name, lxb, shape) { name, lxb, CSS_COMPUTED_SHAPE_##shape },
    CSS_PROPERTY_LIST(CSS_PROPERTY_ROW)
#undef CSS_PROPERTY_ROW
};

/* The name index: a power-of-two open-addressed table over the rows, at most half full, built once per process
   from the immutable row table. A slot holds a row's id plus one, so a zeroed slot is empty. */
#define CSS_PROPERTY_INDEX_CAP 128u
_Static_assert(CSS_PROPERTY_COUNT * 2 <= CSS_PROPERTY_INDEX_CAP,
               "the property name index must stay at most half full; double CSS_PROPERTY_INDEX_CAP");
static uint8_t g_index[CSS_PROPERTY_INDEX_CAP];
static bool    g_index_built;

static uint32_t css_property_hash(const char *name)
{
    uint32_t h = 0x811c9dc5u;
    const unsigned char *p;

    for (p = (const unsigned char *)name; *p != '\0'; p++) {
        h ^= *p;
        h *= 0x01000193u;
    }
    return h;
}

/* Checks each row against lexbor's registry: a row naming a registry id is that entry under the same name, and
   a row naming none is a property the registry does not carry. */
static void css_property_index_build(void)
{
    unsigned i;

    for (i = 0; i < CSS_PROPERTY_COUNT; i++) {
        const CssPropertyRow *row = &CSS_PROPERTY_ROWS[i];
        const lxb_css_entry_data_t *e =
            lxb_css_property_by_name((const lxb_char_t *)row->name, strlen(row->name));
        uint32_t s = css_property_hash(row->name) & (CSS_PROPERTY_INDEX_CAP - 1);

        DCHECKF(row->lxb == LXB_CSS_PROPERTY__UNDEF ? e == NULL : e != NULL && e->unique == row->lxb,
                "`%s`: core/css/css_property_id.h's row disagrees with lexbor's property registry — it names "
                "registry id %u and the registry answers %s (id %u). Correct the row's registry column",
                row->name, (unsigned)row->lxb, e != NULL ? "an entry" : "no entry",
                e != NULL ? (unsigned)e->unique : 0u);
        while (g_index[s] != 0) {
            DCHECKF(strcmp(CSS_PROPERTY_ROWS[g_index[s] - 1].name, row->name) != 0,
                    "`%s`: core/css/css_property_id.h lists the property twice", row->name);
            s = (s + 1) & (CSS_PROPERTY_INDEX_CAP - 1);
        }
        g_index[s] = (uint8_t)(i + 1);
    }
    g_index_built = true;
}

CssPropertyId css_property_id_of(const char *name)
{
    uint32_t s;

    DCHECK(name != NULL, "a property id was asked for no name");
    if (!g_index_built) css_property_index_build();
    for (s = css_property_hash(name) & (CSS_PROPERTY_INDEX_CAP - 1); g_index[s] != 0;
         s = (s + 1) & (CSS_PROPERTY_INDEX_CAP - 1))
        if (strcmp(CSS_PROPERTY_ROWS[g_index[s] - 1].name, name) == 0) return (CssPropertyId)(g_index[s] - 1);
    return CSS_PROPERTY_INVALID;
}

const char *css_property_name(CssPropertyId id)
{
    DCHECKF((unsigned)id < CSS_PROPERTY_COUNT, "a property name was asked for id %u, which is no row", (unsigned)id);
    return CSS_PROPERTY_ROWS[id].name;
}

CssComputedShape css_property_shape(CssPropertyId id)
{
    DCHECKF((unsigned)id < CSS_PROPERTY_COUNT, "a computed shape was asked for id %u, which is no row", (unsigned)id);
    return CSS_PROPERTY_ROWS[id].shape;
}
