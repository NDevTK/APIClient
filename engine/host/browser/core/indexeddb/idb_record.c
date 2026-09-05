/* INDEXED DATABASE §2.12's RECORD SNAPSHOT, and §4.8's IDBRecord over it.
 *
 *     [Exposed=(Window,Worker)]
 *     interface IDBRecord {
 *       readonly attribute any key;
 *       readonly attribute any primaryKey;
 *       readonly attribute any value;
 *     };
 *
 * WHY IT IS A CONSTRUCT OF ITS OWN AND NOT A PLAIN OBJECT WITH THREE PROPERTIES. §2.12 gives the snapshot
 * three fields — a key, a value and a primary key — and §4.8 exposes them as IDL attributes on an interface,
 * which is a page-visible difference in three ways at once: `Object.prototype.toString.call(record)` is
 * "[object IDBRecord]", the three names live on IDBRecord.prototype rather than on the record (so
 * `Object.keys(record)` is empty and `delete record.key` does nothing), and `window.IDBRecord` exists for a
 * bundle to feature-detect. A record built as `{key, primaryKey, value}` answers all three the other way.
 *
 * WHAT THE THREE FIELDS ARE, AND WHY TWO OF THEM ARE KEY RECORDS. §2.12: "a record snapshot has a KEY which is
 * a key", "a VALUE which is a value", "a PRIMARY KEY which is a key". §4.8's two key getters each state "the
 * result of CONVERTING A KEY TO A VALUE" — §7.3 — so what the snapshot holds is §2.4's key record and the
 * conversion belongs to the read. The third is a §2.3 value and is handed back as itself.
 *
 * THE TWO KEYS ARE NOT THE SAME FIELD, AND THE NOTE THAT SAYS SO IS THE WHOLE POINT OF THE INTERFACE. §2.12:
 * "for an INDEX record, the snapshot's primary key is the record's VALUE, which is the key of the record in
 * the index's referenced object store", and "for an OBJECT STORE record, the snapshot's primary key and key
 * are the same key". So a record retrieved from an index carries BOTH the index key it was filed under and
 * the store key it points at — the pair a page otherwise has to run `openCursor` to see. Which key goes in
 * which field is §6.2's and §6.3's decision, not this file's, which is why the constructor takes all three.
 *
 * THE RECORD IS THE COMPONENT'S OWN AND IT TIME-TRAVELS. Three owned JSValues live behind the class opaque,
 * where no property hook can see a write, so the capture goes in the ACCESSOR every member reaches — a record
 * a flow has reached is one it may write, and there is then no write site left to miss. §4.8 declares every
 * member of this interface readonly and this engine mints a snapshot once and never edits it, so today there
 * is no write; the capture is what makes that a fact nothing has to re-check the day a member is added. The
 * offset list is the SAME list the finalizer frees and the gc_mark walks, which is what makes a field added to
 * one and not the others impossible to miss. Both of those go through JS_GetOpaque instead, deliberately: a
 * capture during collection would dup values on an object being torn down.
 *
 * THERE IS NO CONSTRUCTOR. §4.8 declares none, so `new IDBRecord()` is a TypeError — which is also what tells
 * a feature-detecting bundle the interface exists. A snapshot is minted by §6.2's and §6.3's retrieve-multiple
 * arms and by nothing else. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "core/agent_state.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_record.h"
#include "core/realm.h"
#include "solver/cow.h"

/* §2.12's three fields. The two keys are §2.4 key records; the value is a §2.3 value. */
typedef struct IdbRecordData {
    JSValue key;          /* §2.12's key — a key record, OWNED */
    JSValue primary_key;  /* §2.12's primary key — a key record, OWNED */
    JSValue value;        /* §2.12's value — a deserialized ECMAScript value, OWNED */
} IdbRecordData;

static const uint16_t RECORD_VALS[] = { (uint16_t)offsetof(IdbRecordData, key),
                                        (uint16_t)offsetof(IdbRecordData, primary_key),
                                        (uint16_t)offsetof(IdbRecordData, value) };
