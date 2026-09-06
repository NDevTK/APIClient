/* HTML §2.6.5 — DOMStringList. See dom_string_list.h. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/html/dom_string_list.h"
#include "core/idl_args.h"
#include "core/idl_index_arg.h"
#include "core/idl_indexed.h"
#include "core/realm.h"
#include "solver/concolic.h"

static JSClassID g_list_class;
static JSValue   g_strings_key = JS_UNDEFINED;   /* the private Symbol the string Array hangs off */
static JSAtom    g_atom_strings = JS_ATOM_NULL;
static int       g_id_item = -1, g_id_contains = -1;

/* THE BRAND. The object itself is an indexed-property object, which anything with an indexed getter is, so the
   thing that cannot be forged is the own SLOT this component put on it — the same brand core/geometry/
   dom_rect_list.c uses for the same reason. Returns JS_UNDEFINED for anything that is not a DOMStringList. */
static JSValue dsl_strings(JSContext *ctx, JSValueConst v)
{
    JSValue strings;

    if (!JS_IsObject(v)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &strings, v, g_atom_strings) <= 0)   /* an own SLOT, never a lookup */
        return JS_UNDEFINED;
    return strings;
}

/* "The length getter steps are to return this's associated list's size." */
static uint32_t dsl_length(JSContext *ctx, JSValueConst self)
{
    JSValue strings = dsl_strings(ctx, self), len;
    uint32_t n = 0;

    if (!JS_IsObject(strings)) { JS_FreeValue(ctx, strings); return 0; }
    len = JS_GetPropertyStr(ctx, strings, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_FreeValue(ctx, strings);
    return n;
}

/* THE INDEXED PROPERTY GETTER — "the supported property indices are the indices of this's associated list", so
   JS_UNDEFINED past the end, which is what an index lookup outside them is. §2.6.5's `item` operation below
   turns that into the null its IDL declares, and the two answers differing is the whole reason both exist:
   `list[9]` is undefined where `list.item(9)` is null. */
static JSValue dsl_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue strings = dsl_strings(ctx, self), s;

    if (!JS_IsObject(strings)) { JS_FreeValue(ctx, strings); return JS_UNDEFINED; }
    s = JS_GetPropertyUint32(ctx, strings, i);
    JS_FreeValue(ctx, strings);
    DCHECK(JS_IsUndefined(s) || JS_IsString(s),
           "a DOMStringList held something that is not a string — §2.6.5's list is a list of strings and the "
           "construction is the one place anything is ever put in one");
    return s;
}

static const IdlIndexedDecl DOM_STRING_LIST_INDEXED = { "DOMStringList", dsl_length, dsl_item, NULL, 0 };

/* The BRAND asked by the three PROTOTYPE members — Web IDL §3.7.6 Attributes' for `length`, §3.7.7
   Operations' for `item` and `contains`. The two decl callbacks above are reached only
   through an index lookup on an object idl_indexed already resolved, so they answer the empty list for a
   stranger; a member read off `DOMStringList.prototype` directly must THROW, because a page tells that apart
   from `undefined`. */
static bool dsl_is(JSContext *ctx, JSValueConst v)
{
    JSValue strings = dsl_strings(ctx, v);
    bool ok = JS_IsObject(strings);

    JS_FreeValue(ctx, strings);
    return ok;
}

/* "The item(index) method steps are to return the indexth item in this's associated list, or null if index plus
 * one is greater than this's associated list's size."
 *
 * IT IS A STEP MACHINE BECAUSE ITS ONE ARGUMENT CAN BE UNKNOWN. A `JS_ToUint32` of `argv[0]` with its return
 * discarded stood here — the shape core/idl_args.h bans by name — under a DCHECK that refused an unknown index
 * into a NON-EMPTY list and named the fork to build. That fork is core/idl_index_arg.h's elimination chain. */
#define DSL_ITEM_ALGORITHM "HTML §2.6.5 The DOMStringList interface item(index)"
#define DSL_ITEM_STAGES(X)                                                                                    \
    X(DSL_ITEM_READ, DSL_ITEM_ALGORITHM " (return the indexth item in this's associated list, or null)")
