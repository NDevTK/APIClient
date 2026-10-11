/* See platform.h. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/canvas/canvas_rendering_context_2d.h"
#include "core/canvas/image_data.h"
#include "core/canvas/path_2d.h"
#include "core/console/console.h"
#include "core/crypto/crypto.h"
#include "core/css/css_keyword_value.h"
#include "core/css/css_math_value.h"
#include "core/css/css_namespace.h"
#include "core/css/css_numeric_value.h"
#include "core/css/css_unit_value.h"
#include "core/css/media_query_list.h"
#include "core/dom/abort.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/observable.h"
#include "core/dom/scroll_events.h"
#include "core/encoding/encoding.h"
#include "core/encoding/text_stream.h"
#include "core/events/broadcast_channel.h"
#include "core/events/event.h"
#include "core/events/input_device_capabilities.h"
#include "core/events/current_event.h"
#include "core/events/event_target.h"
#include "core/events/message_port.h"
#include "core/workers/worker_global_scope.h"
#include "core/events/report_exception.h"
#include "core/fetch/fetch.h"
#include "core/fetch/headers.h"
#include "core/fetch/request.h"
#include "core/fetch/response.h"
#include "core/file/blob.h"
#include "core/file/file_reader.h"
#include "core/file/file_system.h"
#include "core/file/file_system_access.h"
#include "core/file/file_picker.h"
#include "core/file/file_system_handle.h"
#include "core/file/file_system_writable.h"
#include "core/file/storage_manager.h"
#include "core/fonts/font_face.h"
#include "core/frame/agent_cluster.h"
#include "core/frame/browsing_context_group.h"
#include "core/frame/history.h"
#include "core/frame/navigate_event_fire.h"
#include "core/frame/navigation.h"
#include "core/frame/navigation_destination.h"
#include "core/frame/navigation_history_entry.h"
#include "core/frame/location.h"
#include "core/frame/navigable.h"
#include "core/frame/navigator.h"
#include "core/frame/remote_object.h"
#include "core/frame/remote_op.h"
#include "core/frame/remote_location.h"
#include "core/frame/screen.h"
#include "core/frame/session_history.h"
#include "core/frame/viewport.h"
#include "core/frame/visual_viewport.h"
#include "core/frame/window.h"
#include "core/frame/window_message.h"
#include "core/frame/window_proxy.h"
#include "core/geometry/dom_rect.h"
#include "core/intersection_observer/intersection_observer.h"
#include "core/geometry/dom_rect_list.h"
#include "core/html/dom_string_list.h"
#include "core/html/domparser.h"
#include "core/html/form_data.h"
#include "core/html/html_iframe.h"
#include "core/html/simple_dialogs.h"
#include "core/html/unhandled_rejection.h"
#include "core/html/xml_serializer.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_cursor.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_index_handle.h"
#include "core/indexeddb/idb_index_populate.h"
#include "core/indexeddb/idb_get_all.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/indexeddb/idb_object_store.h"
#include "core/indexeddb/idb_open.h"
#include "core/indexeddb/idb_record.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/indexeddb/idb_version_change_event.h"
#include "core/indexeddb/indexed_db.h"
#include "core/cookie_store/cookie_store.h"
#include "core/loader/cookie_jar.h"
#include "core/loader/module_loader.h"
#include "core/platform.h"
#include "core/realm.h"
#include "core/rendering/animation_frame.h"
#include "core/scheduling/idle_callback.h"
#include "core/rendering/page_reveal.h"
#include "core/rendering/rendering.h"
#include "core/resize_observer/resize_observer.h"
#include "core/storage/storage.h"
#include "core/storage/storage_shed.h"
#include "core/storage/window_storage.h"
#include "core/streams/queuing_strategy.h"
#include "core/streams/readable_stream.h"
#include "core/streams/transform_stream.h"
#include "core/streams/writable_stream.h"
#include "core/structured_clone.h"
#include "core/timing/event_loop.h"
#include "core/timing/hr_time.h"
#include "core/timing/performance.h"
#include "core/timing/performance_entry.h"
#include "core/timing/performance_observer.h"
#include "core/timing/user_timing.h"
#include "core/timing/timer.h"
#include "core/url/origin.h"
#include "core/url/url.h"
#include "core/url/url_search_params.h"
#include "core/xhr/xml_http_request.h"

/* The facts, bundled; this file is the only place that can spell them. platform.h takes them as arguments so no
   host can hand over an incomplete value (platform.h says why `= {0}` is not the fix). They are one value here because the component rows below take one, and a row taking
   nine arguments would copy the fact list onto every row. Each is constructed in the entry whose parameter list it
   copies, on the lines below that list, so a fact added to platform.h and not carried here is one function's diff.
   A `PlatformAgent` is built twice, by `platform_agent_init` and `platform_worker_agent_init`, because §8.1.2.2
   allocates a window agent's cluster and a worker agent's by two algorithms with two fact lists (the residual at
   the second); both constructions are in this file, and no other file can spell one. */
typedef struct {
    const char       *origin;          /* the agent's principal */
    const char       *top_level_url;   /* HTML §8.1.3.1's top-level creation URL of the first realm */
    bool              requests_oac;    /* §7.5.1's `requestsOAC` */
    OpenerPolicyValue opener_policy;   /* §7.1.3's opener policy value of the rooting response */
} PlatformAgent;

typedef struct {
    lxb_html_document_t      *dom;             /* the parsed tree this realm's `document` wraps */
    const char               *url;             /* the document's address */
    const char               *origin;          /* the document's principal */
    DocumentKind              kind;            /* DOM §4.5's type and content type, as §7.4.5's arm decided */
    SerializedPolicyContainer policy;          /* HTML §7.1.7's policy container it is created with */
    SerializedResponsePermissionsPolicy
                              permissions_policy;   /* Permissions Policy §9.1's two response header values */
    SandboxFlags              sandbox_flags;   /* HTML §7.1.5's active sandboxing flag set */
    uint32_t                  doc_id;          /* the world registry's name for this document */
    JSValueConst              nav_proxy;       /* §7.2.3's one WindowProxy for its navigable */
    const char               *about_base_url;  /* HTML §7.4's about base URL, or NULL — see platform.h */
    bool                      is_iframe_srcdoc; /* HTML §4.8.5's class for this Document — see platform.h */
} PlatformDocument;

/* One component, both halves. A component's declaration takes exactly the facts it needs, so the declarations
   cannot share one signature; a thunk below is the whole of the adaptation, the table stays a list of components,
   and a component whose facts change breaks its own thunk and nothing else. */
typedef void PlatformDeclare(JSContext *ctx, const PlatformAgent *a);
typedef void PlatformInstall(JSContext *ctx, JSValueConst g, const PlatformDocument *d);
/* The release takes the runtime, because a component on this column holds state for the whole agent, freed
   against the runtime the declaration was made in (this file remembers it, so no host passes anything). A row that
   wanted a JSContext would be a per-realm component, which goes with its realm. */
typedef void PlatformRelease(JSRuntime *rt);

typedef struct {
    const char      *name;      /* the component, as its file is named — every assert below says which */
    PlatformDeclare *declare;   /* once per agent, or NULL for a component another one declares */
    PlatformInstall *install;   /* once per realm, or NULL for a component with no per-document members */
    PlatformRelease *release;   /* once per agent at teardown, or NULL for a component that holds no agent state */
} PlatformComponent;

/* ---- the agent half ------------------------------------------------------------------------------------- */

static void d_console(JSContext *c, const PlatformAgent *a) { (void)a; console_init(c); }
static void d_css_namespace(JSContext *c, const PlatformAgent *a) { (void)a; css_namespace_init(c); }
static void d_css_numeric_value(JSContext *c, const PlatformAgent *a) { (void)a; css_numeric_value_init(c); }
static void d_css_keyword_value(JSContext *c, const PlatformAgent *a) { (void)a; css_keyword_value_init(c); }
static void d_css_math_value(JSContext *c, const PlatformAgent *a) { (void)a; css_math_value_init(c); }
static void d_css_unit_value(JSContext *c, const PlatformAgent *a) { (void)a; css_unit_value_init(c); }
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
static void d_file_reader(JSContext *c, const PlatformAgent *a) { (void)a; file_reader_init(c); }
static void d_fs_writable(JSContext *c, const PlatformAgent *a) { (void)a; fs_writable_init(c); }
static void d_fs_handle(JSContext *c, const PlatformAgent *a) { (void)a; fs_handle_init(c); }
static void d_storage_manager(JSContext *c, const PlatformAgent *a) { (void)a; storage_manager_init(c); }
static void d_fs_access(JSContext *c, const PlatformAgent *a) { (void)a; file_system_access_init(c); }
static void d_file_picker(JSContext *c, const PlatformAgent *a) { (void)a; file_picker_init(c); }
static void d_crypto(JSContext *c, const PlatformAgent *a) { (void)a; crypto_init(c); }
static void d_storage_shed(JSContext *c, const PlatformAgent *a) { (void)a; storage_shed_init(c); }
static void d_storage(JSContext *c, const PlatformAgent *a) { (void)a; storage_init(c); }
static void d_window_storage(JSContext *c, const PlatformAgent *a) { (void)a; window_storage_init(c); }
static void d_idb_key_range(JSContext *c, const PlatformAgent *a) { (void)a; idb_key_range_init(c); }
static void d_idb_record(JSContext *c, const PlatformAgent *a) { (void)a; idb_record_init(c); }
static void d_indexed_db(JSContext *c, const PlatformAgent *a) { (void)a; indexed_db_init(c); }
static void d_idb_database(JSContext *c, const PlatformAgent *a) { (void)a; idb_database_init(c); }
static void d_idb_transaction(JSContext *c, const PlatformAgent *a) { (void)a; idb_transaction_init(c); }
static void d_idb_request(JSContext *c, const PlatformAgent *a) { (void)a; idb_request_init(c); }
static void d_idb_connection(JSContext *c, const PlatformAgent *a) { (void)a; idb_connection_init(c); }
static void d_idb_object_store(JSContext *c, const PlatformAgent *a) { (void)a; idb_object_store_init(c); }
static void d_idb_cursor(JSContext *c, const PlatformAgent *a) { (void)a; idb_cursor_init(c); }
static void d_idb_index_handle(JSContext *c, const PlatformAgent *a) { (void)a; idb_index_handle_init(c); }
static void d_idb_index_populate(JSContext *c, const PlatformAgent *a) { (void)a; idb_index_populate_init(c); }
static void d_idb_get_all(JSContext *c, const PlatformAgent *a) { (void)a; idb_get_all_init(c); }
static void d_idb_vce(JSContext *c, const PlatformAgent *a) { (void)a; idb_version_change_event_init(c); }
static void d_idb_open(JSContext *c, const PlatformAgent *a) { (void)a; idb_open_init(c); }
static void d_hr_time(JSContext *c, const PlatformAgent *a) { (void)a; hr_time_init(c); }
static void d_performance(JSContext *c, const PlatformAgent *a) { (void)a; performance_init(c); }
static void d_performance_entry(JSContext *c, const PlatformAgent *a) { (void)a; performance_entry_init(c); }
static void d_performance_observer(JSContext *c, const PlatformAgent *a) { (void)a; performance_observer_init(c); }
static void d_user_timing(JSContext *c, const PlatformAgent *a) { (void)a; user_timing_init(c); }
static void d_input_device_capabilities(JSContext *c, const PlatformAgent *a)
{ (void)a; input_device_capabilities_init(c); }
static void d_event(JSContext *c, const PlatformAgent *a) { (void)a; event_init(c); }
static void d_current_event(JSContext *c, const PlatformAgent *a) { (void)a; current_event_init(c); }
static void d_report_exception(JSContext *c, const PlatformAgent *a) { (void)a; report_exception_init(c); }
static void d_message_port(JSContext *c, const PlatformAgent *a) { (void)a; message_port_init(c); }
static void d_worker_global_scope(JSContext *c, const PlatformAgent *a)
{ (void)a; worker_global_scope_init(c); }
static void d_xhr(JSContext *c, const PlatformAgent *a) { (void)a; xhr_init(c); }
static void d_location(JSContext *c, const PlatformAgent *a) { (void)a; location_init(c); }
static void d_session_history(JSContext *c, const PlatformAgent *a) { (void)a; session_history_init(c); }
static void d_history(JSContext *c, const PlatformAgent *a) { (void)a; history_init(c); }
static void d_navigation(JSContext *c, const PlatformAgent *a) { (void)a; navigation_init(c); }
static void d_navigate_event_fire(JSContext *c, const PlatformAgent *a) { (void)a; navigate_event_fire_init(c); }
static void d_nav_history_entry(JSContext *c, const PlatformAgent *a) { (void)a; navigation_history_entry_init(c); }
static void d_nav_destination(JSContext *c, const PlatformAgent *a) { (void)a; navigation_destination_init(c); }
static void d_screen(JSContext *c, const PlatformAgent *a) { (void)a; screen_init(c); }
static void d_navigable(JSContext *c, const PlatformAgent *a) { (void)a; navigable_init(c); }
static void d_event_loop(JSContext *c, const PlatformAgent *a) { (void)a; event_loop_init(c); }
static void d_timer(JSContext *c, const PlatformAgent *a) { (void)a; timer_init(c); }
static void d_window_proxy(JSContext *c, const PlatformAgent *a) { (void)a; window_proxy_init(c); }
static void d_remote_object(JSContext *c, const PlatformAgent *a) { (void)a; remote_object_init(c); }
static void d_remote_op(JSContext *c, const PlatformAgent *a) { (void)a; remote_op_init(c); }
static void d_remote_location(JSContext *c, const PlatformAgent *a) { (void)a; remote_location_init(c); }
static void d_window_message(JSContext *c, const PlatformAgent *a) { (void)a; window_message_init(c); }
static void d_broadcast_channel(JSContext *c, const PlatformAgent *a) { (void)a; broadcast_channel_init(c); }
static void d_unhandled_rejection(JSContext *c, const PlatformAgent *a) { (void)a; unhandled_rejection_init(c); }
static void d_animation_frame(JSContext *c, const PlatformAgent *a) { (void)a; animation_frame_init(c); }
static void d_idle_callback(JSContext *c, const PlatformAgent *a) { (void)a; idle_callback_init(c); }
static void d_page_reveal(JSContext *c, const PlatformAgent *a) { (void)a; page_reveal_init(c); }
static void d_viewport(JSContext *c, const PlatformAgent *a) { (void)a; viewport_init(c); }
static void d_visual_viewport(JSContext *c, const PlatformAgent *a) { (void)a; visual_viewport_init(c); }
static void d_scroll_events(JSContext *c, const PlatformAgent *a) { (void)a; scroll_events_init(c); }
static void d_media_query_list(JSContext *c, const PlatformAgent *a) { (void)a; media_query_list_init(c); }
static void d_rendering(JSContext *c, const PlatformAgent *a) { (void)a; rendering_init(c); }
static void d_headers(JSContext *c, const PlatformAgent *a) { (void)a; headers_init(c); }
static void d_response(JSContext *c, const PlatformAgent *a) { (void)a; response_init(c); }
static void d_request(JSContext *c, const PlatformAgent *a) { (void)a; request_init(c); }
static void d_fetch(JSContext *c, const PlatformAgent *a) { (void)a; fetch_init(c); }
static void d_abort(JSContext *c, const PlatformAgent *a) { (void)a; abort_init(c); }
static void d_observable(JSContext *c, const PlatformAgent *a) { (void)a; observable_init(c); }
static void d_dom_rect(JSContext *c, const PlatformAgent *a) { (void)a; dom_rect_init(c); }
static void d_path_2d(JSContext *c, const PlatformAgent *a) { (void)a; path_2d_init(c); }
static void d_image_data(JSContext *c, const PlatformAgent *a) { (void)a; image_data_init(c); }
static void d_canvas_ctx2d(JSContext *c, const PlatformAgent *a)
{ (void)a; canvas_rendering_context_2d_init(c); }
static void d_dom_rect_list(JSContext *c, const PlatformAgent *a) { (void)a; dom_rect_list_init(c); }
static void d_dom_string_list(JSContext *c, const PlatformAgent *a) { (void)a; dom_string_list_init(c); }
static void d_element(JSContext *c, const PlatformAgent *a) { (void)a; element_init(c); }
static void d_intersection_observer(JSContext *c, const PlatformAgent *a) { (void)a; intersection_observer_init(c); }
static void d_resize_observer(JSContext *c, const PlatformAgent *a) { (void)a; resize_observer_init(c); }
static void d_font_face(JSContext *c, const PlatformAgent *a) { (void)a; font_face_init(c); }
static void d_iframe(JSContext *c, const PlatformAgent *a) { (void)a; iframe_init(c); }
static void d_document(JSContext *c, const PlatformAgent *a) { (void)a; document_init(c); }
static void d_cookie_jar(JSContext *c, const PlatformAgent *a) { (void)a; cookie_jar_init(c); }
static void d_cookie_store(JSContext *c, const PlatformAgent *a) { (void)a; cookie_store_init(c); }
static void d_simple_dialogs(JSContext *c, const PlatformAgent *a) { (void)a; simple_dialogs_init(c); }
static void d_domparser(JSContext *c, const PlatformAgent *a) { (void)a; domparser_init(c); }
static void d_xml_serializer(JSContext *c, const PlatformAgent *a) { (void)a; xml_serializer_init(c); }
/* ECMAScript §16.2.1.10 HostLoadImportedModule is the runtime's hook, which is what an agent is. Without it
   `import(specifier)` resolves to nothing, neither a module nor an error. */
