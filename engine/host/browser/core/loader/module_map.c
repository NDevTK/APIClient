/* HTML §8.1.3.2 "Environment settings objects"' module map. See module_map.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/realm.h"
#include "core/fetch/fetch.h"
#include "core/fetch/headers.h"
#include "core/mime/mime_type.h"
#include "core/loader/script_fetch.h"
#include "core/loader/module_map.h"

/* The per-realm record: a null-prototype object whose property `<module type> <url>` is an entry. An in-flight
   entry is a null-prototype object { promise, resolve, reject } of one capability; a module script is its source
   text, a string. The slot holds one record for the realm's life; what a flow writes is its properties. */
static JSClassID g_slot = JS_INVALID_CLASS_ID;
static int       g_ready;

static JSValue mm_record(JSContext *realm)
{
    JSValue r;

    DCHECK(g_ready, "a realm's module map was reached before HTML §8.1.3.2's module map was declared");
    r = realm_value_get(realm, g_slot);
    DCHECK(JS_IsObject(r), "a realm answered its module map with no record — every realm is given one at "
                           "creation, so this realm never ran module_map_install");
    return r;
}

/* The key `<module type> <url>`: a serialized URL holds no space, so the pair is recoverable and two pairs never
   share a key. Owned; `free` it. */
static char *mm_key(const char *url, const char *module_type)
{
    size_t n = strlen(url) + strlen(module_type) + 2;
    char *k = malloc(n);

    CHECK(k != NULL, "module map: OOM composing an entry's key");
    DCHECKF(strchr(url, ' ') == NULL, "the module map was asked for `%s`, which is not a serialized URL", url);
    snprintf(k, n, "%s %s", module_type, url);
    return k;
}

static JSValue mm_get(JSContext *ctx, JSValueConst rec, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, key);

    CHECK(!JS_IsException(v), "module map: reading an entry of a null-prototype record threw");
    return v;
}

static void mm_set(JSContext *ctx, JSValueConst rec, const char *key, JSValue v)
{
    CHECK(JS_SetPropertyStr(ctx, rec, key, v) >= 0, "module map: OOM writing an entry");
}

static void mm_remove(JSContext *ctx, JSValueConst rec, const char *key)
{
    JSAtom a = JS_NewAtom(ctx, key);

    CHECK(a != JS_ATOM_NULL, "module map: OOM naming an entry to remove");
    CHECK(JS_DeleteProperty(ctx, rec, a, 0) >= 0, "module map: removing an entry of a null-prototype record threw");
    JS_FreeAtom(ctx, a);
}

/* §8.1.4.2's processResponseConsumeBody, step 13 of fetch a single module script, from its failure test to the
   creation of moduleScript: the source text, or NULL where moduleScript stays null. Owned. */
static char *mm_module_source(JSContext *ctx, JSValueConst reply, const char *url, const char *module_type,
                              size_t *out_n)
{
    HeaderList hl = { 0 };
    char *content_type;
    MimeType mt;
    bool js = false;
    JSValue bv;
    const uint8_t *body;
    size_t body_n = 0;
    char *src;

    /* "bodyBytes is null or failure; or response's status is not an ok status": a network error crosses as
       JS_NULL (core/fetch/fetch.h), and a status is the server's, so neither is asserted. */
    if (JS_IsNull(reply) || !script_fetch_status_ok(fetch_reply_status(ctx, reply))) return NULL;
    if (strcmp(module_type, MODULE_TYPE_JAVASCRIPT_OR_WASM) != 0) {
        DFAILF("the module `%s` was fetched with the module type `%s`, and HTML §8.1.4.3 \"Creating scripts\"' "
               "CSS, JSON and text module scripts are not built — build the one this type names, keyed here "
               "beside the JavaScript one", url, module_type);
        return NULL;
    }
    /* "Let mimeType be the result of extracting a MIME type from response's header list"; an absent
       `Content-Type` is extraction failure, which is no JavaScript MIME type. The `application/wasm` arm creates
       a WebAssembly module script.
       Named residual. Not covered: WebAssembly module scripts, so a wasm-typed reply leaves moduleScript null.
       Next diff builds: creating a WebAssembly module script given bodyBytes, stored in this map beside the
       JavaScript one. Absence shows as: an import or a modulepreload of a `.wasm` chunk failing where a browser
       loads it, so a bundle's wasm feature test takes its fallback. */
    fetch_reply_header_list(ctx, reply, &hl);
    content_type = header_list_get(&hl, "content-type");
    mime_type_init(&mt);
    js = mime_type_extract(&mt, content_type) && mime_type_is_javascript(&mt);
    mime_type_free(&mt);
    free(content_type);
    header_list_free(&hl);
    if (!js) return NULL;
    bv = fetch_reply_body(ctx, reply);
    body = fetch_body_bytes(ctx, bv, &body_n);
    src = script_fetch_module_source_text((const char *)body, body_n, out_n);
    JS_FreeValue(ctx, bv);
    return src;
}

