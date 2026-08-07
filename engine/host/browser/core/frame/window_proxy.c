/* WindowProxy — HTML §7.2.5.1, and the reason it is not a formality here.
 *
 * WHAT IT IS. `window`, `self`, `parent`, `top`, `frames[i]`, `opener` and `iframe.contentWindow` are not
 * Window objects. Every one of them is a WindowProxy: an exotic object that FORWARDS to the current active
 * Window of a NAVIGABLE. The distinction exists because a navigable outlives the documents in it — navigate an
 * iframe and its Window is replaced, while the WindowProxy a page is holding stays the same object. HTML says
 * so in one line, and everything else in this file follows from it.
 *
 * WHY IT IS NOT A FORMALITY IN THIS ENGINE. With one navigable and no navigation, a WindowProxy that forwards
 * to the one global is observationally identical to that global, and I had it written down as exactly that —
 * a piece of machinery to build when there was a second window to point at. That is wrong, and TIME TRAVEL is
 * what makes it wrong:
 *
 *   - THE BINDING IS PER-FLOW. One forked arm navigates the frame and its `contentWindow` targets the new
 *     Window; its sibling never navigated and still targets the old one. Both hold the SAME proxy object,
 *     because that is what a proxy is for. So proxy -> Window is per-flow state, and it has to ride the COW
 *     delta like every other piece of shared state a flow writes — which is what cow_capture_host_record is,
 *     and which a global "the window is the global" cannot express at all.
 *   - SO IS THE SAME-ORIGIN CHECK. §7.2.5.1's [[Get]] is filtered by whether the ACCESSOR and the target's
 *     active document are same origin, and the target's origin is whatever the flow navigated it to. A check
 *     against a fixed origin answers for a world the flow is not in.
 *   - AND A FLOW HOLDING ONE MUST BE SUSPENDABLE. A parked flow resumes with its delta, so the proxy it holds
 *     must resolve through that delta rather than through whatever the engine last did. A binding kept
 *     anywhere else is read at resume time from the wrong world.
 *
 * CROSS-INSTANCE. One WASM instance is one document, so a proxy may name a Window in another instance. Then the
 * binding is not a JSValue at all — it is (document, world), and the world is the sending flow's decision
 * vector, because two arms of a fork that navigated a frame differently must not resolve to one Window. That
 * case is NOT built: navigate_to below is the seam, and reaching a proxy whose target is remote crashes naming
 * it rather than silently answering with the local window. */
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/window_proxy.h"
#include "solver/cow.h"
#include "solver/world.h"

/* §7.2.5.1's [[Window]] and the origin its same-origin check reads. BOTH are per-flow — see the file comment —
   which is the whole reason this is a record behind a class rather than a property on an object. */
typedef struct {
    JSValue window;    /* the navigable's active Window, or JS_UNDEFINED when the document is remote (owned) */
    char   *origin;    /* the active document's origin, for §7.2.5.1's same-origin check (owned) */
    /* WHICH DOCUMENT the navigable's active document IS — the same id the world registry names worlds by, so
       "is this remote?" is one comparison against one identity rather than a second naming scheme kept beside
       it. A proxy whose doc is this instance's has a live `window`; any other doc is in a peer instance and
       has none, and those two facts are asserted together because a proxy where they disagree would answer a
       cross-document read with THIS document's Window — the exact failure the same-origin check exists to
       prevent. */
    uint32_t doc;
} ProxyData;

static JSClassID g_proxy_class;
static JSRuntime *g_wp_rt;

#define WP_OFF(f) (uint16_t)offsetof(ProxyData, f)
static const uint16_t PROXY_VALS[] = { WP_OFF(window) };
static const CowRecord PROXY_REC = { sizeof(ProxyData), PROXY_VALS, 1 };

/* THE CAPTURE IS IN THE ACCESSOR, as it is for every other component record: a record a flow has reached is one
   it may write, and there is then no write site to miss. `origin` is a POD pointer and `doc` a POD id inside the
   captured bytes — a navigation REPLACES them rather than mutating what they point at, so the byte copy is a
   complete description of the binding at that moment.
   THE STRINGS ARE THEREFORE NOT FREED ON NAVIGATION. A delta's saved bytes still name the old pointer, and
   freeing it would leave a parked flow resuming onto freed memory. They are owned by the PROXY and released
   with it, which costs one string per navigation and is the only arrangement in which a parked flow's binding
   stays readable. */
static ProxyData *proxy_of(JSValueConst v)
{
    ProxyData *p = JS_GetOpaque(v, g_proxy_class);
    if (p) cow_capture_host_record(v, p, &PROXY_REC);
    return p;
}

bool window_proxy_is(JSValueConst v)
{
    return g_proxy_class != 0 && JS_GetOpaque(v, g_proxy_class) != NULL;
}

/* Every string this proxy has ever held, so a delta that still names an older one resumes onto live memory.
   A list rather than a single slot for the reason the accessor's comment gives. */
typedef struct OwnedStr { char *s; struct OwnedStr *next; } OwnedStr;
static OwnedStr *g_strings;

static char *proxy_strdup(const char *s)
{
    OwnedStr *e;
    char *c;
    if (!s) return NULL;
    c = strdup(s);
    CHECK(c != NULL, "window proxy: OOM recording an origin");
    e = malloc(sizeof *e);
    CHECK(e != NULL, "window proxy: OOM recording an origin");
    e->s = c;
    e->next = g_strings;
    g_strings = e;
    return c;
}

