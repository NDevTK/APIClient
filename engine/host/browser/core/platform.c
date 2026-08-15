/* See platform.h. */
#include <string.h>

#include "check.h"
#include "core/css/media_query_list.h"
#include "core/dom/abort.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/observable.h"
#include "core/encoding/encoding.h"
#include "core/encoding/text_stream.h"
#include "core/events/broadcast_channel.h"
#include "core/events/error_event.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/message_event.h"
#include "core/events/message_port.h"
#include "core/events/report_exception.h"
#include "core/fetch/fetch.h"
#include "core/file/blob.h"
#include "core/file/file_system.h"
#include "core/file/file_system_access.h"
#include "core/file/file_picker.h"
#include "core/file/file_system_handle.h"
#include "core/file/file_system_writable.h"
#include "core/file/storage_manager.h"
#include "core/frame/history.h"
#include "core/frame/location.h"
#include "core/frame/navigable.h"
#include "core/frame/navigator.h"
#include "core/frame/remote_object.h"
#include "core/frame/screen.h"
#include "core/frame/session_history.h"
#include "core/frame/viewport.h"
#include "core/frame/visual_viewport.h"
#include "core/frame/window.h"
#include "core/frame/window_message.h"
#include "core/frame/window_proxy.h"
#include "core/html/form_data.h"
#include "core/html/html_iframe.h"
#include "core/html/unhandled_rejection.h"
#include "core/loader/module_loader.h"
#include "core/platform.h"
#include "core/realm.h"
#include "core/rendering/animation_frame.h"
#include "core/rendering/page_reveal.h"
#include "core/rendering/rendering.h"
#include "core/streams/queuing_strategy.h"
#include "core/streams/readable_stream.h"
#include "core/streams/transform_stream.h"
#include "core/streams/writable_stream.h"
#include "core/structured_clone.h"
#include "core/timing/event_loop.h"
#include "core/timing/timer.h"
#include "core/url/url.h"
#include "core/url/url_search_params.h"
#include "core/xhr/xml_http_request.h"

/* ONE COMPONENT, BOTH HALVES. The thunks below exist because a component's declaration takes exactly the facts
   that component needs and no more — which is right, and is why they cannot share one signature. A thunk is
   the whole of the adaptation, so the table stays a list of components rather than a list of argument
   shuffles, and a component whose facts change breaks its own thunk and nothing else. */
typedef void PlatformDeclare(JSContext *ctx, const PlatformAgent *a);
typedef void PlatformInstall(JSContext *ctx, JSValueConst g, const PlatformDocument *d);

typedef struct {
    const char      *name;      /* the component, as its file is named — every assert below says which */
    PlatformDeclare *declare;   /* once per AGENT, or NULL for a component another one declares */
    PlatformInstall *install;   /* once per REALM, or NULL for a component with no per-document members */
} PlatformComponent;

/* ---- the agent half ------------------------------------------------------------------------------------- */

