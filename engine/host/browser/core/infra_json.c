/* Infra §6 "JSON" — see infra_json.h. */
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/encoding/encoding.h"   /* Encoding §6 "Hooks for standards"'s UTF-8 decode */
#include "core/infra_json.h"
#include "core/realm.h"

static JSClassID g_parse_slot = JS_INVALID_CLASS_ID;

static void infra_json_install_realm(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue parse = JS_GetPropertyStr(ctx, json, "parse");

    CHECK(JS_IsFunction(ctx, parse), "%JSON.parse% is not a function — Infra §6 has no parser to call");
    realm_value_set(ctx, g_parse_slot, parse);
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, global);
}

void infra_json_init(JSContext *ctx)
{
    g_parse_slot = realm_value_declare(ctx, "%JSON.parse% (Infra §6 JSON)");
    agent_state_realm_slot("infra_json", &g_parse_slot,
                           "Infra §6 JSON's per-realm %JSON.parse%, the function every platform JSON parse calls");
    realm_declare_intrinsic(infra_json_install_realm);
}

void infra_json_free(void)
{
    agent_state_undo("infra_json");
}

JSValue infra_json_parse_fn(JSContext *ctx)
{
    DCHECK(g_parse_slot != JS_INVALID_CLASS_ID,
           "%JSON.parse% was asked for before infra_json_init declared its slot");
    return realm_value_get(ctx, g_parse_slot);
}

JSValue infra_json_bytes_string(JSContext *ctx, const uint8_t *bytes, size_t len)
{
    size_t n = 0;
    char *text = encoding_utf8_decode((const char *)bytes, len, &n);
    JSValue s;

    CHECK(text != NULL, "Infra §6: OOM running Encoding §6's UTF-8 decode over JSON bytes");
    s = JS_NewStringLen(ctx, text, n);
    free(text);
    return s;
}