static void d_module_loader(JSContext *c, const PlatformAgent *a) { (void)a; module_loader_install(JS_GetRuntime(c)); }

/* ---- the agent half, undone ----------------------------------------------------------------------------- */

static void r_input_device_capabilities(JSRuntime *rt) { input_device_capabilities_free(rt); }
static void r_console(JSRuntime *rt) { (void)rt; console_free(); }
static void r_css_namespace(JSRuntime *rt) { (void)rt; css_namespace_free(); }
static void r_css_numeric_value(JSRuntime *rt) { (void)rt; css_numeric_value_free(); }
static void r_css_keyword_value(JSRuntime *rt) { (void)rt; css_keyword_value_free(); }
static void r_css_math_value(JSRuntime *rt) { css_math_value_free(rt); }
static void r_css_unit_value(JSRuntime *rt) { (void)rt; css_unit_value_free(); }
static void r_hr_time(JSRuntime *rt) { (void)rt; hr_time_free(); }
static void r_performance(JSRuntime *rt) { (void)rt; performance_free(); }
static void r_performance_entry(JSRuntime *rt) { (void)rt; performance_entry_free(); }
static void r_performance_observer(JSRuntime *rt) { performance_observer_free(rt); }
static void r_user_timing(JSRuntime *rt) { (void)rt; user_timing_free(); }
static void r_cookie_jar(JSRuntime *rt) { (void)rt; cookie_jar_free(); }
static void r_cookie_store(JSRuntime *rt) { (void)rt; cookie_store_free(); }
static void r_navigate_event_fire(JSRuntime *rt) { (void)rt; navigate_event_fire_free(); }
/* §8.1.3.3's about-to-be-notified rejected promises list is a live Array a C static holds for the agent, so it is
   agent state and belongs on this column. */
static void r_unhandled_rejection(JSRuntime *rt) { unhandled_rejection_free(rt); }
/* Permissions §3.2's store is two live Arrays, reached only through navigator_free, since Permissions is declared
   from navigator_init. §6.4.4's UserActivation and Screen come with it: same column, same reason. */
static void r_navigator(JSRuntime *rt) { (void)rt; navigator_free(); }
static void r_screen(JSRuntime *rt) { (void)rt; screen_free(); }
static void r_storage_manager(JSRuntime *rt) { (void)rt; storage_manager_free(); }
/* Indexed Database §2.1's set of databases: one live record per storage key, holding every object store and
   every record in them, agent state freed here. */
static void r_idb_database(JSRuntime *rt) { idb_database_free(rt); }
/* Each holds one runtime-lifetime value for the agent, the private Symbol its instances' internal slots hang off,
   and the transaction also holds §2.7.2's live set and §2.7.1's cleanup set. A component that mints a
   runtime-lifetime value owns it and frees it here; per-realm values go with their contexts. */
static void r_idb_transaction(JSRuntime *rt) { idb_transaction_free(rt); }
static void r_idb_request(JSRuntime *rt) { idb_request_free(rt); }
static void r_idb_connection(JSRuntime *rt) { idb_connection_free(rt); }
static void r_idb_object_store(JSRuntime *rt) { idb_object_store_free(rt); }
static void r_idb_cursor(JSRuntime *rt) { idb_cursor_free(rt); }
static void r_idb_index_handle(JSRuntime *rt) { idb_index_handle_free(rt); }
static void r_idb_index_populate(JSRuntime *rt) { idb_index_populate_free(rt); }
static void r_idb_get_all(JSRuntime *rt) { idb_get_all_free(rt); }
static void r_idb_vce(JSRuntime *rt) { idb_version_change_event_free(rt); }
static void r_idb_open(JSRuntime *rt) { idb_open_free(rt); }
/* The one virtual filesystem and the standards over it. §2.1's two root directory entries are agent state
   (storage is keyed by origin and an instance is an origin-keyed agent cluster), and File System Access §3.2.2's
   recently-picked map is one more. Left unreleased, those roots hold the root realm alive from outside the heap,
   so the leak walk reports a whole leaked browser with nothing naming the owner. */
static void r_file_system(JSRuntime *rt) { file_system_free(rt); }
static void r_fs_writable(JSRuntime *rt) { fs_writable_free(rt); }
static void r_fs_handle(JSRuntime *rt) { (void)rt; fs_handle_free(); }
static void r_fs_access(JSRuntime *rt) { (void)rt; file_system_access_free(); }
static void r_file_picker(JSRuntime *rt) { (void)rt; file_picker_free(); }
/* Web Cryptography's two components hold class ids, per-realm slots, a step id and one interned atom (the
   Algorithm dictionary's `name`), all the agent's. An atom is a JSAtomStruct, not a GC object, so an unreleased one
   shows only in JS_FreeRuntime's atom walk. §14's release is reached through §10's, because §10 declares it. */
static void r_crypto(JSRuntime *rt) { (void)rt; crypto_free(); }
/* Storage §4.3's two sheds are the agent's: storage is keyed by origin and an instance is an origin-keyed agent
   cluster, so a same-origin child navigable reaches this shed. §12.2.1's slot Symbol and §12.2.2's per-Document
   holder Symbol are runtime-lifetime values of the same kind. */
static void r_storage_shed(JSRuntime *rt) { storage_shed_free(rt); }
static void r_storage(JSRuntime *rt) { storage_free(rt); }
static void r_window_storage(JSRuntime *rt) { window_storage_free(rt); }
/* The two delivery callees and §9.5's bus. Each component mints one function object for the agent, the task callee
   a queued delivery runs through, and §9.5's registry of open channels is a live Array beside it. */
static void r_window_message(JSRuntime *rt) { window_message_free(rt); }
static void r_broadcast_channel(JSRuntime *rt) { broadcast_channel_free(rt); }
/* XMLHttpRequest holds the agent's step definitions and, through §5's ProgressEvent, the private Symbol that
   interface's slots hang off. */
static void r_xhr(JSRuntime *rt) { xhr_free(rt); }
/* File API §6.2 holds the agent's two step definitions and the four pool entries its read methods are. */
static void r_file_reader(JSRuntime *rt) { file_reader_free(rt); }
/* §7.2.6.10.3's NavigationDestination holds the private Symbol its internal slots hang off. A private Symbol is a
   JSAtomStruct, not on gc_obj_list, so only JS_FreeRuntime's atom walk (which DCHECKs every surviving atom by kind
   and description) sees a release this column forgets. */
static void r_nav_destination(JSRuntime *rt) { navigation_destination_free(rt); }
/* Fetch §5.1 "Headers class", §5.5 "Response class" and §5.4 "Request class". Together they hold three class ids,
   §5.5's per-realm %JSON.stringify% slot, §5.3's two Body-mixin handles, §5.1's six member declarations and its
   `iterable<ByteString, ByteString>` pair handle, and §5.5's clone machine, all agent state declared to
   core/agent_state.h, which is why they are rows: a carried class id would name a class in a runtime that is
   gone. Their finalizers and gc_mark functions reach the record with JS_GetAnyOpaque, since they run after this
   column. None takes a JSContext: the prototypes, interface objects and each realm's %JSON.stringify% go with
   their contexts. They are their own rows, not sub-components of `fetch` (fetch_free gives back only §5.6's
   interned name and its handles), and sit above `fetch` so each is declared by its own row rather than by
   fetch_init's latched call, as `readable_stream` is. */
static void r_headers(JSRuntime *rt) { (void)rt; headers_free(); }
static void r_response(JSRuntime *rt) { (void)rt; response_free(); }
static void r_request(JSRuntime *rt) { (void)rt; request_free(); }
/* §5's four interned field names. */
static void r_fetch(JSRuntime *rt) { fetch_free(rt); }
/* File API §3, §4 and §5, one component and one row on both columns. blob_free reaches file_list_free, so §5's
   declaration (blob_init's last line is file_list_init) and release travel with §3's and §4's, and
   core/file/file_list.c declares its class id under this row's name. The three class ids include the brand every
   BlobPart and BodyInit position checks. It takes the runtime: §8's blob URL store is a value and its two keys are
   atoms, and §5's slot key is one of each, all agent-lifetime, freed with JS_FreeValueRT and JS_FreeAtomRT. */
static void r_blob(JSRuntime *rt) { blob_free(rt); }
/* URL §6.1 "URL class" and §6.2 "URLSearchParams class": two class ids (one the brand Fetch §5.2's BodyInit arm
   reads a URLSearchParams body through), runtime handles and pool indices, with no JSValue or JSAtom, so neither
   takes the runtime. These are the third and fourth rows, so reverse declaration order runs them last but for
   `console` and `hr_time`, after every component that reads either (Fetch's body extractor through usp_list_of,
   §6.1's `searchParams`). No release in this agent calls either: every caller of url_record_of, usp_new,
   usp_reset and usp_list_of outside core/url/ is a member body or a per-realm install. */
static void r_url(JSRuntime *rt) { (void)rt; url_free(); }
static void r_usp(JSRuntime *rt) { (void)rt; usp_free(); }
/* XHR §4 "Interface FormData": a class id (the brand Fetch §5.2's BodyInit arm, XHR §4's body arm and
   NavigateEvent's `formData` read through form_data_is and form_data_class_id), a runtime handle and two pool
   indices, with no JSValue or JSAtom, so it takes nothing; the prototype and interface object are each realm's.
   As the fifth row it releases after every reader of a FormData (core/fetch/body.c's extractor,
   core/html/html_form.c and its FormDataEvent under `element`, core/events/navigate_event.c under
   `navigate_event_fire`) and before only `url_search_params`, `url`, `console` and `hr_time`, none of which names
   it. No release in this agent calls a form_data entry: every caller outside core/html/form_data.c is a member
   body or a per-realm install. */
static void r_form_data(JSRuntime *rt) { (void)rt; form_data_free(); }
/* Encoding §7.2/§7.4 and §7.5/§7.6: four class ids and one per-realm value slot (realm_value_declare is
   JS_NewClassID plus JS_NewClass), read by a finalizer each and by §7.5/§7.6's shared gc_mark, with runtime
   handles and step ids; no JSValue or JSAtom, so neither takes the runtime. §7.5's streams are built on §7.2's
   decoder (text_stream.c calls enc_decoder_new, enc_decoder_decode and enc_decoder_free), so `text_stream` is
   declared below `encoding` and releases first. No release in this agent calls either: text_stream_decode_op's
   other callers are core/fetch/body.c's and core/file/blob.c's step visits, and every encoding entry reached from
   elsewhere reads generated tables rather than a static this release resets. */
static void r_encoding(JSRuntime *rt) { (void)rt; encoding_free(); }
static void r_text_stream(JSRuntime *rt) { (void)rt; text_stream_free(); }
/* DOM §3.1/§3.2 and the Observable standard. Observable depends on abort: §2.2 SubscribeOptions declares
   `AbortSignal signal` (observable.c brands it through abort_signal_class()), and the Observable standard §2.1
   "The Subscriber interface"'s subscription controller is an AbortSignal. So `abort` is declared first and
   reverse declaration order releases observable, the dependent, first. No release in this agent reads abort's or
   observable's statics (every `_free` in the files that use abort's API gives back only its own names, handles
   and runtime), so rows landing between the two are harmless. Each takes the runtime because each frees an
   agent-lifetime value: abort_free §3.2's slot-key Symbol, observable_free an atom and a value of §2's. */
static void r_abort(JSRuntime *rt) { abort_free(rt); }
static void r_observable(JSRuntime *rt) { observable_free(rt); }
/* The streams group: reverse declaration order runs transform_stream, writable_stream, queuing_strategy,
   readable_stream. Together they hold fifteen class ids and thirty-three per-realm value slots, all declared to
   core/agent_state.h; a carried class id doubles as each component's declaration latch, so the next agent's
   `_init` would return early and brand its streams with ids the live runtime never issued. readable_stream
   cascades: core/streams/pipe.c and core/streams/readable_byte_stream.c are initialised and released by
   readable_stream_init and readable_stream_free, declare their state under `readable_stream`, and call
   agent_state_reached at the end of their release so the undo refuses a slot the cascade never reached. None
   takes a JSContext: prototypes and captured operations are the realms'. */
static void r_readable_stream(JSRuntime *rt) { (void)rt; readable_stream_free(); }
static void r_queuing_strategy(JSRuntime *rt) { (void)rt; queuing_strategy_free(); }
static void r_writable_stream(JSRuntime *rt) { (void)rt; writable_stream_free(); }
static void r_transform_stream(JSRuntime *rt) { (void)rt; transform_stream_free(); }
/* The DOM group, whose root is the largest cascade in this browser. element_free reaches dozens of releases:
   node.c's wrapper identity table (a counted reference to every node wrapper, each holding its prototype and so
   the realm), custom_elements' registry backup and active-constructor map, §4.3's pending mutation-record queue,
   the CSSOM group, the selector engine's lexbor arena, the CSS parser, and the private Symbol slot, shadow_root,
   element_internals, html_dialog and html_form hang their internal slots off, all agent state. The cascade frees
   against a JSRuntime, through JS_FreeValueRT/JS_FreeAtomRT. The four rows (iframe, element, dom_rect_list,
   dom_rect) move as a block in that release order.
   `document` has two halves at different phases. document_init mints §4.5's class, declares pool entries,
   realm-value slots and ten sub-components, and claims §13.2.7's document-load lifecycle slot on the frontier; its
   agent half, document_agent_free, releases them. document_free reads `doc_of(ctx)` and is per-realm, reached
   for a child navigable from quickjs's realm-teardown hook inside JS_RunGC or JS_FreeRuntime, after this list.
   That is safe because the per-realm half reads no static of that file and §gc's realm-mark hook is not given
   back here (both asserted at document_agent_free). */