static void proxy_finalizer(JSRuntime *rt, JSValue val)
{
    ProxyData *p = JS_GetOpaque(val, g_proxy_class);
    (void)rt;
    if (!p) return;
    JS_FreeValueRT(rt, p->window);
    free(p);   /* the strings are the component's, released at teardown — see proxy_of */
}

static void proxy_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ProxyData *p = JS_GetOpaque(val, g_proxy_class);
    if (!p) return;
    /* The Window holds the proxy (as `window`) and the proxy holds the Window: a cycle, and one the collector
       has to see or every navigable leaks its global. */
    JS_MarkValue(rt, p->window, mark_func);
}

JSValue window_proxy_new(JSContext *ctx, JSValueConst window, const char *origin)
{
    JSValue obj;
    ProxyData *p;

    DCHECK(g_wp_rt != NULL, "a WindowProxy was minted before window_proxy_init ran");
    obj = JS_NewObjectClass(ctx, g_proxy_class);
    if (JS_IsException(obj)) return obj;
    p = calloc(1, sizeof *p);
    CHECK(p != NULL, "window proxy: OOM building a WindowProxy");
    p->window = JS_DupValue(ctx, window);
    p->origin = proxy_strdup(origin ? origin : "null");
    p->doc = world_local_doc();
    JS_SetOpaque(obj, p);
    return obj;
}

JSValue window_proxy_new_remote(JSContext *ctx, uint32_t doc, const char *origin)
{
    JSValue obj;
    ProxyData *p;

    DCHECK(g_wp_rt != NULL, "a WindowProxy was minted before window_proxy_init ran");
    /* A REMOTE PROXY THAT NAMES THIS DOCUMENT is a local one that lost its Window — it would resolve to
       JS_UNDEFINED for a navigable this instance can actually answer for. */
    DCHECK(doc != 0 && doc != world_local_doc(),
           "a remote WindowProxy was minted for this instance's own document — use window_proxy_new, which "
           "carries the live Window");
    obj = JS_NewObjectClass(ctx, g_proxy_class);
    if (JS_IsException(obj)) return obj;
    p = calloc(1, sizeof *p);
    CHECK(p != NULL, "window proxy: OOM building a remote WindowProxy");
    p->window = JS_UNDEFINED;   /* it lives in another instance; there is nothing local to hold */
    p->origin = proxy_strdup(origin ? origin : "null");
    p->doc = doc;
    JS_SetOpaque(obj, p);
    return obj;
}

bool window_proxy_is_remote(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether it is remote");
    /* THE TWO FACTS MUST AGREE. Read together on every ask, because a proxy where they disagree hands a
       cross-document read this document's Window and nothing downstream can tell. */
    DCHECK((p->doc == world_local_doc()) == !JS_IsUndefined(p->window),
           "a WindowProxy's document id and its Window disagree — a local proxy must carry a live Window and a "
           "remote one must carry none, or a cross-document read is answered by the wrong document");
    return p->doc != world_local_doc();
}

uint32_t window_proxy_doc(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the document of something that is not a WindowProxy was asked for");
    return p->doc;
}

JSValue window_proxy_window(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the Window of something that is not a WindowProxy was asked for");
    /* THE SEAM, and it crashes rather than answering. A proxy whose navigable lives in another WASM instance
       has no local Window to hand back, and handing back this document's would be a cross-document read that
       silently succeeded — the one failure mode the same-origin check exists to prevent. What has to be built
       is named in the message: the binding is (document, world), and resolving it is a host round trip because
       only the host knows which instance holds that document and which of its flows is in that world. */
    DCHECK(p->doc == world_local_doc(),
           "a WindowProxy names a navigable in ANOTHER WASM instance — build the cross-instance resolve: the "
           "flow SUSPENDS here (the same snapshot path as an await), the peer answers in its own scheduled turn "
           "under this flow's world, and the flow resumes with the value. The host owns the routing because "
           "only it knows which instance holds that document — window_proxy_doc names which one");
    return JS_DupValue(ctx, p->window);
}

const char *window_proxy_origin(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the origin of something that is not a WindowProxy was asked for");
    return p->origin;
}

void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSValueConst window, const char *origin)
{
    ProxyData *p = proxy_of(proxy);   /* CAPTURED FIRST: the flow that navigates owns the change, not its siblings */
    DCHECK(p != NULL, "something that is not a WindowProxy was navigated");
    JS_FreeValue(ctx, p->window);
    p->window = JS_DupValue(ctx, window);
    p->origin = proxy_strdup(origin ? origin : "null");
    p->doc = world_local_doc();
}

void window_proxy_init(JSContext *ctx)
{
    JSClassDef d = { "WindowProxy", .finalizer = proxy_finalizer, .gc_mark = proxy_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_wp_rt == NULL || g_wp_rt == rt, "WindowProxy was installed into a second runtime");
    if (g_wp_rt == rt) return;
    g_wp_rt = rt;
    JS_NewClassID(rt, &g_proxy_class);
    JS_NewClass(rt, g_proxy_class, &d);
}

void window_proxy_free(JSContext *ctx)
{
    OwnedStr *e = g_strings;
    (void)ctx;
    if (!g_wp_rt) return;
    while (e) { OwnedStr *n = e->next; free(e->s); free(e); e = n; }
    g_strings = NULL;
    g_wp_rt = NULL;
}