static void d_url(JSContext *c, const PlatformAgent *a) { (void)a; url_init(c); }
static void d_usp(JSContext *c, const PlatformAgent *a) { (void)a; usp_init(c); }
static void d_form_data(JSContext *c, const PlatformAgent *a) { (void)a; form_data_init(c); }
static void d_readable_stream(JSContext *c, const PlatformAgent *a) { (void)a; readable_stream_init(c); }
static void d_queuing_strategy(JSContext *c, const PlatformAgent *a) { (void)a; queuing_strategy_init(c); }
static void d_writable_stream(JSContext *c, const PlatformAgent *a) { (void)a; writable_stream_init(c); }
static void d_transform_stream(JSContext *c, const PlatformAgent *a) { (void)a; transform_stream_init(c); }
static void d_blob(JSContext *c, const PlatformAgent *a) { (void)a; blob_init(c); }
static void d_encoding(JSContext *c, const PlatformAgent *a) { (void)a; encoding_init(c); }
static void d_text_stream(JSContext *c, const PlatformAgent *a) { (void)a; text_stream_init(c); }
static void d_event_target(JSContext *c, const PlatformAgent *a) { (void)a; event_target_init(c); }
static void d_window(JSContext *c, const PlatformAgent *a) { (void)a; window_init(c); }
static void d_navigator(JSContext *c, const PlatformAgent *a) { (void)a; navigator_init(c); }
static void d_file_system(JSContext *c, const PlatformAgent *a) { (void)a; file_system_init(c); }
static void d_fs_writable(JSContext *c, const PlatformAgent *a) { (void)a; fs_writable_init(c); }
static void d_fs_handle(JSContext *c, const PlatformAgent *a) { (void)a; fs_handle_init(c); }
static void d_storage_manager(JSContext *c, const PlatformAgent *a) { (void)a; storage_manager_init(c); }
static void d_fs_access(JSContext *c, const PlatformAgent *a) { (void)a; file_system_access_init(c); }
static void d_file_picker(JSContext *c, const PlatformAgent *a) { (void)a; file_picker_init(c); }
static void d_event(JSContext *c, const PlatformAgent *a) { (void)a; event_init(c); }
static void d_report_exception(JSContext *c, const PlatformAgent *a) { (void)a; report_exception_init(c); }
static void d_message_port(JSContext *c, const PlatformAgent *a) { (void)a; message_port_init(c); }
static void d_xhr(JSContext *c, const PlatformAgent *a) { (void)a; xhr_init(c); }
static void d_location(JSContext *c, const PlatformAgent *a) { (void)a; location_init(c); }
static void d_session_history(JSContext *c, const PlatformAgent *a) { (void)a; session_history_init(c); }
static void d_history(JSContext *c, const PlatformAgent *a) { (void)a; history_init(c); }
static void d_screen(JSContext *c, const PlatformAgent *a) { (void)a; screen_init(c); }
static void d_navigable(JSContext *c, const PlatformAgent *a) { (void)a; navigable_init(c); }
static void d_event_loop(JSContext *c, const PlatformAgent *a) { (void)a; event_loop_init(c); }
static void d_timer(JSContext *c, const PlatformAgent *a) { (void)a; timer_init(c); }
static void d_window_proxy(JSContext *c, const PlatformAgent *a) { window_proxy_init(c, a->origin); }
static void d_remote_object(JSContext *c, const PlatformAgent *a) { (void)a; remote_object_init(c); }
static void d_window_message(JSContext *c, const PlatformAgent *a) { (void)a; window_message_init(c); }
static void d_broadcast_channel(JSContext *c, const PlatformAgent *a) { broadcast_channel_init(c, a->origin); }
static void d_unhandled_rejection(JSContext *c, const PlatformAgent *a) { (void)a; unhandled_rejection_init(c); }
static void d_animation_frame(JSContext *c, const PlatformAgent *a) { (void)a; animation_frame_init(c); }
static void d_page_reveal(JSContext *c, const PlatformAgent *a) { (void)a; page_reveal_init(c); }
static void d_viewport(JSContext *c, const PlatformAgent *a) { (void)a; viewport_init(c); }
static void d_visual_viewport(JSContext *c, const PlatformAgent *a) { (void)a; visual_viewport_init(c); }
static void d_media_query_list(JSContext *c, const PlatformAgent *a) { (void)a; media_query_list_init(c); }
static void d_rendering(JSContext *c, const PlatformAgent *a) { (void)a; rendering_init(c); }
static void d_fetch(JSContext *c, const PlatformAgent *a) { (void)a; fetch_init(c); }
static void d_abort(JSContext *c, const PlatformAgent *a) { (void)a; abort_init(c); }
static void d_observable(JSContext *c, const PlatformAgent *a) { (void)a; observable_init(c); }
static void d_element(JSContext *c, const PlatformAgent *a) { (void)a; element_init(c); }
static void d_iframe(JSContext *c, const PlatformAgent *a) { (void)a; iframe_init(c); }
static void d_document(JSContext *c, const PlatformAgent *a) { (void)a; document_init(c); }
/* §16.2.1.9's host hook is the RUNTIME's, which is what an agent is. `import(specifier)` with none installed
   resolved to nothing at all — no module, no error, no record — and a page that asks to load code and is
   answered silently is the one shape an unbuilt capability must not have. */
static void d_module_loader(JSContext *c, const PlatformAgent *a) { (void)a; module_loader_install(JS_GetRuntime(c)); }