/* The fourth thing a component can hold for an agent: a slot in another component, a claim. A claim is agent state
 * whose storage is in the other component, released by the claimant and asserted by the holder. `timer` and
 * `rendering` claim §8.1.7's timer step and §8.1.7.3's in-parallel half on the one frontier (solver/engine.c,
 * released after the whole platform). `event_target` holds four: §2.9's tree walk (core/dom/node.c), §2.9's
 * activation predicate and behaviour (core/html/hyperlink.c) and HTML §8.1.7.2's handler-set hook
 * (core/events/message_port.c). `structured_clone` holds the transferable registry. Claims are invisible to both of
 * JS_FreeRuntime's censuses (core/agent_state.h says why). Reverse declaration order makes it work: `element`
 * (whose cascade reaches node.c and hyperlink.c) and `message_port` are declared after `event_target`, so their
 * claims are given back before it is released, and its own asserts check that. None of these releases anything a
 * realm owns: prototypes go with their contexts, and what remains is class ids, step ids, interned atoms, private
 * Symbols and claims. */
/* HTML §7.2.4's Location, a claimant row: it holds §7.2.4's brand and two claims in solver/concolic.c's source
   registry, `location.hash` and `location.search`, each with the percent-encode set its component applies to an
   attacker's bytes. concolic_free asserts that registry empty, and platform_agent_free runs before
   solver_agent_free by construction. */
static void r_location(JSRuntime *rt) { (void)rt; location_free(); }
/* HTML §7.4.1's state machine and §7.2.5's History over it. §7.2.5 is a view over §7.4.1's record and every one of
   its members reads it, so the view releases first, as reverse declaration order gives. Between them they hold two
   realm-value ids, §7.2.5's six member declarations and §7.4.3's step definition, declared to core/agent_state.h. */
static void r_session_history(JSRuntime *rt) { (void)rt; session_history_free(); }
static void r_history(JSRuntime *rt) { (void)rt; history_free(); }
/* CSSOM View §4's viewport and §12's VisualViewport over it: §12's members derive from §4's layout viewport, so
   reverse declaration order releases §12 first. */
static void r_viewport(JSRuntime *rt) { (void)rt; viewport_free(); }
static void r_visual_viewport(JSRuntime *rt) { (void)rt; visual_viewport_free(); }
/* CSSOM View §13.2 "Scrolling"'s pending scroll events: no geometry of its own, but §3.1's perform a scroll fills
   it from the viewport's position, so it is the dependent and releases after the pair above. */
static void r_scroll_events(JSRuntime *rt) { (void)rt; scroll_events_free(); }
/* DOM §2.2 "Interface Event" and the thirteen subclasses core/events/event.c declares beside it, sixty-six slots
   declared to core/agent_state.h. None of the fourteen latches on its class id (each latches on `g_ready`), so a
   second agent re-runs every declaration, and a class id carried past its release would hand JS_NewClass a
   number the live runtime never allocated: either a collision with another class (whose per-realm prototype
   slot `JS_SetClassProto` then overwrites) or a sparse id the allocator hands out again. */
static void r_event(JSRuntime *rt) { event_free(rt); }
static void r_current_event(JSRuntime *rt) { current_event_free(rt); }
/* HTML §8.1.4.6 "Runtime script errors": §8.1.4.6 step 6's private Symbol, the declaration latch, §8.1.4.4 step
   8's step-machine registration and step 7.3's console edge, declared to core/agent_state.h. It depends on the
   `event` row (extract_error_information mints §8.1.4.6's ErrorEvent through error_event_new, which event_init
   declares), so it is declared after `event` and releases before it. Its release reads no other component's
   state: it frees one Symbol against the runtime and resets three statics. */
static void r_report_exception(JSRuntime *rt) { report_exception_free(rt); }
static void r_event_target(JSRuntime *rt) { event_target_free(rt); }
static void r_message_port(JSRuntime *rt) { message_port_free(rt); }
static void r_worker_global_scope(JSRuntime *rt) { worker_global_scope_free(rt); }
static void r_timer(JSRuntime *rt) { timer_free(rt); }
/* HTML §8.1.7's own record and §8.12's map keys. Every `timer` entry is due at a moment on §8.1.7's clock, so
   reverse declaration order releases timer first, then event_loop. The record is a live null-prototyped Object a C
   static holds, which JS_FreeRuntime's gc_obj_list walk (asserted empty here) would report if left. */
static void r_event_loop(JSRuntime *rt) { event_loop_free(rt); }
static void r_animation_frame(JSRuntime *rt) { animation_frame_free(rt); }
static void r_idle_callback(JSRuntime *rt) { idle_callback_free(rt); }
static void r_structured_clone(JSRuntime *rt) { structured_clone_free(rt); }
static void r_rendering(JSRuntime *rt) { rendering_free(rt); }
static void r_document(JSRuntime *rt) { document_agent_free(rt); }
static void r_element(JSRuntime *rt) { element_free(rt); }
static void r_intersection_observer(JSRuntime *rt) { intersection_observer_free(rt); }
static void r_resize_observer(JSRuntime *rt) { resize_observer_free(rt); }
static void r_font_face(JSRuntime *rt) { font_face_free(rt); }
static void r_iframe(JSRuntime *rt) { iframe_free(rt); }
static void r_dom_rect_list(JSRuntime *rt) { dom_rect_list_free(rt); }
static void r_dom_string_list(JSRuntime *rt) { dom_string_list_free(rt); }
static void r_dom_rect(JSRuntime *rt) { (void)rt; dom_rect_free(); }
static void r_path_2d(JSRuntime *rt) { (void)rt; path_2d_free(); }
static void r_image_data(JSRuntime *rt) { (void)rt; image_data_free(); }
static void r_canvas_ctx2d(JSRuntime *rt) { canvas_rendering_context_2d_free(rt); }
/* Rows whose releases a pairing check cannot tell from empty ones: a row with no release and a component that
   declared nothing agree whether it holds nothing or everything. These hold Indexed Database §4.7's IDBKeyRange
   (a class and five pool entries), §4.8's IDBRecord (a class), §4.3's IDBFactory (a class, a realm-value slot and
   five declarations), HTML §8.5.1's DOMParser (a class, its latch and two entries), and §16.2.1.10
   HostLoadImportedModule's loader, which holds a strdup'd specifier per lazy chunk and the buffer they are joined
   into, malloc'd and invisible to both of JS_FreeRuntime's censuses. Each declares its slots to
   core/agent_state.h, so the release is checked. */
static void r_idb_key_range(JSRuntime *rt) { (void)rt; idb_key_range_free(); }
static void r_idb_record(JSRuntime *rt) { (void)rt; idb_record_free(); }
static void r_indexed_db(JSRuntime *rt) { (void)rt; indexed_db_free(); }
static void r_domparser(JSRuntime *rt) { (void)rt; domparser_free(); }
static void r_xml_serializer(JSRuntime *rt) { (void)rt; xml_serializer_free(); }
static void r_module_loader(JSRuntime *rt) { module_loader_free(rt); }
/* The cross-agent seam, a group of four. §7.2.3's WindowProxy is the base whose per-realm prototype every reference
   `remote_object` mints chains to, and whose `location` member answers with a `remote_location` object, so it
   releases last: reverse declaration order gives remote_location, remote_op, remote_object, window_proxy. They
   free through JS_FreeValueRT/JS_FreeAtomRT against the runtime. window_proxy and remote_location give back their
   class ids, and window_proxy its five §7.2.3 pool entries, so a second agent cannot install members out of
   another pool's indices; their finalizers and gc_mark functions reach the record through JS_GetAnyOpaque. */
static void r_window_proxy(JSRuntime *rt) { window_proxy_free(rt); }
/* HTML §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's getters for this document's Window, captured before any of its
   script runs: the document half of the row whose declare half is above. A per-document install, not a per-realm
   intrinsic, because the nine accessors it captures do not exist at intrinsic time: `location` does (location.c
   declares one), and the other eight are installed by the `window` row of the column below, which runs after every
   intrinsic. The row's position in that column is the contract, and window_proxy.c's capture asserts it member by
   member. */
static void i_window_proxy(JSContext *c, JSValueConst g, const PlatformDocument *d)
{
    (void)d;
    window_proxy_install_window_getters(c, g);
}
static void r_remote_object(JSRuntime *rt) { remote_object_free(rt); }
static void r_remote_location(JSRuntime *rt) { remote_location_free(rt); }
/* The fourth of that group. Its agent half is `remote_op_agent_free`, named as `document`'s two halves are,
   because `remote_op_free` frees one parsed record and is not an agent release. */
static void r_remote_op(JSRuntime *rt) { (void)rt; remote_op_agent_free(); }
/* HTML §7.2.2's Window with §7.2.2.5's BarProp under it: two class ids, a realm-value slot id and pool entries,
   every one a registration in the runtime, declared to core/agent_state.h under the one name `window`, because a
   sub-component names the row whose release reaches it. The pool entries start and reset at -1, since 0 is the
   first member idl_method_id_all hands out, and bar_prop.c's class id is reset with them. */
static void r_window(JSRuntime *rt) { window_free(rt); }
/* HTML §7.2.6 "The navigation API" and §7.2.6.5 "The NavigationHistoryEntry interface". `navigation_history_entry`
   is the row after `event_target` (its prototype chains to §2.7's, and its class is what §7.2.7.1's `required
   NavigationHistoryEntry from` brands against) and `navigation` the row after `history`, so the component that
   mints the wrappers and walks the list releases before the interface it mints. They hold two class ids, a
   realm-value slot id and three pool entries, freed against the runtime. Each file answers its brand separately
   from its declaration, which it asserts off a recorded runtime: JS_GetClassID answers JS_INVALID_CLASS_ID (0)
   for every non-object, so a brand folded with a zeroed id would admit every primitive. */
static void r_navigation(JSRuntime *rt) { navigation_free(rt); }
static void r_nav_history_entry(JSRuntime *rt) { navigation_history_entry_free(rt); }
/* HTML §7.4.6.3 "Revealing the document" (with §7.2.7.5 "The PageRevealEvent interface") and CSSOM View §4.2 "The
   MediaQueryList Interface". §4.2's answer is a predicate over the viewport (media_query_matches reads §4's
   per-realm environment), so `media_query_list` is declared after `viewport`, and reverse declaration order gives
   media_query_list, visual_viewport, viewport, page_reveal. Neither component reads the other. They hold thirteen
   slots between them, including two class ids, all declared and released, each declaration asserted off a recorded
   runtime separately from any value. mql_finalizer reaches its record with JS_GetAnyOpaque, because the collector
   runs after this column and `JS_GetOpaque(val, 0)` answers NULL, which would leak each MediaQuerySet, its
   serialization and the record. §4.2's brand is JS_GetOpaque2, which throws for a non-instance. page_reveal.c has
   no collector entry and no brand site. Every pool entry and realm-value slot in both files starts at -1. */
static void r_page_reveal(JSRuntime *rt) { page_reveal_free(rt); }
static void r_media_query_list(JSRuntime *rt) { media_query_list_free(rt); }
static void r_simple_dialogs(JSRuntime *rt) { (void)rt; simple_dialogs_free(); }

/* ---- the document half ---------------------------------------------------------------------------------- */

/* The address, not the origin: `window.origin` is §4.7's serialization of the address, so a Window given an origin
   would re-derive its own `origin` from a string that already is one. */
static void i_window(JSContext *c, JSValueConst g, const PlatformDocument *d) { window_install(c, g, d->url); }
static void i_current_event(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; current_event_install(c, g); }
static void i_navigable(JSContext *c, JSValueConst g, const PlatformDocument *d) { navigable_install(c, g, d->origin); }
static void i_timer(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; timer_install(c, g); }
static void i_window_message(JSContext *c, JSValueConst g, const PlatformDocument *d) { window_message_install(c, g, d->origin); }
static void d_structured_clone(JSContext *c, const PlatformAgent *a) { (void)a; structured_clone_init(c); }
static void i_animation_frame(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; animation_frame_install(c, g); }
static void i_idle_callback(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; idle_callback_install(c, g); }
static void i_page_reveal(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; page_reveal_install(c, g); }
static void i_media_query_list(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; media_query_list_install(c, g); }
static void i_simple_dialogs(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; simple_dialogs_install(c, g); }
static void i_fetch(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; fetch_install(c, g); }
static void i_dom_rect_list(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; dom_rect_list_install(c, g); }
static void i_intersection_observer(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; intersection_observer_install(c, g); }
static void i_resize_observer(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; resize_observer_install(c, g); }
static void i_document(JSContext *c, JSValueConst g, const PlatformDocument *d)
{
    document_install(c, g, d->dom, d->url, d->kind, d->policy, d->permissions_policy, d->sandbox_flags,
                     d->doc_id, d->nav_proxy, d->about_base_url, d->is_iframe_srcdoc);
}
static void i_xml_serializer(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; xml_serializer_install(c, g); }

/* The list, in dependency order, one order for both halves, which keeps a component to one row. A declaration that
   precedes another's (EventTarget before Window: Window.prototype chains to EventTarget.prototype, and
   core/realm.h builds the per-realm prototypes in declaration order) and an install that follows it would
   otherwise be two lists. §8.1.8.2's handlers are Window's own mixins (`GlobalEventHandlers` and
   `WindowEventHandlers`, §8.1.8.2.1 "IDL definitions"), and window.c installs them. §4.8.5 is why `document` is
   last: installing it runs the insertion steps for every <iframe> in the markup, which creates a child navigable,
   so the browsing context, the WindowProxy class and §7.3.1.3 "Child navigables"'s "create a new child navigable"
   must already be here.
   A row with no document half is an interface whose exposure set needs no Document: Web IDL §3.8 "Platform objects
   implementing interfaces" defines the global property references given a realm (step 1, "every interface that is
   exposed in realm"), so the component's own realm intrinsic places its interface objects beside the prototypes
   it builds there, and a realm that reaches no platform_document_install still gets them. */