enum { IDL_STEP_STAGE_BASE(DSL_ITEM_STAGES) DSL_ITEM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DSL_ITEM_STEPS[] = { DSL_ITEM_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_dsl_item(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdlIndexChain *st = state;
    uint32_t i = 0;
    bool past_end = false;
    JSValue s;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == DSL_ITEM_READ,
           "§2.6.5's `item` resumed into a stage the algorithm does not have — it is ONE sentence, and the "
           "chain of questions it may ask is a cursor on this machine's own state rather than a stage apiece");
    DCHECK(argc == 1,
           "§2.6.5's `item` reached its body with an argument count its declaration does not produce — its one "
           "`unsigned long index` is required, so §3.6's argument-count check refuses a shorter call first");
    if (!dsl_is(ctx, hdr->this_val)) {
        JS_ThrowTypeError(ctx, "DOMStringList.prototype.item was reached on something that is not a "
                               "DOMStringList");
        return JS_STEP_ABRUPT;
    }
    if (concolic_is(argv[0])) {
        int rc = idl_index_chain_run(ctx, hdr, st, argv[0], dsl_length(ctx, hdr->this_val),
                                     DSL_ITEM_ALGORITHM, &i, &past_end);
        if (rc)
            return rc;   /* parked at the fork */
        if (past_end) {
            *presult = JS_NULL;   /* §2.6.5's own past-the-end answer */
            return JS_STEP_DONE;
        }
    } else {
        i = idl_index_arg_known(ctx, argv[0], DSL_ITEM_ALGORITHM);
    }
    s = dsl_item(ctx, hdr->this_val, i);
    *presult = JS_IsUndefined(s) ? JS_NULL : s;
    return JS_STEP_DONE;
}

static const IdlStepDecl DSL_ITEM_DECL = {
    js_dsl_item, sizeof(IdlIndexChain), idl_index_chain_visit, NULL,
    DSL_ITEM_ALGORITHM, DSL_ITEM_STEPS, 0, NULL
};

/* "The contains(string) method steps are to return true if this's associated list CONTAINS string, and false
   otherwise" — Infra's list containment, which for strings is code-unit equality. The engine holds a string as
   UTF-8 and that encoding is injective, so equal byte sequences of equal length are equal strings and nothing
   here has to decode. */
static JSValue js_dsl_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *want, *have;
    size_t wlen = 0, hlen = 0;
    uint32_t i, n;
    bool found = false;

    (void)magic;
    if (!dsl_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "DOMStringList.prototype.contains was reached on something that is not a "
                                      "DOMStringList");
    DCHECK(argc >= 1, "§2.6.5's `contains` reached its body with no argument — its IDL argument is required, so "
                      "the declaration's own argument-count check is what should have refused the call");
    n = dsl_length(ctx, this_val);
    if (concolic_is(argv[0])) {
        /* AN UNKNOWN STRING. The empty list is the one size at which that has an answer rather than a fork: no
           string is in a list of no strings, over the whole domain. */
        DCHECK(n == 0,
               "§2.6.5's `contains` was given an UNKNOWN string against a NON-EMPTY DOMStringList — each string "
               "in the list is an equality that would PIN the source (the true arm), and their negation is the "
               "false arm, so the call must FORK one flow per entry plus the not-in-the-list arm instead of "
               "deciding it here");
        return JS_FALSE;
    }
    want = JS_ToCStringLen(ctx, &wlen, argv[0]);
    if (!want) return JS_EXCEPTION;
    for (i = 0; i < n && !found; i++) {
        JSValue s = dsl_item(ctx, this_val, i);

        have = JS_ToCStringLen(ctx, &hlen, s);
        CHECK(have != NULL, "a DOMStringList entry's bytes could not be read");
        found = hlen == wlen && memcmp(have, want, wlen) == 0;
        JS_FreeCString(ctx, have);
        JS_FreeValue(ctx, s);
    }
    JS_FreeCString(ctx, want);
    return JS_NewBool(ctx, found);
}

static JSValue js_dsl_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!dsl_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "DOMStringList.prototype.length was reached on something that is not a "
                                      "DOMStringList");
    return JS_NewUint32(ctx, dsl_length(ctx, this_val));
}

JSValue dom_string_list_new(JSContext *ctx, JSValue strings)
{
    JSValue proto, obj;

    DCHECK(g_list_class != 0, "a DOMStringList was built before dom_string_list_init declared the interface");
    DCHECK(JS_IsArray(strings),
           "a DOMStringList was built over something that is not an Array — the list is held as one so that it "
           "forks per flow and parks with the flow that holds it");
    proto = JS_GetClassProto(ctx, g_list_class);
    DCHECK(!JS_IsNull(proto), "a DOMStringList was built in a realm that never ran its prototype install");
    obj = idl_indexed_new(ctx, proto, &DOM_STRING_LIST_INDEXED);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DOMStringList could not be allocated");
    JS_DefinePropertyValue(ctx, obj, g_atom_strings, strings, 0);   /* CONSUMES strings */
    return obj;
}

