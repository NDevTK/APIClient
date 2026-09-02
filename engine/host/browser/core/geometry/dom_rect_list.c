/* GEOMETRY INTERFACES §4 — DOMRectList. See dom_rect_list.h. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/geometry/dom_rect.h"
#include "core/geometry/dom_rect_list.h"
#include "core/idl_args.h"
#include "core/idl_index_arg.h"
#include "core/idl_indexed.h"
#include "core/realm.h"
#include "solver/concolic.h"

static JSClassID g_list_class;
static JSValue   g_rects_key = JS_UNDEFINED;   /* the private Symbol the rectangle Array hangs off */
static JSAtom    g_atom_rects = JS_ATOM_NULL;
static int       g_id_item = -1;

/* THE BRAND. The object itself is an indexed-property object, which anything with an indexed getter is, so the
   thing that cannot be forged is the own SLOT this component put on it — the same brand collections.c uses for
   the same reason. Returns JS_UNDEFINED for anything that is not a DOMRectList. */
static JSValue drl_rects(JSContext *ctx, JSValueConst v)
{
    JSValue rects;

    if (!JS_IsObject(v)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &rects, v, g_atom_rects) <= 0)   /* an own SLOT, never a lookup */
        return JS_UNDEFINED;
    return rects;
}

/* §4: "The length attribute must return the total number of DOMRect objects associated with the object." */
static uint32_t drl_length(JSContext *ctx, JSValueConst self)
{
    JSValue rects = drl_rects(ctx, self), len;
    uint32_t n = 0;

    if (!JS_IsObject(rects)) { JS_FreeValue(ctx, rects); return 0; }
    len = JS_GetPropertyStr(ctx, rects, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_FreeValue(ctx, rects);
    return n;
}

/* THE INDEXED PROPERTY GETTER — JS_UNDEFINED past the end, which is what an index lookup outside the supported
   property indices is. §4's `item()` operation below turns that into the null its IDL declares. */
static JSValue drl_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue rects = drl_rects(ctx, self), r;

    if (!JS_IsObject(rects)) { JS_FreeValue(ctx, rects); return JS_UNDEFINED; }
    r = JS_GetPropertyUint32(ctx, rects, i);
    JS_FreeValue(ctx, rects);
    DCHECK(JS_IsUndefined(r) || dom_rect_is(r),
           "a DOMRectList held something that is not a DOMRect — its indexed getter and its `item` both declare "
           "`DOMRect?`, and the construction is the one place anything is ever put in one");
    return r;
}

static const IdlIndexedDecl DOM_RECT_LIST_INDEXED = { "DOMRectList", drl_length, drl_item, NULL, 0 };

/* The BRAND asked by the two PROTOTYPE members — Web IDL §3.7.6 Attributes' for `length`, §3.7.7 Operations'
   for `item`. The two decl callbacks above are reached only
   through an index lookup on an object idl_indexed already resolved, so they answer the empty list for a
   stranger; a member read off `DOMRectList.prototype` directly must THROW, because a page tells that apart
   from `undefined`. */
static bool drl_is(JSContext *ctx, JSValueConst v)
{
    JSValue rects = drl_rects(ctx, v);
    bool ok = JS_IsObject(rects);

    JS_FreeValue(ctx, rects);
    return ok;
}

/* §4's `item(index)` — "return null when index is greater than or equal to the number of DOMRect objects
 * associated with the DOMRectList. Otherwise, the DOMRect object at index must be returned." The difference
 * from the indexed getter is exactly that null, which is why both exist.
 *
 * IT IS A STEP MACHINE BECAUSE ITS ONE ARGUMENT CAN BE UNKNOWN. A `JS_ToUint32` of `argv[0]` with its return
 * discarded stood here — the shape core/idl_args.h bans by name — under a DCHECK that refused an unknown index
 * into a NON-EMPTY list and named the fork to build. That fork is core/idl_index_arg.h's elimination chain,
 * and the DCHECK's own sentence is what it now performs: one flow per supported index, plus the null arm for
 * an index past the end. */
#define DRL_ITEM_ALGORITHM "Geometry §4 The DOMRectList interface item(index)"
#define DRL_ITEM_STAGES(X)                                                                                    \
    X(DRL_ITEM_READ, DRL_ITEM_ALGORITHM " (return the DOMRect object at index, or null past the end)")