static const PlatformComponent PLATFORM[] = {
    /* HR-TIME §4's time origin is the first field a realm gets. §4 puts it on the environment settings object, "a
       moment early in the initialization" of it; core/realm.h creates that environment with the realm and stamps
       §8.1.3.1's top-level creation URL before running this list, so being first among the intrinsics means no
       later install (DOM §2.5 stamps every Event) reads a timestamp from a realm with no origin. It depends on no
       row: its install reads the event loop's virtual clock, built in the declare pass, which finishes before the
       first realm's intrinsics begin. */
    { "hr_time",             d_hr_time,             NULL,        r_hr_time },
    /* Console §1. §1.4's timer table reads HR-TIME §4's current high resolution time, so it follows `hr_time`;
       no other row touches the console namespace, and every later row may call it. The namespace goes on the
       global through a realm intrinsic, so a child navigable gets its own count map, group stack and timer table. */
    { "console",             d_console,             NULL,        r_console },
    /* URL §6.1 "URL class" and §6.2 "URLSearchParams class", both `[Exposed=*]`: no document half. */
    { "url",                 d_url,                 NULL,        r_url },
    { "url_search_params",   d_usp,                 NULL,        r_usp },
    /* XHR §4 "Interface FormData", `[Exposed=(Window,Worker)]`: no document half. Its component is in core/html/
       because HTML §4.10.22.4 builds the entry list as one; the exposure set is `xhr.idl`'s. */
    { "form_data",           d_form_data,           NULL,        r_form_data },
    /* The Streams Standard's thirteen interfaces (§4.2, §4.4–§4.8, §5.2–§5.4, §6.2, §6.3, §7.2, §7.3), every one
       `[Exposed=*]`: no document half. Thirteen interfaces, four rows: §4.7's and §4.8's belong to
       core/streams/readable_byte_stream.c, which has no row; its prototypes and interface objects come from a
       second realm intrinsic that `readable_stream`'s declare half registers. */
    { "readable_stream",     d_readable_stream,     NULL,        r_readable_stream },
    { "queuing_strategy",    d_queuing_strategy,    NULL,        r_queuing_strategy },
    { "writable_stream",     d_writable_stream,     NULL,        r_writable_stream },
    { "transform_stream",    d_transform_stream,    NULL,        r_transform_stream },
    /* File API §3 "The Blob Interface and Binary Data" and §4 "The File Interface", `[Exposed=(Window,Worker)]`:
       no document half; blob_install_protos places both interface objects. This row also declares §5 "The
       FileList Interface" (blob_init calls file_list_init), whose own realm intrinsic places its interface object. */
    { "blob",                d_blob,                NULL,        r_blob },
    /* Encoding §7.2 TextDecoder, §7.4 TextEncoder, §7.5 TextDecoderStream and §7.6 TextEncoderStream, all
       `[Exposed=*]`: no document half. */
    { "encoding",            d_encoding,            NULL,        r_encoding },
    { "text_stream",         d_text_stream,         NULL,        r_text_stream },
    /* §2.7 before §7.2.5, and its per-document half is inside window_install for the reason above. */
    { "event_target",        d_event_target,        NULL,        r_event_target },
    /* HTML §10.2.1.1 "The WorkerGlobalScope common interface" and §10.2.1.2 "Dedicated workers and the
       DedicatedWorkerGlobalScope interface": the objects a worker realm's global is made of. It follows
       `event_target` because its per-realm install calls event_target_derived_proto. It precedes every component
       that owns a `WorkerGlobalScope` or `WindowOrWorkerGlobalScope` member for `Window` (`performance`, `crypto`,
       `indexed_db`): Web IDL §3.7.3 "Interface prototype object" puts those members on the prototype this row
       builds, and they install through core/idl_args.c, which asks §3.7.3's conditional of the generated band and
       calls the function this row registers, inside the asking component's own realm intrinsic.
       worker_global_scope_proto asserts the order. It reads no other component's state.
       Its document column is empty: a WorkerGlobalScope realm has no Document (HTML §10.2.6.2 "Script settings for
       workers" states its environment instead). It has no witness row, because the witness loop runs only in a
       Window realm, where §3.3.7 [Exposed] step 1 leaves both names unowed; test_forced.c's exposure_selftest,
       which builds a second realm, is its oracle. */
    { "worker_global_scope", d_worker_global_scope, NULL,        r_worker_global_scope },
    /* HR-TIME §7's Performance. `interface Performance : EventTarget`, so its per-realm install calls
       event_target_derived_proto and follows `event_target`; §7.1/§7.2 are operations of §4, which `hr_time`
       owns. §8.1's `performance` is declared by `WindowOrWorkerGlobalScope`, so Web IDL §2.3 "Interface mixins"
       and §3.7.3 place it on the global in a Window realm and on `WorkerGlobalScope.prototype` in a worker one,
       and this row follows `worker_global_scope` too: the dependency is §8.1's, not §7's. The member is per realm
       because this component's realm intrinsic builds the Performance object, so a child navigable gets its own
       over its own time origin. */
    { "performance",         d_performance,         NULL,        r_performance },
    /* Performance Timeline §3's PerformanceEntry, and User Timing §2 over it. §3's prototype chains to
       %Object.prototype% and depends on no row above; User Timing §2.2's `interface PerformanceMark :
       PerformanceEntry` chains to §3's, so `user_timing` follows `performance_entry`, and §2.1.1's mark() is a
       partial member installed onto HR-TIME §7's prototype, so it follows `performance` too. core/realm.h runs
       realm intrinsics in this declaration order, so both hold for a child navigable's realm. */
    { "performance_entry",   d_performance_entry,   NULL,        r_performance_entry },
    /* Performance Timeline §4's PerformanceObserver and §4.2.2's PerformanceObserverEntryList,
       `[Exposed=(Window,Worker)]`: no document half. It reads §3's record (`entryType` and `name`) at §5.1 step 3
       and §5.5, so it follows `performance_entry`; User Timing §2.1.1 step 2 calls §5.1 and user_timing_init
       declares `mark` as a §4.5 supported entry type, so `user_timing` follows this. Its prototype chains to
       %Object.prototype%, so it adds no constraint of its own. */
    { "performance_observer", d_performance_observer, NULL,      r_performance_observer },
    { "user_timing",         d_user_timing,         NULL,        r_user_timing },
    /* HTML §7.2.6.5's NavigationHistoryEntry: its prototype chains to §2.7's, and its class is what §7.2.7.1's
       `required NavigationHistoryEntry from` brands against, so it is declared before `event`, where every Event
       subclass is declared. */
    { "navigation_history_entry", d_nav_history_entry, NULL, r_nav_history_entry },
    /* HTML §7.2.6.10.3's NavigationDestination: its class is what §7.2.6.10.1's `required NavigationDestination
       destination` brands against, so it is declared before `event`, where NavigateEvent is declared. It inherits
       nothing. */
    { "navigation_destination", d_nav_destination,   NULL,     r_nav_destination },
    { "window",              d_window,              i_window,    r_window },
    /* Web Cryptography §10's Crypto and §14's SubtleCrypto, one row because §10 declares §14 as its own dependency,
       so `crypto.subtle` is never another realm's object or missing. No document half: §3.7 gives every realm its
       own interface prototype, and `crypto` and `crypto.subtle` are `[SameObject]` per realm, so both install
       through core/realm.h.
       It precedes `navigator` as a dependency, not a grouping: Web Locks §3.1 is declared under `navigator`, and
       §3.2.2's clientId is drawn at install from this realm's §10.1 stream, so `lock_manager_install_realm` reads
       the per-realm value `crypto_install_realm` writes, and core/realm.h runs per-realm installs in declaration
       order. core/realm.h does not refuse an order in which an install reads a later-declared writer's value, so
       this position is the only thing that keeps the slot written before it is read. */
    { "crypto",              d_crypto,              NULL,        r_crypto },
    /* §8.10.1's Navigator, and with it Permissions §6 (navigator.permissions), Storage §2 and File System §3
       (navigator.storage) and §6.4.4's UserActivation. */
    { "navigator",           d_navigator,           NULL,        r_navigator },
    /* The one virtual filesystem and the File System Standard over it. The model goes first (its two roots are
       built at the pre-boot baseline, so no flow's creation becomes every sibling's); §2.5's stream after §5's
       WritableStream, whose prototype it chains to; §2.2–§2.4's handles after the stream, since
       `createWritable()` mints one; §3's StorageManager after §8.10.1's Navigator, because `navigator.storage` is
       a partial interface member of the object that component builds. */
    { "file_system",         d_file_system,         NULL,        r_file_system },
    { "file_system_writable", d_fs_writable,        NULL,        r_fs_writable },
    { "file_system_handle",  d_fs_handle,           NULL,        r_fs_handle },
    { "storage_manager",     d_storage_manager,     NULL,        r_storage_manager },
    /* File System Access, a different standard over the same model: §2.2's "file-system" powerful feature and
       §2.3's two members, then §3's three pickers. Both rows follow `navigator`, because Permissions §4's registry,
       where the feature's row lives, is declared under it, and `file_system_handle`, because §2.3's members
       install onto the prototype that component builds. */
    { "file_system_access",  d_fs_access,           NULL,        r_fs_access },
    { "file_picker",         d_file_picker,         NULL,        r_file_picker },
    /* Web Storage: Storage §4's model, then HTML §12.2.1's interface over it, then §12.2.2's and §12.2.3's two
       Window getters, each reading the one before. The model's shed, shelf, bucket and bottles are built at the
       pre-boot COW baseline, so every flow shares one map object and each flow's writes are captured; built
       lazily, they would be created inside whichever flow read first, and each arm of a fork would get a private
       localStorage. The last two rows have no document half: §12.2.1's prototype and interface object are per
       realm (§3.7), and the getters go on each realm's own global. */
    { "storage_shed",        d_storage_shed,        NULL,        r_storage_shed },
    { "storage",             d_storage,             NULL,        r_storage },
    { "window_storage",      d_window_storage,      NULL,        r_window_storage },
    /* HTML §2.6.5 "The DOMStringList interface", `[Exposed=(Window,Worker)]`: no document half. It precedes its
       consumers (Indexed Database §4.4, §4.5 and §4.10 each answer with one; HTML §7.2.5's `ancestorOrigins` is
       another), so its prototype is in a realm before any member builds a list there. The three §2.6.4
       collections above it are `[Exposed=Window]` and keep their per-document column. */
    { "dom_string_list",     d_dom_string_list,     NULL,        r_dom_string_list },
    /* Indexed Database, in the standard's dependency order. §2.4's key orders §2.2's list of records, bounds
       §2.9's range and orders §2.10's cursor walk; §4.7's IDBKeyRange is the interface over it, and §4.3's
       IDBFactory is the door to the rest. Neither row has a document half: §3.7 gives every realm its own
       interface prototype, and the IDBFactory `indexedDB` answers with is `[SameObject]` per realm.
       Indexed Database §2.1's database set is an agent row with a release: "each storage key has an associated set of databases",
       an instance is an origin-keyed agent cluster and storage is keyed by origin, so a same-origin child
       navigable reaches the agent's set, freed against the runtime here (core/platform.h's third column). */
    { "idb_key_range",       d_idb_key_range,       NULL,        r_idb_key_range },
    /* §2.12's record snapshot with §4.8's IDBRecord over it: a value type installing one interface per realm,
       holding a class, which is agent-lifetime state. It precedes every row that mints one (§6.2's and §6.3's
       retrieve-multiple arms, reached from §4.5's and §4.6's members): a snapshot built in a realm with no
       IDBRecord.prototype crashes at the mint. */
    { "idb_record",          d_idb_record,          NULL,        r_idb_record },
    { "indexed_db",          d_indexed_db,          NULL,        r_indexed_db },
    { "idb_database",        d_idb_database,        NULL,        r_idb_database },
    /* §2.7's transaction and §2.8's request, in that order because a request is placed against a transaction
       and §5.6 asserts its state before it makes one. Both follow §2.7's EventTarget, since each interface
       prototype chains to it. Neither has a document half: §3.7 gives every realm its own interface prototype
       and a C member answers in the realm that defined it. */
    { "idb_transaction",     d_idb_transaction,     NULL,        r_idb_transaction },
    { "idb_request",         d_idb_request,         NULL,        r_idb_request },
    /* §2.1.1's connection with §4.4's IDBDatabase over it, and §2.2.1's object store handle with §4.5's
       IDBObjectStore. Both follow §2.7's EventTarget (IDBDatabase inherits it) and the two rows above, which
       reach them. Each holds one agent-lifetime value, the private Symbol its internal slots hang off. */
    { "idb_connection",      d_idb_connection,      NULL,        r_idb_connection },
    { "idb_object_store",    d_idb_object_store,    NULL,        r_idb_object_store },
    /* §2.6.1's index handle with §4.6's IDBIndex, after §2.2.1's because §4.6's `objectStore` answers with one
       and §4.5's `createIndex` returns one. It holds one agent-lifetime value, the private Symbol its slots hang
       off. */
    { "idb_index_handle",    d_idb_index_handle,    NULL,        r_idb_index_handle },
    /* §2.10's cursor with §4.9's IDBCursor and IDBCursorWithValue over it, after §2.2.1's and §2.6.1's handles
       because a cursor's source handle is one of those and §4.9's `source` answers with it. It holds the private
       Symbol §2.10's eleven fields hang off and §6.7's step machine. */
    { "idb_cursor",          d_idb_cursor,          NULL,        r_idb_cursor },
    /* Indexed Database §4.5's createIndex note, as the request it says it is: "the index creation itself is processed as an
       asynchronous request within the upgrade transaction". No interface and no realm install: it declares one
       step machine, agent-lifetime state the release gives back. It follows the rows above because §4.5's member
       reaches it and it places its request through §5.6's. */
    { "idb_index_populate",  d_idb_index_populate,  NULL,        r_idb_index_populate },
    /* §5.12's creating a request to retrieve multiple items, with §6.2's and §6.3's retrieve-multiple under it.
       No interface and no realm install: it declares one step machine, which the release gives back. It follows
       §2.2.1's and §2.6.1's handles, whose §4.5 and §4.6 members reach it, and §2.12's IDBRecord, which its
       "record" arm mints. */
    { "idb_get_all",         d_idb_get_all,         NULL,        r_idb_get_all },
    /* Input Device Capabilities §"The InputDeviceCapabilities interface", one row ahead of `event`. §"Extensions
       to the UIEvent interface and UIEventInit dictionary" makes it the type of UIEventInit's
       `sourceCapabilities`, and `event` declares UIEvent and its three subclasses, each of which brands that
       member against this class at its declaration, so a later row would hand all four a class id of zero. Its
       prototype chains to Object.prototype and its two attributes read its own slot record. */
    { "input_device_capabilities", d_input_device_capabilities, NULL,
                                                          r_input_device_capabilities },
    { "event",               d_event,               NULL,        r_event },
    /* DOM §2.3 "Legacy extensions to the Window interface"'s `event`, a row of its own because the member is
       DOM's, not HTML §7.2.2's (@webref/idl carries `[Replaceable] readonly attribute (Event or undefined) event`
       in `dom.idl` only). It follows `event` because the value it holds is an Event, though its install builds
       only §2.3's initial `undefined`. Its one writer is DOM §2.9's inner invoke (core/events/event_target.c steps
       2.8.1, 2.8.2 and 2.13). */
    { "current_event",       d_current_event,       i_current_event, r_current_event },
    /* §4.2's IDBVersionChangeEvent, here rather than beside the other Indexed Database rows because its prototype
       chains to Event.prototype, which the row above builds. §5.1's machinery follows it, because §5.1 fires three
       of these events and registers the two rendezvous idb_connection and idb_transaction declare. */
    { "idb_version_change_event", d_idb_vce,        NULL,        r_idb_vce },
    { "idb_open",            d_idb_open,            NULL,        r_idb_open },
    /* `message_event` and `error_event` have no rows: the `event` row declares both (event_init calls their inits),
       and their interface objects (HTML §9.1 "The MessageEvent interface", §8.1.4.6 "Runtime script errors") are
       placed by their own realm intrinsics, so a row would have every column NULL, which platform_check_table
       refuses. Their PLATFORM_WITNESS rows are under `event`. */
    { "report_exception",    d_report_exception,    NULL,        r_report_exception },
    { "message_port",        d_message_port,        NULL,        r_message_port },
    /* XHR §3 "Interface XMLHttpRequest" (`XMLHttpRequestEventTarget`, `XMLHttpRequestUpload` and `XMLHttpRequest`,
       `[Exposed=(Window,DedicatedWorker,SharedWorker)]`) and §5 "Interface ProgressEvent"
       (`[Exposed=(Window,Worker)]`): no document half. XHR §5's interface object comes from progress_event.c's own
       intrinsic. XHR's per-document state (§3.5.1's API base URL, §3.5.4's credentials mode) is read from the
       running realm when a member is called, never when the interface object is minted. */
    { "xml_http_request",    d_xhr,                 NULL,        r_xhr },
    /* File API §6 "Reading Data", after `xml_http_request`: §6.4.1 "Event Summary" makes every event it fires a
       ProgressEvent, whose class and per-realm prototype `xhr_init` declares, and it reads §3's Blob, which `blob`
       declares. §6.2 "The FileReader API" is `[Exposed=(Window,Worker)]`: no document half. §6.5.1 "The
       FileReaderSync API" is absent, as core/file/file_reader.c's banner states. */
    { "file_reader",         d_file_reader,         NULL,        r_file_reader },
    { "location",            d_location,            NULL,        r_location },
    /* §7.4.1's state machine BEFORE §7.2.5's History, whose every member reads the record it builds. */
    { "session_history",     d_session_history,     NULL,        r_session_history },
    { "history",             d_history,             NULL,        r_history },
    /* HTML §7.2.6's navigation API, AFTER §7.4.1's state machine whose entries it is a view over. */
    { "navigation",          d_navigation,          NULL,        r_navigation },
    /* HTML §7.2.6.10.4, which fires the navigate event at the Navigation the row above builds. It declares the
       commit handler job's step definition, which the runtime must know before any navigation enqueues one, and
       installs no member: the interfaces it builds objects of are `navigation_destination` and NavigateEvent
       under `event`. */
    { "navigate_event_fire", d_navigate_event_fire, NULL,        r_navigate_event_fire },
    { "screen",              d_screen,              NULL,        r_screen },
    { "navigable",           d_navigable,           i_navigable },
    /* HTML §8.1.7's event loop, before the task sources it orders: the virtual clock, §8.1.7.1's last render
       opportunity time and the insertion order a source breaks ties by are the loop's per-flow heap state, so the
       record exists before any flow can write one. */
    { "event_loop",          d_event_loop,          NULL,        r_event_loop },
    { "timer",               d_timer,               i_timer,     r_timer },
    /* Its install column follows `window` and `location`, which §7.2.1.3.4's capture depends on: the nine
       cross-origin accessor members it takes the getters of are installed by those two rows, and a capture before
       either would take a hole for a member and leave a peer answering it from whatever the page later writes onto
       its global. Eight come from `window`; `location` is a realm intrinsic (location.c), present before this
       column begins. The capture's DCHECK names the member if either stops being true. */
    { "window_proxy",        d_window_proxy,        i_window_proxy, r_window_proxy },
    { "remote_object",       d_remote_object,       NULL,        r_remote_object },
    /* The peer's half of the same seam: what this agent does when asked to perform one. Its per-realm install
       captures %Reflect.set%/%Reflect.apply%, so it declares before any component whose install could run page
       code; none here does, so it sits beside the asking half. */
    { "remote_op",           d_remote_op,           NULL,        r_remote_op },
    /* HTML §7.2.4's cross-origin Location, after window_proxy: §7.2.2's `location` member answers with one, and
       this row builds the per-realm surface that member hands across an origin boundary. */
    { "remote_location",     d_remote_location,     NULL,        r_remote_location },
    /* After window_proxy: §9.4.4's `postMessage` is installed on the WindowProxy prototype. */
    { "window_message",      d_window_message,      i_window_message, r_window_message },
    /* HTML §9.5 "Broadcasting to other browsing contexts": html.idl declares `[Exposed=(Window,Worker)] interface
       BroadcastChannel : EventTarget`, so no document half. */
    { "broadcast_channel",   d_broadcast_channel,   NULL,        r_broadcast_channel },
    /* HTML §2.7.10 "Structured cloning API"'s member has no install column because of Web IDL §3.7.3 "Interface
       prototype object": it is declared on a mixin both `WorkerGlobalScope` and `Window` include, so the object it
       lands on differs per realm, and this column (whose DCHECK below refuses a realm whose §3.3.8 [Global] names
       are not `Window`) could serve only one. The component registers a per-realm intrinsic, as `crypto`,
       `indexed_db` and `performance` do for their §8.2 members. */
    { "structured_clone",    d_structured_clone,    NULL,        r_structured_clone },
    /* HTML §8.1.4.7 "Unhandled promise rejections" declares `PromiseRejectionEvent` `[Exposed=*]`, so Web IDL
       §3.3.7 [Exposed] step 1 returns before it looks at the realm, and every realm owes the name: no document
       half. unhandled_rejection.c's realm intrinsic also builds the two rejection drivers. */
    { "unhandled_rejection", d_unhandled_rejection, NULL,        r_unhandled_rejection },
    /* §8.12 "Animation frames"'s map before §8.1.7.3 step 14 consumes it, and §7.4.6.3's reveal after Event. */
    { "animation_frame",     d_animation_frame,     i_animation_frame, r_animation_frame },
    /* Cooperative Scheduling of Background Tasks §4 and §5, beside the row above because it is the same shape (a
       Window's list of registered callbacks drained by a rung of the event loop) and neither constrains the
       other: §4's concepts and §4.3's prototype chain to nothing this list builds, and §5's rung is registered on
       the scheduler. One row, two interfaces: §4.3's IdleDeadline is declared and installed from this component
       (its only producer is §5.2 step 3.2), so both witness names map here. */
    { "idle_callback",       d_idle_callback,       i_idle_callback, r_idle_callback },
    { "page_reveal",         d_page_reveal,         i_page_reveal, r_page_reveal },
    { "viewport",            d_viewport,            NULL,        r_viewport },
    { "visual_viewport",     d_visual_viewport,     NULL,        r_visual_viewport },
    /* CSSOM View §13.2 "Scrolling"'s pending scroll events, before `rendering`: update-the-rendering step 9 drains
       the list this row declares, and step 4's unnecessary-rendering test asks whether a document has one queued.
       It follows `event` because its step 2 fires one. */
    { "scroll_events",       d_scroll_events,       NULL,        r_scroll_events },
    { "media_query_list",    d_media_query_list,    i_media_query_list, r_media_query_list },
    /* After the three whose algorithms update-the-rendering's steps 8 and 10 are. */
    { "rendering",           d_rendering,           NULL,        r_rendering },
    /* DOM §3.1 "Interface AbortController" and §3.2 "Interface AbortSignal", ahead of `request`: Fetch §5.4's
       RequestInit declares `AbortSignal? signal`, and a declared interface type states the class it brands against
       at the declaration, so a later row would hand it a class id of zero. abort_init allocates a symbol and two
       classes, and its per-realm install chains AbortSignal.prototype to EventTarget.prototype. Both are
       `[Exposed=*]`: no document half. abort_install_protos mints both interface objects with §3.2's three
       statics, `abort`, `timeout` and `any`, which are members of that object. */
    { "abort",               d_abort,               NULL,        r_abort },
    /* Fetch §5.4 "Request class", §5.5 "Response class" and §5.1 "Headers class", above `fetch`, so the release
       runs headers, response, request. `request` follows `abort`: request_init states the class §5.4's `signal`
       brands against at the declaration, reading abort_signal_class(). All three are `[Exposed=(Window,Worker)]`:
       no document half. fetch_init calls all three inits too, because §5.6 needs §5.4's dictionary table and
       §5.3's stream reader; each init opens on its own runtime latch, so here they are no-ops, as with
       `readable_stream`. */
    { "request",             d_request,             NULL,        r_request },
    { "response",            d_response,            NULL,        r_response },
    { "headers",             d_headers,             NULL,        r_headers },
    /* The document half here installs one member and no interface object. §5.6 "Fetch methods" declares `fetch` on
       `partial interface mixin WindowOrWorkerGlobalScope`: a member, placed by Web IDL §3.7.7 "Operations" over a
       [Global] object's own interface chain rather than a §3.8 property reference, and browser/idl_exposure.h
       answers for it from IDL_MEMBER_EXPOSURE, not IDL_EXPOSURE. A worker realm is owed it and does not get it
       (core/workers/worker_global_scope.c's residual (7) names it). Removing this column would remove the member
       from every realm, which the PLATFORM_WITNESS row `{ "fetch", "fetch" }` catches. */
    { "fetch",               d_fetch,               i_fetch,     r_fetch },
    /* Observable §2.1 "The Subscriber interface" and §2.2 "The Observable interface", both `[Exposed=*]`: no
       document half. observable_install_protos mints both interface objects with §2.3.1 "from()"'s static, which
       is a member of §2.2's interface object. */
    { "observable",          d_observable,          NULL,        r_observable },
    /* Geometry Interfaces §3 and §4, before the component that returns one. A rectangle is four numbers, so only the
       consumer decides the position: CSSOM View §6's `getBoundingClientRect`, installed on Element.prototype by the
       row below, mints a DOMRect out of the element's own realm. §3 "The DOMRect interfaces" declares
       DOMRectReadOnly and DOMRect `[Exposed=(Window,Worker)]`, so dom_rect.c's realm intrinsic places them, with
       §3's `LegacyWindowAlias=SVGRect`. §4 "The DOMRectList interface" is `[Exposed=Window]`, so it keeps its
       per-document column. browser/idl_exposure.h states each exposure set from the corpus. */
    { "dom_rect",            d_dom_rect,            NULL,        r_dom_rect },
    { "dom_rect_list",       d_dom_rect_list,       i_dom_rect_list, r_dom_rect_list },
    /* HTML §4.12.5.1.7 "Path2D objects", with §4.12.5.1.6 "Building paths"'s CanvasPath mixin. §4.12.5.1.7
       declares `Path2D` `[Exposed=(Window,Worker)]`: no document half. It inherits from no interface
       (browser/idl_inheritance.h records IDL_PROTO_OBJECT), so its position is free; it sits beside the geometry
       rows because a path is coordinates. */
    { "path_2d",             d_path_2d,             NULL,        r_path_2d },
    /* HTML §8.11.1 "The ImageData interface", `[Exposed=(Window,Worker)]`: no document half. It inherits from no
       interface (IDL_PROTO_OBJECT), so its position is free; it sits with the canvas rows. */
    { "image_data",          d_image_data,          NULL,        r_image_data },
    /* HTML §4.12.5.1 "The 2D rendering context", after `image_data`: §4.12.5.1.16's `getImageData` mints an
       ImageData through core/canvas/image_data.h, which reads the class that row declares. `getContext` is a
       member on HTMLCanvasElement, declared with the element interfaces, so this row installs only a realm
       intrinsic; the interface is `[Exposed=Window]`, and its name is placed from the realm as Path2D's is. */
    { "canvas_ctx2d",        d_canvas_ctx2d,        NULL,        r_canvas_ctx2d },
    { "element",             d_element,             NULL,        r_element },
    /* CSSOM §8.1 "The CSS.escape() Method"'s `CSS` namespace and CSS Conditional Rules 3 §7.5 "The CSS namespace"'s
       partial namespace on it (`css_namespace`, below), after `element`, whose cssom_init creates the agent's one
       CSS parser that both members reach (§7.5's supports() runs a real declaration parse and `<supports-condition>`
       evaluation). It is its own row because it installs a global, which several standards extend, and so it has
       a witness. The namespace goes on through a realm intrinsic, so a child navigable gets its own object. */
    /* CSS Typed OM 1's value objects (§2's CSSStyleValue, §4.3.1's CSSNumericValue, §4.3.3's CSSUnitValue) are
       the `css_unit_value` row, declared before `css_namespace`: §4.3.5's sixty-three numeric factories are
       members of the CSS namespace and each returns an object of a class that row declares. It installs three
       globals of its own, which browser/platform_names.h lists, through a realm intrinsic. */
    /* §4.3.1's member declarations, before the row that installs them. It owns no realm state (the prototype its
       two members land on is `css_unit_value`'s, per §3.7.3 "Interface prototype object") and declares two
       argument-pool entries; `css_numeric_value_member_id` aborts on a read of an undeclared id, so a reordering
       here crashes rather than installing a member under another component's pool entry. */
    { "css_numeric_value",   d_css_numeric_value,   NULL,        r_css_numeric_value },
    /* §4.3.4's math values, before the row that installs them: it owns no realm state (its seven interface
       prototype objects and CSSNumericArray's are built from `css_unit_value`'s realm intrinsic) and declares
       eight class ids and six argument-pool entries, and an undeclared read aborts. It follows `css_numeric_value`
       because its constructors brand their `CSSNumberish` arm against that component's predicate. */
    { "css_math_value",      d_css_math_value,      NULL,        r_css_math_value },
    /* §4.2's CSSKeywordValue, before the row that installs it: it owns no realm state (its prototype is built from
       `css_unit_value`'s realm intrinsic) and declares one class id and two argument-pool entries, and an
       undeclared read aborts. */
    { "css_keyword_value",   d_css_keyword_value,   NULL,        r_css_keyword_value },
    { "css_unit_value",      d_css_unit_value,      NULL,        r_css_unit_value },
    { "css_namespace",       d_css_namespace,       NULL,        r_css_namespace },
    /* Intersection Observer, after `element` and the geometry rows. Its declaration brands `observe(Element
       target)` and §2.4's `(Element or Document)? root` against the Node class, which element_init creates
       (through node_init), and every entry it queues carries three DOMRects minted in the observed element's realm.
       Its prototype chains to Object.prototype. */
    { "intersection_observer", d_intersection_observer, i_intersection_observer, r_intersection_observer },
    /* Resize Observer, after `element`: its declaration brands `observe(Element target)` and `unobserve(Element
       target)` against the Node class and narrows both to `element_is`, so an earlier row would hand them a class
       id of zero. §3.4.4 step 6's DOMRectReadOnly and §3.4.6's ErrorEvent are minted at run time, so neither
       `dom_rect` nor `event` constrains this row. One row, three interfaces: §2.3's ResizeObserverEntry and
       ResizeObserverSize are declared and installed from this component, so all three witness names map here.
       ResizeObserver.prototype and §3.2.1's [[resizeObservers]] go on through realm_declare_intrinsic. */
    { "resize_observer",     d_resize_observer,     i_resize_observer, r_resize_observer },
    /* CSS Font Loading §2's FontFace. It declares no base interface, so its prototype is built over the realm's
       %Object.prototype% and the row is under no ordering constraint; §3's FontFaceSet (`interface FontFaceSet :
       EventTarget`) will carry one, stated in core/fonts/font_face.h. `[Exposed=(Window,Worker)]`: no document
       half. A name the document column places is a name only a Window realm can have, so an interface whose
       exposure set reaches a worker is placed from its realm intrinsic; `resize_observer`,
       `intersection_observer`, `media_query_list`, `dom_rect_list`, `page_reveal`, `xml_serializer` and
       `document`'s XMLDocument are `[Exposed=Window]`. */
    { "font_face",           d_font_face,           NULL,              r_font_face },
    { "html_iframe",         d_iframe,              NULL,        r_iframe },
    /* RFC 6265 §5.3's cookie store, before the component whose §3.1.4 members read it. The store is the user
       agent's and an instance is one origin-keyed agent cluster, so it belongs to the JSRuntime, not to any
       JSContext, and is released here, since nothing frees it when a realm goes. */
    { "cookie_jar",          d_cookie_jar,          NULL,        r_cookie_jar },
    /* After event_target: `interface CookieStore : EventTarget` creates its prototype over the realm's
       EventTarget.prototype. */
    { "cookie_store",        d_cookie_store,        NULL,        r_cookie_store },
    { "document",            d_document,            i_document,  r_document },
    /* HTML §8.9.1's simple dialogs, after `document`: its install is three methods on the global, but every member
       opens on §8.9.1's "we cannot show simple dialogs", whose forced step reads the Document's active sandboxing
       flag set, so the members are declared against the component that owns Documents. */
    { "simple_dialogs",      d_simple_dialogs,      i_simple_dialogs, r_simple_dialogs },
    /* HTML §8.5.1's DOMParser, after `document`: every member builds a second Document through
       core/dom/document.h's `document_new`, so it is declared against the component that owns Documents. Its
       install puts an interface object on the global, and its prototype chains to Object.prototype. */
    { "domparser",           d_domparser,           NULL,        r_domparser },
    /* HTML §8.5.8's XMLSerializer, after `element`: its declaration brands `serializeToString(Node root)` against
       the Node class, which element_init creates (through node_init). DOM Parsing and Serialization §5.2.1 "XML
       Serialization" walks the tree it is handed, so it does not depend on `document`. Its prototype chains to
       Object.prototype. */
    { "xml_serializer",      d_xml_serializer,      i_xml_serializer, r_xml_serializer },
    { "module_loader",       d_module_loader,       NULL,        r_module_loader },
};
static const int PLATFORM_N = (int)(sizeof PLATFORM / sizeof PLATFORM[0]);