static const CowRecord RECORD_REC = { sizeof(IdbRecordData), RECORD_VALS, 3 };

static JSClassID g_record_class;

/* WHICH OF §4.8's TWO KEY GETTERS a read is — one body, because the two state the same sentence over two
   fields and a second copy could drift from its twin. */
enum { IDB_RECORD_SIDE_KEY = 0, IDB_RECORD_SIDE_PRIMARY };

static IdbRecordData *record_of(JSValueConst v)
{
    IdbRecordData *r = JS_GetOpaque(v, g_record_class);

    if (r) cow_capture_host_record(v, r, &RECORD_REC);
    return r;
}

/* WEB IDL §3.7.6 Attributes' BRAND CHECK. `IDBRecord.prototype.key` read off anything else is a TypeError,
   and a page
   tells that apart from `undefined`. */
static IdbRecordData *record_here(JSContext *ctx, JSValueConst v)
{
    IdbRecordData *r = record_of(v);

    if (!r) {
        JS_ThrowTypeError(ctx, "an IDBRecord member was reached on something that is not an IDBRecord");
        return NULL;
    }
    return r;
}

bool idb_record_is(JSValueConst v)
{
    return g_record_class != 0 && JS_GetClassID(v) == g_record_class;
}

/* THE COLLECTOR'S TWO ENTRIES READ NO STATIC THIS COMPONENT'S RELEASE RESETS — core/agent_state.h's rule. Both
   run AFTER core/platform.c's release column, so a snapshot a page still holds would be finalized with
   `g_record_class` already back at 0 and `JS_GetOpaque(val, 0)` would answer NULL: three counted references
   leak, and an unmarked child keeps the internal reference gc_decref exists to subtract, so gc_scan reads the
   stored VALUE — an arbitrary structured clone of the page's own object graph — as rooted from outside the
   heap. JS_GetAnyOpaque, because the collector dispatched here THROUGH the class. This is the mechanism the
   head comment above already gives the reason for: not the accessor, because a capture during collection would
   dup values on an object being torn down — and not the id either, because the id is gone by then. */