/* ---- the document half ---------------------------------------------------------------------------------- */

static void i_url(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; url_install(c, g); }
static void i_usp(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; usp_install(c, g); }
static void i_form_data(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; form_data_install(c, g); }
static void i_readable_stream(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; readable_stream_install(c, g); }
static void i_queuing_strategy(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; queuing_strategy_install(c, g); }
static void i_writable_stream(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; writable_stream_install(c, g); }
static void i_transform_stream(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; transform_stream_install(c, g); }
static void i_blob(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; blob_install(c, g); }
static void i_encoding(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; encoding_install(c, g); }
static void i_text_stream(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; text_stream_install(c, g); }
/* THE ADDRESS, NOT THE ORIGIN. `window.origin` is §4.7's serialization OF the address, so a host that handed
   this the origin got a `location`-free Window whose own `origin` was re-derived from a string that was
   already one — and the WPT runner, the one host whose whole job is measuring fidelity, did exactly that. */
static void i_window(JSContext *c, JSValueConst g, const PlatformDocument *d) { window_install(c, g, d->url); }
static void i_event(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; event_install(c, g); }
static void i_message_event(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; message_event_install(c, g); }
static void i_error_event(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; error_event_install(c, g); }
static void i_message_port(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; message_port_install(c, g); }
static void i_xhr(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; xhr_install(c, g); }
static void i_navigable(JSContext *c, JSValueConst g, const PlatformDocument *d) { navigable_install(c, g, d->origin); }
static void i_timer(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; timer_install(c, g); }
static void i_window_message(JSContext *c, JSValueConst g, const PlatformDocument *d) { window_message_install(c, g, d->origin); }
static void i_broadcast_channel(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; broadcast_channel_install(c, g); }
static void i_structured_clone(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; structured_clone_install(c, g); }
static void i_unhandled_rejection(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; unhandled_rejection_install(c, g); }
static void i_animation_frame(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; animation_frame_install(c, g); }
static void i_page_reveal(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; page_reveal_install(c, g); }
static void i_media_query_list(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; media_query_list_install(c, g); }
static void i_fetch(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; fetch_install(c, g); }
static void i_abort(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; abort_install(c, g); }
static void i_observable(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; observable_install(c, g); }
static void i_document(JSContext *c, JSValueConst g, const PlatformDocument *d)
{
    document_install(c, g, d->dom, d->url, d->csp, d->doc_id, d->nav_proxy);
}

/* THE LIST. ORDER IS DEPENDENCY ORDER and it is ONE order for both halves, which is what keeps a component to
   one row: a declaration that has to precede another's (EventTarget before Window, because Window.prototype
   chains to EventTarget.prototype and core/realm.h builds the per-realm prototypes in declaration order) and
   an install that has to follow it (§7.2.2's event handlers, which sit on the global once the chain is made)
   would otherwise be two orders and therefore two lists. They are not: §8.1.7.2's handlers are Window's own
   mixins and window.c installs them, which is where the spec puts them anyway.
   §4.8.5 is why `document` is last: installing it runs the insertion steps for every <iframe> in the markup,
   which CREATES a child navigable — so the browsing context, the WindowProxy class and §7.4's create must all
   already be here. */
