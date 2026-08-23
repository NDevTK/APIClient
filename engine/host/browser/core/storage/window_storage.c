/* HTML §12.2.2 "The sessionStorage getter" and §12.2.3 "The localStorage getter". The IDL and why this is its
 * own component are in window_storage.h.
 *
 * THE HOLDER IS THE WHOLE OF THE ALGORITHM'S STATE, and it is per DOCUMENT rather than per realm: "A Document
 * object has an associated local storage holder, which is null or a Storage object. It is initially null."
 * `localStorage === localStorage` is an identity every bundle rests on — a fresh object per read would make
 * `if (a !== b)` shims true forever — and it is also what stops one document holding two proxy maps over one
 * bottle, which §12.2.1's broadcast counts.
 *
 * IT IS AN INTERNAL SLOT ON THE `document` OBJECT (core/idl_slots.h), so it is an ordinary property write the
 * COW delta captures. That makes the holder PER FLOW, which is the correct reading of both halves at once: the
 * Storage object a flow minted is the flow's, the BOTTLE MAP under it is the shared baseline every flow layers
 * its own writes over (core/storage/storage_shed.c), and a flow parked to the cold tier resumes holding the
 * same object it had.
 *
 * BOTH GETTERS ANSWER FROM THE REALM, which is this engine's standing convention for a Window member
 * (core/frame/viewport.c's `js_vp_get` states it the same way): §3.7 gives every realm its own copy of the
 * accessor, js_call_c_function takes `ctx` from the function object, and a Window is one realm's — so `ctx` IS
 * "this's associated Document"'s realm for every spelling that reaches the member through an object rather
 * than through a stolen getter.
 *
 * §4.2's FAILURE IS A SecurityError AND NOTHING ELSE. Storage §4.2 step 2 returns failure for an OPAQUE
 * ORIGIN, and both sections say what to do with it: "throw a 'SecurityError' DOMException if the Document's
 * origin is an opaque origin or if the request violates a policy decision". A sandboxed `<iframe>` without
 * `allow-same-origin` therefore THROWS here, which is exactly what real bundles feature-detect with a
 * try/catch — and answering `undefined` instead would take the wrong arm of that catch. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/storage/storage.h"
#include "core/storage/storage_shed.h"
#include "core/storage/window_storage.h"

static JSValue g_holder_key = JS_UNDEFINED;   /* the Symbol the Document's two holders hang off */
static int     g_ready;

enum { WS_SESSION = 0, WS_LOCAL };
static const char *const WS_HOLDER[] = { "sessionStorageHolder", "localStorageHolder" };
static const char *const WS_IDENTIFIER[] = { "sessionStorage", "localStorage" };

/* The Document's slot record, created on first use. OWNED. */
static JSValue ws_holders(JSContext *ctx)
{
    JSValueConst doc = document_object(ctx);
    JSValue rec;
    JSAtom k;

    DCHECK(JS_IsObject(doc), "a storage getter ran in a realm with no `document` — §12.2.2 and §12.2.3 both "
                             "hold their Storage on this's associated Document, so there is nowhere to put it");
    k = JS_ValueToAtom(ctx, g_holder_key);
    CHECK(k != JS_ATOM_NULL, "window storage: the holder slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &rec, doc, k) <= 0) {
        rec = idl_slots_new(ctx);
        CHECK(!JS_IsException(rec), "a Document's storage holder record could not be allocated");
        JS_SetProperty(ctx, doc, k, JS_DupValue(ctx, rec));
    }
    JS_FreeAtom(ctx, k);
    return rec;
}

/* §12.2.2's and §12.2.3's steps, which differ only in their storage TYPE, their storage IDENTIFIER and which
   holder they read — so they are one body with the member as its magic, rather than two copies of six steps. */
static JSValue js_ws_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    StorageType type = (magic == WS_LOCAL) ? STORAGE_TYPE_LOCAL : STORAGE_TYPE_SESSION;
    JSValue holders, held, map, storage;

    (void)this_val;
    DCHECK(g_ready, "a storage getter ran before window_storage_init declared it");
    DCHECK(magic == WS_LOCAL || magic == WS_SESSION,
           "a storage getter was installed with a magic naming neither §12.2.2's member nor §12.2.3's");

    holders = ws_holders(ctx);
    held = JS_GetPropertyStr(ctx, holders, WS_HOLDER[magic]);
    if (JS_IsObject(held)) {                                             /* step 1 */
        DCHECK(storage_is(held), "a Document's storage holder is not a Storage object — nothing but this "
                                 "component writes that slot, and §12.2.2 types it `null or a Storage object`");
        JS_FreeValue(ctx, holders);
        return held;
    }
    JS_FreeValue(ctx, held);

    map = storage_shed_obtain_bottle_map(ctx, type, WS_IDENTIFIER[magic]);   /* step 2 */
    if (JS_IsUndefined(map)) {                                               /* step 3 */
        JS_FreeValue(ctx, holders);
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "this document's origin has no storage key, so it has no %s",
                                    WS_IDENTIFIER[magic]);
    }
    storage = storage_new(ctx, map, type);                                   /* step 4 */
    if (JS_IsException(storage)) { JS_FreeValue(ctx, holders); return storage; }
    JS_SetPropertyStr(ctx, holders, WS_HOLDER[magic], JS_DupValue(ctx, storage));   /* step 5 */
    JS_FreeValue(ctx, holders);
    return storage;                                                          /* step 6 */
}

/* The two members go on the GLOBAL rather than on a prototype, which Web IDL states twice. §3.7.6
   "Attributes": "Regular attributes are exposed on the interface prototype object, unless the attribute is
   unforgeable or if the interface was declared with the [Global] extended attribute, in which case they are
   exposed on every object that implements the interface." §3.3.8 "[Global]" says the same from the other end:
   "Interface members from the interface will correspond to properties on the object itself rather than on
   interface prototype objects." It is what `Object.getOwnPropertyDescriptor(window, 'localStorage')` reports
   in a browser, and what core/frame/screen.c does for CSSOM VIEW §4's `screen`. */
static void window_storage_install_realm(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    DCHECK(g_ready, "a realm asked for the storage getters before window_storage_init declared them");
    idl_install_accessor(ctx, global, "sessionStorage", js_ws_get, WS_SESSION, -1);
    idl_install_accessor(ctx, global, "localStorage", js_ws_get, WS_LOCAL, -1);
    JS_FreeValue(ctx, global);
}

void window_storage_init(JSContext *ctx)
{
    DCHECK(!g_ready, "window_storage_init ran twice — the holder key is declared once per AGENT");
    g_holder_key = JS_NewSymbol(ctx, "documentStorageHolders", false);
    CHECK(!JS_IsException(g_holder_key), "the Document storage holder key could not be allocated");
    g_ready = 1;
    agent_state_value("window_storage", &g_holder_key,
                      "§12.2.2's and §12.2.3's per-Document storage holder Symbol");
    agent_state_flag("window_storage", &g_ready, "the declaration latch");
    realm_declare_intrinsic(window_storage_install_realm);
}

void window_storage_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_holder_key);
    g_holder_key = JS_UNDEFINED;
    g_ready = 0;
}
