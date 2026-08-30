/* See platform.h. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/console/console.h"
#include "core/crypto/crypto.h"
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
#include "core/events/input_device_capabilities.h"
#include "core/events/event_target.h"
#include "core/events/message_event.h"
#include "core/events/message_port.h"
#include "core/events/report_exception.h"
#include "core/fetch/fetch.h"
#include "core/file/blob.h"
#include "core/file/file_reader.h"
#include "core/file/file_system.h"
#include "core/file/file_system_access.h"
#include "core/file/file_picker.h"
#include "core/file/file_system_handle.h"
#include "core/file/file_system_writable.h"
#include "core/file/storage_manager.h"
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
#include "core/html/unhandled_rejection.h"
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
#include "core/loader/cookie_jar.h"
#include "core/loader/module_loader.h"
#include "core/platform.h"
#include "core/realm.h"
#include "core/rendering/animation_frame.h"
#include "core/rendering/page_reveal.h"
#include "core/rendering/rendering.h"
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
#include "core/timing/timer.h"
#include "core/url/origin.h"
#include "core/url/url.h"
#include "core/url/url_search_params.h"
#include "core/xhr/xml_http_request.h"

/* THE FACTS, BUNDLED — AND THIS FILE IS THE ONLY PLACE THAT CAN SPELL THEM. platform.h takes them as arguments
   so that no host can hand over an incomplete value (see the paragraph there for why a hand-filled local was
   the defect and why `= {0}` is not the fix). They are one value HERE because the ninety-odd component rows
   below take one, and a row taking nine arguments would put a copy of the fact list on every one of them.
   THERE IS EXACTLY ONE CONSTRUCTION OF EACH, in the entry whose parameter list it copies, six lines below that
   parameter list — so a fact added to platform.h and not carried here is one function's diff and not three
   files'. */
typedef struct {
    const char       *origin;          /* the agent's PRINCIPAL */
    const char       *top_level_url;   /* HTML §8.1.3.1's TOP-LEVEL CREATION URL of the first realm */
    bool              requests_oac;    /* §7.5.1's `requestsOAC` */
    OpenerPolicyValue opener_policy;   /* §7.1.3's OPENER POLICY VALUE of the rooting response */
} PlatformAgent;

typedef struct {
    lxb_html_document_t      *dom;             /* the parsed tree this realm's `document` wraps */
    const char               *url;             /* the document's ADDRESS */
    const char               *origin;          /* the document's PRINCIPAL */
    SerializedPolicyContainer policy;          /* HTML §7.1.7's POLICY CONTAINER it is created with */
    SandboxFlags              sandbox_flags;   /* HTML §7.1.5's ACTIVE SANDBOXING FLAG SET */
    uint32_t                  doc_id;          /* the world registry's name for this document */
    JSValueConst              nav_proxy;       /* §7.2.3's ONE WindowProxy for its navigable */
} PlatformDocument;

/* ONE COMPONENT, BOTH HALVES. The thunks below exist because a component's declaration takes exactly the facts
   that component needs and no more — which is right, and is why they cannot share one signature. A thunk is
   the whole of the adaptation, so the table stays a list of components rather than a list of argument
   shuffles, and a component whose facts change breaks its own thunk and nothing else. */
typedef void PlatformDeclare(JSContext *ctx, const PlatformAgent *a);
typedef void PlatformInstall(JSContext *ctx, JSValueConst g, const PlatformDocument *d);
/* THE RELEASE TAKES THE RUNTIME, because that is what an AGENT is: a component on this column holds state for
   the whole agent (that is the entry condition for being on it), and agent state is freed against the runtime
   the declaration was made in — which platform.c already remembers, so no host has to pass anything. A row that
   wanted a JSContext would be a per-realm component in the wrong column; those go with their realm. */
typedef void PlatformRelease(JSRuntime *rt);

typedef struct {
    const char      *name;      /* the component, as its file is named — every assert below says which */
    PlatformDeclare *declare;   /* once per AGENT, or NULL for a component another one declares */
    PlatformInstall *install;   /* once per REALM, or NULL for a component with no per-document members */
    PlatformRelease *release;   /* once per AGENT at teardown, or NULL for a component that holds no agent state */
} PlatformComponent;

/* ---- the agent half ------------------------------------------------------------------------------------- */

static void d_console(JSContext *c, const PlatformAgent *a) { (void)a; console_init(c); }
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
static void d_input_device_capabilities(JSContext *c, const PlatformAgent *a)
{ (void)a; input_device_capabilities_init(c); }
static void d_event(JSContext *c, const PlatformAgent *a) { (void)a; event_init(c); }
static void d_report_exception(JSContext *c, const PlatformAgent *a) { (void)a; report_exception_init(c); }
static void d_message_port(JSContext *c, const PlatformAgent *a) { (void)a; message_port_init(c); }
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
static void d_page_reveal(JSContext *c, const PlatformAgent *a) { (void)a; page_reveal_init(c); }
static void d_viewport(JSContext *c, const PlatformAgent *a) { (void)a; viewport_init(c); }
static void d_visual_viewport(JSContext *c, const PlatformAgent *a) { (void)a; visual_viewport_init(c); }
static void d_media_query_list(JSContext *c, const PlatformAgent *a) { (void)a; media_query_list_init(c); }
static void d_rendering(JSContext *c, const PlatformAgent *a) { (void)a; rendering_init(c); }
static void d_fetch(JSContext *c, const PlatformAgent *a) { (void)a; fetch_init(c); }
static void d_abort(JSContext *c, const PlatformAgent *a) { (void)a; abort_init(c); }
static void d_observable(JSContext *c, const PlatformAgent *a) { (void)a; observable_init(c); }
static void d_dom_rect(JSContext *c, const PlatformAgent *a) { (void)a; dom_rect_init(c); }
static void d_dom_rect_list(JSContext *c, const PlatformAgent *a) { (void)a; dom_rect_list_init(c); }
static void d_dom_string_list(JSContext *c, const PlatformAgent *a) { (void)a; dom_string_list_init(c); }
static void d_element(JSContext *c, const PlatformAgent *a) { (void)a; element_init(c); }
static void d_intersection_observer(JSContext *c, const PlatformAgent *a) { (void)a; intersection_observer_init(c); }
static void d_iframe(JSContext *c, const PlatformAgent *a) { (void)a; iframe_init(c); }
static void d_document(JSContext *c, const PlatformAgent *a) { (void)a; document_init(c); }
static void d_cookie_jar(JSContext *c, const PlatformAgent *a) { (void)a; cookie_jar_init(c); }
static void d_domparser(JSContext *c, const PlatformAgent *a) { (void)a; domparser_init(c); }
/* ECMAScript §16.2.1.10 HostLoadImportedModule is the RUNTIME's hook, which is what an agent is.
   `import(specifier)` with none installed resolved to nothing at all — no module, no error, no record — and a
   page that asks to load code and is answered silently is the one shape an unbuilt capability must not
   have. */
static void d_module_loader(JSContext *c, const PlatformAgent *a) { (void)a; module_loader_install(JS_GetRuntime(c)); }

/* ---- the agent half, undone ----------------------------------------------------------------------------- */

static void r_input_device_capabilities(JSRuntime *rt) { input_device_capabilities_free(rt); }
static void r_console(JSRuntime *rt) { (void)rt; console_free(); }
static void r_hr_time(JSRuntime *rt) { (void)rt; hr_time_free(); }
static void r_cookie_jar(JSRuntime *rt) { (void)rt; cookie_jar_free(); }
static void r_navigate_event_fire(JSRuntime *rt) { (void)rt; navigate_event_fire_free(); }
/* §8.1.3.3's about-to-be-notified rejected promises list is a live Array a C static holds for the agent, so it is
   agent state and belongs on this column. It was a line in each host's own teardown instead, the WPT runner's
   copy did not have it, and the consequence was not a subtle one: EVERY file that gate ran ended on
   JS_FreeRuntime's gc_obj_list walk with a leaked Array, so a test that had already passed was reported as an
   abort. That is exactly the drift this column exists to end. */
static void r_unhandled_rejection(JSRuntime *rt) { unhandled_rejection_free(rt); }
/* PERMISSIONS §3.2's STORE IS TWO LIVE ARRAYS, and it is reached only through navigator_free — Permissions is
   declared from navigator_init, so it is released from there, which is right and is exactly why this row has to
   exist. The WPT runner had no `navigator_free` line, so §3.2's store and the sources record beside it were
   built for every file that gate ran and freed for none: two leaked GC objects per file, which is enough on its
   own to end every run on JS_FreeRuntime's leak walk. It is the same defect as the row above, found by the same
   reading, and the count could not show the first one being fixed while this one was still there — an abort is
   per FILE, so removing one of several universal leaks moves it by nothing.
   §6.4.4's UserActivation and Screen come with it: same column, same reason, and a component released from a
   list one host writes out by hand is a component some other host leaks. */