/* The other side of the list: the name each component puts on a realm's global, asserted once the whole list has
 * run, since a list that is only run cannot disagree with anything. An absent name fires with the component that
 * owed it, and a row whose component is renamed or removed fires from platform_check_table's row lookup. A name
 * whose install moves to another component leaves a row naming a component that still exists, so both directions
 * pass; edit the row at the move (the duplicate check below sees a move that adds a row).
 *
 * The third column is Web IDL §3.3.7 [Exposed]'s condition, and the probe is two-sided against it: exposed means
 * present, not exposed means absent. Web IDL §3.3.13 [SecureContext]'s example ("there will be no \"HeartbeatSensor\"
 * property on Window") is a name owed only in a secure realm. The condition is stated here, not read back from
 * the install, because this list is an oracle: an expectation derived from the gate it checks asserts nothing.
 * `IDL_EXPOSED` is 0 and means the IDL carries no condition, so a row that omits the column states the
 * unconditional case. The four Window members come from realm intrinsics, and are asserted here because this is
 * where the realm is finished. */
static const struct { const char *name, *component; IdlExposure exposure; } PLATFORM_WITNESS[] = {
    /* A name on browser/platform_names.h is one solver/absent.c declines to mint a concolic for, so a missing
       install reads as a concrete `undefined`: a guard such as `if (window.performance)` is decided with no fork,
       and a call one frame later throws for a reason that names nothing. These rows report such an install. */
    { "console",               "console" },
    { "window",                "window" },
    /* HTML §7.2.2.5 "Historical browser interface element APIs"'s interface object. Its §3.7.3 prototype is built
       per realm, so `Object.prototype.toString.call(window.locationbar)` answers "[object BarProp]" whatever this
       row sees; the absent direction is what it checks. It names `window` because a sub-component names the row
       that reaches it: bar_prop_install is called from window_install and bar_prop_free from window_free. */
    { "BarProp",               "window" },
    /* DOM §2.3's `event`. Real code reads it as a fallback (`n || window.event`), not a feature test, so a missing
       install restores `undefined` with no throw and no guard flipping. */
    { "event",                 "current_event" },
    { "onload",                "event_target" },
    { "document",              "document" },
    { "navigator",             "navigator" },
    /* Web Locks API §3.2's and §3.3's interface objects, `[SecureContext, Exposed=(Window,Worker)]`: over plain
       http the realm has neither, which is what `"locks" in navigator && isSecureContext` discovers. Their component
       is `navigator`, because §3.1 declares an `interface mixin` that `Navigator includes`, so it is declared from
       navigator_init and released from navigator_free. */
    { "LockManager",           "navigator", IDL_SECURE_CONTEXT },
    { "Lock",                  "navigator", IDL_SECURE_CONTEXT },
    { "location",              "location" },
    { "screen",                "screen" },
    { "history",               "history" },
    { "navigation",            "navigation" },
    { "NavigationHistoryEntry", "navigation_history_entry" },
    { "NavigationDestination", "navigation_destination" },
    { "NavigateEvent",         "event" },
    { "open",                  "navigable" },
    { "fetch",                 "fetch" },
    { "setTimeout",            "timer" },
    /* HR-TIME §7's interface object and §8.1's `performance` attribute. A missing install decides
       `if (window.performance && performance.now)` false, so every path a bundle puts behind that guard is
       unreachable in a browser where the guard is always true. */
    { "Performance",           "performance" },
    { "performance",           "performance" },
    /* §3.7.1's interface objects, unconditional; a realm without `PerformanceMark` is one where
       `performance.mark(...)` throws a TypeError. */
    { "PerformanceEntry",      "performance_entry" },
    /* Performance Timeline §4 and §4.2.2's interface objects. A bundle reads
       `PerformanceObserver.supportedEntryTypes.includes(t)` before anything else, so a missing install is a
       ReferenceError on the arm's first line. */
    { "PerformanceObserver",   "performance_observer" },
    { "PerformanceObserverEntryList", "performance_observer" },
    { "PerformanceMark",       "user_timing" },
    /* User Timing §2.3's interface object, beside §2.2's. A name with no row here is a missing witness, not an abort, so the
       list reports clean over it. A realm without `PerformanceMeasure` is one where `performance.measure(...)`
       throws a TypeError. Its IDL carries no condition this engine's two realm kinds tell apart. */
    { "PerformanceMeasure",    "user_timing" },
    { "postMessage",           "window_message" },
    { "structuredClone",       "structured_clone" },
    { "requestAnimationFrame", "animation_frame" },
    /* Cooperative Scheduling of Background Tasks §4's two Window members and §4.3's IdleDeadline interface object.
       A bundle's guard is `window.requestIdleCallback ? window.requestIdleCallback(f) : setTimeout(f, 1)`, so a
       missing install takes the setTimeout arm unseen; `IdleDeadline` is what a page reads to detect the
       interface. */
    { "requestIdleCallback",   "idle_callback" },
    { "cancelIdleCallback",    "idle_callback" },
    { "IdleDeadline",          "idle_callback" },
    { "matchMedia",            "media_query_list" },
    /* CSSOM §8.1's namespace object: a missing install makes `CSS.escape` read `undefined`, and only the call
       throws. */
    { "CSS",                   "css_namespace" },
    /* CSS Typed OM 1's three interface objects: a page reads `window.CSSUnitValue` to decide whether the Typed OM
       exists, and `x instanceof CSSUnitValue` to tell one of §4.3.5's results from a number. */
    { "CSSStyleValue",         "css_unit_value" },
    { "CSSNumericValue",       "css_unit_value" },
    { "CSSUnitValue",          "css_unit_value" },
    /* CSS Typed OM §4.2's interface object, placed by the `css_unit_value` install through
       core/css/css_keyword_value.c, so the
       witness names the row whose failure would take it. */
    { "CSSKeywordValue",       "css_unit_value" },
    /* §4.3.4's eight, placed by the `css_unit_value` install through core/css/css_math_value.c. `x instanceof
       CSSMathSum` is how a page tells one of §4.3.1's arithmetic results from a CSSUnitValue. */
    { "CSSMathValue",          "css_unit_value" },
    { "CSSMathSum",            "css_unit_value" },
    { "CSSMathProduct",        "css_unit_value" },
    { "CSSMathNegate",         "css_unit_value" },
    { "CSSMathInvert",         "css_unit_value" },
    { "CSSMathMin",            "css_unit_value" },
    { "CSSMathMax",            "css_unit_value" },
    { "CSSNumericArray",       "css_unit_value" },
    /* HTML §8.9.1's three members: a missing install makes `window.confirm` read `undefined`, and only the call
       throws. */
    { "alert",                 "simple_dialogs" },
    { "confirm",               "simple_dialogs" },
    { "prompt",                "simple_dialogs" },
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
    { "InputDeviceCapabilities", "input_device_capabilities" },
    { "Event",                 "event" },
    /* Both owed by the `event` row: message_event_install_realm and error_event_install_realm place them, and
       core/events/event.c's event_init declares both. */
    { "MessageEvent",          "event" },
    { "ErrorEvent",            "event" },
    { "MessageChannel",        "message_port" },
    { "XMLHttpRequest",        "xml_http_request" },
    { "FileReader",            "file_reader" },
    { "BroadcastChannel",      "broadcast_channel" },
    { "PromiseRejectionEvent", "unhandled_rejection" },
    { "PageRevealEvent",       "page_reveal" },
    { "AbortController",       "abort" },
    { "DOMRect",               "dom_rect" },
    { "DOMRectList",           "dom_rect_list" },
    { "Path2D",                "path_2d" },
    { "ImageData",             "image_data" },
    { "IntersectionObserver",  "intersection_observer" },
    { "IntersectionObserverEntry", "intersection_observer" },
    /* CSS Font Loading §2's FontFace and Resize Observer's three names, all mapped to the one component that
       declares and installs them. Real bundles construct both unguarded (`new FontFace(`, `new ResizeObserver(`),
       so a missing install is a ReferenceError that ends the script and every endpoint behind it. */
    { "FontFace",              "font_face" },
    { "ResizeObserver",        "resize_observer" },
    { "ResizeObserverEntry",   "resize_observer" },
    { "ResizeObserverSize",    "resize_observer" },
    /* Web Storage: §12.2.1's interface object and §12.2.2's and §12.2.3's Window members. A missing
       `window.localStorage` answers `undefined`, so a `&&` takes its else arm unseen. `storage_shed` has no
       witness because Storage §4's model installs nothing. */
    { "Storage",               "storage" },
    { "localStorage",          "window_storage" },
    { "sessionStorage",        "window_storage" },
    /* §12.2.4's interface object. Its component is `event`, because core/events/event.c's subclass list declares
       every Event subclass, as with NavigateEvent. */
    { "StorageEvent",          "event" },
    /* Web Cryptography's names: §10's interface object, §14's, and the Window member the mixin's partial declares.
       A missing `window.crypto` answers `undefined` instead of throwing. */
    { "Crypto",                "crypto" },
    /* `[SecureContext,Exposed=(Window,Worker)] interface SubtleCrypto`. `interface Crypto` is
       `[Exposed=(Window,Worker)]` with no [SecureContext], and the Window member is `[SameObject] readonly
       attribute Crypto crypto` on the WindowOrWorkerGlobalScope partial mixin, also unconditional; only `subtle`,
       `randomUUID` and this interface object are gated. Over plain http a page finds `Crypto` and `crypto` and
       neither `crypto.subtle` nor `SubtleCrypto`, which is what `crypto && crypto.subtle` discovers. */
    { "SubtleCrypto",          "crypto", IDL_SECURE_CONTEXT },
    /* §13's interface object, gated by the same attribute. Its component is `crypto`, the row core/crypto's
       subtle_crypto is declared under; core/realm.h runs its per-realm install as an intrinsic. */
    { "CryptoKey",             "crypto", IDL_SECURE_CONTEXT },
    { "crypto",                "crypto" },
    { "DOMStringList",         "dom_string_list" },
    { "IDBKeyRange",           "idb_key_range" },
    { "indexedDB",             "indexed_db" },
    { "IDBTransaction",        "idb_transaction" },
    { "IDBRequest",            "idb_request" },
    { "IDBOpenDBRequest",      "idb_request" },
    { "IDBDatabase",           "idb_connection" },
    { "IDBObjectStore",        "idb_object_store" },
    { "IDBIndex",              "idb_index_handle" },
    { "IDBVersionChangeEvent", "idb_version_change_event" },
    /* The Observable standard's two. One realm intrinsic places both with two
       `idl_define_global_property_reference` calls, so a row per name, not per component, sees one dropped. This
       loop runs at the end of platform_document_install, which a worker realm never reaches, so it asserts the
       Window arm of `[Exposed=*]`; test_forced.c's exposure_selftest asserts the worker arm. */
    { "Observable",            "observable" },
    { "Subscriber",            "observable" },
    { "DOMParser",             "domparser" },
    /* HTML §8.5.8 "The XMLSerializer interface", `[Exposed=Window]`, unconditional. A missing install decides
       `if (window.XMLSerializer)` with no fork, and `new XMLSerializer()` is a ReferenceError. */
    { "XMLSerializer",         "xml_serializer" },
    /* File System Access §3's three factories, whose partial interface is `[SecureContext]`: Window members rather
       than interface objects, witnessed in both directions because a bundle feature-detects them
       (`'showOpenFilePicker' in window`). Their component installs from core/realm.h's intrinsic list. */
    { "showOpenFilePicker",    "file_picker", IDL_SECURE_CONTEXT },
    { "showSaveFilePicker",    "file_picker", IDL_SECURE_CONTEXT },
    { "showDirectoryPicker",   "file_picker", IDL_SECURE_CONTEXT },
};
static const int PLATFORM_WITNESS_N = (int)(sizeof PLATFORM_WITNESS / sizeof PLATFORM_WITNESS[0]);

