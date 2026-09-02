/* DOM §4.3.3 "Interface MutationRecord".
 *
 * ONE PROBLEM: the record is a VALUE. §4.3.2 step 3.1 constructs it with nine fields in one step and nothing
 * ever writes to it again — the IDL declares nine readonly attributes and no operations — so this file holds a
 * constructor, nine getters and nothing else. The algorithm that decides WHICH records exist is §4.3.2's, in
 * mutation_observer.c; keeping them apart is what makes this one assertable with a single fixture.
 *
 * THE FIELDS ARE A JS ARRAY IN AN OWN SLOT, not a malloc'd C struct behind JS_SetOpaque. A record lives in an
 * observer's record queue, and that queue must PARK to the cold tier with the flow that holds it and FORK per
 * flow — CLAUDE.md's rule for platform data a flow queues, and the same reason a custom element's reaction
 * queue is an Array. A C struct captured as a POINTER reverts the pointer on a context switch and leaves the
 * record reachable from nothing.
 *
 * ONE SLOT AND NOT NINE, because the nine are written together and read one at a time: the getter's magic IS
 * the field's index, so the IDL's declaration order and the array's are the same list and a field added to one
 * without the other is a compile error at the enum rather than a wrong answer at a getter.
 *
 * `addedNodes` AND `removedNodes` ARE [SameObject]. The static NodeList is built at construction and held, so
 * `r.addedNodes === r.addedNodes` — building one per read is the shape that answers that false, and a page
 * that stashes the list from a first record and compares it later is how that surfaces. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/collections.h"
#include "core/dom/mutation_record.h"

/* THE FIELD ORDER IS THE IDL'S ORDER, and the getter's magic is the index into it. */
enum {
    MR_F_TYPE = 0, MR_F_TARGET, MR_F_ADDED, MR_F_REMOVED, MR_F_PREV, MR_F_NEXT,
    MR_F_ATTR_NAME, MR_F_ATTR_NS, MR_F_OLD_VALUE,
    MR_F_COUNT
};

/* §4.3.3's `type` values, indexed by the MR_TYPE_* the caller states. */
static const char *const MR_TYPE_NAMES[3] = { "attributes", "characterData", "childList" };

static JSClassID g_record_class;
static JSValue   g_fields_key = JS_UNDEFINED;   /* the private symbol the field array hangs off */
static JSAtom    g_atom_fields = JS_ATOM_NULL;

/* THE BRAND. §3.7.6 Attributes: an interface's attribute getter throws a TypeError when `this` does not
   implement it, and
   a page distinguishes that from `undefined` — `MutationRecord.prototype.type` read off a plain object must
   throw. The class id is the brand because this class carries no opaque: the record's whole state is the own
   slot, which anything could be given, so the class is the only thing that cannot be forged. */
static JSValue mr_fields(JSContext *ctx, JSValueConst this_val)
{
    JSValue f;

    if (JS_GetClassID(this_val) != g_record_class)
        return JS_ThrowTypeError(ctx, "not a MutationRecord");
    if (JS_GetOwnSlot(ctx, &f, this_val, g_atom_fields) <= 0)
        f = JS_UNDEFINED;
    DCHECK(JS_IsObject(f), "a MutationRecord carries no field list — §4.3.2 step 3.1 builds every field before "
                           "the record exists, so a record without one was made somewhere else");
    return f;
}

static JSValue js_mr_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue f = mr_fields(ctx, this_val), v;

    if (JS_IsException(f)) return f;
    DCHECK(magic >= 0 && magic < MR_F_COUNT, "a MutationRecord attribute ran with a magic §4.3.3 does not "
                                             "declare");
    v = JS_GetPropertyUint32(ctx, f, (uint32_t)magic);
    JS_FreeValue(ctx, f);
    return v;
}