static const PlatformComponent PLATFORM[] = {
    { "url",                 d_url,                 i_url },
    { "url_search_params",   d_usp,                 i_usp },
    { "form_data",           d_form_data,           i_form_data },
    { "readable_stream",     d_readable_stream,     i_readable_stream },
    { "queuing_strategy",    d_queuing_strategy,    i_queuing_strategy },
    { "writable_stream",     d_writable_stream,     i_writable_stream },
    { "transform_stream",    d_transform_stream,    i_transform_stream },
    { "blob",                d_blob,                i_blob },
    { "encoding",            d_encoding,            i_encoding },
    { "text_stream",         d_text_stream,         i_text_stream },
    /* §2.7 before §7.2.5, and its per-document half is inside window_install for the reason above. */
    { "event_target",        d_event_target,        NULL },
    { "window",              d_window,              i_window },
    /* §8.10.1's Navigator, and with it Permissions §6 (navigator.permissions), Storage §2 and File System §3
       (navigator.storage) and §6.4.4's UserActivation. This row is the one whose absence from one host's copy
       of the list left four standards uncollected by the gate that reports on them. */
    { "navigator",           d_navigator,           NULL },
    /* THE ONE VIRTUAL FILESYSTEM and the File System Standard over it. The MODEL goes first (its two roots are
       built at this pre-boot baseline, so no flow's creation becomes every sibling's); §2.5's stream after §5's
       WritableStream, whose prototype it chains to; §2.2-§2.4's handles after the stream, since
       `createWritable()` mints one; §3's StorageManager after §8.10.1's Navigator, because `navigator.storage`
       is a partial interface member of the object that component builds. */
    { "file_system",         d_file_system,         NULL },
    { "file_system_writable", d_fs_writable,        NULL },
    { "file_system_handle",  d_fs_handle,           NULL },
    { "storage_manager",     d_storage_manager,     NULL },
    /* FILE SYSTEM ACCESS, a DIFFERENT standard over that same model: §2.2's "file-system" powerful feature and
       §2.3's two members, then §3's three pickers. Both rows come after `navigator` because Permissions §4's
       registry — where the feature's row lives — is declared under it, and after `file_system_handle` because
       §2.3's members install onto the prototype that component builds and core/realm.h runs the per-realm
       installs in declaration order. */
    { "file_system_access",  d_fs_access,           NULL },
    { "file_picker",         d_file_picker,         NULL },
    { "event",               d_event,               i_event },
    /* Declared by the components that own the events they carry; the interface objects are this realm's. */
    { "message_event",       NULL,                  i_message_event },
    { "error_event",         NULL,                  i_error_event },
    { "report_exception",    d_report_exception,    NULL },
    { "message_port",        d_message_port,        i_message_port },
    { "xml_http_request",    d_xhr,                 i_xhr },
    { "location",            d_location,            NULL },
    /* §7.4.1's state machine BEFORE §7.2.5's History, whose every member reads the record it builds. */
    { "session_history",     d_session_history,     NULL },
    { "history",             d_history,             NULL },
    { "screen",              d_screen,              NULL },
    { "navigable",           d_navigable,           i_navigable },
    /* HTML §8.1.7's EVENT LOOP, before the task sources that are ordered by it: the virtual clock, §8.1.7.1's
       last render opportunity time and the insertion order a source breaks its ties by are the LOOP's, and
       they are per-flow heap state, so the record has to exist before any flow can write one. */
    { "event_loop",          d_event_loop,          NULL },
    { "timer",               d_timer,               i_timer },
    { "window_proxy",        d_window_proxy,        NULL },
    { "remote_object",       d_remote_object,       NULL },
    /* AFTER window_proxy: §9.4.4's `postMessage` is installed on the WindowProxy PROTOTYPE. */
    { "window_message",      d_window_message,      i_window_message },
    { "broadcast_channel",   d_broadcast_channel,   i_broadcast_channel },
    { "structured_clone",    NULL,                  i_structured_clone },
    { "unhandled_rejection", d_unhandled_rejection, i_unhandled_rejection },
    /* §8.9's map before §8.1.7.3 step 14 consumes it, and §7.4.6.3's reveal after Event. */
    { "animation_frame",     d_animation_frame,     i_animation_frame },
    { "page_reveal",         d_page_reveal,         i_page_reveal },
    { "viewport",            d_viewport,            NULL },
    { "visual_viewport",     d_visual_viewport,     NULL },
    { "media_query_list",    d_media_query_list,    i_media_query_list },
    /* AFTER the three whose algorithms update-the-rendering's steps 8 and 10 are. */
    { "rendering",           d_rendering,           NULL },
    { "fetch",               d_fetch,               i_fetch },
    { "abort",               d_abort,               i_abort },
    { "observable",          d_observable,          i_observable },
    { "element",             d_element,             NULL },
    { "html_iframe",         d_iframe,              NULL },
    { "document",            d_document,            i_document },
    { "module_loader",       d_module_loader,       NULL },
};
static const int PLATFORM_N = (int)(sizeof PLATFORM / sizeof PLATFORM[0]);