static void r_navigator(JSRuntime *rt) { (void)rt; navigator_free(); }
static void r_screen(JSRuntime *rt) { (void)rt; screen_free(); }
static void r_storage_manager(JSRuntime *rt) { (void)rt; storage_manager_free(); }
/* INDEXED DATABASE §2.1's set of databases — one live record per storage key, holding every object store and
   every record in them. It is the agent's for the reason its row states, so it is freed here. */
static void r_idb_database(JSRuntime *rt) { idb_database_free(rt); }
/* Each holds ONE runtime-lifetime value for the agent — the private Symbol its instances' internal slots
   hang off — and the transaction additionally holds §2.7.2's live set and §2.7.1's cleanup set. A component
   that mints a runtime-lifetime value owns it, and nothing else can free one: a per-realm value would go with
   its context, and these belong to the agent. */
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
/* THE ONE VIRTUAL FILESYSTEM AND THE STANDARDS OVER IT, and this is the row whose absence was measured rather
   than argued. §2.1's two ROOT DIRECTORY ENTRIES are agent state by the file's own reasoning (storage is keyed
   by origin and an instance is an origin-keyed agent cluster), and File System Access §3.2.2's recently-picked
   map is one more. Every one of those five releases was written out by hand into main.c's teardown and again
   into wpt_runner.c's, and test_forced.c — the entry every gate in this tree actually links — had NONE of
   them. The runtime's leak walk named the result exactly: `{kind:"directory", name:""}` and
   `{kind:"directory", name:"local"}` with a null prototype, and §3.2.2's empty map beside them, each held with
   refcount 1 from OUTSIDE the heap. Behind those three roots the ROOT REALM could never be collected — 2614
   Functions, 408 shapes and a JSContext at refcount 3108 — so three C statics nobody freed were reported as a
   whole leaked browser with nothing naming the owner. That is the third column's entire purpose, twice over:
   a host cannot express the omission, and there is no longer a hand-copied list for it to drift out of. */
static void r_file_system(JSRuntime *rt) { file_system_free(rt); }
static void r_fs_writable(JSRuntime *rt) { fs_writable_free(rt); }
static void r_fs_handle(JSRuntime *rt) { (void)rt; fs_handle_free(); }
static void r_fs_access(JSRuntime *rt) { (void)rt; file_system_access_free(); }
static void r_file_picker(JSRuntime *rt) { (void)rt; file_picker_free(); }
/* WEB CRYPTOGRAPHY's two components hold class ids, per-realm slots, a step id and ONE INTERNED ATOM (the
   Algorithm dictionary's `name`), all of which are the AGENT's. The atom is the reason this row exists rather
   than being left to the realm teardown: an atom is a JSAtomStruct and not a GC object, so a release nobody
   calls is invisible to JS_FreeRuntime's object walk and shows up only in its atom walk, by description.
   §14's release is reached through §10's, because §10 declares it. */
static void r_crypto(JSRuntime *rt) { (void)rt; crypto_free(); }
/* STORAGE §4.3's TWO SHEDS ARE THE AGENT'S, and that is the same argument §2.1's set of databases
   makes two rows down: storage is keyed by ORIGIN, §Security makes an instance an origin-keyed agent
   cluster, so the shed a same-origin child navigable reaches is THIS one rather than a second. What a C
   static holds for the agent is freed against the runtime here. §12.2.1's slot Symbol and §12.2.2's
   per-Document holder Symbol are runtime-lifetime values of the same kind. */
static void r_storage_shed(JSRuntime *rt) { storage_shed_free(rt); }
static void r_storage(JSRuntime *rt) { storage_free(rt); }
static void r_window_storage(JSRuntime *rt) { window_storage_free(rt); }
/* THE TWO DELIVERY CALLEES AND §9.5's BUS, found by the same reading and leaked by the same host. Each of these
   components mints ONE function object for the whole agent — the task callee a queued delivery runs through —
   and §9.5's registry of open channels is a live Array beside it; the walk named both callees as
   `Function { length: 2, name: "" }` and the registry as an empty Array. A component that mints a
   runtime-lifetime value owns it, and this column is where it gives it back. */
static void r_window_message(JSRuntime *rt) { window_message_free(rt); }
static void r_broadcast_channel(JSRuntime *rt) { broadcast_channel_free(rt); }
/* XMLHttpRequest holds the agent's step definitions and, through §5's ProgressEvent, the private Symbol that
   interface's own slots hang off. main.c freed it, wpt_runner.c and test_forced.c did not. */
static void r_xhr(JSRuntime *rt) { xhr_free(rt); }
/* File API §6.2 holds the agent's two step definitions and the four pool entries its read methods are. */
static void r_file_reader(JSRuntime *rt) { file_reader_free(rt); }
/* §7.2.6.10.3's NavigationDestination, AND IT IS THE ROW THE AUDIT FOUND RATHER THAN THE LEAK WALK. Its release
   was written, exported from its header, and CALLED BY NOBODY — not by a host, not by another component — so
   the private Symbol its internal slots hang off had been leaked by every run this engine had made up to that
   commit. The walk could not report it: a private Symbol is a JSAtomStruct, which is not on gc_obj_list, so
   `JS_FreeRuntime`'s object walk does not see it, and the atom walk beside it was behind ENABLE_DUMPS +
   JS_DUMP_ATOM_LEAKS, a bit no host in this tree ever set. THAT SECOND HALF IS NO LONGER TRUE and the sentence
   is not left standing: `JS_FreeRuntime`'s atom walk is unconditional now and carries a DCHECK that names the
   surviving atom by kind and description, so a release this column forgets aborts at the report instead of
   going unseen. What stays true is the first half — the audit found this row, not a detector — and the reason
   is the general one: a component released by nobody is what a THIRD copy of a hand-written list produces, and
   here there was no copy at all. */
static void r_nav_destination(JSRuntime *rt) { navigation_destination_free(rt); }
/* §5's four interned field names. fetch was one of the forty-three rows with a declare and an EMPTY third
   column, and the atom walk named all four on 118 files of an area that touches fetch only incidentally. */
static void r_fetch(JSRuntime *rt) { fetch_free(rt); }
/* THE DOM GROUP, WHOSE ROOT IS THE LARGEST CASCADE IN THIS BROWSER. element_free reaches forty-two further
   releases — node.c's WRAPPER IDENTITY TABLE (a counted reference to every node wrapper ever minted, and a
   wrapper holds its prototype, which holds the realm), custom_elements' registry backup and active-constructor
   map, §4.3's pending mutation-record queue, the CSSOM group, the selector engine's lexbor arena, the CSS
   parser, and the private Symbol every one of slot/shadow_root/element_internals/html_dialog/html_form hangs
   its internal slots off. Every one of those is a C static held for the AGENT, which is the entry condition for
   this column, and all of it was written by hand into three host teardowns instead — correct in all three
   today, and one edit from not being, which is the whole of what happened to the eight components before it.
   WHAT BLOCKED IT WAS A SIGNATURE AND NOTHING ELSE: the cascade freed against a JSContext, so it could not be
   a row. It takes a JSRuntime now and reaches the same values through JS_FreeValueRT/JS_FreeAtomRT — the same
   operation, since JS_FreeValue is JS_FreeValueRT(ctx->rt, v).
   THE FOUR ROWS MOVE AS A BLOCK, and that is why html_iframe, dom_rect_list and dom_rect come with it rather
   than after it. Reverse declaration order puts them in exactly the sequence the hand-written lists had them
   in — iframe, element, dom_rect_list, dom_rect — so nothing about the group's internal order changes; taking
   only the middle one would have reordered it against its own neighbours.
   `document` IS here now, and the sentence that used to stand in its place was half a rule read as a whole
   one. `document_free` really does read `doc_of(ctx)` and clear the realm's own opaque, and it really is a
   per-realm release that this column cannot express — but that is one of TWO halves this component has, and the
   other one had no home at all. `document_init` mints §4.5's class, declares fifteen pool entries and two
   realm-value slots, and declares TEN sub-components; three of those (§4.7, §4.8, §4.2.2) were being released
   from element_free's cascade, which is a release undoing another row's declaration, and the other seven were
   released by nobody. It also CLAIMED §13.2.7's document-load lifecycle slot on the ONE frontier — out of the
   per-DOCUMENT install, so every <iframe> re-claimed it and nothing ever gave it back.
   THE HALVES RUN AT DIFFERENT PHASES AND NEITHER ORDER IS THIS COLUMN'S TO PICK: a child navigable's
   document_free is reached from quickjs's realm-teardown hook, which fires inside JS_RunGC or JS_FreeRuntime —
   both after this whole list. What makes that safe is stated and asserted at document_agent_free rather than
   arranged here: the per-realm half reads NO static of that file, and §gc's realm-mark hook is deliberately NOT
   given back, because the collection that runs after this list is exactly what tears those child realms down. */