JSValue mutation_record_new(JSContext *ctx, int type, lxb_dom_node_t *target,
                            JSValue added, JSValue removed,
                            lxb_dom_node_t *prev, lxb_dom_node_t *next,
                            const char *name, const char *ns, const char *old, size_t old_len)
{
    JSValue rec, f, proto;

    DCHECK(g_record_class != 0, "a mutation record was made before mutation_record_init declared the class");
    DCHECK(type >= 0 && type <= MR_TYPE_CHILD_LIST, "a mutation record was made with a type §4.3.2 does not "
                                                    "queue");
    DCHECK(target != NULL, "a mutation record was made with no target — §4.3.2 is entered with one");
    proto = JS_GetClassProto(ctx, g_record_class);
    DCHECK(!JS_IsNull(proto), "a mutation record was made in a realm that never ran its prototype install");
    rec = JS_NewObjectProtoClass(ctx, proto, g_record_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(rec), "a MutationRecord could not be allocated");

    f = JS_NewArray(ctx);
    CHECK(!JS_IsException(f), "a MutationRecord's field list could not be allocated");
    JS_SetPropertyUint32(ctx, f, MR_F_TYPE, JS_NewString(ctx, MR_TYPE_NAMES[type]));
    JS_SetPropertyUint32(ctx, f, MR_F_TARGET, node_wrap(ctx, target));
    /* The two [SameObject] NodeLists, made once. `collections_static` takes the array. */
    JS_SetPropertyUint32(ctx, f, MR_F_ADDED, collections_static(ctx, added));
    JS_SetPropertyUint32(ctx, f, MR_F_REMOVED, collections_static(ctx, removed));
    JS_SetPropertyUint32(ctx, f, MR_F_PREV, prev ? node_wrap(ctx, prev) : JS_NULL);
    JS_SetPropertyUint32(ctx, f, MR_F_NEXT, next ? node_wrap(ctx, next) : JS_NULL);
    JS_SetPropertyUint32(ctx, f, MR_F_ATTR_NAME, name ? JS_NewString(ctx, name) : JS_NULL);
    JS_SetPropertyUint32(ctx, f, MR_F_ATTR_NS, ns ? JS_NewString(ctx, ns) : JS_NULL);
    /* `oldValue` IS A DOMString? AND AN ATTRIBUTE'S VALUE MAY CONTAIN A NUL — the length travels with it, so
       the record carries what the attribute held rather than what strlen could see. */
    JS_SetPropertyUint32(ctx, f, MR_F_OLD_VALUE,
                         old ? JS_NewStringLen(ctx, old, old_len) : JS_NULL);
    JS_DefinePropertyValue(ctx, rec, g_atom_fields, f, 0);
    return rec;
}

void mutation_record_init(JSContext *ctx)
{
    JSClassDef d = { "MutationRecord" };

    if (g_record_class) return;   /* one AGENT, one class */
    JS_NewClassID(JS_GetRuntime(ctx), &g_record_class);
    JS_NewClass(JS_GetRuntime(ctx), g_record_class, &d);
    g_fields_key = JS_NewSymbol(ctx, "mutationRecordFields", false);
    CHECK(!JS_IsException(g_fields_key), "the MutationRecord field-list slot key allocation failed");
    g_atom_fields = JS_ValueToAtom(ctx, g_fields_key);
    CHECK(g_atom_fields != JS_ATOM_NULL, "the MutationRecord field-list slot key could not be interned");
    realm_declare_intrinsic(mutation_record_install_proto);
}

void mutation_record_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_record_class != 0, "a realm asked for MutationRecord.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_record_class);
    DCHECK(JS_IsNull(prev), "mutation_record_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "MutationRecord.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "MutationRecord");
    idl_install_accessor(ctx, proto, "type", js_mr_get, MR_F_TYPE, -1);
    idl_install_accessor(ctx, proto, "target", js_mr_get, MR_F_TARGET, -1);
    idl_install_accessor(ctx, proto, "addedNodes", js_mr_get, MR_F_ADDED, -1);
    idl_install_accessor(ctx, proto, "removedNodes", js_mr_get, MR_F_REMOVED, -1);
    idl_install_accessor(ctx, proto, "previousSibling", js_mr_get, MR_F_PREV, -1);
    idl_install_accessor(ctx, proto, "nextSibling", js_mr_get, MR_F_NEXT, -1);
    idl_install_accessor(ctx, proto, "attributeName", js_mr_get, MR_F_ATTR_NAME, -1);
    idl_install_accessor(ctx, proto, "attributeNamespace", js_mr_get, MR_F_ATTR_NS, -1);
    idl_install_accessor(ctx, proto, "oldValue", js_mr_get, MR_F_OLD_VALUE, -1);
    JS_SetClassProto(ctx, g_record_class, proto);
}

void mutation_record_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_record_class);

    DCHECK(!JS_IsNull(proto), "MutationRecord was installed in a realm that never ran its prototype install");
    /* §4.3.3 declares NO constructor, so the interface object's call and construct both throw. */
    JS_SetPropertyStr(ctx, (JSValue)global, "MutationRecord",
                      idl_interface_object(ctx, "MutationRecord", proto));
    JS_FreeValue(ctx, proto);
}

void mutation_record_free(JSRuntime *rt)
{
    if (!g_record_class) return;
    JS_FreeAtomRT(rt, g_atom_fields);
    g_atom_fields = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_fields_key);
    g_fields_key = JS_UNDEFINED;
}