/* THE OTHER SIDE OF THE LIST. A list that is only ever RUN is unfalsifiable: every host goes through it, so
   nothing disagrees with it and a component whose install quietly stops installing anything looks identical to
   one that works. This is the disagreement — the NAME a component puts on a realm's global, asserted present
   once the whole list has run.
 *
 * It is two-sided in the same way core/idl_args.h's conditional-member exclusion is. A name that is absent
 * fires with the component that owed it, which is exactly the report the original defect never produced
 * (`navigator` was missing from the WPT gate's realm and the gate reported numbers for four standards rooted
 * there). And a row whose component is renamed or removed leaves a name nobody installs, which fires the same
 * assert from the other direction — so a witness cannot outlive its component the way a comment does.
 *
 * The four Window members come from realm intrinsics rather than from an install in this file, and asserting
 * them HERE is deliberate: this is the point at which the realm is finished, and whether the intrinsic list
 * and this list agree is precisely the question. */
static const struct { const char *name, *component; } PLATFORM_WITNESS[] = {
    { "window",                "window" },
    { "onload",                "event_target" },
    { "document",              "document" },
    { "navigator",             "navigator" },
    { "location",              "location" },
    { "screen",                "screen" },
    { "history",               "history" },
    { "open",                  "navigable" },
    { "fetch",                 "fetch" },
    { "setTimeout",            "timer" },
    { "postMessage",           "window_message" },
    { "structuredClone",       "structured_clone" },
    { "requestAnimationFrame", "animation_frame" },
    { "matchMedia",            "media_query_list" },
    { "URL",                   "url" },
    { "URLSearchParams",       "url_search_params" },
    { "FormData",              "form_data" },
    { "Blob",                  "blob" },
    { "TextEncoder",           "encoding" },
    { "TextDecoderStream",     "text_stream" },
    { "ReadableStream",        "readable_stream" },
    { "WritableStream",        "writable_stream" },
    { "TransformStream",       "transform_stream" },
    { "CountQueuingStrategy",  "queuing_strategy" },
    { "Event",                 "event" },
    { "MessageEvent",          "message_event" },
    { "ErrorEvent",            "error_event" },
    { "MessageChannel",        "message_port" },
    { "XMLHttpRequest",        "xml_http_request" },
    { "BroadcastChannel",      "broadcast_channel" },
    { "PromiseRejectionEvent", "unhandled_rejection" },
    { "PageRevealEvent",       "page_reveal" },
    { "AbortController",       "abort" },
    { "Observable",            "observable" },
};
static const int PLATFORM_WITNESS_N = (int)(sizeof PLATFORM_WITNESS / sizeof PLATFORM_WITNESS[0]);

/* THE AGENT IS ONE RUNTIME AND THE LIST RUNS ONCE IN IT. Every component's own `_init` already asserts it did
   not run twice; this asserts the same thing about the list, so the report names the LIST rather than whichever
   component happened to be first. */
static JSRuntime *g_declared_in;

static void platform_check_table(void)
{
    int i, k;

    for (i = 0; i < PLATFORM_N; i++) {
        DCHECK(PLATFORM[i].name != NULL && *PLATFORM[i].name,
               "a platform component has no name — every assertion about this list says which component, and "
               "an unnamed row can only report a number");
        DCHECK(PLATFORM[i].declare != NULL || PLATFORM[i].install != NULL,
               "a platform component builds neither an agent half nor a document half — a row that builds "
               "nothing is a component that is not in this browser, and it must be deleted rather than listed");
        for (k = 0; k < i; k++)
            DCHECK(strcmp(PLATFORM[k].name, PLATFORM[i].name) != 0,
                   "a platform component is listed twice — its declaration would mint a second class and a "
                   "second pool entry, and everything already chained to the first would answer out of an "
                   "object the realm has thrown away");
    }
    for (i = 0; i < PLATFORM_WITNESS_N; i++) {
        int found = 0;
        for (k = 0; k < PLATFORM_N; k++)
            if (strcmp(PLATFORM[k].name, PLATFORM_WITNESS[i].component) == 0) { found = 1; break; }
        DCHECK(found, "a platform witness names a component this list does not have — the component was "
                      "renamed or removed and the name it used to install is now owed by nobody, which is the "
                      "state a stale exclusion rots into");
    }
}