/* THE FOURTH THING A COMPONENT CAN HOLD FOR AN AGENT: A SLOT IN ANOTHER COMPONENT. These five were on no
 * column at all, and the reason was the same signature that kept the DOM group off it — each `_free` took a
 * JSContext, so it could not be a row, so it stayed a line in three hosts' hand-written teardowns. Not one of
 * them releases anything a REALM owns: every prototype they build is in that realm's class-proto slot and goes
 * with the context, and what is left over is class ids, step ids, interned atoms, private Symbols and — the
 * part no detector could see — CLAIMS, in both directions: a slot one of them holds for somebody else, or a
 * slot it took in somebody else.
 *
 * A CLAIM IS AGENT STATE WHOSE STORAGE IS IN THE OTHER COMPONENT, so it is released by the CLAIMANT and
 * asserted by the HOLDER. `timer` and `rendering` CLAIM: §8.1.7's timer step and §8.1.7.3's in-parallel half
 * are slots on the ONE frontier (solver/engine.c, released after the whole platform), and neither was ever
 * given back — the timer's was not even claimed once, it was re-claimed by every REALM, out of the
 * per-document install. `event_target` HOLDS four: §2.9's tree walk (core/dom/node.c), §2.9's activation
 * predicate and behaviour (core/html/hyperlink.c) and HTML §8.1.7.2's handler-set hook
 * (core/events/message_port.c), and not one of those three claimants released. `structured_clone` holds the
 * transferable registry, which is one row and a count nothing ever reset. Every one of them is the defect
 * core/agent_state.h found in Indexed Database §2.7.1's cleanup, and every one is invisible to both of
 * JS_FreeRuntime's censuses for the reason that file gives.
 *
 * REVERSE DECLARATION ORDER IS WHAT MAKES IT WORK, and it works because the claims run the same way as the
 * values: `element` (whose cascade reaches node.c and hyperlink.c) and `message_port` are both declared AFTER
 * `event_target`, so all three claims on that component are given back before it is released and its own
 * asserts are what say so. The hand-written lists had `event_target_free` BEFORE `message_port_free` in two of
 * the three hosts, which is that order exactly backwards. */
/* HTML §7.2.4's Location, AND IT IS A CLAIMANT ROW RATHER THAN A HANDLE ROW. Its release was a line in each of
   the three hosts' hand-written teardowns — the list this file exists to abolish, and the shape that cost the
   WPT runner §3.2's permission store — while it held the brand for §7.2.4 (never given back at all, which is
   core/agent_state.h's dom_rect defect) and TWO CLAIMS in solver/concolic.c's source registry: `location.hash`
   and `location.search`, with the percent-encode set each component applies to an attacker's bytes.
   THE CLAIMS ARE WHY THE POSITION MATTERS. concolic_free asserts that registry is empty, and the only thing
   that used to place this release before that assert was three hosts happening to write the lines in that
   order; on this column it is placed by reverse declaration order, and platform_agent_free runs before
   solver_agent_free by construction. */
static void r_location(JSRuntime *rt) { (void)rt; location_free(); }
/* HTML §7.4.1's state machine and §7.2.5's History over it, OFF THE THREE HOSTS' HAND-WRITTEN LISTS and onto
   this column — and this pair is the one that shows what those lists cost even when every one of them has the
   line. All three hosts DID call both, and they called them in TWO DIFFERENT PLACES: the WPT runner released
   them immediately after the viewport pair, main.c and test_forced.c a whole teardown later, past the realm
   intrinsics and the whole streams group. The order WITHIN the pair was also wrong in all three — §7.2.5 is a
   VIEW over §7.4.1's record and every one of its members reads it, so the view goes first, which is exactly
   what reverse declaration order gives and what a hand-written `session_history_free(); history_free();` does
   not. Neither component's release could be checked while they sat there either: a row with an empty release
   column may declare no agent state (platform_check_agent_state below), so the nine slots these two hold
   between them — two realm-value ids, §7.2.5's six member declarations and §7.4.3's step definition — were
   given back by a line this file could not see and asserted by nothing at all. */
static void r_session_history(JSRuntime *rt) { (void)rt; session_history_free(); }
static void r_history(JSRuntime *rt) { (void)rt; history_free(); }
/* CSSOM VIEW §4's viewport and §12's VisualViewport over it, off those same three lists for the same reason.
   The pair was written `viewport_free(); visual_viewport_free();` in all three hosts, which is again the
   dependent component released SECOND — §12's members are derivations over §4's layout viewport — and again
   reverse declaration order is what decides it here instead of three authors agreeing. */
static void r_viewport(JSRuntime *rt) { (void)rt; viewport_free(); }
static void r_visual_viewport(JSRuntime *rt) { (void)rt; visual_viewport_free(); }
static void r_event_target(JSRuntime *rt) { event_target_free(rt); }
static void r_message_port(JSRuntime *rt) { message_port_free(rt); }
static void r_timer(JSRuntime *rt) { timer_free(rt); }
static void r_structured_clone(JSRuntime *rt) { structured_clone_free(rt); }
static void r_rendering(JSRuntime *rt) { rendering_free(rt); }
static void r_document(JSRuntime *rt) { document_agent_free(rt); }
static void r_element(JSRuntime *rt) { element_free(rt); }
static void r_intersection_observer(JSRuntime *rt) { intersection_observer_free(rt); }
static void r_iframe(JSRuntime *rt) { iframe_free(rt); }
static void r_dom_rect_list(JSRuntime *rt) { dom_rect_list_free(rt); }
static void r_dom_string_list(JSRuntime *rt) { dom_string_list_free(rt); }
static void r_dom_rect(JSRuntime *rt) { (void)rt; dom_rect_free(); }
/* THE FIVE ROWS THAT HAD NO RELEASE FUNCTION AT ALL, which is the arm the pairing below silently passes: a row
   with an empty release column and a component that declared nothing agree, and they agree whether the
   component holds nothing or holds everything and gives none of it back. All five held. Indexed Database §4.7's
   IDBKeyRange carried a class and five pool entries, §4.8's IDBRecord a class, §4.3's IDBFactory a class, a
   realm-value slot and five declarations, HTML §8.5.1's DOMParser a class, its latch and two entries; and
   §16.2.1.10 HostLoadImportedModule's loader — the one that is not an int — carried a strdup'd specifier per
   lazy chunk and the buffer they are joined into, which is a malloc'd leak neither of JS_FreeRuntime's
   censuses can see and which a second agent would have REPORTED as its own document's imports. Each declares
   its slots to core/agent_state.h now, so the release is CHECKED rather than merely present. */
static void r_idb_key_range(JSRuntime *rt) { (void)rt; idb_key_range_free(); }
static void r_idb_record(JSRuntime *rt) { (void)rt; idb_record_free(); }
static void r_indexed_db(JSRuntime *rt) { (void)rt; indexed_db_free(); }
static void r_domparser(JSRuntime *rt) { (void)rt; domparser_free(); }
static void r_module_loader(JSRuntime *rt) { module_loader_free(rt); }
/* THE CROSS-AGENT SEAM, AS A GROUP OF FOUR, and the group is what makes it one entry rather than three. All
   three hosts wrote `remote_object_free; window_proxy_free; remote_location_free;` by hand — and that is the
   DEPENDENT-FIRST order twice over: §7.2.3's WindowProxy is the base whose per-realm prototype every reference
   minted by `remote_object` chains to and whose `location` member is what answers with a `remote_location`
   object, so it was being released before both components built over it. Reverse declaration order gives
   remote_location, remote_op, remote_object, window_proxy, which is that sequence inverted, and no author has
   to agree with any other about it.
   WHAT BLOCKED THREE OF THE FOUR WAS A SIGNATURE AND NOTHING ELSE: each `_free` took a JSContext, so it could
   not be a row. They take a JSRuntime now and reach the same values through JS_FreeValueRT/JS_FreeAtomRT —
   the same operation, since JS_FreeValue is JS_FreeValueRT(ctx->rt, v) and JS_FreeAtom is
   JS_FreeAtomRT(ctx->rt, a).
   AND TWO OF THEM WERE CARRYING THEIR CLASS IDS, which core/agent_state.h settles: an id names a class in a
   runtime that is gone. window_proxy additionally kept all FIVE of §7.2.3's pool entries, so a second agent in
   one process would have installed that whole member surface out of indices into a pool it had not built —
   a live-looking number with a wrong answer behind it, which neither of JS_FreeRuntime's censuses can report.
   Both now give the ids back, and window_proxy's finalizer and gc_mark reach their record through
   JS_GetAnyOpaque because of it (remote_location's already did). */
static void r_window_proxy(JSRuntime *rt) { window_proxy_free(rt); }
static void r_remote_object(JSRuntime *rt) { remote_object_free(rt); }
static void r_remote_location(JSRuntime *rt) { remote_location_free(rt); }
/* AND THE FOURTH OF THAT GROUP, WHOSE RELEASE COLUMN WAS EMPTY FOR A DIFFERENT REASON: its file already had a
   `remote_op_free` and that name is a PER-OPERATION free of one parsed record, not an agent release. A
   collision is not a release, and nothing here could tell the two apart — the row declared no agent state and
   released none, which is the pair of silences this list reads as agreement. The agent half is
   `remote_op_agent_free`, named as `document`'s two halves are. */
