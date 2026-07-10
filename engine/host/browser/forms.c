/* HTML form submission -> @H endpoint — see forms.h. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "forms.h"
#include "attr_shadow.h"   /* attr_shadow_find/opaque — a JS-set control value's taint */
#include "endpoint.h"      /* record_endpoint */
#include "url.h"           /* url_resolve — WHATWG canonicalization (browser/url.c) */
#include <lexbor/dom/dom.h>

/* Borrowed from main.c: the element class id (unwrap the form element) and the page origin. */
extern JSClassID g_el_class_id;
extern const char *g_origin;

static void form_enc(const char *s, char *out, size_t cap) {   /* application/x-www-form-urlencoded value */
    static const char *hex = "0123456789ABCDEF"; size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out[o++]=(char)c;
        else if (c==' ') out[o++]='+';
        else { out[o++]='%'; out[o++]=hex[c>>4]; out[o++]=hex[c&15]; }
    }
    out[o] = 0;
}
static char *form_field_value(JSContext *ctx, lxb_dom_element_t *el) {   /* malloc'd; caller frees */
    int si = attr_shadow_find(el, "value");
    if (si >= 0 && JS_IsOpaque(attr_shadow_opaque(si))) {
        JSValue ex = JS_OpaqueExample(ctx, attr_shadow_opaque(si)); char *r = NULL;
        if (!JS_IsUndefined(ex)) { const char *s = JS_ToCString(ctx, ex); if (s) { r = strdup(s); JS_FreeCString(ctx, s); } }
        JS_FreeValue(ctx, ex);
        if (!r) { const char *sh = JS_OpaqueShapeC(attr_shadow_opaque(si)); r = strdup(sh ? sh : "{opaque}"); }
        return r;
    }
    size_t vl = 0; const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"value", 5, &vl);
    if (v) { char *r = malloc(vl + 1); if (r) { memcpy(r, v, vl); r[vl] = 0; } return r; }
    return strdup("");
}
static void form_collect(JSContext *ctx, lxb_dom_node_t *node, JSValue params, uint32_t *idx,
                         char *body, size_t bcap, const char *loc) {
    for (lxb_dom_node_t *n = node->first_child; n; n = n->next) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"name", 4, &nl);
            if (nm && nl) {
                char nbuf[256]; size_t nn = nl < 255 ? nl : 255; memcpy(nbuf, nm, nn); nbuf[nn] = 0;
                char *val = form_field_value(ctx, el);
                JSValue po = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, po, "name", JS_NewString(ctx, nbuf));
                JS_SetPropertyStr(ctx, po, "location", JS_NewString(ctx, loc));
                JSValue vv = JS_NewArray(ctx);
                if (val && val[0]) JS_SetPropertyUint32(ctx, vv, 0, JS_NewString(ctx, val));
                JS_SetPropertyStr(ctx, po, "validValues", vv);
                JS_SetPropertyUint32(ctx, params, (*idx)++, po);
                char enc[512]; form_enc(val, enc, sizeof enc);
                size_t bl = strlen(body);
                snprintf(body + bl, bcap - bl, "%s%s=%s", bl ? "&" : "", nbuf, enc);
                free(val);
            }
        }
        form_collect(ctx, n, params, idx, body, bcap, loc);   /* nested controls (fields inside divs/fieldsets) */
    }
}
JSValue js_form_submit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *form = JS_GetOpaque(this_val, g_el_class_id);
    if (!form) return JS_UNDEFINED;   /* not a real element (defensive) — nothing to submit */
    size_t ml = 0; const lxb_char_t *mv = lxb_dom_element_get_attribute(form, (const lxb_char_t *)"method", 6, &ml);
    char method[8] = "GET";
    if (mv && ml) { size_t mn = ml < 7 ? ml : 7; for (size_t i = 0; i < mn; i++) { char c = (char)mv[i]; method[i] = (c>='a'&&c<='z')?(char)(c-32):c; } method[mn] = 0; }
    int is_body = (strcmp(method,"POST")==0 || strcmp(method,"PUT")==0 || strcmp(method,"DELETE")==0 || strcmp(method,"PATCH")==0);
    size_t al = 0; const lxb_char_t *av = lxb_dom_element_get_attribute(form, (const lxb_char_t *)"action", 6, &al);
    char *action = NULL;
    if (av && al) { action = malloc(al + 1); if (action) { memcpy(action, av, al); action[al] = 0; } }
    char *url = url_resolve(action ? action : "", g_origin);
    if (!url) url = strdup(g_origin ? g_origin : "");
    free(action);
    JSValue params = JS_NewArray(ctx); uint32_t pidx = 0;
    char fbody[4096]; fbody[0] = 0;
    form_collect(ctx, lxb_dom_interface_node(form), params, &pidx, fbody, sizeof fbody, is_body ? "body" : "query");
    char *final_url = url;
    if (!is_body && fbody[0]) {   /* GET: the controls ARE the query string */
        size_t need = strlen(url) + 1 + strlen(fbody) + 1;
        final_url = malloc(need); if (final_url) snprintf(final_url, need, "%s?%s", url, fbody); else final_url = url;
    }
    JSValue ep = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, method));
    JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, final_url ? final_url : ""));
    JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
    JS_SetPropertyStr(ctx, ep, "params", params);
    if (is_body && fbody[0]) JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, fbody));
    record_endpoint(ctx, ep);   /* emits @H unless a candidate flow (the sink respects g_candidate) */
    if (final_url != url) free(final_url);
    free(url);
    return JS_UNDEFINED;
}
