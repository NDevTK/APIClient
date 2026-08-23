/* Web Cryptography API §10's Crypto interface and §10.2.1's `subtle` — see crypto.h for why the other two
 * members of §10 are honestly absent and what has to be decided before they exist. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/crypto/crypto.h"
#include "core/crypto/subtle_crypto.h"
#include "core/idl_args.h"
#include "core/realm.h"

static JSClassID g_crypto_class;
static int       g_obj_slot = -1;

/* §10.2.1's `[SecureContext] readonly attribute SubtleCrypto subtle`: "The subtle attribute provides an
   instance of the SubtleCrypto interface which provides low-level cryptographic primitives and algorithms."
   THE OBJECT IS THE REALM'S, asked of the component that owns it. `crypto.subtle === crypto.subtle` holds
   because that component keeps one per realm, not because this getter caches — a cache here would be a second
   statement of an identity, and the two would agree until one of them was reset. */
static JSValue crypto_get_subtle(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    /* WEB IDL §3.7.5's BRAND CHECK on an attribute getter. `Object.getOwnPropertyDescriptor(Crypto.prototype,
       'subtle').get.call({})` is a TypeError, and the attribute's type is not a promise, so it THROWS. */
    if (JS_GetClassID(this_val) != g_crypto_class)
        return JS_ThrowTypeError(ctx, "the `subtle` getter was reached on something that is not a Crypto");
    return subtle_crypto_object(ctx);
}

/* WindowOrWorkerGlobalScope's `[SameObject] readonly attribute Crypto crypto`. */
static JSValue crypto_get_crypto(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_obj_slot);
}

static void crypto_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, obj;

    prev = JS_GetClassProto(ctx, g_crypto_class);
    DCHECK(JS_IsNull(prev), "crypto_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Crypto.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Crypto");
    /* THE MEMBER IS `[SecureContext]`, THE INTERFACE IS NOT. Web IDL §3.3.13 removes the member in a
       non-secure realm rather than making it throw, so `crypto.subtle` is `undefined` over plain http —
       which is exactly what real Chrome answers there and what a bundle's `crypto && crypto.subtle` guard
       is testing for. */
    idl_install_accessor_exposed(ctx, proto, "subtle", crypto_get_subtle, 0, -1, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_crypto_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Crypto", idl_interface_object(ctx, "Crypto", proto));

    obj = JS_NewObjectProtoClass(ctx, proto, g_crypto_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "this realm's Crypto could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);

    /* THE WINDOW MEMBER. The mixin's partial puts it on the global, so it goes on THIS realm's global — a
       nested navigable's `crypto` is its own, which is what `[SameObject]` means per realm. */
    idl_install_accessor(ctx, global, "crypto", crypto_get_crypto, 0, -1);
    JS_FreeValue(ctx, global);
}

void crypto_init(JSContext *ctx)
{
    JSClassDef d = { "Crypto" };

    DCHECK(g_obj_slot < 0, "crypto_init ran twice — the class and the slot are declared once per AGENT");
    /* §14's INTERFACE IS THIS COMPONENT'S DEPENDENCY and is declared here rather than by each host, for the
       reason core/realm.h gives: a host that installed Crypto and not SubtleCrypto would answer `subtle` with
       an object built in some other realm, or with nothing at all. core/realm.h runs the per-realm installs in
       DECLARATION order, so SubtleCrypto's prototype exists before this realm's `subtle` can answer with an
       instance of it. */
    subtle_crypto_init(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_crypto_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_crypto_class, &d) == 0,
          "Crypto: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Web Cryptography §10 this realm's Crypto object");
    agent_state_id("crypto", &g_obj_slot, "§10's per-realm Crypto slot, and the declaration latch");
    realm_declare_intrinsic(crypto_install_realm);
}

void crypto_free(void)
{
    g_obj_slot = -1;
    subtle_crypto_free();
}