static void r_remote_op(JSRuntime *rt) { (void)rt; remote_op_agent_free(); }
/* HTML §7.2.2's Window WITH §7.2.2.5's BarProp UNDER IT, and this pair is the worst of the groups moved onto
   this column so far — not because a host was missing the line, but because all three had it and NOWHERE NEAR
   each other: main.c and test_forced.c ran `window_free` between §7.2.6.5's NavigationHistoryEntry and the
   cross-agent seam, wpt_runner.c ran it a whole teardown later, after solver_agent_free and after
   document_free. Three positions, three hosts, one component.
   WHAT BLOCKED IT WAS THE SIGNATURE AND NOTHING ELSE — `window_free` took a JSContext and used it for nothing
   but handing it on. It takes a JSRuntime now, which is what it always held: two class ids, a realm-value slot
   id and six pool entries, every one a registration in the runtime.
   AND THE ROW'S RELEASE COLUMN WAS EMPTY WHILE BOTH FILES DECLARED NOTHING, which is the arm the pairing below
   passes in silence: a component that holds everything and gives none of it back produces the same report as
   one that holds nothing. Both held. bar_prop.c's `g_bar_class` was reset by NOTHING — its release said in its
   own words that there was "nothing to release here any more" — and window.c's SIX POOL ENTRIES were declared
   with no initialiser at all, so their pre-init value was 0, which idl_method_id_all hands out as the FIRST
   member the platform declares. A second agent would have installed §7.2.2's `close`, `blur`, `stop` and three
   setters out of six indices into a pool it had not built, and minted every BarProp under a class id the live
   runtime never issued. Both are declared to core/agent_state.h now, under the ONE name `window`, because a
   sub-component names the row whose release reaches it. */
static void r_window(JSRuntime *rt) { window_free(rt); }
/* HTML §7.2.6 The navigation API AND §7.2.6.5 The NavigationHistoryEntry interface, as a PAIR, and this group
   is the one where the hand-written lists were RIGHT: all three hosts wrote `navigation_free(ctx);
   navigation_history_entry_free(ctx);` adjacently and in that order, which is exactly what reverse declaration
   order gives — `navigation_history_entry` is the row after `event_target` (its prototype chains to §2.7's and
   its CLASS is what §7.2.7.1's `required NavigationHistoryEntry from` brands against), `navigation` is the row
   after `history`, so the component that MINTS the wrappers and walks the list is released before the
   interface it mints them of. Three authors agreeing is still three answers; this is one.
   WHAT BLOCKED BOTH WAS THE SIGNATURE AND NOTHING ELSE — each took a JSContext and used it for nothing but
   JS_FreeValue, which is JS_FreeValueRT(ctx->rt, v). They take a JSRuntime now, which is what they always
   held: two class ids, a realm-value slot id and three pool entries.
   AND BOTH ROWS' RELEASE COLUMNS WERE EMPTY WHILE BOTH FILES DECLARED NOTHING, which is the arm the pairing
   below passes in silence. Both held, and both CARRIED THEIR CLASS ID — the defect core/agent_state.h settles,
   arriving here without any of the extra shapes the earlier groups had: the pool entries were already `-1`
   (both files gave them an initialiser, so entry 0 was never a pre-init value here) and neither class has a
   finalizer or a gc_mark, so nothing had to move to JS_GetAnyOpaque. What each DID have was a BRAND folded
   together with its own declaration — `g_nav_class != 0 &&`-shaped in navigation.c and, in
   navigation_history_entry.c, FIVE comparison sites of which four asked no declaration question at all.
   JS_GetClassID answers JS_INVALID_CLASS_ID, which quickjs.h defines as 0, for everything that is not an
   object, so the instant the release zeroes the id those sites report every PRIMITIVE as an instance: the four
   bare DCHECKs that assert "this IS one" go vacuously true, and because navigation.c's arm is spelled `!=` its
   brand does not reject a live Navigation but ADMITS `undefined` and then reads a realm-value slot that is
   back at −1. Each file answers the brand plainly now
   and asserts the declaration off a recorded runtime, which is also what replaced navigation_history_entry.c's
   `g_ready` — a flag that answered the same question with less in it. */