/* The agent is one runtime and the list runs once in it. Each component's `_init` asserts it did not run twice;
   this asserts it of the list, so the report names the list rather than the first component. */
static JSRuntime *g_declared_in;

/* Whether a name is a row: the one answer to a question the witness table (a component per global) and
   core/agent_state.h's registry (a component per declared slot, and a sub-component naming the row that releases
   it) both ask. The compiler checks none of those spellings, and a misspelled one leaves its check unrun while
   another row reports a failure describing a different repair. */
static int platform_has_row(const char *name)
{
    int i;

    DCHECK(name != NULL && *name, "the platform list was asked about a component with no name");
    for (i = 0; i < PLATFORM_N; i++)
        if (strcmp(PLATFORM[i].name, name) == 0) return 1;
    return 0;
}

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
        /* A release without a declaration has nothing to release: agent state is made by the agent half, so a row
           that frees without declaring frees another component's state or nothing. */
        DCHECK(PLATFORM[i].release == NULL || PLATFORM[i].declare != NULL,
               "a platform component releases agent state it never declared — the release column is the inverse "
               "of the DECLARE column, and a row with only the inverse is undoing somebody else's work");
        for (k = 0; k < i; k++)
            DCHECK(strcmp(PLATFORM[k].name, PLATFORM[i].name) != 0,
                   "a platform component is listed twice — its declaration would mint a second class and a "
                   "second pool entry, and everything already chained to the first would answer out of an "
                   "object the realm has thrown away");
    }
    /* The witness table is keyed by global name, and its key is the `%s` of an abort naming what to install and
       decides which row the probe loop answers from, so it gets the same well-formedness checks. */
    for (i = 0; i < PLATFORM_WITNESS_N; i++) {
        /* The row names the global it is an oracle about. `idl_exposed_in_realm` answers true for an identifier
           the corpus has no row for (browser/idl_exposure.h), so an unnamed row is owed and absent, and the probe
           would abort naming nothing. It is caught here, where the row still has a component to name. */
        DCHECKF(PLATFORM_WITNESS[i].name != NULL && *PLATFORM_WITNESS[i].name,
                "a platform witness row names NO GLOBAL and says the component `%s` owes it — the probe at the "
                "end of the per-document install would still run it (an identifier with no row in the corpus "
                "is EXPOSED, so an unnamed row is owed and absent) and would abort naming nothing a reader "
                "could install. State the identifier Web IDL §3.8 `define the global property references` puts "
                "on the global, or delete the row",
                PLATFORM_WITNESS[i].component);
        DCHECKF(platform_has_row(PLATFORM_WITNESS[i].component),
                "the platform witness for `%s` names the component `%s`, and this list does not have it — the "
                "component was renamed or removed and the name it used to install is now owed by nobody, "
                "which is the state a stale exclusion rots into",
                PLATFORM_WITNESS[i].name, PLATFORM_WITNESS[i].component);
        /* One name, one answer. Two rows for one name happen when an install moves to another component and a row
           is added rather than edited; both pass, and a later abort blames whichever row the probe reaches first.
           If their `exposure` columns differ, the probe asserts both owed and not owed of one name in one realm. */
        for (k = 0; k < i; k++)
            DCHECKF(strcmp(PLATFORM_WITNESS[k].name, PLATFORM_WITNESS[i].name) != 0,
                    "the global `%s` has TWO witness rows, owed by `%s` and by `%s` — this list is an oracle "
                    "keyed by the name, so one of those components does not install it and the abort a later "
                    "reader gets names whichever row this loop reached first. An install that MOVED between "
                    "components edits the row it moved from; it does not add a second one",
                    PLATFORM_WITNESS[i].name, PLATFORM_WITNESS[k].component, PLATFORM_WITNESS[i].component);
    }
}

