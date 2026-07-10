/* element.classList — see classlist.h. */
#include <string.h>
#include <stdlib.h>
#include "core/dom/classlist.h"
#include "solver/dom_cow.h"   /* dom_attr_capture — the class-attr write is COW-captured for per-flow isolation */
#include <lexbor/dom/dom.h>

extern JSClassID g_el_class_id;   /* borrowed from main.c: unwrap the element behind the classList object */

static int class_has_token(const lxb_char_t *cls, size_t cl, const char *tok) {
    if (!cls || !tok || !tok[0]) return 0;
    size_t tk = strlen(tok), i = 0;
    while (i < cl) {
        while (i < cl && (cls[i]==' '||cls[i]=='\t'||cls[i]=='\n'||cls[i]=='\r'||cls[i]=='\f')) i++;
        size_t s = i;
        while (i < cl && !(cls[i]==' '||cls[i]=='\t'||cls[i]=='\n'||cls[i]=='\r'||cls[i]=='\f')) i++;
        if (i - s == tk && !memcmp(cls + s, tok, tk)) return 1;
    }
    return 0;
}
static JSValue js_classlist_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue elw = JS_GetPropertyStr(ctx, this_val, "__el");
    lxb_dom_element_t *el = JS_GetOpaque(elw, g_el_class_id); JS_FreeValue(ctx, elw);
    if (!el || argc < 1) return JS_FALSE;
    size_t cl = 0; const lxb_char_t *cls = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"class", 5, &cl);
    const char *tok = JS_ToCString(ctx, argv[0]);
    int found = class_has_token(cls, cl, tok);
    if (tok) JS_FreeCString(ctx, tok);
    return found ? JS_TRUE : JS_FALSE;
}
/* FAITHFUL classList.add/remove/toggle: mutate the REAL class attribute (COW-captured, isolated per flow) so
   `classList.add('admin'); if(classList.contains('admin')) fetch(...)` reaches the class-gated endpoint — a
   no-op mutator (the old stub) silently dropped it. magic: 0=add 1=remove 2=toggle. */
#define CL_WS(c) ((c)==' '||(c)=='\t'||(c)=='\n'||(c)=='\r'||(c)=='\f')
static JSValue js_classlist_mut(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    JSValue elw = JS_GetPropertyStr(ctx, this_val, "__el");
    lxb_dom_element_t *el = JS_GetOpaque(elw, g_el_class_id); JS_FreeValue(ctx, elw);
    if (!el || argc < 1) return magic == 2 ? JS_FALSE : JS_UNDEFINED;
    const char *tok = JS_ToCString(ctx, argv[0]);
    if (!tok || !tok[0]) { if (tok) JS_FreeCString(ctx, tok); return magic == 2 ? JS_FALSE : JS_UNDEFINED; }
    size_t cl = 0; const lxb_char_t *cls = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"class", 5, &cl);
    int present = class_has_token(cls, cl, tok);
    int want_add = (magic == 0) ? 1 : (magic == 1) ? 0 : !present;
    if (want_add == present) { JS_FreeCString(ctx, tok); return magic == 2 ? (present ? JS_TRUE : JS_FALSE) : JS_UNDEFINED; }
    size_t tk = strlen(tok);
    char *nc = malloc(cl + tk + 2);
    if (!nc) { JS_FreeCString(ctx, tok); return magic == 2 ? JS_FALSE : JS_UNDEFINED; }
    size_t no = 0, i = 0;   /* copy every token except `tok`, then append it if adding */
    while (i < cl) {
        while (i < cl && CL_WS(cls[i])) i++;
        size_t s = i; while (i < cl && !CL_WS(cls[i])) i++;
        if (i == s) break;
        if (i - s == tk && !memcmp(cls + s, tok, tk)) continue;
        if (no) nc[no++] = ' ';
        memcpy(nc + no, cls + s, i - s); no += i - s;
    }
    if (want_add) { if (no) nc[no++] = ' '; memcpy(nc + no, tok, tk); no += tk; }
    dom_attr_capture(el, "class");
    lxb_dom_element_set_attribute(el, (const lxb_char_t *)"class", 5, (const lxb_char_t *)nc, no);
    free(nc); JS_FreeCString(ctx, tok);
    return magic == 2 ? (want_add ? JS_TRUE : JS_FALSE) : JS_UNDEFINED;
}
JSValue js_el_classlist_get(JSContext *ctx, JSValueConst this_val) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__el", JS_DupValue(ctx, this_val));
    JS_SetPropertyStr(ctx, o, "contains", JS_NewCFunction(ctx, js_classlist_contains, "contains", 1));
    JS_SetPropertyStr(ctx, o, "add", JS_NewCFunctionMagic(ctx, js_classlist_mut, "add", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, o, "remove", JS_NewCFunctionMagic(ctx, js_classlist_mut, "remove", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, o, "toggle", JS_NewCFunctionMagic(ctx, js_classlist_mut, "toggle", 1, JS_CFUNC_generic_magic, 2));
    return o;
}