static void r_navigation(JSRuntime *rt) { navigation_free(rt); }
static void r_nav_history_entry(JSRuntime *rt) { navigation_history_entry_free(rt); }

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
static void i_file_reader(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; file_reader_install(c, g); }
static void i_navigable(JSContext *c, JSValueConst g, const PlatformDocument *d) { navigable_install(c, g, d->origin); }
static void i_timer(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; timer_install(c, g); }
static void i_window_message(JSContext *c, JSValueConst g, const PlatformDocument *d) { window_message_install(c, g, d->origin); }
static void i_broadcast_channel(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; broadcast_channel_install(c, g); }
static void d_structured_clone(JSContext *c, const PlatformAgent *a) { (void)a; structured_clone_init(c); }
static void i_structured_clone(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; structured_clone_install(c, g); }
static void i_unhandled_rejection(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; unhandled_rejection_install(c, g); }
static void i_animation_frame(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; animation_frame_install(c, g); }
static void i_page_reveal(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; page_reveal_install(c, g); }
static void i_media_query_list(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; media_query_list_install(c, g); }
static void i_fetch(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; fetch_install(c, g); }
static void i_abort(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; abort_install(c, g); }
static void i_observable(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; observable_install(c, g); }
static void i_dom_rect(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; dom_rect_install(c, g); }
static void i_dom_rect_list(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; dom_rect_list_install(c, g); }
static void i_intersection_observer(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; intersection_observer_install(c, g); }
static void i_dom_string_list(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; dom_string_list_install(c, g); }
static void i_document(JSContext *c, JSValueConst g, const PlatformDocument *d)
{
    document_install(c, g, d->dom, d->url, d->policy, d->sandbox_flags, d->doc_id, d->nav_proxy);
}
static void i_domparser(JSContext *c, JSValueConst g, const PlatformDocument *d) { (void)d; domparser_install(c, g); }

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
    /* HR-TIME §4's TIME ORIGIN IS THE FIRST FIELD A REALM GETS, and the position is the argument. §4 puts the
       field on the ENVIRONMENT SETTINGS OBJECT and says it holds "a moment early in the initialization" of it;
       core/realm.h creates that environment with the realm and stamps §8.1.3.1's top-level creation URL before
       running this list at all, so the moment named here is the same one, and being first among the intrinsics
       is what makes it impossible for a later install to stamp a timestamp out of a realm that has no origin
       yet — which is what DOM §2.5 does for every Event that is ever minted.
       IT DEPENDS ON NOTHING IN THIS LIST. Its install reads the event loop's virtual clock, and the loop's
       record is built in the DECLARE pass, which runs to the end before the first realm's intrinsics begin —
       so a row this early is not a row that reads a half-built agent. */
    { "hr_time",             d_hr_time,             NULL,        r_hr_time },
    /* CONSOLE §1, WHICH DEPENDS ON EXACTLY ONE ROW AND IS DEPENDED ON BY NONE. §1.4's timer table reads
       HR-TIME §4's current high resolution time, so it follows `hr_time`; nothing else in this list
       touches the console namespace, and every row after it may CALL one — which is the argument for
       putting it second rather than late. The namespace object goes on the global through a realm
       intrinsic, so a child navigable gets its own count map, group stack and timer table. */
    { "console",             d_console,             NULL,        r_console },
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
    { "event_target",        d_event_target,        NULL,        r_event_target },
    /* HTML §7.2.6.5's NavigationHistoryEntry, whose prototype chains to §2.7's and whose CLASS is what
       §7.2.7.1's `required NavigationHistoryEntry from` brands against — so it is declared before `event`,
       which is where every Event subclass including that one is declared. */
    { "navigation_history_entry", d_nav_history_entry, NULL, r_nav_history_entry },
    /* HTML §7.2.6.10.3's NavigationDestination, whose CLASS is what §7.2.6.10.1's `required NavigationDestination
       destination` brands against — so it is declared before `event`, which is where every Event subclass
       including NavigateEvent is declared. It inherits nothing, so its own prototype needs no earlier row. */
    { "navigation_destination", d_nav_destination,   NULL,     r_nav_destination },
    { "window",              d_window,              i_window,    r_window },
    /* §8.10.1's Navigator, and with it Permissions §6 (navigator.permissions), Storage §2 and File System §3
       (navigator.storage) and §6.4.4's UserActivation. This row is the one whose absence from one host's copy
       of the list left four standards uncollected by the gate that reports on them. */
    { "navigator",           d_navigator,           NULL,        r_navigator },
    /* THE ONE VIRTUAL FILESYSTEM and the File System Standard over it. The MODEL goes first (its two roots are
       built at this pre-boot baseline, so no flow's creation becomes every sibling's); §2.5's stream after §5's
       WritableStream, whose prototype it chains to; §2.2-§2.4's handles after the stream, since
       `createWritable()` mints one; §3's StorageManager after §8.10.1's Navigator, because `navigator.storage`
       is a partial interface member of the object that component builds. */
    { "file_system",         d_file_system,         NULL,        r_file_system },
    { "file_system_writable", d_fs_writable,        NULL,        r_fs_writable },
    { "file_system_handle",  d_fs_handle,           NULL,        r_fs_handle },
    { "storage_manager",     d_storage_manager,     NULL,        r_storage_manager },
    /* FILE SYSTEM ACCESS, a DIFFERENT standard over that same model: §2.2's "file-system" powerful feature and
       §2.3's two members, then §3's three pickers. Both rows come after `navigator` because Permissions §4's
       registry — where the feature's row lives — is declared under it, and after `file_system_handle` because
       §2.3's members install onto the prototype that component builds and core/realm.h runs the per-realm
       installs in declaration order. */
    { "file_system_access",  d_fs_access,           NULL,        r_fs_access },
    { "file_picker",         d_file_picker,         NULL,        r_file_picker },
    /* WEB CRYPTOGRAPHY §10's Crypto and §14's SubtleCrypto. ONE ROW for both, because §10 declares §14 as its
       own dependency the way Permissions declares PermissionStatus — a host that had one and not the other
       would answer `crypto.subtle` with an object from another realm or with nothing. Neither has a document
       half: §3.7 gives every realm its own interface prototype, `crypto` and `crypto.subtle` are both
       `[SameObject]` PER REALM, and a nested navigable's `crypto` is its own — so both install through
       core/realm.h. */
    { "crypto",              d_crypto,              NULL,        r_crypto },
    /* WEB STORAGE — Storage §4's model, then HTML §12.2.1's interface over it, then §12.2.2's and
       §12.2.3's two Window getters, in that order because each reads the one before it. The model is
       first for a second reason its own file states: its shed, shelf, bucket and bottles are built HERE,
       at the pre-boot COW baseline, so every flow shares one map object and each flow's writes to it are
       captured — built lazily on the first read they would be created inside whichever flow asked first
       and each arm of a fork would get a private localStorage. Neither of the last two rows has a
       document half: §12.2.1's prototype and interface object are per-REALM (§3.7), and the two getters
       go on each realm's own global, so both install through core/realm.h. */
    { "storage_shed",        d_storage_shed,        NULL,        r_storage_shed },
    { "storage",             d_storage,             NULL,        r_storage },
    { "window_storage",      d_window_storage,      NULL,        r_window_storage },
    /* HTML §2.6.5's DOMStringList, BEFORE the standard that is currently its only consumer — Indexed Database
       §4.4, §4.5 and §4.10 each answer with one, and core/realm.h runs the per-realm installs in DECLARATION
       order, so its prototype must be in a realm before any member below can build a list in it. It is an HTML
       row and not an Indexed Database one because it is an HTML type; HTML §7.2.5's `ancestorOrigins` is the
       next consumer and reaches the same component. */
    { "dom_string_list",     d_dom_string_list,     i_dom_string_list, r_dom_string_list },
    /* INDEXED DATABASE, in the standard's own dependency order and no further. §2.4's KEY is what §2.2's list
       of records is sorted by, what §2.9's range is bounded by and what §2.10's cursor walks in, so it is the
       first thing that standard can have; §4.7's IDBKeyRange is the interface over it, and §4.3's IDBFactory
       is the door the rest of the standard will be reached through. Neither row has a document half: both
       install per REALM (core/realm.h), because §3.7 gives every realm its own interface prototype and because
       the one IDBFactory `indexedDB` answers with is `[SameObject]` per realm.
       WHAT THE ROWS DO NOT BUILD is honestly absent and says so where a page reaches it: there is no
       connection, no transaction and no request yet, so `indexedDB.open` is a TypeError naming the member
       rather than a shape-only object that would report a store nothing wrote to.
       §2.1's DATABASE is the third row and it is an AGENT row with a RELEASE, which the two above it are not:
       §2.1 says "each storage key has an associated set of databases", §Security makes an instance an
       origin-keyed agent cluster, and storage is keyed by origin — so the set is the agent's, a same-origin
       child navigable reaches THAT one rather than a second, and what a C static holds for the agent is freed
       against the runtime here (core/platform.h's third column) rather than in each host's own teardown. */
    { "idb_key_range",       d_idb_key_range,       NULL,        r_idb_key_range },
    /* §2.12's RECORD SNAPSHOT with §4.8's IDBRecord over it, beside §2.9's key range because it is the same
       kind of row: a value type of this standard, installing one interface per realm. THE SENTENCE THAT USED
       TO STAND HERE SAID IT HELD NO AGENT-LIFETIME STATE, and it held a class — which is a fact about a
       RUNTIME, so it is exactly agent-lifetime, and saying otherwise is how these three rows kept an empty
       release column while the pairing that would have caught it read the two silences as agreement.
       It is declared before every row that MINTS one — §6.2's and §6.3's retrieve-multiple arms, reached from
       §4.5's and §4.6's members — because core/realm.h runs the per-realm installs in DECLARATION order and a
       snapshot built in a realm with no IDBRecord.prototype crashes at the mint. */
    { "idb_record",          d_idb_record,          NULL,        r_idb_record },
    { "indexed_db",          d_indexed_db,          NULL,        r_indexed_db },
    { "idb_database",        d_idb_database,        NULL,        r_idb_database },
    /* §2.7's TRANSACTION and §2.8's REQUEST, in that order because a request is placed against a transaction
       and §5.6 asserts its state before it makes one. Both are declared after §2.7's EventTarget above,
       because each interface prototype CHAINS to that one and core/realm.h builds the per-realm prototypes in
       declaration order — an interface declared first would chain to a prototype that did not exist yet.
       Neither has a document half: both install per REALM, since §3.7 gives every realm its own interface
       prototype and a C member answers in the realm that defined it. */
    { "idb_transaction",     d_idb_transaction,     NULL,        r_idb_transaction },
    { "idb_request",         d_idb_request,         NULL,        r_idb_request },
    /* §2.1.1's CONNECTION with §4.4's IDBDatabase over it, and §2.2.1's OBJECT STORE HANDLE with §4.5's
       IDBObjectStore. Both are declared AFTER §2.7's EventTarget (IDBDatabase inherits it) and after the two
       rows above, because core/realm.h runs the per-realm installs in DECLARATION order and each of these
       interfaces is reached from the ones before it. Both hold one agent-lifetime value — the private Symbol
       their internal slots hang off — which is what the release column is for. */
    { "idb_connection",      d_idb_connection,      NULL,        r_idb_connection },
    { "idb_object_store",    d_idb_object_store,    NULL,        r_idb_object_store },
    /* §2.6.1's INDEX HANDLE with §4.6's IDBIndex, after §2.2.1's because §4.6's `objectStore` answers with one
       and §4.5's `createIndex` returns one — and because core/realm.h runs the per-realm installs in
       DECLARATION order. It holds one agent-lifetime value, the private Symbol its slots hang off. */
    { "idb_index_handle",    d_idb_index_handle,    NULL,        r_idb_index_handle },
    /* §2.10's CURSOR with §4.9's IDBCursor and IDBCursorWithValue over it, after §2.2.1's and §2.6.1's
       handles because a cursor's SOURCE HANDLE is one of those two and §4.9's `source` answers with it — and
       because core/realm.h runs the per-realm installs in DECLARATION order. It holds one agent-lifetime
       value, the private Symbol §2.10's eleven fields hang off, plus §6.7's step machine. */
    { "idb_cursor",          d_idb_cursor,          NULL,        r_idb_cursor },
    /* §4.5's createIndex NOTE, as the request it says it is — "the index creation itself is processed as an
       asynchronous request within the upgrade transaction". It holds no interface and installs into no realm:
       what it declares is ONE step machine, which is agent-lifetime state and is what the release gives back.
       It is placed after the two rows above because it is reached from §4.5's member and it places its request
       through §5.6's, both of which are declared by then. */
    { "idb_index_populate",  d_idb_index_populate,  NULL,        r_idb_index_populate },
    /* §5.12's CREATING A REQUEST TO RETRIEVE MULTIPLE ITEMS, with §6.2's and §6.3's retrieve-multiple under
       it. Like the row above it holds no interface and installs into no realm: what it declares is ONE step
       machine, which is agent-lifetime state and is what the release gives back. It is placed after §2.2.1's
       and §2.6.1's handles because §4.5's and §4.6's members are what reach it, and after §2.12's IDBRecord
       row because its "record" arm mints one. */
    { "idb_get_all",         d_idb_get_all,         NULL,        r_idb_get_all },
    /* INPUT DEVICE CAPABILITIES §"The InputDeviceCapabilities interface", and it is HERE — one row
       AHEAD of `event` — for the reason that decides every position in this list. §"Extensions to the
       UIEvent interface and UIEventInit dictionary" makes it the TYPE of UIEventInit's
       `sourceCapabilities`, and the row below declares UIEvent and its three subclasses, each of
       which brands that member against this class AT ITS DECLARATION. A row after `event` would hand
       all four a class id of zero. It depends on nothing itself: its prototype chains to
       Object.prototype and its two attributes read its own slot record. */
    { "input_device_capabilities", d_input_device_capabilities, NULL,
                                                          r_input_device_capabilities },
    { "event",               d_event,               i_event },
    /* §4.2's IDBVersionChangeEvent, and it is HERE rather than beside the other Indexed Database rows for the
       one reason that decides every position in this list: its prototype chains to Event.prototype, which the
       row above builds, and core/realm.h runs the per-realm installs in declaration order. §5.1's own
       machinery follows it, because that is what FIRES three of these events — and because §5.1 registers the
       two rendezvous idb_connection and idb_transaction declare, so both must already exist. */
    { "idb_version_change_event", d_idb_vce,        NULL,        r_idb_vce },
    { "idb_open",            d_idb_open,            NULL,        r_idb_open },
    /* Declared by the components that own the events they carry; the interface objects are this realm's. */
    { "message_event",       NULL,                  i_message_event },
    { "error_event",         NULL,                  i_error_event },
    { "report_exception",    d_report_exception,    NULL },
    { "message_port",        d_message_port,        i_message_port, r_message_port },
    { "xml_http_request",    d_xhr,                 i_xhr,       r_xhr },
    /* FILE API §6 Reading Data, AFTER `xml_http_request` and not beside `blob`. Two rows decide it and
       both are the standards' own: §6.4.1 Event Summary makes every event this component fires a
       ProgressEvent, whose interface XHR §5 defines and whose class and per-realm prototype `xhr_init`
       declares — so a row before it would build a FileReader whose events had no interface. It reads §3's
       Blob (its byte sequence, its type and the source identity a File carries), which the `blob` row far
       above has already declared. */
    { "file_reader",         d_file_reader,         i_file_reader, r_file_reader },
    { "location",            d_location,            NULL,        r_location },
    /* §7.4.1's state machine BEFORE §7.2.5's History, whose every member reads the record it builds. */
    { "session_history",     d_session_history,     NULL,        r_session_history },
    { "history",             d_history,             NULL,        r_history },
    /* HTML §7.2.6's navigation API, AFTER §7.4.1's state machine whose entries it is a view over. */
    { "navigation",          d_navigation,          NULL,        r_navigation },
    /* HTML §7.2.6.10.4, which FIRES the navigate event at the Navigation the row above builds — its
       declaration is the step definition of the commit handler job, which the runtime must know before
       any navigation enqueues one. It installs no member of its own: the interfaces it builds objects of
       are `navigation_destination` above and NavigateEvent under `event`. */
    { "navigate_event_fire", d_navigate_event_fire, NULL,        r_navigate_event_fire },
    { "screen",              d_screen,              NULL,        r_screen },
    { "navigable",           d_navigable,           i_navigable },
    /* HTML §8.1.7's EVENT LOOP, before the task sources that are ordered by it: the virtual clock, §8.1.7.1's
       last render opportunity time and the insertion order a source breaks its ties by are the LOOP's, and
       they are per-flow heap state, so the record has to exist before any flow can write one. */
    { "event_loop",          d_event_loop,          NULL },
    { "timer",               d_timer,               i_timer,     r_timer },
    { "window_proxy",        d_window_proxy,        NULL,        r_window_proxy },
    { "remote_object",       d_remote_object,       NULL,        r_remote_object },
    /* The PEER's half of the same seam: what this agent does when it is ASKED to perform one. Its per-realm
       install captures %Reflect.set%/%Reflect.apply%, so it declares before any component whose install could
       run page code — which is none of them, and is why it sits beside the asking half rather than at the end. */
    { "remote_op",           d_remote_op,           NULL,        r_remote_op },
    /* HTML §7.2.4's CROSS-ORIGIN Location, AFTER window_proxy: §7.2.2's `location` member is what answers
       with one, and this row builds the per-realm surface that member hands across an origin boundary. */
    { "remote_location",     d_remote_location,     NULL,        r_remote_location },
    /* AFTER window_proxy: §9.4.4's `postMessage` is installed on the WindowProxy PROTOTYPE. */
    { "window_message",      d_window_message,      i_window_message, r_window_message },
    { "broadcast_channel",   d_broadcast_channel,   i_broadcast_channel, r_broadcast_channel },
    { "structured_clone",    d_structured_clone,    i_structured_clone, r_structured_clone },
    { "unhandled_rejection", d_unhandled_rejection, i_unhandled_rejection, r_unhandled_rejection },
    /* §8.12 Animation frames's map before §8.1.7.3 step 14 consumes it, and §7.4.6.3's reveal after Event. */
    { "animation_frame",     d_animation_frame,     i_animation_frame },
    { "page_reveal",         d_page_reveal,         i_page_reveal },
    { "viewport",            d_viewport,            NULL,        r_viewport },
    { "visual_viewport",     d_visual_viewport,     NULL,        r_visual_viewport },
    { "media_query_list",    d_media_query_list,    i_media_query_list },
    /* AFTER the three whose algorithms update-the-rendering's steps 8 and 10 are. */
    { "rendering",           d_rendering,           NULL,        r_rendering },
    /* DOM §3.2 Interface AbortSignal, MOVED AHEAD OF `fetch` and not tidied there. Fetch §5.4's
       RequestInit declares `AbortSignal? signal`, and a DECLARED interface type states the class it
       brands against AT THE DECLARATION — which `fetch` makes, since fetch_init declares Request. A row
       after `fetch` handed that declaration a class id of zero. It depends on nothing this list has not
       already built: abort_init allocates a symbol and two classes, and its per-realm install chains
       AbortSignal.prototype to EventTarget.prototype, whose row is far above both of these. */
    { "abort",               d_abort,               i_abort },
    { "fetch",               d_fetch,               i_fetch,     r_fetch },
    { "observable",          d_observable,          i_observable },
    /* GEOMETRY INTERFACES §3 and §4, before the component that returns one. Neither reads anything of the DOM's
       — a rectangle is four numbers — so their position is decided only by their CONSUMER: CSSOM VIEW §6's
       `getBoundingClientRect` is installed on Element.prototype by the row below, and it mints a DOMRect out of
       the element's own realm, so both prototypes must already be in every realm the list has built. */
    { "dom_rect",            d_dom_rect,            i_dom_rect,  r_dom_rect },
    { "dom_rect_list",       d_dom_rect_list,       i_dom_rect_list, r_dom_rect_list },
    { "element",             d_element,             NULL,        r_element },
    /* INTERSECTION OBSERVER, AFTER `element` and after the two GEOMETRY rows. After element because its
       declaration brands both `observe(Element target)` and §2.4's `(Element or Document)? root` against the
       Node class, which element_init is what creates (through node_init); after DOMRect because every entry it
       queues carries three of them, minted in the observed element's own realm. Its own prototype chains to
       Object.prototype, so no earlier row is required for that half. */
    { "intersection_observer", d_intersection_observer, i_intersection_observer, r_intersection_observer },
    { "html_iframe",         d_iframe,              NULL,        r_iframe },
    /* RFC 6265 §5.3's COOKIE STORE, before the component whose §3.1.4 members read it. It is the first row with
       a RELEASE, and the reason is the reason it is a row at all: the store is the USER AGENT's by the
       standard's own words and an instance is one origin-keyed agent cluster, so it belongs to the JSRuntime
       and not to any JSContext — which means nothing frees it when a realm goes. */
    { "cookie_jar",          d_cookie_jar,          NULL,        r_cookie_jar },
    { "document",            d_document,            i_document,  r_document },
    /* HTML §8.5.1's DOMParser, AFTER `document` — not because its install needs one (it puts an interface
       object on the global and nothing else) but because that is what this list's order MEANS: every member of
       this interface builds a second Document through core/dom/document.h's `document_new`, so the component
       that owns Documents is what it is declared against. Its own prototype chains to Object.prototype, so no
       earlier row is required for that half. */
    { "domparser",           d_domparser,           i_domparser, r_domparser },
    { "module_loader",       d_module_loader,       NULL,        r_module_loader },
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
    { "console",               "console" },
    { "window",                "window" },
    { "onload",                "event_target" },
    { "document",              "document" },
    { "navigator",             "navigator" },
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
    { "InputDeviceCapabilities", "input_device_capabilities" },
    { "Event",                 "event" },
    { "MessageEvent",          "message_event" },
    { "ErrorEvent",            "error_event" },
    { "MessageChannel",        "message_port" },
    { "XMLHttpRequest",        "xml_http_request" },
    { "FileReader",            "file_reader" },
    { "BroadcastChannel",      "broadcast_channel" },
    { "PromiseRejectionEvent", "unhandled_rejection" },
    { "PageRevealEvent",       "page_reveal" },
    { "AbortController",       "abort" },
    { "DOMRect",               "dom_rect" },
    { "DOMRectList",           "dom_rect_list" },
    { "IntersectionObserver",  "intersection_observer" },
    { "IntersectionObserverEntry", "intersection_observer" },
    /* WEB STORAGE's three names. §12.2.1's interface object, and §12.2.2's and §12.2.3's two Window members —
       and those two are the reason this row matters more than most: BOTH are on browser/platform_names.h, so
       the absent-global seam declines to mint a concolic for either, and a `window.localStorage` that resolves
       to nothing answers `undefined` rather than throwing. An install that silently stopped happening would
       therefore restore EXACTLY the silent defect this component was built to end — a `&&` taking its else arm
       with nothing to say so — which is what this probe now refuses. `storage_shed` has no witness because it
       installs nothing: Storage §4's model is state, not surface. */
    { "Storage",               "storage" },
    { "localStorage",          "window_storage" },
    { "sessionStorage",        "window_storage" },
    /* §12.2.4's interface object, whose component is `event` because core/events/event.c's subclass list is
       where every Event subclass is declared — the same row NavigateEvent's witness names. A `storage` event
       fired at a document that does not expose the interface would be an event whose `instanceof` a listener
       cannot make sense of. */
    { "StorageEvent",          "event" },
    /* WEB CRYPTOGRAPHY's three names — §10's interface object, §14's, and the Window member the mixin's
       partial declares. `crypto` is on browser/platform_names.h, so the absent-global seam declines to mint a
       concolic for it and a `window.crypto` that resolves to nothing answers `undefined` instead of throwing:
       an install that silently stopped happening would restore exactly the silent defect these witnesses
       exist to refuse. */
    { "Crypto",                "crypto" },
    { "SubtleCrypto",          "crypto" },
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
    { "Observable",            "observable" },
    { "DOMParser",             "domparser" },
};
static const int PLATFORM_WITNESS_N = (int)(sizeof PLATFORM_WITNESS / sizeof PLATFORM_WITNESS[0]);

