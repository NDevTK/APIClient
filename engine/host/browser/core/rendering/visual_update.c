/* A DOCUMENT'S PENDING VISUAL UPDATE — HTML §8.1.7.3's Unnecessary rendering step's visible effect. See
   visual_update.h. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/rendering/visual_update.h"

static JSClassID g_slot = JS_INVALID_CLASS_ID;   /* the per-realm record (a baseline object) */
static JSAtom    g_atom_pending = JS_ATOM_NULL;
/* The runtime this component was declared in, and the only slot that says "declared". */
static JSRuntime *g_rt;

/* This document's record, OWNED. */
static JSValue vu_record(JSContext *docctx)
{
    JSValue rec;

    DCHECK(g_rt != NULL, "a document's pending visual update was reached before the component was declared, "
                         "or after visual_update_free gave its slot back");
    rec = realm_value_get(docctx, g_slot);
    DCHECK(JS_IsObject(rec), "a realm has no pending-visual-update record — visual_update_install_proto is "
                             "on realm.h's one list, so this realm skipped it");
    return rec;
}

static void vu_set(JSContext *docctx, bool pending)
{
    JSValue rec = vu_record(docctx);

    JS_SetProperty(docctx, rec, g_atom_pending, JS_NewBool(docctx, pending));
    JS_FreeValue(docctx, rec);
}

bool visual_update_pending(JSContext *docctx)
{
    JSValue rec = vu_record(docctx), v = JS_GetProperty(docctx, rec, g_atom_pending);
    bool pending = JS_ToBool(docctx, v);

    JS_FreeValue(docctx, v);
    JS_FreeValue(docctx, rec);
    return pending;
}

void visual_update_taken(JSContext *docctx)
{
    if (visual_update_pending(docctx)) vu_set(docctx, false);
}

/* DOM §4.2.3's children changed steps: `parent`'s children or data changed, so its document's rendering may
   have. Only a realm's active document is rendered (document_active_realm_of answers NULL for a DOMParser or
   createHTMLDocument document). `ctx` is the mutating realm and decides nothing here. Written only on the
   change from clear, so a flow's run of mutations captures the record once. */
static void vu_children_changed(JSContext *ctx, lxb_dom_node_t *parent)
{
    JSContext *docctx;

    (void)ctx;
    DCHECK(parent != NULL && parent->owner_document != NULL,
           "DOM §4.2.3's children changed steps named a parent that has no node document");
    docctx = document_active_realm_of(lxb_dom_interface_node(parent->owner_document));
    if (!docctx) return;
    if (!visual_update_pending(docctx)) vu_set(docctx, true);
}

void visual_update_init(JSContext *ctx)
{
    DCHECK(g_rt == NULL, "visual_update_init ran twice — one declaration per agent");
    g_rt = JS_GetRuntime(ctx);
    g_atom_pending = JS_NewAtom(ctx, "visualUpdatePending");
    CHECK(g_atom_pending != JS_ATOM_NULL, "the pending-visual-update key could not be interned");
    g_slot = realm_value_declare(ctx, "§8.1.7.3 a document's pending visual update");
    node_add_children_changed_hook(vu_children_changed);
    agent_state_ptr("visual_update", &g_rt,
                    "the runtime the pending-visual-update realm-value slot was declared in");
    agent_state_atom("visual_update", &g_atom_pending, "the per-realm record's key");
    agent_state_realm_slot("visual_update", &g_slot,
                           "the per-realm slot a document's pending visual update lives in");
    realm_declare_intrinsic(visual_update_install_proto);
}

void visual_update_install_proto(JSContext *ctx)
{
    JSValue rec;

    DCHECK(g_rt != NULL, "a realm asked for its pending-visual-update record before the component was declared");
    /* Built with the realm, so the record is the pre-boot baseline's and a flow's write is captured by its
       COW delta rather than private to it. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "a document's pending-visual-update record could not be allocated");
    JS_SetProperty(ctx, rec, g_atom_pending, JS_FALSE);
    realm_value_set(ctx, g_slot, rec);
}

void visual_update_free(JSRuntime *rt)
{
    DCHECK(g_rt != NULL, "the pending visual update was released in an agent that never declared it");
    DCHECK(g_rt == rt, "the pending visual update was released against a runtime other than the one it was "
                       "declared in");
    /* The records are the realms' — each goes with its context. node.c gives the hook list back per agent. */
    JS_FreeAtomRT(rt, g_atom_pending);
    g_atom_pending = JS_ATOM_NULL;
    g_slot = JS_INVALID_CLASS_ID;
    g_rt = NULL;
}