/* A string field of an in-flight entry. Owned C string. */
static const char *mm_field(JSContext *ctx, JSValueConst fetch, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, fetch, name);
    const char *c;

    DCHECKF(JS_IsString(v), "an in-flight module map entry has no `%s` — module_map_ask writes it with the entry",
            name);
    c = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    CHECK(c != NULL, "module map: OOM reading an in-flight entry");
    return c;
}

void module_map_settle(JSContext *ctx, JSValueConst fetch, JSValueConst reply)
{
    JSValue rec = JS_GetPropertyStr(ctx, fetch, "map"), now, half;
    const char *key = mm_field(ctx, fetch, "key"), *url = mm_field(ctx, fetch, "url");
    const char *type = mm_field(ctx, fetch, "type");
    size_t src_n = 0;
    char *src;

    DCHECK(JS_IsObject(rec), "an in-flight module map entry names no map — module_map_ask writes it with the entry");
    DCHECK(JS_IsObject(reply) || JS_IsNull(reply),
           "a module fetch was settled with something other than the host's reply record or a network error's "
           "null — the park delivers one or the other");
    /* The one place an entry and a fetch meet: a fetch is issued only by step 7, which writes this entry, and is
       answered once, so the map still holds this fetch's own entry here. Another value is a second fetch for the
       entry or a second answer to this one. */
    now = mm_get(ctx, rec, key);
    DCHECKF(JS_IsObject(now) && JS_VALUE_GET_PTR(now) == JS_VALUE_GET_PTR(fetch),
            "the module map entry for `%s` is not the in-flight entry whose fetch is answering — two fetches "
            "were issued for one (url, module type), or one fetch was answered twice", key);
    JS_FreeValue(ctx, now);
    /* Step 13's tail: "Let callbacks be moduleMap[(url, moduleType)]. If moduleScript is null, then remove
       moduleMap[(url, moduleType)]; otherwise set moduleMap[(url, moduleType)] to moduleScript", then each
       callback runs. Every callback is a reaction on the entry's promise, so one settlement runs them in the
       order they were appended. */
    src = mm_module_source(ctx, reply, url, type, &src_n);
    if (src == NULL) {
        JSValue err;

        mm_remove(ctx, rec, key);
        /* HTML §8.1.6.7.3 HostLoadImportedModule's onSingleFetchComplete makes a null moduleScript "a new
           TypeError"; a modulepreload's callback reads only that the result is null. */
        JS_ThrowTypeError(ctx, "Failed to fetch dynamically imported module: %s", url);
        err = JS_GetException(ctx);
        half = JS_GetPropertyStr(ctx, fetch, "reject");
        CHECK(JS_SettleCapability(ctx, half, err) == 0, "module map: rejecting a module fetch's promise threw");
        JS_FreeValue(ctx, err);
    } else {
        JSValue sv = JS_NewStringLen(ctx, src, src_n);

        free(src);
        CHECK(!JS_IsException(sv), "module map: OOM holding a module script's source text");
        mm_set(ctx, rec, key, JS_DupValue(ctx, sv));
        half = JS_GetPropertyStr(ctx, fetch, "resolve");
        CHECK(JS_SettleCapability(ctx, half, sv) == 0, "module map: fulfilling a module fetch's promise threw");
        JS_FreeValue(ctx, sv);
    }
    JS_FreeValue(ctx, half);
    JS_FreeValue(ctx, rec);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, type);
}