/* THE AGENT IS ONE RUNTIME AND THE LIST RUNS ONCE IN IT. Every component's own `_init` already asserts it did
   not run twice; this asserts the same thing about the list, so the report names the LIST rather than whichever
   component happened to be first. */
static JSRuntime *g_declared_in;

/* IS THIS NAME A ROW? THE ONE ANSWER TO A QUESTION THREE OTHER LISTS ASK, and it exists because a row's name
   is written down in more than one place and every one of those places can be MISSPELLED. The witness table
   above names a component per global; core/agent_state.h's registry names a component per declared slot,
   twice over (a component naming its own row, and a SUB-component naming the row that releases it). None of
   those spellings is checked by the compiler, so each of them is one keystroke from naming a component this
   browser does not have — and in every case the damage is the same shape: the check that name was supposed to
   reach is not weakened, it is never run, while some OTHER row reports a failure whose words describe a
   different repair entirely. */
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
        /* A RELEASE WITHOUT A DECLARATION HAS NOTHING TO RELEASE. Agent state is made by the agent half, so a
           row that frees without declaring is either freeing another component's state or freeing nothing, and
           both are worse than either alone. */
        DCHECK(PLATFORM[i].release == NULL || PLATFORM[i].declare != NULL,
               "a platform component releases agent state it never declared — the release column is the inverse "
               "of the DECLARE column, and a row with only the inverse is undoing somebody else's work");
        for (k = 0; k < i; k++)
            DCHECK(strcmp(PLATFORM[k].name, PLATFORM[i].name) != 0,
                   "a platform component is listed twice — its declaration would mint a second class and a "
                   "second pool entry, and everything already chained to the first would answer out of an "
                   "object the realm has thrown away");
    }
    for (i = 0; i < PLATFORM_WITNESS_N; i++)
        DCHECKF(platform_has_row(PLATFORM_WITNESS[i].component),
                "the platform witness for `%s` names the component `%s`, and this list does not have it — the "
                "component was renamed or removed and the name it used to install is now owed by nobody, "
                "which is the state a stale exclusion rots into",
                PLATFORM_WITNESS[i].name, PLATFORM_WITNESS[i].component);
}