enum { IDL_STEP_STAGE_BASE(DRL_ITEM_STAGES) DRL_ITEM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DRL_ITEM_STEPS[] = { DRL_ITEM_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_drl_item(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdlIndexChain *s = state;
    uint32_t i = 0;
    bool past_end = false;
    JSValue r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == DRL_ITEM_READ,
           "§4's `item` resumed into a stage the algorithm does not have — it is ONE sentence, and the chain "
           "of questions it may ask is a cursor on this machine's own state rather than a stage apiece");
    DCHECK(argc == 1,
           "§4's `item` reached its body with an argument count its declaration does not produce — its one "
           "`unsigned long index` is required, so §3.6's argument-count check refuses a shorter call first");
    if (!drl_is(ctx, hdr->this_val)) {
        JS_ThrowTypeError(ctx, "DOMRectList.prototype.item was reached on something that is not a DOMRectList");
        return JS_STEP_ABRUPT;
    }
    if (concolic_is(argv[0])) {
        int rc = idl_index_chain_run(ctx, hdr, s, argv[0], drl_length(ctx, hdr->this_val),
                                     DRL_ITEM_ALGORITHM, &i, &past_end);
        if (rc)
            return rc;   /* parked at the fork */
        if (past_end) {
            /* §4's OWN past-the-end answer: null "when index is greater than or equal to the number of DOMRect
               objects associated with the DOMRectList". */
            *presult = JS_NULL;
            return JS_STEP_DONE;
        }
    } else {
        i = idl_index_arg_known(ctx, argv[0], DRL_ITEM_ALGORITHM);
    }
    r = drl_item(ctx, hdr->this_val, i);
    *presult = JS_IsUndefined(r) ? JS_NULL : r;
    return JS_STEP_DONE;
}

static const IdlStepDecl DRL_ITEM_DECL = {
    js_drl_item, sizeof(IdlIndexChain), idl_index_chain_visit, NULL,
    DRL_ITEM_ALGORITHM, DRL_ITEM_STEPS, 0, NULL
};

static JSValue js_drl_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!drl_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "DOMRectList.prototype.length was reached on something that is not a "
                                      "DOMRectList");
    return JS_NewUint32(ctx, drl_length(ctx, this_val));
}

JSValue dom_rect_list_new(JSContext *ctx, JSValue rects)
{
    JSValue proto, obj;

    DCHECK(g_list_class != 0, "a DOMRectList was built before dom_rect_list_init declared the interface");
    DCHECK(JS_IsArray(rects),
           "a DOMRectList was built over something that is not an Array — the list is held as one so that it "
           "forks per flow and parks with the flow that holds it");
    proto = JS_GetClassProto(ctx, g_list_class);
    DCHECK(!JS_IsNull(proto), "a DOMRectList was built in a realm that never ran its prototype install");
    obj = idl_indexed_new(ctx, proto, &DOM_RECT_LIST_INDEXED);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DOMRectList could not be allocated");
    JS_DefinePropertyValue(ctx, obj, g_atom_rects, rects, 0);
    return obj;
}

void dom_rect_list_init(JSContext *ctx)
{
    JSClassDef d = { "DOMRectList" };
    static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };

    /* NOT `if (g_list_class) return;` — one declaration site, so it could never be true, and it hid
       dom_rect_list_free leaving the class id set. See core/agent_state.h. */
    DCHECK(g_list_class == 0, "dom_rect_list_init ran twice — the class is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_list_class);
    JS_NewClass(JS_GetRuntime(ctx), g_list_class, &d);
    g_rects_key = JS_NewSymbol(ctx, "domRectListRects", false);
    CHECK(!JS_IsException(g_rects_key), "the DOMRectList slot key allocation failed");
    g_atom_rects = JS_ValueToAtom(ctx, g_rects_key);
    CHECK(g_atom_rects != JS_ATOM_NULL, "the DOMRectList slot key could not be interned");
    /* §4's `item` IS A MACHINE — a declaration and not a dispatch, since there is no second body for
       anything to select against. Its one `unsigned long index` can be unknown external input. */
    g_id_item = idl_method_id_step(ctx, ONE_ULONG, 1, NULL, 0, &DRL_ITEM_DECL, 0);
    agent_state_class("dom_rect_list", &g_list_class, "the DOMRectList class, and the declaration latch");
    agent_state_value("dom_rect_list", &g_rects_key, "the private Symbol the rectangle Array hangs off");
    agent_state_atom("dom_rect_list", &g_atom_rects, "that Symbol, interned");
    agent_state_id("dom_rect_list", &g_id_item, "the `item` declaration");
    realm_declare_intrinsic(dom_rect_list_install_proto);
}

void dom_rect_list_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_list_class != 0, "a realm asked for DOMRectList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_list_class);
    DCHECK(JS_IsNull(prev), "dom_rect_list_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DOMRectList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DOMRectList");
    idl_install_accessor_no_user_code(ctx, proto, "length", js_drl_length, 0, -1);
    idl_install_method(ctx, proto, "item", g_id_item);
    /* Web IDL §3.7.9 step 1.1: an interface with an indexed property getter and an integer-typed `length` is given
       %Array.prototype.values% as its @@iterator, which is what makes `[...el.getClientRects()]` work. It
       declares no `iterable<>`, so it gets that and NOT `entries`/`keys`/`forEach` — two different clauses. */
    idl_indexed_install_iterable(ctx, proto);
    JS_SetClassProto(ctx, g_list_class, proto);
}

void dom_rect_list_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_list_class);

    DCHECK(!JS_IsNull(proto), "DOMRectList was installed in a realm that never ran its prototype install");
    /* §4 declares no constructor, so the interface object's call and construct both throw. */
    JS_SetPropertyStr(ctx, (JSValue)global, "DOMRectList",
                      idl_interface_object(ctx, "DOMRectList", proto));
    JS_FreeValue(ctx, proto);
}

void dom_rect_list_free(JSRuntime *rt)
{
    /* NOT `if (!g_list_class) return;` — the declare pass of core/platform.c's one list is unconditional, and
       a guard on the very handle this release must clear is the shape that hid it not being cleared. */
    DCHECK(g_list_class != 0, "DOMRectList was released in an agent that never declared it");
    JS_FreeAtomRT(rt, g_atom_rects);
    g_atom_rects = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_rects_key);
    g_rects_key = JS_UNDEFINED;
    g_id_item = -1;
    g_list_class = 0;   /* the latch the init above consults — see core/agent_state.h */
}