void dom_string_list_init(JSContext *ctx)
{
    JSClassDef d = { "DOMStringList" };
    static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };

    /* NOT `if (g_list_class) return;` — one declaration site, so it could never be true, and it would hide
       dom_string_list_free leaving the class id set. See core/agent_state.h. */
    DCHECK(g_list_class == 0, "dom_string_list_init ran twice — the class is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_list_class);
    JS_NewClass(JS_GetRuntime(ctx), g_list_class, &d);
    g_strings_key = JS_NewSymbol(ctx, "domStringListStrings", false);
    CHECK(!JS_IsException(g_strings_key), "the DOMStringList slot key allocation failed");
    g_atom_strings = JS_ValueToAtom(ctx, g_strings_key);
    CHECK(g_atom_strings != JS_ATOM_NULL, "the DOMStringList slot key could not be interned");
    /* §2.6.5's `item` IS A MACHINE — a declaration and not a dispatch, since there is no second body for
       anything to select against. Its one `unsigned long index` can be unknown external input. */
    g_id_item = idl_method_id_step(ctx, ONE_ULONG, 1, NULL, 0, &DSL_ITEM_DECL, 0);
    g_id_contains = idl_method_id(ctx, ONE_STR, 1, js_dsl_contains, 0);
    agent_state_class("dom_string_list", &g_list_class, "the DOMStringList class, and the declaration latch");
    agent_state_value("dom_string_list", &g_strings_key, "the private Symbol the string Array hangs off");
    agent_state_atom("dom_string_list", &g_atom_strings, "that Symbol, interned");
    agent_state_id("dom_string_list", &g_id_item, "the `item` declaration");
    agent_state_id("dom_string_list", &g_id_contains, "the `contains` declaration");
    realm_declare_intrinsic(dom_string_list_install_realm);
}

/* HTML §2.6.5 The DOMStringList interface, FOR ONE REALM — its Web IDL §3.7.3 interface prototype object, its
   §3.7.1 interface object, and Web IDL §3.8's property reference for its name.

   THE INTERFACE OBJECT IS HERE BECAUSE WEB IDL §3.8 IS GIVEN A REALM. `define the global property references`
   is "To define the global property references on target, given realm realm", and its step 1 is "Let
   interfaces be a list that contains every interface that is exposed in realm" — the population is a REALM's
   and the algorithm names no Document. §2.6.5 declares `[Exposed=(Window,Worker)]`, so a realm whose global
   object implements a worker scope owes the name; while it was placed from core/platform.c's per-DOCUMENT
   column, which such a realm never reaches, it got nothing, and nor did a Window realm until a Document was
   installed over it. The prototype is in hand here, so the separate per-document entry's JS_GetClassProto
   re-read is gone: re-reading it would be a second answer to a question this function has just settled. */
void dom_string_list_install_realm(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_list_class != 0, "a realm asked for DOMStringList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_list_class);
    DCHECK(JS_IsNull(prev), "dom_string_list_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DOMStringList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DOMStringList");
    idl_install_accessor_no_user_code(ctx, proto, "length", js_dsl_length, 0, -1);
    idl_install_method(ctx, proto, "item", g_id_item);
    idl_install_method(ctx, proto, "contains", g_id_contains);
    /* Web IDL §3.7.9 step 1.1: an interface with an indexed property getter and an integer-typed `length` is given
       %Array.prototype.values% as its @@iterator, which is what makes `[...db.objectStoreNames]` work. §2.6.5
       declares no `iterable<>`, so it gets that and NOT `entries`/`keys`/`forEach` — two different clauses. */
    idl_indexed_install_iterable(ctx, proto);

    /* WEB IDL §3.7.1's INTERFACE OBJECT AND §3.8's STEP 3.1.3 FOR ITS NAME. §2.6.5 declares no constructor, so
       the interface object's call and construct both throw; it declares no [LegacyWindowAlias] either, so
       §3.8 step 3.1.4 has nothing to do for this interface, and no [LegacyFactoryFunction], so neither does
       step 3.2. */
    {
        JSValue global = JS_GetGlobalObject(ctx);

        idl_define_global_property_reference(ctx, global, "DOMStringList",
                                             idl_interface_object(ctx, "DOMStringList", proto));
        JS_FreeValue(ctx, global);
    }

    JS_SetClassProto(ctx, g_list_class, proto);   /* the realm owns it from here */
}

void dom_string_list_free(JSRuntime *rt)
{
    /* NOT `if (!g_list_class) return;` — the declare pass of core/platform.c's one list is unconditional, and a
       guard on the very handle this release must clear is the shape that hid it not being cleared. */
    DCHECK(g_list_class != 0, "DOMStringList was released in an agent that never declared it");
    JS_FreeAtomRT(rt, g_atom_strings);
    g_atom_strings = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_strings_key);
    g_strings_key = JS_UNDEFINED;
    g_id_item = -1;
    g_id_contains = -1;
    g_list_class = 0;   /* the latch the init above consults — see core/agent_state.h */
}