static JSValue mm_settle_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                              JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "a module fetch's processResponseConsumeBody was called with no reply");
    module_map_settle(ctx, func_data[0], argv[0]);
    return JS_UNDEFINED;
}

JSValue module_map_settle_function(JSContext *ctx, JSValueConst fetch)
{
    JSValue fn = JS_NewCFunctionData(ctx, mm_settle_call, 1, 0, 1, &fetch);

    CHECK(!JS_IsException(fn), "module map: OOM minting a module fetch's processResponseConsumeBody");
    return fn;
}

ModuleMapAsk module_map_ask(JSContext *realm, const char *url, const char *module_type, JSValue *promise,
                            JSValue *fetch)
{
    JSValue rec = mm_record(realm), cur, entry, resolving[2], p;
    char *key = mm_key(url, module_type);
    ModuleMapAsk ask;

    DCHECK(promise != NULL && fetch != NULL, "module_map_ask was given nowhere to put its answer");
    *fetch = JS_UNDEFINED;
    cur = mm_get(realm, rec, key);
    if (JS_IsString(cur)) {
        /* Step 5: "If moduleMap[(url, moduleType)] is a module script, run onComplete given
           moduleMap[(url, moduleType)], and return." */
        *promise = JS_NewSettledPromise(realm, false, cur);
        CHECK(!JS_IsException(*promise), "module map: OOM answering a module script already in the map");
        ask = MODULE_MAP_READY;
    } else if (JS_IsObject(cur)) {
        /* Step 6: "If moduleMap[(url, moduleType)] is a list, append onComplete to moduleMap[(url, moduleType)],
           and return." */
        *promise = JS_GetPropertyStr(realm, cur, "promise");
        DCHECK(JS_IsPromise(*promise), "an in-flight module map entry holds no promise — step 7 writes one");
        ask = MODULE_MAP_JOINED;
    } else {
        /* Step 7: "Set moduleMap[(url, moduleType)] to « onComplete »." The caller issues the fetch. The entry
           carries its own key, so the fetch's delivery reaches it without the realm. */
        DCHECK(JS_IsUndefined(cur), "a module map entry is neither absent, in flight nor a module script");
        p = JS_NewPromiseCapability(realm, resolving);
        CHECK(!JS_IsException(p), "module map: OOM minting an in-flight entry's promise");
        entry = JS_NewObjectProto(realm, JS_NULL);
        CHECK(!JS_IsException(entry), "module map: OOM minting an in-flight entry");
        mm_set(realm, entry, "promise", JS_DupValue(realm, p));
        mm_set(realm, entry, "resolve", resolving[0]);
        mm_set(realm, entry, "reject", resolving[1]);
        mm_set(realm, entry, "map", JS_DupValue(realm, rec));
        mm_set(realm, entry, "key", JS_NewString(realm, key));
        mm_set(realm, entry, "url", JS_NewString(realm, url));
        mm_set(realm, entry, "type", JS_NewString(realm, module_type));
        mm_set(realm, rec, key, JS_DupValue(realm, entry));
        *fetch = entry;
        *promise = p;
        ask = MODULE_MAP_FETCH;
    }
    JS_FreeValue(realm, cur);
    JS_FreeValue(realm, rec);
    free(key);
    return ask;
}

/* HTML §8.1.3.2: an environment settings object has "A module map that is used when importing JavaScript
   modules", empty at the realm's creation, so the record is in the pre-boot baseline every flow forks from. */
static void module_map_install(JSContext *ctx)
{
    JSValue r = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(r), "module map: OOM building a realm's record");
    realm_value_set(ctx, g_slot, r);
}

void module_map_init(JSContext *ctx)
{
    g_slot = realm_value_declare(ctx, "HTML §8.1.3.2's module map of the environment settings object");
    g_ready = 1;
    realm_declare_intrinsic(module_map_install);
    agent_state_flag("module_map", &g_ready, "the declaration latch");
    agent_state_realm_slot("module_map", &g_slot, "the per-realm slot a settings object's module map is held in");
}

void module_map_free(JSRuntime *rt)
{
    (void)rt;
    DCHECK(g_ready, "HTML §8.1.3.2's module map was released in an agent that never declared it");
    agent_state_undo("module_map");
}