/* The release column's other side: the witness table asserts what a component installs, this asserts what it
 * holds, since a release that frees a value and keeps its handle looks like one that works. Three directions: a
 * row with a release that declared no agent state is a release nothing can check; a row with no release that
 * declared agent state is a leak, reported by the runtime as an anonymous object with nothing naming its owner;
 * and a declaration naming no row. The last runs first, because the other two start from a row and are blind to a
 * name no row carries: a misspelled name leaves its slots unchecked while the owning row reports "declared no agent
 * state". With the registry walk first, reaching the row loop proves every declaration names a row. */
static void platform_check_agent_state(JSRuntime *rt, uint32_t minted_before, int declared_before)
{
#if APICLIENT_DEV
    const char *component, *what, *file;
    int i, line;
    uint32_t minted, declared;

    /* The declaring site goes in the abort, because this DFAILF stamps this file for every declaration and the
       repair is chosen by reading the declaring file (core/agent_state.h's macros capture it at the call). It is
       not the component name under another spelling: a sub-component declares under its releasing row's name. */
    for (i = 0; agent_state_slot(i, &component, &what, &file, &line); i++) {
        if (platform_has_row(component)) continue;
        DFAILF("%s:%d declared %d slot(s) of agent state under `%s` and THIS LIST HAS NO ROW OF THAT NAME; the "
               "first is %s. Nothing about those slots is checked less carefully — the question is never "
               "ASKED. The row pairing below can only ask \"does anybody RELEASE this?\" about a name a row "
               "carries, so for these it is not run at all, and the row that really owns them is left "
               "reporting \"declared no agent state\" in the exact words a component that declared nothing "
               "would use. A SUB-COMPONENT NAMES THE ROW THAT RELEASES IT, NEVER ITS OWN FILE: "
               "core/dom/selection.c declares under `document` and core/crypto/subtle_crypto.c under `crypto`, "
               "because that is whose release column their release is reached from. So this is one of two "
               "repairs, and they are not alike — either the owning row is misspelled at the line named "
               "above, or this component is declared and released by nobody and owes a row here "
               "(core/agent_state.h). THE ADDRESS IS THIS DECLARATION'S AND THE COUNT IS THE NAME'S: a "
               "component may declare from several files, so a repair that edits only the line above is "
               "finished when this walk stops firing and not before",
               file, line, agent_state_count(component), component, what);
    }
    for (i = 0; i < PLATFORM_N; i++) {
        int n = agent_state_count(PLATFORM[i].name);

        if (PLATFORM[i].release != NULL && n == 0)
            DFAILF("`%s` is on the release column and NOTHING IN THIS AGENT DECLARED agent state under that "
                   "name — and it is not a misspelling somewhere else, because the walk above has already "
                   "proved that every declaration this agent made names a row. So the release column really "
                   "is the inverse of nothing: agent_state_check_released holds no slot of this component's "
                   "to assert, and a release that frees a value and keeps its handle is indistinguishable "
                   "from one that works (core/agent_state.h)", PLATFORM[i].name);
        if (PLATFORM[i].release == NULL && n > 0)
            DFAILF("`%s` declared %d slot(s) of agent state and its row's release column is EMPTY — what a C "
                   "static holds for the whole agent is freed by nothing when the agent goes, which is what "
                   "every leak this file's comments record already was. If another component's release "
                   "already reaches this one's, the declarations belong under THAT row's name, which is how "
                   "a sub-component is declared (core/agent_state.h)", PLATFORM[i].name, n);
    }
    /* What this column minted is what it declared. It runs last because the walks above name an exact repair at
     * an exact line while this names a number, and a misspelled declaration counts here either way. It is a
     * difference over the declare column's window, not an absolute count: the allocator is the process's and the
     * registry the browser's, and solver/concolic.c's `concolic_init` mints a class id no row may declare (one host
     * calls it before platform_agent_init). Both sides are lifetime counts over the window: JS_ClassIDsMinted
     * only rises, and agent_state_class_id_count only rises until agent_state_reset. The remedy for a firing is a
     * declaration beside the mint, never a tolerance here. */
    minted   = JS_ClassIDsMinted(rt) - minted_before;
    declared = (uint32_t)(agent_state_class_id_count() - declared_before);
    DCHECKF(minted == declared,
            "the platform declare column MINTED %u class id(s) and DECLARED %u — every class id is handed out "
            "at one line (JS_NewClassID's `rt->js_class_id_alloc++`), so the allocator's own count is a number "
            "no spelling of a mint can evade, and core/agent_state.h's SLOT_CLASS and SLOT_REALM rows are "
            "everything this browser was told about. MORE MINTED THAN DECLARED is a static holding a class id "
            "that no `agent_state_class`/`agent_state_realm_slot` names: nothing gives it back at the release, "
            "so the next agent in this process finds it SET, its component's `_init` reads it as a latch and "
            "returns before re-registering, and every object that component mints is branded with an id the "
            "live runtime never handed out. MORE DECLARED THAN MINTED is the other direction and is not the "
            "same repair: a row declared ABOVE the line that assigns its slot, counted against an allocator it "
            "never asked. Derive the sites rather than reading a number here: `node engine/agentstate.mjs`",
            minted, declared);
    /* Named residual: the identity names a count, not an id.
     *   Not covered: a reader meeting this abort gets a number and a command, and the address comes from a text
     *     sweep that cannot see a mint in a spelling it does not search. A sum is also satisfiable by
     *     cancellation (an undeclared mint plus a row declared above the line that assigns its slot); a realm
     *     slot has agent_state_realm_slot_at's assert against that, a class slot has none.
     *   Next diff builds: per-id accounting over the same window. `agent_state_class_id_declared(id)` reads each
     *     SLOT_CLASS and SLOT_REALM row's slot, and every id in [JS_CLASS_INIT_COUNT + minted_before,
     *     JS_CLASS_INIT_COUNT + minted_after) that no row holds aborts, named through JS_IsRegisteredClass and
     *     JS_GetClassName (a dup'd atom, JS_ATOM_NULL for an unregistered id, and it needs a JSContext).
     *   How its absence shows: this abort's gap and `node engine/agentstate.mjs`'s queue disagree, the run's
     *     number larger, with nothing naming which mints make the difference. */
#else
    (void)rt; (void)minted_before; (void)declared_before;
#endif
}

/* The prologue every agent shares, whatever §8.1.2.2 algorithm allocated its cluster. Everything here is true of
   an agent as such and reads no fact telling a window agent from a worker one: the two entries differ in the
   cluster their agent joins, in whether they have a browsing context group, and in their first realm's [Global]
   kind. */
static void agent_prologue(JSContext *ctx, const char *origin)
{
    DCHECK(ctx != NULL, "the platform was declared into no realm");
    /* The declaration pass builds the browser's baseline, so it runs at the baseline stamp. Every object allocated
       below carries the current generation, and cow.c captures only `JS_ObjFlowGen(obj) > d->fork_gen`; the
       first delta has fork_gen 0 and the first slice opens at 1, so an object stamped above 0 is skipped by every
       delta. A prototype is never written, so most components would not show it, but shared baseline state held
       in a JS object (core/storage/storage_shed.c's sheds) would stop being shared across a fork. main.c asserts
       the same fact leaving qjs_step; the bump belongs to the slice bracket, and cow_delta_fork makes it. */
    DCHECK(JS_FlowGen() == 0,
           "the platform declaration pass began with the FLOW STAMP UP — every object this browser's baseline "
           "is made of would be stamped as belonging to a flow, and cow.c's `JS_ObjFlowGen(obj) > fork_gen` "
           "then skips it in every delta, so a component holding shared state in a JS object silently stops "
           "sharing it across a fork; whatever this host did before this call left a generation raised");
    DCHECK(origin != NULL && *origin,
           "an agent was brought up with no PRINCIPAL — §9.5's named bus is keyed by it and §7.2.1 decides "
           "remoteness by it, so an agent without one cannot answer either question");
    /* At most one agent has declared this platform. The agent half of this list is declared into file-scope
       statics (core/agent_state.h holds their addresses), so it is held once per process; a second live agent
       would overwrite g_declared_in, and the first agent's release column would run against the second's runtime.
       Named residual.
       Not covered: HTML §10.2.4 "Processing model"'s run a worker, step 4, obtains a worker agent that runs
       concurrently with its owner (its onComplete steps entangle two live ports), so core/agent_state.h's
       pre-init discipline, which restores slots after a release, cannot serve it.
       Next diff builds: agent-state slots reached through the declaring runtime rather than a file-scope static,
       so two live agents hold two sets; core/agent_state.h's declarations are the census of what is per process.
       How its absence shows: a host that brings up a second agent while the first is live fires here, at the
       declaration, rather than in an unrelated component's release or `_init` latch. */
    DCHECK(g_declared_in == NULL,
           "a second agent tried to declare the platform while one is already live. The agent half of this "
           "list lives in file-scope statics, so it is held once per PROCESS — this browser can host one "
           "agent at a time, and the second declaration would overwrite the runtime the FIRST agent's release "
           "column is going to be run against. Key the slots core/agent_state.h declares on their declaring "
           "runtime before bringing up a second agent");
    platform_check_table();
    g_declared_in = JS_GetRuntime(ctx);
    /* The principal becomes a record before any component is declared. It is not a row, since it installs
       nothing: it is the agent's origin (core/url/origin.h), against which §7.2.1's filter, §7.3.2.1's
       inheritance and Storage's key are decided. One adopt per agent gives an opaque principal its identity: the
       host states "null", and the nonce minted here is shared by every document of this agent. */
    origin_agent_adopt(origin);
}

/* The declaration column and the identity that brackets it. No thunk reads the `PlatformAgent` it is handed (each
   opens `(void)a`), so what a component declares is a fact about the engine, not the agent kind, and both entries
   share this one loop. */
static void agent_declare_column(JSContext *ctx, const PlatformAgent *agent)
{
    int i;
    /* The two ends of the mint window, read around the loop here and in platform_check_agent_state, because the
       identity is about this column: solver/concolic.c mints a class id at its own bring-up, and quickjs's
       predefined classes were never minted. */
    uint32_t minted_before;
    int declared_before;

    minted_before = JS_ClassIDsMinted(g_declared_in);
    declared_before = agent_state_class_id_count();
    for (i = 0; i < PLATFORM_N; i++)
        if (PLATFORM[i].declare)
            PLATFORM[i].declare(ctx, agent);
    platform_check_agent_state(g_declared_in, minted_before, declared_before);
}

/* §8.1.2.1 "Integration with the JavaScript agent formalism"'s create an agent, step 3's [[CanBlock]], which
   §8.1.2.2 hands it: "Only shared and dedicated worker agents allow the use of JavaScript Atomics APIs to
   potentially block." An agent is a JSRuntime here, so quickjs's per-runtime entry carries it, and `Atomics.wait`
   throws a TypeError where it is false. JS_NewRuntime's calloc leaves it false, which a worker agent must not
   answer, so each entry states it with its cluster allocation, since §8.1.2.2 is what calls create an agent.
   Step 4's event loop is the `event_loop` row, declared once per agent; running it is HTML §10.2.4 "Processing
   model"'s onComplete step 15. [[Signifier]]'s only stated reader is §8.1.2.1's realm-to-agent mapping, which
   this engine answers through the JSRuntime. */
static void agent_create_can_block(bool can_block)
{
    DCHECK(g_declared_in != NULL,
           "§8.1.2.1's create-an-agent stated its [[CanBlock]] before this agent had declared the platform — "
           "the bit belongs to the JSRuntime this agent IS, and agent_prologue is what records which runtime "
           "that is");
    JS_SetCanBlock(g_declared_in, can_block);
}

void platform_agent_init(JSContext *ctx, const char *origin, const char *top_level_url, bool requests_oac,
                         OpenerPolicyValue opener_policy)
{
    /* The one construction of this value for a window agent: every fact this call was handed, and nothing else.
       A fact added to platform.h's parameter list and forgotten here is an unused parameter; a struct field with
       no parameter to fill it cannot be reached from any host. */
    const PlatformAgent a = {
        .origin        = origin,
        .top_level_url = top_level_url,
        .requests_oac  = requests_oac,
        .opener_policy = opener_policy,
    };
    const PlatformAgent *agent = &a;
    /* realm_install_intrinsics asserts this too; asserting it here reports a host that passed nothing at the call
       it made, not from inside the first realm's intrinsics forty declarations later. */
    DCHECK(agent->top_level_url != NULL && *agent->top_level_url,
           "an agent was brought up with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide whether "
           "the first realm is a SECURE CONTEXT, and Web IDL §3.3.13's members are installed or absent by that "
           "answer, so this agent's platform surface is undecided");
    agent_prologue(ctx, agent->origin);
    /* §8.1.2.1's create an agent, given false: a similar-origin window agent is not one of the types that "allow
       the use of JavaScript Atomics APIs to potentially block", so `Atomics.wait` on this agent throws a
       TypeError. */
    agent_create_can_block(false);
    /* The browsing context group, before the cluster, because §8.1.2.2's obtain a similar-origin window agent takes
       it as an argument beside the origin and requestsOAC, and its step 3 reads the group's cross-origin isolation
       mode. §7.3.2.3's group is the other half of the `(browsing context group, origin)` key SECURITY.md makes
       this instance; it installs nothing, so it is not a row. */
    browsing_context_group_create(agent->opener_policy);
    /* The agent's cluster: §8.1.2.2's obtain a similar-origin window agent allocates the agent this call brings
       up, keyed by the origin adopted above. It installs nothing, so it is not a row, and it exists before
       `originAgentCluster`, §7.1.1.2's `document.domain` setter or anything else asks which cluster this is. */
    agent_cluster_obtain_window_agent(origin_agent(), agent->requests_oac);
    agent_declare_column(ctx, agent);
    /* The agent's first realm, built through the same call a child navigable's realm makes, so the first document
       cannot get a different set of per-realm intrinsics. It is a Window realm, and that is this entry's statement,
       not the host's: every realm this entry brings up gets `document`, `location`, `history` and `navigator` from
       the install column, so Web IDL §3.3.7 [Exposed] step 1's [Global] interface is HTML §7.2.2's Window by
       construction. A worker agent is the second entry below, not a parameter here, since one entry taking the
       [Global] kind would also take a browsing context group, a `requestsOAC` and a top-level creation URL a
       worker has not got. `false` is §8.1.3.5 step 1.2.1's operand, read from a WorkerGlobalScope's §10.2.1.1
       owner set, which a Window has not got; core/realm.c asserts a non-worker realm states exactly this. */
    realm_install_intrinsics(ctx, agent->top_level_url, "Window", false);
}