static void record_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    IdbRecordData *r = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!r) return;`. idb_record_new is the one mint — §4.8 declares no constructor — and it sets the
       record with nothing in between that allocates in the JS heap. */
    DCHECK(r != NULL, "an IDBRecord was finalized with no snapshot — §4.8 declares no constructor, so the one "
                      "mint is idb_record_new and it sets the record with nothing in between that could "
                      "collect");
    JS_FreeValueRT(rt, r->key);
    JS_FreeValueRT(rt, r->primary_key);
    JS_FreeValueRT(rt, r->value);
    free(r);
}

static void record_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    IdbRecordData *r = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(r != NULL, "an IDBRecord was marked with no snapshot — its three fields are counted references and "
                      "an unmarked child is read by gc_scan as rooted from outside the heap");
    JS_MarkValue(rt, r->key, mark_func);
    JS_MarkValue(rt, r->primary_key, mark_func);
    JS_MarkValue(rt, r->value, mark_func);
}

JSValue idb_record_new(JSContext *ctx, JSValue key, JSValue primary_key, JSValue value)
{
    JSValue proto, obj;
    IdbRecordData *r;

    DCHECK(g_record_class != 0, "a record snapshot was built before idb_record_init declared the interface");
    /* §2.12: the snapshot's key and primary key are each a KEY. §7.3 is what the getters run over them, and it
       is stated over a key record — a page value that had already been converted would be converted a second
       time and answer with a laundered copy of itself rather than with the key. */
    DCHECK(JS_IsObject(key) && JS_IsObject(primary_key),
           "a record snapshot was built over something that is not a §2.4 key record — §4.8's two key getters "
           "each run §7.3 over what the snapshot holds, so the CONVERSION belongs to the read and the field "
           "holds the key");
    proto = JS_GetClassProto(ctx, g_record_class);
    DCHECK(!JS_IsNull(proto), "a record snapshot was built in a realm with no IDBRecord.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_record_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "IndexedDB: an IDBRecord could not be allocated");
    r = calloc(1, sizeof *r);
    CHECK(r != NULL, "IndexedDB: a record snapshot's allocation failed");
    r->key = key;
    r->primary_key = primary_key;
    r->value = value;
    JS_SetOpaque(obj, r);
    return obj;
}

/* "The KEY getter steps are to return the result of converting a key to a value with this's key."
   "The PRIMARY KEY getter steps are to return the result of converting a key to a value with this's primary
   key." One body over §2.12's two key fields, chosen by the magic the install states. */
static JSValue js_record_key_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    IdbRecordData *r = record_here(ctx, this_val);

    if (!r) return JS_EXCEPTION;
    DCHECK(magic == IDB_RECORD_SIDE_KEY || magic == IDB_RECORD_SIDE_PRIMARY,
           "an IDBRecord key getter was installed with a magic naming neither of §2.12's two key fields");
    return idb_key_to_value(ctx, magic == IDB_RECORD_SIDE_KEY ? r->key : r->primary_key);
}

/* "The VALUE getter steps are to return this's value." No conversion: §6.2's and §6.3's "record" arm ran
   StructuredDeserialize before the snapshot was made, so this hands back the same object every read. */
static JSValue js_record_value_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    IdbRecordData *r = record_here(ctx, this_val);

    (void)magic;
    if (!r) return JS_EXCEPTION;
    return JS_DupValue(ctx, r->value);
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

static void idb_record_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_record_class != 0, "a realm asked for IDBRecord before the interface was declared");
    prev = JS_GetClassProto(ctx, g_record_class);
    DCHECK(JS_IsNull(prev), "idb_record_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBRecord.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBRecord");
    /* IN §4.8's OWN ORDER. */
    idl_install_accessor(ctx, proto, "key", js_record_key_get, IDB_RECORD_SIDE_KEY, -1);
    idl_install_accessor(ctx, proto, "primaryKey", js_record_key_get, IDB_RECORD_SIDE_PRIMARY, -1);
    idl_install_accessor(ctx, proto, "value", js_record_value_get, 0, -1);
    JS_SetClassProto(ctx, g_record_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT — §4.8 declares no constructor, so `new IDBRecord()` is a TypeError. */
    ctor = idl_interface_object(ctx, "IDBRecord", proto);
    CHECK(!JS_IsException(ctor), "the IDBRecord interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "IDBRecord", ctor);
    JS_FreeValue(ctx, global);
}

void idb_record_init(JSContext *ctx)
{
    JSClassDef d = { "IDBRecord", record_finalizer, record_gc_mark };

    DCHECK(g_record_class == 0, "idb_record_init ran twice — the interface is declared once per AGENT, and a "
                                "second class id would leave every snapshot already built branded with the "
                                "first");
    JS_NewClassID(JS_GetRuntime(ctx), &g_record_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_record_class, &d) == 0,
          "IDBRecord: the class could not be declared");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. One slot, and it survived every
       agent this engine has torn down: this row was on core/platform.c's list with an EMPTY release column and
       no release function existed anywhere. Neither of JS_FreeRuntime's censuses can report a class id, and
       the only reader of a stale one is the next agent's `idb_record_init`, which consults it precisely to
       decide it need not run. */
    agent_state_class("idb_record", &g_record_class,
                      "Indexed Database §4.8's IDBRecord class, and this component's declaration latch");
    realm_declare_intrinsic(idb_record_install_realm);
}

/* THE INVERSE OF THE DECLARATION ABOVE, WHICH DID NOT EXIST. The prototype is the REALMS' and goes with their
   contexts; what is the AGENT's is the class this runtime registered. It goes back to 0 because a class is
   registered in a RUNTIME — core/agent_state.h's one policy — and it is also this component's latch, so a
   carried id would make the next agent's `idb_record_init` return before re-registering it and every snapshot
   that agent mints would be branded with a number the live runtime never gave out. */
void idb_record_free(void)
{
    DCHECK(g_record_class != 0, "§4.8's IDBRecord was released in an agent that never declared it");
    g_record_class = 0;
}
