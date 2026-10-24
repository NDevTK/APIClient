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

/* The user-interface event types a page user causes by acting on a node. A click is stated as itself, because a
   live verifier performs it through HTMLElement's click(); every other listed type is dispatched. */
static const char *const ACT_TYPES[] = {
    "click", "dblclick", "auxclick", "contextmenu", "mousedown", "mouseup", "mouseover", "mouseout",
    "mouseenter", "mouseleave", "pointerdown", "pointerup", "pointerover", "pointerout", "focus", "blur",
    "focusin", "focusout", "input", "change", "keydown", "keyup", "keypress", "submit", NULL
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

static char *act_json(const char *type, const char *target)
{
    size_t n = strlen(type) + strlen(target) + 64;
    char *out = malloc(n);

    CHECK(out, "listener act: OOM spelling an act");
    snprintf(out, n, "{\"kind\":\"%s\",\"target\":\"%s\",\"type\":\"%s\"}",
             strcmp(type, "click") ? "dispatch" : "click", target, type);
    return out;
}

char *listener_act_of(JSContext *ctx, JSValueConst fn)
{
    lxb_dom_document_t *d = document_associated(ctx);
    lxb_dom_node_t *doc;
    JSValueConst win;
    char *type;

    if (!JS_IsFunction(ctx, fn) || d == NULL) return NULL;
    doc = lxb_dom_interface_node(d);
    for (lxb_dom_node_t *u = doc; u; u = shadow_root_next_in_shadow_including(ctx, u, doc)) {
        JSValueConst w = node_wrap_peek(u);
        char *loc, *act;

        if (!JS_IsObject(w) || !(type = event_target_listener_type_of(ctx, w, fn))) continue;
        if (!act_type_ok(type)) { free(type); return NULL; }
        if (u == doc) loc = strdup("document");
        else if (!(loc = locator_by_id(ctx, u, doc))) loc = locator_by_path(u);
        if (!loc) { free(type); return NULL; }
        act = act_json(type, loc);
        free(type);
        free(loc);
        return act;
    }
    win = document_window_of(doc);
    if (JS_IsObject(win) && (type = event_target_listener_type_of(ctx, win, fn))) {
        char *act = act_type_ok(type) ? act_json(type, "window") : NULL;
        free(type);
        return act;
    }
    return NULL;
}
