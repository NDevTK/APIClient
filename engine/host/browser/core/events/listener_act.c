/* Which user act runs a function as an event listener.
 *
 * The solver drives a function nothing called (solver/engine.c's orphan drive), and a fire verified inside that
 * drive reproduces in a browser only if a person can cause the call. A listener registered on a node for a
 * user-interface event type is such a function: the act is that event on that node. This component answers the
 * act from DOM §2.7's event listener lists as the running flow's world holds them, and answers nothing for a
 * function no list holds or a type no page user causes, so the PoC composition can refuse the fire. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/events/listener_act.h"
#include "core/frame/opener_policy.h"
#include "core/frame/window_proxy.h"
#include "core/html/input_value.h"
#include "solver/world.h"

/* The user-interface event types a page user causes by acting on a node. A click is stated as itself, because a
   live verifier performs it through HTMLElement's click(); a drop is the person dropping a file (HTML §6.11.5);
   every other listed type is dispatched. */
static const char *const ACT_TYPES[] = {
    "click", "dblclick", "auxclick", "contextmenu", "mousedown", "mouseup", "mouseover", "mouseout",
    "mouseenter", "mouseleave", "pointerdown", "pointerup", "pointerover", "pointerout", "focus", "blur",
    "focusin", "focusout", "input", "change", "keydown", "keyup", "keypress", "submit", "drop", NULL
};

static bool act_type_ok(const char *type)
{
    for (int i = 0; ACT_TYPES[i]; i++) if (!strcmp(ACT_TYPES[i], type)) return true;
    return false;
}

static bool id_is_simple(const char *s, size_t n)
{
    if (n == 0 || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return false;
    for (size_t i = 1; i < n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '_' || s[i] == '-'))
            return false;
    return true;
}

static const char *node_id(lxb_dom_node_t *n, size_t *len)
{
    *len = 0;
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;
    return (const char *)lxb_dom_element_get_attribute(lxb_dom_interface_element(n), (const lxb_char_t *)"id", 2,
                                                       len);
}

/* `#id` where the element's identifier is a plain name no other element of the document carries. */
static char *locator_by_id(JSContext *ctx, lxb_dom_node_t *el, lxb_dom_node_t *doc)
{
    size_t n = 0, m = 0;
    const char *id = node_id(el, &n);
    int same = 0;
    char *out;

    if (!id || !id_is_simple(id, n)) return NULL;
    for (lxb_dom_node_t *u = doc; u; u = shadow_root_next_in_shadow_including(ctx, u, doc)) {
        const char *o = node_id(u, &m);
        if (o && m == n && !memcmp(o, id, n)) same++;
    }
    if (same != 1) return NULL;
    out = malloc(n + 2);
    CHECK(out, "listener act: OOM spelling an element locator");
    out[0] = '#';
    memcpy(out + 1, id, n);
    out[n + 1] = 0;
    return out;
}

/* `html > body > button:nth-of-type(2)`: each element's local name and its position among same-named siblings,
   from the document element down. NULL for a node outside the document element's tree. */
static char *locator_by_path(lxb_dom_node_t *el)
{
    char *out = NULL;
    size_t len = 0;
    lxb_dom_node_t *u;

    for (u = el; u && u->type == LXB_DOM_NODE_TYPE_ELEMENT; u = u->parent) {
        size_t ln = 0;
        const char *name = (const char *)lxb_dom_element_local_name(lxb_dom_interface_element(u), &ln);
        int k = 1;
        char seg[160];
        int sn;
        char *grown;

        for (lxb_dom_node_t *s = u->prev; s; s = s->prev) {
            size_t sl = 0;
            const char *sname;
            if (s->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            sname = (const char *)lxb_dom_element_local_name(lxb_dom_interface_element(s), &sl);
            if (sl == ln && !memcmp(sname, name, ln)) k++;
        }
        sn = snprintf(seg, sizeof seg, "%.*s:nth-of-type(%d)%s", (int)ln, name, k, len ? " > " : "");
        CHECK(sn > 0 && (size_t)sn < sizeof seg, "listener act: an element's local name is too long to locate");
        grown = malloc(len + (size_t)sn + 1);
        CHECK(grown, "listener act: OOM spelling an element locator");
        memcpy(grown, seg, (size_t)sn);
        if (len) memcpy(grown + sn, out, len);
        grown[sn + len] = 0;
        free(out);
        out = grown;
        len += (size_t)sn;
    }
    if (u == NULL || u->type != LXB_DOM_NODE_TYPE_DOCUMENT) { free(out); return NULL; }
    return out;
}

/* The function's own `name` data property where it is a non-empty identifier, read without running an
   accessor; NULL otherwise. malloc'd. */
static char *handler_name(JSContext *ctx, JSValueConst fn)
{
    JSPropertyDescriptor d;
    JSAtom a = JS_NewAtom(ctx, "name");
    char *out = NULL;
    int r;

    CHECK(a != JS_ATOM_NULL, "listener act: OOM naming a handler");
    r = JS_GetOwnSlotDesc(ctx, &d, fn, a);
    JS_FreeAtom(ctx, a);
    if (r <= 0) return NULL;
    if (!(d.flags & JS_PROP_GETSET) && JS_IsString(d.value)) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, d.value);

        CHECK(s != NULL, "listener act: OOM reading a handler's name");
        if (id_is_simple(s, n)) { out = strdup(s); CHECK(out, "listener act: OOM copying a handler's name"); }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, d.value);
    JS_FreeValue(ctx, d.getter);
    JS_FreeValue(ctx, d.setter);
    return out;
}