/* The worker agent's entry. HTML §8.1.2.1 "Integration with the JavaScript agent formalism" types an agent by its
 * global ("Dedicated worker agent — Contains a single DedicatedWorkerGlobalScope"), so a worker agent is never
 * assembled from the window entry plus a second realm. It has no browsing context group (§7.3.2.3's creation is a
 * step no worker algorithm runs), no `requests_oac` (§8.1.2.2's worker algorithm takes none, and §7.5.1 reads it
 * off a navigation response) and no top-level creation URL (HTML §10.2.6.2 "Script settings for workers": "top-level
 * creation URL to null"). It has no install column: core/workers/worker_global_scope.c installs §10.2.1.1's
 * surface through the realm-intrinsic column this entry ends in, and a worker agent has no Document.
 *
 * Named residual: the `PlatformAgent` this entry constructs carries three fields a worker agent has not got.
 *   Not covered: `top_level_url`, `requests_oac` and `opener_policy` are written as the values §10.2.6.2 and
 *     §8.1.2.2 give a worker environment; the type is the window agent's fact bundle.
 *   Next diff builds: a per-kind fact bundle, once a declare row reads one; every thunk opens `(void)a` today.
 *   How its absence would show: a component's declaration reads an agent fact, and a reader of this entry has to
 *     decide what a worker agent answers to a window agent's question. */
void platform_worker_agent_init(JSContext *ctx, const char *origin, const char *global_interface,
                                bool is_top_level, bool can_block, bool owner_is_secure_context)
{
    /* The one construction of this value for a worker agent, on the lines below the parameter list it copies. The
       three fields that list does not hold are the residual above. */
    const PlatformAgent a = {
        .origin        = origin,
        .top_level_url = NULL,                        /* §10.2.6.2: "top-level creation URL to null" */
        .requests_oac  = false,                       /* §8.1.2.2's worker algorithm takes no requestsOAC */
        .opener_policy = OPENER_POLICY_UNSAFE_NONE,   /* no browsing context group to give a mode to */
    };
    const PlatformAgent *agent = &a;

    DCHECK(global_interface != NULL && *global_interface,
           "a worker agent was brought up without naming the Web IDL §3.3.8 [Global] interface its one global "
           "object implements — §8.1.2.1 types an agent by that global, so an agent without it is an agent of "
           "no type, and core/realm.h's one call cannot decide which members its realm may hold");
    agent_prologue(ctx, agent->origin);
    /* §8.1.2.2's two steps in its order: the cluster, then the agent added to it. `can_block` is §8.1.2.2's third
       argument passed to §8.1.2.1's create an agent: true from obtain a dedicated/shared worker agent, false from
       obtain a worklet agent and obtain a service worker agent. A service worker's global is a worker global, so
       the [Global] kind does not decide it, and it is a parameter. */
    agent_cluster_obtain_worker_agent(is_top_level);
    agent_create_can_block(can_block);
    agent_declare_column(ctx, agent);
    /* The agent's first realm is its only one, through the same call the window entry ends in. The arguments are
       §10.2.6.2's and §8.1.3.5's, and realm_install_intrinsics asserts the pair: NULL for the top-level creation
       URL, and the owner's secure-context answer, which §8.1.3.5 "Secure contexts" step 1.2.1 reads from this
       global's §10.2.1.1 owner set, a fact about the creating agent the caller states. */
    realm_install_intrinsics(ctx, NULL, global_interface, owner_is_secure_context);
}

void platform_agent_free(void)
{
    int i;

    DCHECK(g_declared_in != NULL,
           "the platform was released in an agent that never declared it — there is nothing here to undo, and a "
           "host that reaches teardown without having reached platform_agent_init did not build this browser");
    /* No step machine is still live: a precondition of the whole column. A suspended builtin's state is a live
       activation of the component that owns it (a §2.9 dispatch, a custom-element reaction, an
       IntersectionObserver delivery, HTML §8.1.4.6's report), and tearing one down runs that component's `fini`,
       which reads its agent state, so a machine outliving this line reads a class id, private Symbol or step id
       this loop gave back. It is one question for every row, asked from the runtime's own census
       (`rt->step_census_n`), which solver/flow.c asserts zero once the frontier is gone; the fix for a firing is
       to release the frontier first. */
    DCHECK(JS_StepMachineCount(g_declared_in) == 0,
           "the platform's agent state is about to be released while STEP MACHINES are STILL LIVE — each is a "
           "browser algorithm some flow is suspended inside, and its teardown runs that component's own `fini` "
           "against state this column has already given back; release the frontier (solver_frontier_free) "
           "BEFORE this call, and this half's own agent state after it");
    /* Reverse declaration order, because the forward order is dependency order: a component declared after another
       may hold a value that component minted, so it gives it up first. */
    for (i = PLATFORM_N - 1; i >= 0; i--)
        if (PLATFORM[i].release)
            PLATFORM[i].release(g_declared_in);
    /* The origins go last, after every component that names one. They are the agent's, not a realm's (a
       WindowProxy holds one as a POD pointer inside the bytes its COW delta captures). */
    agent_cluster_release();   /* the cluster names an origin, so it goes BEFORE the origins it named */
    /* The group after the cluster, the reverse of creation: §8.1.2.2 allocated the cluster from the group's
       mode. */
    browsing_context_group_release();
    origin_release();
    /* Every declared slot is back where a fresh process would find it, asserted at the last instant the question
       has an answer: the next reader of a stale handle is a second agent's `_init`, which reads it as a latch. */
    agent_state_check_released();
    agent_state_reset();
    g_declared_in = NULL;
}

void platform_document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                               const char *origin, DocumentKind kind, SerializedPolicyContainer policy,
                               SerializedResponsePermissionsPolicy permissions_policy,
                               SandboxFlags sandbox_flags,
                               uint32_t doc_id, JSValueConst nav_proxy, const char *about_base_url,
                               bool is_iframe_srcdoc)
{
    /* The one construction of this value, on the lines below the parameter list it copies. */
    const PlatformDocument d = {
        .dom                = dom,
        .url                = url,
        .origin             = origin,
        .kind               = kind,
        .policy             = policy,
        .permissions_policy = permissions_policy,
        .sandbox_flags      = sandbox_flags,
        .doc_id             = doc_id,
        .nav_proxy          = nav_proxy,
        .about_base_url     = about_base_url,
        .is_iframe_srcdoc   = is_iframe_srcdoc,
    };
    const PlatformDocument *doc = &d;
    int i;

    DCHECK(ctx != NULL, "a document was installed into no realm");
    DCHECK(JS_IsObject(global), "a document was installed on something that is not the global object");
    DCHECK(doc->dom != NULL, "a document was installed with no parsed tree — `document` is a wrapper over one");
    DCHECK(doc->url != NULL && *doc->url,
           "a document was installed with no ADDRESS — a document is loaded FROM somewhere, and HTML §8.1.3.2 "
           "Environment settings objects' API base URL is what every relative URL the page builds resolves "
           "against (§4.4 stood here and is Grouping content)");
    DCHECK(doc->origin != NULL && *doc->origin,
           "a document was installed with no PRINCIPAL — every same-origin check compares it");
    /* This column's realm is a Window realm: Web IDL §3.3.8 [Global], asked of the realm's own stated global names
       rather than of which host called. The column installs members directly onto `global` without §3.7.3's
       conditional (idl_global_member_target, which two components ask), which §3.7.6/§3.7.7's [Global] arm allows
       only where the realm's [Global] interface declares them, and the placement loop at the end of this function
       relies on a worker realm never reaching here. idl_global_names_are_window reads browser/idl_exposure.h's
       generated IDL_GLOBALS bit, the corpus's own §3.3.8 answer; a hand-picked worker test would pass a worklet
       realm. */
    DCHECK(idl_global_names_are_window(realm_global_names(ctx)),
           "core/platform.c's per-DOCUMENT column ran in a realm whose Web IDL §3.3.8 [Global] global names "
           "are not `Window`. This column installs a Document's surface, and most of it goes STRAIGHT onto "
           "the realm's global object without asking §3.7.3's conditional (core/idl_args.h's "
           "idl_global_member_target) — which is correct for a Window realm, where §3.8's \"Define the regular "
           "operations of interface on instance\" arm places them, and is a §3.8 violation in any other, where "
           "the same members belong on a non-[Global] ancestor's §3.7.3 interface prototype object. A realm "
           "that is not a Window realm has no Document to install: build it with realm_install_intrinsics and "
           "its own component's per-realm install, and route any member this column shares with it through "
           "idl_global_member_target so the target is decided by the band rather than by which host called");
    /* The address and the principal are two arguments, compared by §7.3.2.1 "Creating browsing contexts" step 5:
       the principal is the address's origin. Steps 3 and 4 inherit (`about:srcdoc` always, `about:blank` with a
       source origin), and steps 1 and 2 mint an opaque origin serializing to `null`, so those are excluded by
       their own facts; `data:` and `file:` are opaque on both sides and agree as `null`. What it catches is a
       principal stated before the bytes were fetched: a peer instance is provisioned from the origin a
       `navigable.create` notice carried, derived from the request URL, so an off-origin redirect leaves it a
       principal its own document contradicts, consistently, where no same-origin check inside it can see.
       §7.4.5 takes a navigation's origin from the response's URL for this reason. The block is guarded because
       the check is a URL parse and an origin serialization, which a DCHECK condition may not contain. */
#if APICLIENT_DEV
    {
        UrlRecord u;
        bool parsed;

        url_record_init(&u);
        parsed = url_parse(&u, doc->url, strlen(doc->url), NULL);
        DCHECK(parsed,
               "a document was installed with an ADDRESS §4.4's parser refuses — every caller states an "
               "absolute serialization, so a record that cannot be recovered from it is a serializer and a "
               "parser disagreeing about one string rather than anything a page can cause");
        if (parsed && strcmp(doc->origin, "null") != 0 &&
            !url_matches_about(&u, "srcdoc", true) && !url_matches_about(&u, "blank", false)) {
            char *addr_origin = origin_serialize_of_url(&u);

            CHECK(addr_origin != NULL, "platform: OOM stating §7.3.2.1 step 5's origin for a document's address");
            DCHECK(strcmp(addr_origin, doc->origin) == 0,
                   "a document's PRINCIPAL is not its ADDRESS's origin, and §7.3.2.1 step 5 says it must be — "
                   "the three steps that inherit or mint one instead are excluded above, so this is a "
                   "principal decided somewhere other than from these bytes. The shape that produces it is a "
                   "principal stated BEFORE the fetch: a peer instance provisioned from a `navigable.create` "
                   "notice adopts the origin of the REQUEST url, and a response that REDIRECTED off-origin "
                   "then gives that agent a principal its own document contradicts — consistently, so no "
                   "same-origin check inside it can notice. Determine the origin where §7.4.5 does, from the "
                   "RESPONSE's URL (core/frame/navigable.c's load job does this for every navigation), and "
                   "state THAT to the instance the host provisions");
            free(addr_origin);
        }
        url_record_free(&u);
    }
#endif
    DCHECK(g_declared_in == JS_GetRuntime(ctx),
           "a document was installed in an agent whose platform was never declared — a per-realm install "
           "builds this realm's copy of a class the declaration mints, so there is nothing here to copy");

    for (i = 0; i < PLATFORM_N; i++)
        if (PLATFORM[i].install)
            PLATFORM[i].install(ctx, global, doc);

#if APICLIENT_DEV
    /* Web IDL §3.8's descriptor walk over the global, now that this column has finished. core/realm.c also runs it
       at the end of the per-realm intrinsics, which ends a worker realm's construction but not a Window's. It runs
       before the witness loop because it names the identifier that reached the global the wrong way (an
       address), where the witness names a component that owes a name. */
    realm_assert_global_property_references(ctx);

    /* The other direction of the same algorithm, which only this call site may state: the walk above sees only
       properties that exist, and §3.7.3's census of interface prototype objects sees one never placed. It is a
       separate entry, called only where both columns have run, because at the end of realm_install_intrinsics a
       Window realm legitimately holds prototypes whose property reference this column has not placed yet. A realm
       that reaches no document install is not audited by it (core/realm.h). */
    realm_assert_interface_objects_asked(ctx);

    for (i = 0; i < PLATFORM_WITNESS_N; i++) {
        JSAtom a = JS_NewAtom(ctx, PLATFORM_WITNESS[i].name);
        /* What this realm owes, decided by Web IDL §3.3.7 [Exposed]'s "is exposed in realm" rather than a copy of
           it here (core/idl_args.h says why an oracle may ask that predicate). Both conditions are asked: step 1's
           exposure set against the realm's global names and step 2's [SecureContext], which the row's `exposure`
           column states, so an `[Exposed=Window]` name is not demanded of a realm the standard excludes it from.
           Members such as `fetch`, `onload`, `document` and `setTimeout` have no row in the generated table, which
           is keyed by the identifiers §3.8 defines on a global, so they take the owed direction. */
        bool owed = idl_exposed(ctx, PLATFORM_WITNESS[i].exposure) &&
                    idl_exposed_in_realm(ctx, PLATFORM_WITNESS[i].name);
        int has;
        CHECK(a != JS_ATOM_NULL, "a platform witness name could not be interned");
        has = JS_HasProperty(ctx, global, a);
        JS_FreeAtom(ctx, a);
        CHECK(has >= 0, "a platform witness probe threw — [[HasProperty]] over the global runs no page code");
        /* The message is the name the component owed, and says which side failed, since the repairs differ: an
           owed name that is absent is an install that stopped, or an IDL exposure condition the row does not
           state. */
        DCHECKF(!owed || has == 1,
                "the platform owes this realm the global `%s` and it is not there — `%s` did not install it, "
                "or its IDL carries a Web IDL §3.3.7 [Exposed] condition (§3.3.13 [SecureContext] is the one "
                "this engine models) that this witness row does not state. If the component gates the install, "
                "the row states the same condition and this probe then requires the name to be ABSENT here",
                PLATFORM_WITNESS[i].name, PLATFORM_WITNESS[i].component);
        /* Web IDL §3.3.13's example: "In such a context, there will be no \"HeartbeatSensor\" property on Window." A page
           reads that as `'X' in window` answering false, distinct from a property that answers undefined. */
        DCHECKF(owed || has == 0,
                "the global `%s` is on a realm whose Web IDL §3.3.7 [Exposed] condition excludes it — §3.3.13 "
                "[SecureContext] REMOVES the construct rather than making it throw, so `'%s' in window` must "
                "answer false here and answers true. `%s` installs it unconditionally; the install states its "
                "exposure as data (the `_exposed` installers' IdlExposure parameter), never as an `if`",
                PLATFORM_WITNESS[i].name, PLATFORM_WITNESS[i].name, PLATFORM_WITNESS[i].component);
    }
#endif
}