/* THE THIRD COLUMN'S OWN OTHER SIDE. The witness table above asserts what a component INSTALLS; this asserts
   what a component HOLDS, and it is the same argument: a release column that is only ever RUN is unfalsifiable,
   because a release that frees a value and keeps its handle looks exactly like one that works.
 *
 * TWO OF THE THREE DIRECTIONS ARE THE SAME MISTAKE SEEN FROM ITS TWO ENDS. A row with a release that declared
 * no agent state is a release nothing can check — and the four defects core/agent_state.h names were all found
 * by reading rather than by any detector, which is what "cannot be checked" costs. A row with NO release that
 * declared agent state is the other one, and it is the shape of every leak this file's comments record: a C
 * static held for the whole agent with an empty third column, reported eventually as an anonymous Function or
 * a directory entry with nothing naming its owner.
 *
 * THE THIRD IS THE ONE NEITHER OF THOSE CAN ASK, AND IT RUNS FIRST BECAUSE THE OTHER TWO ARE ONLY HONEST
 * AFTER IT. Both of them start from a ROW and ask the registry about the name on it, so both are structurally
 * blind to a declaration that names no row at all — and a component's name is written TWICE, once at each
 * `agent_state_*` call and once in the row above, with nothing but two authors' spelling between them. The
 * cost of a mismatch is not a check that got weaker. It is a check that is NOT RUN (nothing ever asks whether
 * anybody releases those slots) sitting behind a message that is WRONG (the row that does own them reports
 * "declared no agent state", which is character-for-character what a component that really declared nothing
 * produces). One answer, three states — the defect core/agent_state.h exists to refuse, arriving through the
 * NAME rather than through a value.
 * SO THE ORDER IS THE MECHANISM AND NOT A PREFERENCE: with the registry walk first, reaching the row loop at
 * all is a PROOF that every declaration in this agent names a row, and the row loop's two messages can say so
 * in their own words. Ambiguity is removed by making the ambiguous state unreachable before the ambiguous
 * sentence can be printed, never by adding an adjective to the sentence. */