/* The event types that follow a person typing into a text control, in UI Events' order after the value
   changes: `input`, the `keyup` of the last key, and `change` at the commit (HTML §4.10.5.5 Common event
   behaviors). A `keydown` or `keypress` listener runs before the value changes and reads it untyped. */
static bool typing_type(const char *type)
{
    return !strcmp(type, "input") || !strcmp(type, "keyup") || !strcmp(type, "change");
}

static char *act_json(JSContext *ctx, JSValueConst fn, const char *kind, const char *type, const char *target)
{
    char *name = handler_name(ctx, fn);
    size_t n = strlen(kind) + strlen(type) + strlen(target) + (name ? strlen(name) : 0) + 80;
    char *out = malloc(n);

    CHECK(out, "listener act: OOM spelling an act");
    snprintf(out, n, "{\"kind\":\"%s\",\"target\":\"%s\",\"type\":\"%s\"%s%s%s}", kind, target, type,
             name ? ",\"handler\":\"" : "", name ? name : "", name ? "\"" : "");
    free(name);
    return out;
}

/* The node of the document whose listener list holds `fn`, in tree order, with the listener's type (malloc'd);
   NULL when no node's list holds it. */
static lxb_dom_node_t *listener_holder(JSContext *ctx, JSValueConst fn, char **type)
{
    lxb_dom_document_t *d = document_associated(ctx);
    lxb_dom_node_t *doc;

    *type = NULL;
    if (!JS_IsFunction(ctx, fn) || d == NULL) return NULL;
    doc = lxb_dom_interface_node(d);
    for (lxb_dom_node_t *u = doc; u; u = shadow_root_next_in_shadow_including(ctx, u, doc)) {
        JSValueConst w = node_wrap_peek(u);

        if (JS_IsObject(w) && (*type = event_target_listener_type_of(ctx, w, fn))) return u;
    }
    return NULL;
}

JSValueConst listener_act_typed_control(JSContext *ctx, JSValueConst fn)
{
    char *type;
    lxb_dom_node_t *u = listener_holder(ctx, fn, &type);
    bool typed = u && typing_type(type) && input_value_user_types(ctx, node_wrap_peek(u));

    free(type);
    return typed ? node_wrap_peek(u) : JS_UNDEFINED;
}

char *listener_act_of(JSContext *ctx, JSValueConst fn)
{
    lxb_dom_document_t *d = document_associated(ctx);
    lxb_dom_node_t *doc, *u;
    JSValueConst win;
    char *type;

    if (!JS_IsFunction(ctx, fn) || d == NULL) return NULL;
    doc = lxb_dom_interface_node(d);
    if ((u = listener_holder(ctx, fn, &type))) {
        char *loc, *act;
        const char *kind;

        if (!act_type_ok(type)) { free(type); return NULL; }
        if (u == doc) loc = strdup("document");
        else if (!(loc = locator_by_id(ctx, u, doc))) loc = locator_by_path(u);
        if (!loc) { free(type); return NULL; }
        kind = !strcmp(type, "click") ? "click" : !strcmp(type, "drop") ? "drop"
             : typing_type(type) && input_value_user_types(ctx, node_wrap_peek(u)) ? "input" : "dispatch";
        act = act_json(ctx, fn, kind, type, loc);
        free(type);
        free(loc);
        return act;
    }
    win = document_window_of(doc);
    if (JS_IsObject(win) && (type = event_target_listener_type_of(ctx, win, fn))) {
        char *act = act_type_ok(type) ? act_json(ctx, fn, !strcmp(type, "click") ? "click"
                                                          : !strcmp(type, "drop") ? "drop" : "dispatch", type,
                                                "window") : NULL;

        /* §9.3.3's `message` on the Window is caused by another document's postMessage, so it is an act when
           an attacker document holds this window: as its opener, unless the document's Cross-Origin-Opener-
           Policy severs a cross-origin opener (every value but `unsafe-none`, HTML §7.1.3), or as its parent,
           where the response lets a cross-origin document embed it (HTML §7.7 and frame-ancestors, as the host
           stated it on the navigable). Its data and origin are the event argument's, which the PoC composition
           does not solve yet, so solve.c refuses a fire verified under one (`firedUnderUnsolvedAct`) and states
           the act on the parked search. */
        if (!act && !strcmp(type, "message")) {
            char *name = handler_name(ctx, fn);
            JSValue proxy = window_proxy_of_document(ctx, world_local_doc());
            OpenerPolicyValue coop = window_proxy_opener_policy(proxy);
            int framing = window_proxy_cross_origin_framing(proxy);
            const char *route = coop == OPENER_POLICY_UNSAFE_NONE ? "\"route\":\"opener\""
                                : framing == 1 ? "\"route\":\"frame\""
                                : framing == 0 ? "\"route\":\"none\",\"routeRefused\":\"Cross-Origin-Opener-Policy "
                                                 "and the response's framing policy\""
                                : "\"route\":\"none\",\"routeRefused\":\"Cross-Origin-Opener-Policy, and no "
                                  "framing policy was stated for the response\"";
            size_t n = (name ? strlen(name) : 0) + strlen(route) + 96;

            act = malloc(n);
            CHECK(act, "listener act: OOM spelling an act");
            snprintf(act, n, "{\"kind\":\"message\",\"target\":\"window\",\"type\":\"message\",%s%s%s%s}",
                     route, name ? ",\"handler\":\"" : "", name ? name : "", name ? "\"" : "");
            free(name);
            JS_FreeValue(ctx, proxy);
        }
        free(type);
        return act;
    }
    return NULL;
}
