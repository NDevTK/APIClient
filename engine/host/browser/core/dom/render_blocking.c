/* HTML §3.1.6 Render-blocking mechanism. See render_blocking.h. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/realm.h"
#include "core/dom/abstract_range.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/render_blocking.h"
#include "solver/engine.h"

static JSClassID g_set_slot = JS_INVALID_CLASS_ID;

void render_blocking_init(JSContext *ctx)
{
    DCHECK(g_set_slot == JS_INVALID_CLASS_ID,
           "render_blocking_init ran twice — §3.1.6's realm slot is declared once per agent");
    g_set_slot = realm_value_declare(ctx, "HTML §3.1.6 render-blocking element set");
    agent_state_realm_slot("document", &g_set_slot,
                           "the per-realm slot HTML §3.1.6's render-blocking element set lives in");
}

void render_blocking_free(void)
{
    DCHECK(g_set_slot != JS_INVALID_CLASS_ID,
           "§3.1.6's render-blocking element set was released in an agent that never declared it");
    agent_state_reached("document");   /* the slot is reset by document_agent_free's agent_state_undo */
}

/* "Each Document has a render-blocking element set, a set of elements, initially the empty set." Built with the
   realm so it is baseline rather than created inside whichever flow blocks first. */
void render_blocking_install(JSContext *ctx)
{
    JSValue set = JS_NewArray(ctx);

    CHECK(!JS_IsException(set), "§3.1.6: OOM building a realm's render-blocking element set");
    realm_value_set(ctx, g_set_slot, set);
}

/* OWNED. The set of the document `realm` holds. */
static JSValue rb_set(JSContext *realm)
{
    JSValue set = realm_value_get(realm, g_set_slot);

    DCHECK(JS_IsArray(set), "a realm answered for its render-blocking element set with no set");
    return set;
}

static uint32_t rb_len(JSContext *realm, JSValueConst set)
{
    JSValue len = JS_GetPropertyStr(realm, set, "length");
    uint32_t n = 0;

    JS_ToUint32(realm, &n, len);
    JS_FreeValue(realm, len);
    return n;
}

/* The index of `n` in the set, or -1. */
static int64_t rb_index(JSContext *realm, JSValueConst set, const lxb_dom_node_t *n)
{
    uint32_t len = rb_len(realm, set), i;

    for (i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(realm, set, i);
        bool hit = node_of(v) == n;

        JS_FreeValue(realm, v);
        if (hit) return i;
    }
    return -1;
}

const lxb_dom_node_t *render_blocking_parse_position(JSContext *ctx)
{
    return lxb_dom_interface_node(engine_document_parse_position(document_doc(ctx)));
}

/* "the body element of document is null", read at the parse position: the body element exists in this
   engine's tree before the parser reaches it, so it counts as null while the parser stands at a node the body
   element does not precede. A position no longer in the tree (a script the page removed before it ran) has
   lost what it would say, and the tree as it stands answers. */
static bool rb_body_null_at(const lxb_dom_node_t *doc, const lxb_dom_node_t *at)
{
    lxb_dom_node_t *body = document_body_of(doc);

    if (body == NULL) return true;
    if (at == NULL || node_root((lxb_dom_node_t *)at) != node_root(body)) return false;
    return boundary_position(body, 0, (lxb_dom_node_t *)at, 0) != BP_BEFORE;
}

bool render_blocking_allows_adding(const lxb_dom_node_t *doc, const lxb_dom_node_t *at)
{
    const char *type;

    DCHECK(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "§3.1.6's allows-adding question was asked of something that is not a Document");
    type = document_content_type_of(lxb_dom_interface_document((lxb_dom_node_t *)doc));
    return strcmp(type, "text/html") == 0 && rb_body_null_at(doc, at);
}

/* Named residual. Not covered: producers other than a `<link rel=stylesheet>` the parser created — HTML §2.5.8's
   explicit `blocking=render` token on any element, a parser-created `<style>` (§4.2.6, unblocked in a queued
   task), and a link a `document.write` parse created. Next diff builds: §2.5.8's blocking tokens set as the one
   predicate those elements' "potentially render-blocking" read, with §4.12.1.1's block and the unblock in
   "execute the script element". How its absence would show: a rendering opportunity taken while an element a
   browser counts as render-blocking is still loading. */
void render_blocking_block(lxb_dom_element_t *el, const lxb_dom_node_t *at)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSContext *realm = document_realm_of(n);   /* step 1: "Let document be el's node document" */
    JSValue set, wrap;

    DCHECK(node_is_connected(n),
           "rendering was blocked on an element that is not connected — §3.1.6 unblocks one that becomes "
           "disconnected, so a disconnected member would stay in the set");
    if (realm == NULL) return;
    if (!render_blocking_allows_adding(lxb_dom_interface_node(n->owner_document), at)) return;   /* step 2 */
    set = rb_set(realm);
    if (rb_index(realm, set, n) < 0) {   /* an Infra set append of a member changes nothing */
        wrap = node_wrap(realm, n);
        CHECK(!JS_IsException(wrap), "§3.1.6: OOM wrapping an element to block rendering on");
        JS_SetPropertyUint32(realm, set, rb_len(realm, set), wrap);
    }
    JS_FreeValue(realm, set);
}

void render_blocking_unblock(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSContext *realm = document_realm_of(n);   /* step 1 */
    JSValue set;
    int64_t at;
    uint32_t len, i;

    if (realm == NULL) return;
    set = rb_set(realm);
    at = rb_index(realm, set, n);
    if (at >= 0) {   /* step 2: "Remove el from document's render-blocking element set" */
        len = rb_len(realm, set);
        for (i = (uint32_t)at; i + 1 < len; i++)
            JS_SetPropertyUint32(realm, set, i, JS_GetPropertyUint32(realm, set, i + 1));
        JS_SetPropertyStr(realm, set, "length", JS_NewUint32(realm, len - 1));
    }
    JS_FreeValue(realm, set);
}

/* "A Document document is render-blocked if both of the following are true: document's render-blocking element
   set is non-empty, or document allows adding render-blocking elements; and the current high resolution time
   given document's relevant global object has not exceeded an implementation-defined timeout value."
   This user agent defines no timeout: every member leaves the set through its own completion, a network error
   included, so the second clause never decides. */
bool render_blocking_document_render_blocked(JSContext *ctx)
{
    JSValue set = rb_set(ctx);
    const lxb_dom_node_t *doc = node_of(document_object(ctx));
    uint32_t len = rb_len(ctx, set), i;

    DCHECK(doc != NULL, "§3.1.6's render-blocked was asked of a realm with no Document");
    for (i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, set, i);
        lxb_dom_node_t *m = node_of(v);

        DCHECKF(m != NULL && m->owner_document == lxb_dom_interface_document((lxb_dom_node_t *)doc) &&
                node_is_connected(m),
                "member %u of a render-blocking element set is not a connected element of its document — "
                "§3.1.6 unblocks an element that becomes disconnected, and the removing steps are where that "
                "runs", i);
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, set);
    return len > 0 || render_blocking_allows_adding(doc, render_blocking_parse_position(ctx));
}
