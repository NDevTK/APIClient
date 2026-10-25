/* The regions of a document whose children are the parse of unknown markup — see unknown_markup.h. */
#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/unknown_markup.h"
#include "solver/concolic.h"

static JSClassID g_slot = JS_INVALID_CLASS_ID;
static int g_ready;

/* This realm's list of [region wrapper, markup] entries. Owned. */
static JSValue um_list(JSContext *ctx)
{
    JSValue list = realm_value_get(ctx, g_slot);

    DCHECK(JS_IsArray(list), "a realm answered for its unknown-markup regions with no list — the list is built "
                             "with the realm by unknown_markup_install_document");
    return list;
}

static int64_t um_length(JSContext *ctx, JSValueConst list)
{
    int64_t n = 0;

    CHECK(JS_GetLength(ctx, list, &n) == 0, "unknown_markup: the region list's length could not be read");
    return n;
}

/* The entry index whose region is `region`, or -1. */
static int64_t um_find(JSContext *ctx, JSValueConst list, lxb_dom_node_t *region)
{
    int64_t n = um_length(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyInt64(ctx, list, i), w = JS_GetPropertyUint32(ctx, e, 0);
        bool hit = node_of(w) == region;

        JS_FreeValue(ctx, w);
        JS_FreeValue(ctx, e);
        if (hit) return i;
    }
    return -1;
}

/* Remove entry `at`, keeping the order of the rest: the list is a heap object the delta captures, so it is
   edited in place rather than replaced. */
static void um_remove(JSContext *ctx, JSValueConst list, int64_t at)
{
    int64_t n = um_length(ctx, list), i;

    for (i = at; i + 1 < n; i++)
        CHECK(JS_SetPropertyInt64(ctx, list, i, JS_GetPropertyInt64(ctx, list, i + 1)) >= 0,
              "unknown_markup: a region entry could not be moved");
    CHECK(JS_SetLength(ctx, list, n - 1) >= 0, "unknown_markup: the region list could not be shortened");
}

void unknown_markup_note(JSContext *ctx, lxb_dom_node_t *region, JSValueConst markup)
{
    JSValue list, e;
    int64_t at;

    DCHECK(g_ready, "an unknown-markup region was noted before unknown_markup_init declared the list");
    DCHECK(region != NULL && concolic_is(markup), "an unknown-markup region was noted with no node or with "
                                                  "markup that is not unknown");
    list = um_list(ctx);
    at = um_find(ctx, list, region);
    if (at >= 0) um_remove(ctx, list, at);
    e = JS_NewArray(ctx);
    CHECK(!JS_IsException(e), "unknown_markup: a region entry could not be allocated");
    CHECK(JS_SetPropertyUint32(ctx, e, 0, node_wrap(ctx, region)) >= 0 &&
          JS_SetPropertyUint32(ctx, e, 1, JS_DupValue(ctx, markup)) >= 0,
          "unknown_markup: a region entry could not be filled");
    CHECK(JS_SetPropertyInt64(ctx, list, um_length(ctx, list), e) >= 0,
          "unknown_markup: a region entry could not be appended");
    JS_FreeValue(ctx, list);
}

void unknown_markup_forget(JSContext *ctx, lxb_dom_node_t *region)
{
    JSValue list;
    int64_t at;

    DCHECK(g_ready, "an unknown-markup region was forgotten before unknown_markup_init declared the list");
    list = um_list(ctx);
    at = um_find(ctx, list, region);
    if (at >= 0) um_remove(ctx, list, at);
    JS_FreeValue(ctx, list);
}

int unknown_markup_count(JSContext *ctx)
{
    JSValue list;
    int64_t n;

    DCHECK(g_ready, "the unknown-markup regions were counted before unknown_markup_init declared the list");
    list = um_list(ctx);
    n = um_length(ctx, list);
    JS_FreeValue(ctx, list);
    DCHECK(n >= 0 && n <= INT32_MAX, "the unknown-markup region list's length is not a count");
    return (int)n;
}

JSValue unknown_markup_at(JSContext *ctx, int i, lxb_dom_node_t **region)
{
    JSValue list = um_list(ctx), e = JS_GetPropertyInt64(ctx, list, i), w, markup;

    DCHECK(JS_IsArray(e), "an unknown-markup region index past the list was read");
    w = JS_GetPropertyUint32(ctx, e, 0);
    markup = JS_GetPropertyUint32(ctx, e, 1);
    *region = node_of(w);
    DCHECK(*region != NULL && concolic_is(markup), "an unknown-markup region entry lost its node or its markup");
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, list);
    return markup;
}

void unknown_markup_init(JSContext *ctx)
{
    DCHECK(!g_ready, "unknown_markup_init ran twice — the region list is declared once per agent");
    g_slot = realm_value_declare(ctx, "the document's regions whose children are the parse of unknown markup");
    g_ready = 1;
    agent_state_realm_slot("document", &g_slot, "the per-realm list of unknown-markup regions");
}

void unknown_markup_install_document(JSContext *ctx)
{
    JSValue list;

    DCHECK(g_ready, "the unknown-markup region list was installed before unknown_markup_init ran");
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "unknown_markup: this realm's region list could not be allocated");
    realm_value_set(ctx, g_slot, list);
}

/* Released by its declarer, document_agent_free; the slot goes back through the `document` row's undo. */
void unknown_markup_free(void)
{
    DCHECK(g_ready, "the unknown-markup region list was released in an agent that never declared it");
    g_ready = 0;
    agent_state_reached("document");
}