static void platform_check_agent_state(void)
{
#if APICLIENT_DEV
    const char *component, *what;
    int i;

    for (i = 0; agent_state_slot(i, &component, &what); i++) {
        if (platform_has_row(component)) continue;
        DFAILF("`%s` declared %d slot(s) of agent state and THIS LIST HAS NO ROW OF THAT NAME; the first is "
               "%s. Nothing about those slots is checked less carefully — the question is never ASKED. The "
               "row pairing below can only ask \"does anybody RELEASE this?\" about a name a row carries, so "
               "for these it is not run at all, and the row that really owns them is left reporting "
               "\"declared no agent state\" in the exact words a component that declared nothing would use. "
               "A SUB-COMPONENT NAMES THE ROW THAT RELEASES IT, NEVER ITS OWN FILE: core/dom/selection.c "
               "declares under `document` and core/crypto/subtle_crypto.c under `crypto`, because that is "
               "whose release column their release is reached from. So this is one of two repairs, and they "
               "are not alike — either the owning row is misspelled at the declaration, or this component is "
               "declared and released by nobody and owes a row here (core/agent_state.h)",
               component, agent_state_count(component), what);
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
#endif
}

void platform_agent_init(JSContext *ctx, const char *origin, const char *top_level_url, bool requests_oac,
                         OpenerPolicyValue opener_policy)
{
    /* THE ONE CONSTRUCTION OF THIS VALUE IN THE PROGRAM — every fact this call was handed, and nothing that was
       not handed to it. A fact added to platform.h's parameter list and forgotten here is an unused-parameter
       in a nine-line function whose argument list is on the line above; a fact added to the STRUCT without a
       parameter to fill it cannot be reached from any host at all. */
    const PlatformAgent a = {
        .origin        = origin,
        .top_level_url = top_level_url,
        .requests_oac  = requests_oac,
        .opener_policy = opener_policy,
    };
    const PlatformAgent *agent = &a;
    int i;

    DCHECK(ctx != NULL, "the platform was declared into no realm");
    /* THE DECLARATION PASS BUILDS THE BROWSER'S BASELINE, so it runs at the BASELINE STAMP — and that is a
       precondition of THIS call rather than of any one component, which is the whole reason it is asserted
       here. Everything below allocates: every interface prototype, every interface object, every component's
       agent-lifetime JS state. An object created while the flow stamp is up carries that generation, and
       cow.c's capture test is `JS_ObjFlowGen(obj) > d->fork_gen` — the first flow's delta is calloc'd with
       fork_gen 0 and the first slice opens at generation 1, so anything stamped above 0 is skipped by EVERY
       delta and a later write to it by any flow is recorded nowhere. It is the same fact main.c asserts on the
       way OUT of qjs_step, said on the way IN to the platform.
       IT IS SILENT FOR MOST COMPONENTS AND SILENT-AND-WRONG FOR THE REST, which is why one component's own
       assert is not enough: nothing writes a prototype, so a mis-stamped one never shows; a component whose
       SHARED BASELINE STATE is a JS object (core/storage/storage_shed.c's sheds are the first) silently stops
       being shared across a fork instead. A host that reaches here with the stamp up is a host that forked a
       COW delta in its own time (solver/world.c's segment materialization is the way in) — the bump belongs
       to the slice bracket that owns the stamp, and cow_delta_fork now makes it. */
    DCHECK(JS_FlowGen() == 0,
           "the platform declaration pass began with the FLOW STAMP UP — every object this browser's baseline "
           "is made of would be stamped as belonging to a flow, and cow.c's `JS_ObjFlowGen(obj) > fork_gen` "
           "then skips it in every delta, so a component holding shared state in a JS object silently stops "
           "sharing it across a fork; whatever this host did before this call left a generation raised");
    DCHECK(agent->origin != NULL && *agent->origin,
           "an agent was brought up with no PRINCIPAL — §9.5's named bus is keyed by it and §7.2.1 decides "
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
    /* THE PRINCIPAL BECOMES A RECORD BEFORE ANY COMPONENT IS DECLARED, and it is not a row on the list because
       it is not a surface: no class, no member, nothing installed into a realm. It is the agent's ORIGIN
       (core/url/origin.h) — the value §7.2.1's filter, §7.3.2.1's inheritance and Storage's key are all decided
       against — and it must exist before the first declaration that asserts it does. ONE adopt per agent is
       also what gives an opaque principal its IDENTITY: the host states "null", and the nonce minted here is
       what every document of this agent then shares. */
    origin_agent_adopt(agent->origin);
    /* AND THE BROWSING CONTEXT GROUP, BEFORE THE CLUSTER, BECAUSE §8.1.2.2 TAKES IT AS AN ARGUMENT. Its
       signature is "obtain a similar-origin window agent, given an origin, a BROWSING CONTEXT GROUP and a
       boolean requestsOAC", and its step 3 reads that group's cross-origin isolation mode — so a group created
       after the cluster would be a group whose mode the allocation could not have seen. §7.3.2.3's group is
       the other half of the `(browsing context group, origin)` key SECURITY.md makes this instance, and it
       installs nothing, so like the cluster below it is not a row on the list. */
    browsing_context_group_create(agent->opener_policy);
    /* AND THE AGENT'S CLUSTER, IN THE SAME BREATH AND FOR THE SAME REASON. §8.1.2.2's obtain-a-similar-origin-
       window-agent is what ALLOCATES the agent this whole call is bringing up, and its key is derived from the
       origin adopted on the line above — so it is not a row on the list either: it installs nothing, and it has
       to exist before `originAgentCluster`, §7.1.1.2's `document.domain` setter or anything else can ask which
       cluster this agent is in. Every host reaches it here, which is what stops one host answering a question
       about a cluster that was never allocated. */
    agent_cluster_obtain_window_agent(origin_agent(), agent->requests_oac);
    for (i = 0; i < PLATFORM_N; i++)
        if (PLATFORM[i].declare)
            PLATFORM[i].declare(ctx, agent);
    platform_check_agent_state();
    /* THE AGENT'S FIRST REALM IS A REALM. Every per-realm intrinsic the components above declared is built
       here, through the same one call a child navigable's realm makes — so the first document cannot get a
       different set from the rest, which is the whole failure mode this file and core/realm.h exist to end. */
    realm_install_intrinsics(ctx, agent->top_level_url);
}

void platform_agent_free(void)
{
    int i;

    DCHECK(g_declared_in != NULL,
           "the platform was released in an agent that never declared it — there is nothing here to undo, and a "
           "host that reaches teardown without having reached platform_agent_init did not build this browser");
    /* REVERSE DECLARATION ORDER, because the forward order is dependency order: a component declared after
       another may hold a value that component minted, so it must give it up first. */
    for (i = PLATFORM_N - 1; i >= 0; i--)
        if (PLATFORM[i].release)
            PLATFORM[i].release(g_declared_in);
    /* THE ORIGINS GO LAST, AFTER EVERY COMPONENT THAT NAMES ONE. They are the agent's, not a realm's — a
       WindowProxy holds one as a POD pointer inside the bytes its COW delta captures — so the whole table is
       released here, once, when nothing is left that could read it. */
    agent_cluster_release();   /* the cluster names an origin, so it goes BEFORE the origins it named */
    /* AND THE GROUP AFTER THE CLUSTER, which is the reverse of the creation order above: §8.1.2.2
       allocated the cluster FROM the group's mode, so the group is what the cluster was derived from and
       goes second. */
    browsing_context_group_release();
    origin_release();
    /* EVERY DECLARED SLOT IS BACK WHERE A FRESH PROCESS WOULD HAVE FOUND IT, asserted here because here is the
       last instant at which the question has an answer: after this the agent is gone and the next reader of a
       stale handle is a SECOND agent's `_init`, which consults it precisely to decide that it need not run. */
    agent_state_check_released();
    agent_state_reset();
    g_declared_in = NULL;
}

void platform_document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                               const char *origin, SerializedPolicyContainer policy, SandboxFlags sandbox_flags,
                               uint32_t doc_id, JSValueConst nav_proxy)
{
    /* THE ONE CONSTRUCTION OF THIS VALUE IN THE PROGRAM — see platform_agent_init above for why it is here and
       not in the three hosts that used to write these seven assignments out by hand. */
    const PlatformDocument d = {
        .dom           = dom,
        .url           = url,
        .origin        = origin,
        .policy        = policy,
        .sandbox_flags = sandbox_flags,
        .doc_id        = doc_id,
        .nav_proxy     = nav_proxy,
    };
    const PlatformDocument *doc = &d;
    int i;

    DCHECK(ctx != NULL, "a document was installed into no realm");
    DCHECK(JS_IsObject(global), "a document was installed on something that is not the global object");
    DCHECK(doc->dom != NULL, "a document was installed with no parsed tree — `document` is a wrapper over one");
    DCHECK(doc->url != NULL && *doc->url,
           "a document was installed with no ADDRESS — a document is loaded FROM somewhere, and §4.4's API "
           "base URL is what every relative URL the page builds resolves against");
    DCHECK(doc->origin != NULL && *doc->origin,
           "a document was installed with no PRINCIPAL — every same-origin check compares it");
    /* THE TWO FACTS ARE TWO ARGUMENTS, which is the whole of the fix for them: a host with one of them passed
       whichever it had, and the address is the one that decides where `fetch("api/users")` goes.
       AND THEY ARE NOW COMPARED, BY §7.3.2.1 "Creating browsing contexts"'s OWN STEP 5, WHICH IS NOT
       THE COMPARISON THIS SITE USED TO SAY COULD NOT BE MADE. The reason recorded here was that "a document
       AT the origin root legitimately has both the same" — which argues against comparing the ADDRESS to the
       PRINCIPAL as strings, a comparison nobody would write. The one §7.3.2.1 "Creating browsing contexts"
       defines is between the principal and the address's ORIGIN ("return url's origin"), it is well defined
       for every Document, and it was the assertion the prose stood in for.
       THREE STEPS LEGITIMATELY BREAK IT AND EACH IS EXCLUDED BY ITS OWN FACT RATHER THAN BY A TOLERANCE.
       Steps 3 and 4 INHERIT (`about:srcdoc` always, `about:blank` when there is a source origin), so a
       document whose address is one of those two carries its SOURCE's principal and the address says nothing
       about it. Step 1 mints a fresh OPAQUE origin for a sandboxed document, and step 2 does the same for a
       null URL; both serialize to `null`, which is §7.1.1's "no serialization it can be recreated from" and is
       therefore unconstrained by any address. Everything else is step 5, where the principal IS the address's
       origin — including `data:` and `file:`, whose origins are opaque on BOTH sides of this comparison and so
       agree as `null`.
       WHAT IT CATCHES IS A PRINCIPAL STATED BEFORE THE BYTES WERE FETCHED. A peer instance is provisioned from
       the origin a `navigable.create` notice carried, which core/frame/navigable.c derives from the REQUEST
       url; that instance runs platform_agent_init and adopts it BEFORE anything is fetched, and its root
       Document then arrives as bytes the host fetched. If that fetch REDIRECTED off-origin, the peer holds a
       principal its own response contradicts — and it is wrong ONCE and CONSISTENTLY, so origin_agent_adopt's
       one-adopt-per-agent assert cannot see it and neither can any same-origin check inside that heap. §7.4.5
       determines a navigation's origin from the RESPONSE's URL for exactly this reason; this is that sentence
       asked of the one value every Document in this engine is installed with. */
    /* GUARDED BECAUSE THE WORK IS THE CHECK. A DCHECK's condition is compiled out in release, and this one's
       inputs are a URL PARSE and an origin SERIALIZATION — real allocation, per document — so leaving them
       outside the guard would make a release build pay for an assertion it does not make. That is the reason
       for the block rather than a side-effect-free condition: §7.3.2.1 step 5 cannot be asked without running
       §4.4's parser, and a parse is not something a DCHECK may contain. */
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