void platform_agent_init(JSContext *ctx, const PlatformAgent *agent)
{
    int i;

    DCHECK(ctx != NULL, "the platform was declared into no realm");
    DCHECK(agent != NULL, "the platform was declared with no agent facts");
    DCHECK(agent->origin != NULL && *agent->origin,
           "an agent was brought up with no PRINCIPAL — §9.5's named bus is keyed by it and §7.2.5.1 decides "
           "remoteness by it, so an agent without one cannot answer either question");
    /* realm_install_intrinsics asserts this too, and asserting it HERE as well is the point: the first realm's
       intrinsics are the LAST thing this function does, so a host that passed nothing would otherwise be told
       so from inside a call it did not make, forty declarations after the mistake. */
    DCHECK(agent->top_level_url != NULL && *agent->top_level_url,
           "an agent was brought up with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide whether "
           "the first realm is a SECURE CONTEXT, and Web IDL §3.3.13's members are installed or absent by that "
           "answer, so this agent's platform surface is undecided");
    DCHECK(g_declared_in == NULL || g_declared_in != JS_GetRuntime(ctx),
           "the platform was declared twice in one agent — a declaration is per JSRuntime, and the second one "
           "re-mints every class id the first realm's objects are already branded with");
    platform_check_table();
    g_declared_in = JS_GetRuntime(ctx);
    for (i = 0; i < PLATFORM_N; i++)
        if (PLATFORM[i].declare)
            PLATFORM[i].declare(ctx, agent);
    /* THE AGENT'S FIRST REALM IS A REALM. Every per-realm intrinsic the components above declared is built
       here, through the same one call a child navigable's realm makes — so the first document cannot get a
       different set from the rest, which is the whole failure mode this file and core/realm.h exist to end. */
    realm_install_intrinsics(ctx, agent->top_level_url);
}

void platform_document_install(JSContext *ctx, JSValueConst global, const PlatformDocument *doc)
{
    int i;

    DCHECK(ctx != NULL, "a document was installed into no realm");
    DCHECK(JS_IsObject(global), "a document was installed on something that is not the global object");
    DCHECK(doc != NULL, "a document was installed with no document facts");
    DCHECK(doc->dom != NULL, "a document was installed with no parsed tree — `document` is a wrapper over one");
    DCHECK(doc->url != NULL && *doc->url,
           "a document was installed with no ADDRESS — a document is loaded FROM somewhere, and §4.4's API "
           "base URL is what every relative URL the page builds resolves against");
    DCHECK(doc->origin != NULL && *doc->origin,
           "a document was installed with no PRINCIPAL — every same-origin check compares it");
    /* THE TWO FACTS ARE TWO FIELDS, which is the whole of the fix for them: a host with one field passed
       whichever it had, and the address is the one that decides where `fetch("api/users")` goes. There is no
       assertion comparing them, because a document AT the origin root legitimately has both the same. */
    DCHECK(g_declared_in == JS_GetRuntime(ctx),
           "a document was installed in an agent whose platform was never declared — a per-realm install "
           "builds this realm's copy of a class the declaration mints, so there is nothing here to copy");

    for (i = 0; i < PLATFORM_N; i++)
        if (PLATFORM[i].install)
            PLATFORM[i].install(ctx, global, doc);

#if APICLIENT_DEV
    for (i = 0; i < PLATFORM_WITNESS_N; i++) {
        JSAtom a = JS_NewAtom(ctx, PLATFORM_WITNESS[i].name);
        int has;
        CHECK(a != JS_ATOM_NULL, "a platform witness name could not be interned");
        has = JS_HasProperty(ctx, global, a);
        JS_FreeAtom(ctx, a);
        CHECK(has >= 0, "a platform witness probe threw — [[HasProperty]] over the global runs no page code");
        /* The message is the NAME the component owed: a `@WHY` reading `navigator` is a reader standing at the
           realm that has none, which is the whole report the three copies of this list never produced. */
        DCHECK(has == 1, PLATFORM_WITNESS[i].name);
    }
#endif
}
