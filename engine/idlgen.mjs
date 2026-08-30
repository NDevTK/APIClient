/* Web IDL gap AUDITOR — the IDL's real job: tell us what browser logic is MISSING, not generate stub bindings.
 * For each interface we implement, it reads the canonical .idl (@webref/idl, the W3C-curated corpus browsers
 * use), parses it (webidl2), flattens inherited + mixin members, and DIFFS the spec member list against the
 * members the component actually installs (a scan of the component .c for the property names it wires). It
 * prints the missing members so we implement them at the root — never a generated noop/DCHECK stub.
 *
 * IT IS A BUILD STAGE AND IT FAILS. It ran at build time nowhere at all — nothing in the tree invoked it, so
 * every component's member gap was unmeasured while `node engine/build.mjs` reported a complete-looking total.
 * "Best-effort, skipped if the toolchain isn't installed" is what stood here, and it was stale twice over:
 * @webref/idl and webidl2 are DECLARED devDependencies of this package and are installed, and a gate that
 * skips itself when its input is absent is the excluded check §Testing forbids. So the exit code is the
 * verdict — a spec member no component installs is a gap to implement at the root, and the run is RED until
 * it is. There is no baseline file, no allowlist, no threshold and no --warn-only: every one of those is a
 * number about nothing, and the honest first count is the measurement rather than a reason to soften it.
 *
 * WHAT A COMPONENT INSTALLS IS READ FROM THE INSTALL CONSTRUCTS THEMSELVES (engine/idl_installed.mjs), never
 * from the file's text. It used to be a scan for any string literal, which made the audit lie in both
 * directions and made it IMPOSSIBLE TO EXTEND: naming html_form.c, input_value.c or constraint_validation.c in
 * the map below would have credited every content-attribute name in them ("required", "pattern", "min",
 * "step") as an installed member, turning a false ABSENT into a false COMPLETE — so those rows had to be
 * withheld, and an auditor a row can make lie is an auditor whose silence means nothing. An install construct
 * whose member name cannot be resolved statically is reported UNRESOLVED with its file and line rather than
 * being dropped (a gap that is not there) or counted (a gap that is filled and is not).
 *
 * AND EACH MEMBER IS FILED UNDER THE INTERFACE ITS TARGET IS, not under the file it was written in. The audit
 * was FILE-granular one level below the construct scan: a row credited every member any of its files installed,
 * so html_form.c's `value` — installed on HTMLTextAreaElement.prototype — counted for HTMLInputElement, and
 * element_internals.c could not be named in a row at all because its `validity`, `labels` and `form` land on
 * ElementInternals. Web IDL §3.7.3 already makes every interface prototype object carry the interface's
 * identifier as its @@toStringTag, so the fact is in the components; idl_installed.mjs solves which object each
 * install lands on and reads the tag off it. A row's FILE LIST is therefore no longer what decides the count —
 * it is a CROSS-CHECK, and a row naming a file whose members all land on other interfaces now says so. An
 * install whose target's interface cannot be decided is UNATTRIBUTED: named with file, line and member, never
 * credited back to its file, which is the false COMPLETE this exists to remove.
 *
 * IT ALSO GENERATES THE ONE THING THE IDL — AND ONLY THE IDL — CAN ANSWER: WHICH GLOBAL NAMES THE PLATFORM OWNS.
 * absent.c has to tell a Web API this engine has not built (whose ReferenceError is the forcing function that
 * names the next component) from server-injected app state (which is unknown INPUT and must fork). That question
 * is decided by whether the name is exposed on Window, which is exactly what `[Exposed=Window]` says. It had been
 * decided by a 22-name list typed into main.c, and every real interface off that list — Node, Element, Event,
 * DOMException, HTMLElement — was mistaken for app state: a branch on it FORKED instead of throwing, so a page
 * that touches eight of them multiplies the frontier by 256 and a WPT document exhausted 2.8 GB in 40 seconds.
 * A list a person maintains cannot be right about a surface of 1500 names.
 *
 * The generated header is COMMITTED, because the build must work with no network and the platform table is not
 * optional the way the audit is. THE AUDIT RUN NEVER WRITES IT: a build that rewrites a committed source is
 * §Testing's gate measuring a tree that no longer exists, and the checkout is shared, so a build stage editing
 * absent.c's input under another lane's compile is the loaded-machine defect with a generator behind it. A
 * checked-in table that no longer matches the corpus is therefore a FAILING category like any other gap, and
 * `node engine/idlgen.mjs --regen` is the one command that writes it. */
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { loadEnvironment, installedMembers } from "./idl_installed.mjs";
import { loadIdl, windowGlobals, iterationMembers } from "./idl_members.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const HOST = join(HERE, "host");
const BROWSER = join(HOST, "browser");

const REGEN = process.argv.includes("--regen");

/* THE VERDICT IS A LEDGER OF CATEGORIES, NOT A TOTAL. §Testing: one number in which one area answers most of
   the corpus makes every other component invisible, and that is exactly what a single "N members missing"
   would be here — the ABSENT count is dominated by whichever interface inherits the widest base. So every
   category this run can report counts SEPARATELY, the per-interface table below names the areas, and the exit
   code is non-zero if ANY category is non-empty. What is NOT a defect is stated by declaration rather than by
   omission: an interface declared UNBUILT with its reason, and a member declared CONDITIONAL by
   idl_members_excluded, are the two accepted steady states — and both are checked from the other side, so a
   declaration that has gone stale is itself a category. */
const defects = new Map();
const defect = (kind, n = 1) => { if (n) defects.set(kind, (defects.get(kind) || 0) + n); };
const gapRows = [];

// interface -> the component .c that implements it, or the componentS: one interface's surface can be split
// across components when the spec puts unrelated capabilities on one object. Window is the case that forces it —
// its members are installed by window.c (browsing context), document.c, location.c, fetch.c and event_target.c.
// SINCE ATTRIBUTION, THIS LIST NO LONGER DECIDES WHAT COUNTS: a member belongs to the interface its target is
// tagged with, wherever that is written, so a missing file can no longer make a shipping member read ABSENT.
// What the list still says is which components this interface is BELIEVED to be built out of, and the audit
// checks that belief: a file here whose members all land on other interfaces is reported (CROSS-CHECK), and the
// UNRESOLVED and UNATTRIBUTED constructs of these files are the ones reported against this row. The many
// comments below explaining why a base's or a mixin's file had to be named are now history rather than
// mechanism — inheritance is read from the IDL, and a mixin's members are tagged with the interface they are
// installed on.
/* Every HTML interface is built on the same four files: its own table in html_element.c, the reflection and
   attribute machinery in element.c, the Node base, and EventTarget. */
const HTML_BASE = ["core/html/html_element.c", "core/dom/element.c", "core/dom/node.c",
                   "core/events/event_target.c", "core/css/css_style_declaration.c"];

const INTERFACES = {
  EventTarget:          "core/events/event_target.c",
  AbortSignal:         ["core/dom/abort.c", "core/events/event_target.c"],
  AbortController:      "core/dom/abort.c",
  IntersectionObserver: "core/intersection_observer/intersection_observer.c",
  /* INTERSECTION OBSERVER §2.3. Its own component, in the same directory: it is its own interface with its own
     constructor and its own dictionary, and the rectangles on it belong to Geometry Interfaces §3 again. */
  IntersectionObserverEntry: "core/intersection_observer/intersection_observer_entry.c",
  MutationObserver:     "core/dom/mutation_observer.c",
  ResizeObserver:       "core/resize_observer/resize_observer.c",
  PerformanceObserver:  "core/timing/performance_observer.c",
  /* An interface that includes the BODY mixin has its readers and `bodyUsed` installed by the shared
     component, so body.c is where the audit finds them — naming only the interface's own file reported six
     members absent that both including interfaces have. */
  /* byte_reader.c IS NOT IN THESE FOUR ROWS, and the run is what settled it: it installs nothing statically
     nameable and declares no interface. Its `JS_SetPropertyStr(ctx, proto, d->readers[k].name, …)` reads the
     name off a table a RUNTIME handle picks, so the audit reads the two-halved form's DECLARE site instead
     (TABLE_FORMS' byte_reader_declare/byte_reader_install pair) and emits the members at the INSTALL CALL in
     blob.c and body.c, attributed to the prototype those files hand it. Naming the shared machine here
     therefore credited nothing and only made four rows report a stranger. */
  Response:            ["core/fetch/response.c", "core/fetch/body.c"],
  Request:             ["core/fetch/request.c", "core/fetch/body.c"],
  FormData:            ["core/html/form_data.c", "core/idl_iter.c"],
  /* §4's `interface File : Blob` shares blob.c with the interface it inherits: one struct, one class id, and
     a prototype chained to Blob.prototype — which is what the inheritance MEANS, so the members it inherits
     are found in the same file. */
  /* HTML 9.4.1. Its two type-constrained members (source, ports) name interfaces that do not exist yet, so the
     audit is what will notice the day they do and this file still answers only their empty value. */
  MessageEvent:        ["core/events/message_event.c", "core/events/event.c"],
  /* UI Events §3.2.1 and §3.3.1, and the two interfaces that share UIEvent's dictionary levels. Each names its
     own file plus everything it INHERITS — a member installed on UIEvent.prototype really is reachable on a
     FocusEvent, so reporting it absent would be the audit lying in the direction that gets a real gap ignored.
     They had no row at all, which is the audit lying by OMISSION: an interface nobody audits is an interface
     whose gaps are not zero but unreported. ui_event.c carries the four modifier attributes and
     getModifierState that MouseEvent and KeyboardEvent declare over its shared key modifier state. */
  /* INPUT DEVICE CAPABILITIES §"The InputDeviceCapabilities interface" — the TYPE of the one member that
     specification adds to UIEvent, and therefore the reason FOUR rows below it reported `sourceCapabilities`
     absent from one gap. It names only its own file: its prototype chains to Object.prototype, so it inherits
     nothing an audited component installs. */
  InputDeviceCapabilities: "core/events/input_device_capabilities.c",
  UIEvent:             ["core/events/ui_event.c", "core/events/event.c"],
  FocusEvent:          ["core/events/focus_event.c", "core/events/ui_event.c", "core/events/event.c"],
  MouseEvent:          ["core/events/mouse_event.c", "core/events/ui_event.c", "core/events/event.c"],
  KeyboardEvent:       ["core/events/keyboard_event.c", "core/events/ui_event.c", "core/events/event.c"],
  /* HTML §7.2.7.2 and §7.2.7.3 — the two events a SESSION HISTORY TRAVERSAL fires. Each names its own file plus
     event.c, whose Event.prototype members really are reachable on one. */
  PopStateEvent:       ["core/events/pop_state_event.c", "core/events/event.c"],
  HashChangeEvent:     ["core/events/hash_change_event.c", "core/events/event.c"],
  /* HTML §12.2.4 — the event HTML §12.2.1's broadcast fires. Its own file plus event.c, whose Event.prototype
     members really are reachable on one. */
  StorageEvent:        ["core/events/storage_event.c", "core/events/event.c"],
  /* HTML 9.4.2/9.4.3. MessagePort is an EventTarget, so its inherited members are event_target.c's. */
  MessagePort:         ["core/events/message_port.c", "core/events/event_target.c"],
  MessageChannel:       "core/events/message_port.c",
  /* HTML 9.5. A BroadcastChannel is an EventTarget, so its inherited members are event_target.c's. */
  BroadcastChannel:    ["core/events/broadcast_channel.c", "core/events/event_target.c"],
  /* XHR §3 and §5. XMLHttpRequest's file list carries event_target.c because its prototype chain reaches
     EventTarget.prototype through XMLHttpRequestEventTarget — the seven handler attributes and the three
     listener members really ARE reachable on one, so reporting them absent would be the audit lying. */
  XMLHttpRequest:       ["core/xhr/xml_http_request.c", "core/events/event_target.c"],
  XMLHttpRequestUpload: ["core/xhr/xml_http_request.c", "core/events/event_target.c"],
  XMLHttpRequestEventTarget: ["core/xhr/xml_http_request.c", "core/events/event_target.c"],
  ProgressEvent:       ["core/xhr/progress_event.c", "core/events/event.c"],
  TextEncoder:          "core/encoding/encoding.c",
  TextDecoder:          "core/encoding/encoding.c",
  /* §7.5 and §7.6 include the GenericTransformStream mixin, so `readable` and `writable` are members the audit
     expects — they are installed by this component onto its own two prototypes, not inherited. */
  TextDecoderStream:    "core/encoding/text_stream.c",
  TextEncoderStream:    "core/encoding/text_stream.c",
  /* §4.2.4's ReadableStreamPipeTo belongs to neither half of the standard — it holds a reader on one stream
     and a writer on another — so `pipeTo` and `pipeThrough` are declared in their own component and installed
     onto this prototype. Naming only readable_stream.c reported them absent while they were shipping. */
  ReadableStream:      ["core/streams/readable_stream.c", "core/streams/pipe.c"],
  ReadableStreamDefaultReader: "core/streams/readable_stream.c",
  WritableStream:       "core/streams/writable_stream.c",
  WritableStreamDefaultWriter: "core/streams/writable_stream.c",
  WritableStreamDefaultController: "core/streams/writable_stream.c",
  TransformStream:      "core/streams/transform_stream.c",
  TransformStreamDefaultController: "core/streams/transform_stream.c",
  Blob:                 "core/file/blob.c",
  File:                 "core/file/blob.c",
  /* File API §6.2 The FileReader API. Its six event handler IDL attributes are §6.2.1's, declared ON this
     interface, and the three members it INHERITS from EventTarget come off the shared component — the same
     rule Node's and VisualViewport's rows state. §6.5.1's FileReaderSync has no row because no component
     declares it: its IDL is `[Exposed=(DedicatedWorker,SharedWorker)]` and this engine has no worker global,
     so it is honestly absent rather than unbuilt, and file_reader.c's own header says so beside the algorithm
     (§6.3's package data) that a worker exposure would reach it through. */
  FileReader:          ["core/file/file_reader.c", "core/events/event_target.c"],
  /* File System Standard §2.2-§2.4. These three shipped with no row at all, so the audit said nothing about
     them in either direction — the lying-by-omission this map's own comment names. Each names the component
     plus File System Access §2.3's partial interface (queryPermission/requestPermission), whose members land
     on the prototype file_system_handle.c builds.
     §2.4.1's `async_iterable<USVString, FileSystemHandle>` is bound by the SHARED default asynchronous
     iterator object, so idl_async_iter.c is where the audit finds `entries`, `keys` and `values` — naming only
     the component would report three members absent that the interface has. */
  FileSystemHandle:          ["core/file/file_system_handle.c", "core/file/file_system_access.c"],
  FileSystemFileHandle:      ["core/file/file_system_handle.c", "core/file/file_system_access.c"],
  FileSystemDirectoryHandle: ["core/file/file_system_handle.c", "core/file/file_system_access.c",
                              "core/idl_async_iter.c"],
  /* INDEXED DATABASE §4.7 and §4.3. Both are rows from the day their components land, because an interface with
     no row is the lying-by-omission this map's own comment names — and here the omission would hide exactly the
     gap that matters: IDBFactory ships `open` and `cmp` and NOT `deleteDatabase` or `databases()`, each of
     which is a whole algorithm of §5 rather than a member. A row is what makes that count a number rather
     than a sentence in a comment somebody has to keep true. */
  /* WEB CRYPTOGRAPHY §10 and §14. Both are rows from the day their components land, and the gap each one
     reports is the plan rather than the neglect. §10 IS COMPLETE — `subtle`, §10.1.1's `getRandomValues` and
     §10.1.2's `randomUUID` — and the two that used to be listed here as pending an answer to "what does
     randomness mean for a solver whose flows resume byte-identically" have that answer now; it is argued in
     core/crypto/crypto.h rather than restated here, because a reason a reader can check belongs beside the
     code it decided and not in the auditor's map. SubtleCrypto ships `digest` and none of the rest, every one
     of which needs §13's CryptoKey model first. Without rows the audit would report both interfaces as
     nothing at all rather than as a member list with a reason — and the list's LENGTH is deliberately not
     written here: it is what the row prints, it changed under this comment once already when the modern-algos
     IDL added the encapsulation members, and a count in prose is the one thing a row makes redundant. */
  Crypto:               "core/crypto/crypto.c",
  SubtleCrypto:         "core/crypto/subtle_crypto.c",
  IDBKeyRange:          "core/indexeddb/idb_key_range.c",
  IDBFactory:           "core/indexeddb/indexed_db.c",
  /* §4.1's IDBRequest and §4.10's IDBTransaction. Each inherits EventTarget, so event_target.c is named
     beside it for the three members it contributes AND for the event handler IDL attributes, which live on
     that component's one X-list — naming only the component would report `onsuccess`, `onerror`,
     `onabort` and `oncomplete` as absent when they are installed.
     THE ROWS ARE WHAT MAKE THE GAP A NUMBER. IDBTransaction now ships `db` and `objectStore()` beside `mode`,
     `durability`, `error`, `commit()` and `abort()`; the one member it does not is `objectStoreNames`, which
     needs a DOMStringList. Without a row the audit would report the interface as nothing at all rather than
     as one member to build. */
  IDBRequest:          ["core/indexeddb/idb_request.c", "core/events/event_target.c"],
  IDBTransaction:      ["core/indexeddb/idb_transaction.c", "core/events/event_target.c"],
  /* §4.1's second interface, §4.4, §4.5 and §4.2. IDBOpenDBRequest declares only its two event handler IDL
     attributes and INHERITS the rest, so the flattened member list is IDBRequest's plus two and every one of
     the inherited members is installed by the file that builds the prototype it chains to.
     THE ROWS ARE WHAT MAKE THE REMAINING GAP A NUMBER, and it is a specific one: IDBDatabase ships `name`,
     `version`, `close()`, `createObjectStore()`, `deleteObjectStore()` and `onversionchange` and NOT
     `objectStoreNames` (a DOMStringList, which this engine does not have) or `transaction()` (Web IDL
     §3.2.25's `(DOMString or sequence<DOMString>)` union, whose resolution reads @@iterator off the page's
     value and so must be a request); IDBObjectStore ships `name`, `keyPath`, `transaction`, `autoIncrement`,
     `put()`, `add()`, `get()` and `getKey()` and NOT the index members (§2.6 does not exist) or the members
     whose §6 algorithm does not (`delete`, `clear`, `count`, the four getAll* and the two cursor openers). */
  IDBOpenDBRequest:    ["core/indexeddb/idb_request.c", "core/events/event_target.c"],
  IDBDatabase:         ["core/indexeddb/idb_connection.c", "core/events/event_target.c"],
  IDBObjectStore:       "core/indexeddb/idb_object_store.c",
  IDBVersionChangeEvent: "core/indexeddb/idb_version_change_event.c",
  /* Headers exists and had no row, so the audit said nothing about it at all — which is the lying-by-omission
     this map's own comment names, and it was silent from the moment the component landed. */
  /* An `iterable<>` interface's keys/values/entries/forEach are installed by the SHARED default iterator
     object, so idl_iter.c is where the audit finds them — naming only the component would report four
     members absent that every such interface has. */
  Headers:             ["core/fetch/headers.c", "core/idl_iter.c"],
  Notification:         "modules/notification.c",
  /* THE GLOBAL ITSELF — the interface a page touches before any other. §7.2.5's members are spread over the
     components that own them: the browsing-context half, §7.4's `open` (which lives with the navigable it
     creates, not with the Window it hangs off), the six BarProps, the event-loop timers, §9.4.4's postMessage,
     and EventTarget, which Window inherits. Its ABSENT list is long, and that is the measurement rather than a
     reason not to take it.
     THIS ROW WAS WRITTEN TWICE, and a duplicate key in an object literal is not a merge — the second one
     REPLACED the first, silently, so fetch.c, module_loader.c, abort.c and the CSSOM were dropped out of the
     row that ran and `fetch`, `structuredClone` and `AbortController` read as ABSENT while they were shipping.
     One row, the union of both. */
  /* module_loader.c IS NOT IN THIS ROW, and the cross-check is what settled it: it installs no member on
     anything — §8.1.3's dynamic `import()` and `import.meta` are ECMAScript syntax, not Web IDL members of
     Window — so the row's belief that Window is partly built out of it was false. It came in with the union
     above and nothing ever read it. */
  Window:              ["core/frame/window.c", "core/dom/document.c", "core/frame/location.c",
                        "core/fetch/fetch.c", "core/events/event_target.c",
                        "core/timing/timer.c", "core/frame/navigator.c", "core/frame/screen.c",
                        "core/dom/abort.c", "core/css/css_style_declaration.c",
                        "core/frame/navigable.c", "core/frame/bar_prop.c",
                        "core/frame/window_message.c", "core/structured_clone.c",
                        /* CSSOM VIEW §4's Window extensions — innerWidth/innerHeight, outerWidth/outerHeight,
                           scrollX/pageXOffset, scrollY/pageYOffset, screenX/screenLeft, screenY/screenTop and
                           devicePixelRatio — live with the viewport they read, not with the Window they hang
                           off, for the per-realm reason viewport.h gives. */
                        "core/frame/viewport.c", "core/frame/visual_viewport.c",
                        /* §7.2.6.2's `[Replaceable] readonly attribute Navigation navigation`, installed
                           with the object it answers with rather than with the Window it hangs off. */
                        "core/frame/navigation.c",
                        /* §7.1.2's `originAgentCluster` and §8.1.7.1's `crossOriginIsolated` — two answers
                           about one agent cluster, installed by the component that computes it because
                           §7.1.1.2's `document.domain` setter reads the same fact. */
                        "core/frame/agent_cluster.c",
                        /* Indexed Database §4.3's `[SameObject] readonly attribute IDBFactory indexedDB`, a
                           member of the WindowOrWorkerGlobalScope mixin Window includes — installed with the
                           component that builds the object it answers with, the same rule navigation.c's
                           entry above states. */
                        "core/indexeddb/indexed_db.c",
                        /* HTML §12.2.2's `sessionStorage` and §12.2.3's `localStorage` — the WindowSessionStorage
                           and WindowLocalStorage mixins Window includes. They are installed by the component
                           that owns their algorithm rather than by the Window they hang off, the same rule
                           `navigation` and `indexedDB` above state. */
                        "core/storage/window_storage.c"],
  /* HTML §8.10.1, plus the `partial interface Navigator`s other standards declare on it. Beacon §2.1's
     `sendBeacon` is its own component for the reason navigator_beacon.h gives — it is a REQUEST and not a
     client-identity value — so this row names it beside the interface's own file, the same rule Window's row
     states for `navigation`, `indexedDB` and the two storages. */
  Navigator:           ["core/frame/navigator.c", "core/frame/navigator_beacon.c"],
  /* CSSOM VIEW §12. Its own seven attributes are its file's; the three event handler IDL attributes it
     declares (`onresize`, `onscroll`, `onscrollend`) and the three members it INHERITS from EventTarget come
     off the shared component, which is the same rule Node's row states. */
  VisualViewport:      ["core/frame/visual_viewport.c", "core/events/event_target.c"],
  /* HTML §6.4.4. The interface had no row at all, so the audit said nothing about it — and its two getters and
     the object Navigator's `userActivation` answers with live with §6.4's state rather than with the Navigator
     that exposes them, which is the file named here. */
  UserActivation:       "core/html/user_activation.c",
  History:              "core/frame/history.c",
  /* HTML §7.2.6, the navigation API. Its entry list is a view over §7.4.1's session history entries, and the
     three fields it reads off one (their navigation API state, key and id) belong to the ENTRY — so
     session_history.c is named beside it, the way node.c's row names the mixin it also installs. The
     `dispose` and `currententrychange` event handler IDL attributes come off the shared EventTarget
     component, which is the same rule VisualViewport's row states. */
  Navigation:               ["core/frame/navigation.c", "core/events/event_target.c"],
  NavigationHistoryEntry:   ["core/frame/navigation_history_entry.c", "core/events/event_target.c"],
  NavigationCurrentEntryChangeEvent: "core/events/navigation_current_entry_change_event.c",
  /* §7.2.6.10.1's event and §7.2.6.10.3's destination — BUILT. NavigateEvent's row names event_target.c too
     because §7.2.6.10.1 inherits Event, whose `composedPath` and the rest come off the shared component, the
     same rule VisualViewport's row states. */
  NavigateEvent:            ["core/events/navigate_event.c", "core/events/event.c"],
  NavigationDestination:    "core/frame/navigation_destination.c",
  /* §7.2.6.10.2 and the two §7.2.6.8/§7.2.6.9 objects, DECLARED UNBUILT below and given the paths their
     components will take — which is what makes the declaration checkable from both sides: the day one of these
     files exists, the audit reports the exemption as STALE instead of going on believing it. */
  NavigationPrecommitController: "core/frame/navigation_precommit_controller.c",
  NavigationTransition:     "core/frame/navigation_transition.c",
  NavigationActivation:     "core/frame/navigation_activation.c",
  Screen:               "core/frame/screen.c",
  /* HTML §7.2.4. The interface had no row at all, so the audit said nothing about it — its setters, `assign`,
     `replace`, `reload` and `ancestorOrigins` were not reported as zero gaps, they were not reported.
     Its members are NOT on Location.prototype and that is the spec, not a shortcut: every one of them is
     [LegacyUnforgeable], and Web IDL §3.7.6 REMOVES the unforgeable attributes from the interface prototype
     object and puts them on the object itself. Attribution follows the instance the component builds over the
     prototype it tagged, which is the same edge that files MessageChannel's `port1`/`port2`. */
  Location:             "core/frame/location.c",
  URL:                  "core/url/url.c",
  URLSearchParams:     ["core/url/url_search_params.c", "core/idl_iter.c"],
  /* CSSOM §6.6.1's two. CSSStyleDeclaration is the block's own eight members and nothing is an instance of it;
     CSSStyleProperties CHAINS to it and carries `cssFloat` plus the per-property attributes of §6.6.1's three
     partial interfaces, which are installed from LEXBOR'S REGISTRY in a loop — read by idl_installed.mjs's
     GENERATED_FORMS out of the same array the component indexes at run time.
     NEITHER ROW CAN EVER REPORT ONE OF THOSE ATTRIBUTES ABSENT, AND THAT IS A FACT ABOUT THE IDL RATHER THAN
     ABOUT THIS ENGINE: §6.6.1 spells the three partials with the placeholder members `_camel_cased_attribute`,
     `_webkit_cased_attribute` and `_dashed_attribute`, each standing for a hundred names rather than naming
     one, and @webref/idl carries none of the three — so the flattened member list here is `cssFloat` and the
     base's eight, full stop. A gap in that surface is found by READING §6.6.1, never by working this audit's
     list; two of the three partials were missing entirely while both rows reported nothing at all. What each
     row is EXPECTED to report missing is therefore: nothing. */
  CSSStyleDeclaration:  "core/css/css_style_declaration.c",
  CSSStyleProperties:   "core/css/css_style_declaration.c",
  /* CSSOM §6.1's two. StyleSheet is a base nothing instantiates, so its six members live on their own
     prototype and CSSStyleSheet.prototype CHAINS to it — both rows name the one component, and reporting
     StyleSheet's members absent from CSSStyleSheet would be the audit lying by direction. What each row is
     EXPECTED to report missing is real: StyleSheet's `media` — §4.4's MediaList is built now, but a SHEET's
     media list is the `media` content attribute of the `<style>` or `<link>` that created it, which HTML
     §4.2.6 hands to the create and this engine does not carry — and CSSStyleSheet's `replace`, `replaceSync`,
     `rules`, `addRule` and `removeRule`. `cssRules`, `insertRule` and `deleteRule` are BUILT and are off that
     list rather than still named. */
  StyleSheet:           "core/css/css_style_sheet.c",
  CSSStyleSheet:        "core/css/css_style_sheet.c",
  /* CSSOM §4.4. Its collection of media queries is a JS Array of the per-query serializations, so `length`
     and `item` are reads of it and `mediaText` is §4.2's serialize-a-media-query-list over it. */
  MediaList:            "core/css/media_list.c",
  /* CSSOM §6.2.2. §6.2.3's `styleSheets` is installed by this same component onto Document.prototype and
     ShadowRoot.prototype, so Document's row names it too. ShadowRoot has no row at all — it is one of the
     interfaces this map is still silent about, which is the audit lying by omission rather than by direction,
     and it is not this change's to decide. */
  StyleSheetList:       "core/css/style_sheet_list.c",
  /* CSSOM §6.4 and CSS Conditional §7.2/§7.3/§7.4. SIX interfaces, ONE component: CSSRule, CSSGroupingRule and
     CSSConditionRule are abstract bases nothing instantiates, so their members live on their own prototypes
     and the three concrete ones CHAIN through them — every row names the one component, because reporting an
     inherited member absent from the interface that really has it would be the audit lying by direction.
     What is EXPECTED to report missing is on CSSRule: nothing. CSSMediaRule and CSSSupportsRule are SIBLINGS
     under CSSConditionRule and each declares a `matches` of its own — §7.3's conjoins "in a stylesheet
     attached to a document" and §7.4's is the feature query alone — so two accessors answer one name and the
     audit sees both because each is installed on its own interface's prototype. */
  CSSRule:              "core/css/css_rule.c",
  CSSGroupingRule:      "core/css/css_rule.c",
  CSSStyleRule:         "core/css/css_rule.c",
  CSSConditionRule:     "core/css/css_rule.c",
  CSSMediaRule:         "core/css/css_rule.c",
  CSSSupportsRule:      "core/css/css_rule.c",
  /* CSS Conditional 5 §9.1, the THIRD sibling under CSSConditionRule and the same component again. It is
     expected to report NOTHING missing: §9.1 declares `containerName`, `containerQuery` and `conditions`, all
     three are installed, and it declares NO `matches` — its two siblings each have one because their condition
     is a fact about the document, while a container query is a fact about an element's query container and has
     no receiver on the rule. A `matches` appearing in this row's MISSING list would mean §9.1 grew one. */
  CSSContainerRule:     "core/css/css_rule.c",
  /* §6.4.4, §6.4.9 and CSS Fonts §12.1's rule, same component and the same reason. CSSImportRule is EXPECTED
     to report `styleSheet` missing, and that is the audit doing its job rather than a row to silence: this
     engine fetches no CSS subresource, so the member has no answer that is not indistinguishable from the
     spec's own null — css_rule.h states the whole case and names the sheet fetch to build. */
  CSSImportRule:        "core/css/css_rule.c",
  CSSNamespaceRule:     "core/css/css_rule.c",
  CSSFontFaceRule:      "core/css/css_rule.c",
  /* §6.4.7 and §6.4.8, same component and the same reason. CSSPageRule inherits CSSGroupingRule, so the row
     is expected to report NOTHING missing — `cssRules`, `insertRule` and `deleteRule` are reachable on one and
     really are installed. CSSMarginRule inherits CSSRule and declares only `name` and `style`. */
  CSSPageRule:          "core/css/css_rule.c",
  CSSMarginRule:        "core/css/css_rule.c",
  /* CSS Animations §6.2 and §6.3, same component and the same reason. Both derive from CSSRule directly — a
     `@keyframes` holds rules and is still not a CSSGroupingRule, which the IDL states outright — so both rows
     are expected to report NOTHING missing. §6.3.1's indexed property getter is not a named member and so is
     not a row the audit can see; it is core/idl_indexed.h's mechanism reached from the rule class's own
     exotic hooks. */
  CSSKeyframeRule:      "core/css/css_rule.c",
  CSSKeyframesRule:     "core/css/css_rule.c",
  /* CSS Cascade §8.1 and §8.2, same component and the same reason. CSSLayerBlockRule inherits
     CSSGroupingRule — §6.4.4.1 makes a `@layer` block "a conditional group rule with a true condition" and the
     IDL follows — so its row is expected to report NOTHING missing: `cssRules`, `insertRule` and `deleteRule`
     are reachable on one and really are installed. CSSLayerStatementRule inherits CSSRule and declares only
     `nameList`, so it is expected to report nothing either. */
  CSSLayerBlockRule:    "core/css/css_rule.c",
  CSSLayerStatementRule: "core/css/css_rule.c",
  /* CSS Properties and Values API 1 §6.1, same component again. It inherits CSSRule and declares four readonly
     attributes and NO `style`, so its row is expected to report NOTHING missing — and a `style` appearing in
     this row's MISSING list would mean the spec grew one, not that this build lost one. */
  CSSPropertyRule:      "core/css/css_rule.c",
  /* CSS Fonts §12.1's and CSSOM §6.4.7's descriptor blocks. Their file is the DECLARATION-BLOCK component and
     not the rule's: the descriptor attributes are installed beside §6.6.1's per-property ones, over the same
     record. Both rows are expected to report nothing missing — each interface's members are exactly its own
     table's two spellings plus the eight CSSStyleDeclaration declares. */
  CSSFontFaceDescriptors: "core/css/css_style_declaration.c",
  CSSPageDescriptors:     "core/css/css_style_declaration.c",
  CSSRuleList:          "core/css/css_rule_list.c",
  /* HTML §4.2.6's `disabled` and CSSOM §6.3.2's LinkStyle `sheet` are installed onto HTMLStyleElement's
     prototype by their own component, for the same reason §4.10's rows name theirs. */
  HTMLStyleElement:    ["core/html/html_style_element.c", "core/html/html_element.c"],
  /* The TREE. These were absent from the audit entirely — the interfaces a page touches most had no gap report
     at all, which is the audit lying by omission rather than by direction. They are auditable now because their
     members live on real prototypes rather than being copied onto each wrapper. */
  Event:                "core/events/event.c",
  /* PromiseRejectionEvent's files include event.c because its prototype CHAINS to Event.prototype — every
     Event member really is reachable on one, so reporting them absent would be the audit lying. */
  PromiseRejectionEvent: ["core/html/unhandled_rejection.c", "core/events/event.c"],
  Node:                ["core/dom/node.c", "core/events/event_target.c"],
  CharacterData:       ["core/dom/node.c", "core/events/event_target.c"],
  /* Element's file list includes node.c because Element.prototype INHERITS from Node.prototype: a member
     installed on the base really is reachable on an element, and reporting it ABSENT would be the audit lying
     in the direction that gets a real gap ignored. Document is listed the same way for the same reason — it is
     a node_wrap of the document node now, with Document.prototype chained to Node.prototype. */
  DOMTokenList:        "core/dom/dom_token_list.c",
  /* DocumentFragment's files are its own plus node.c, for the same reason Element's are: it inherits Node, and
     node.c is also where the ParentNode mixin it INCLUDES is installed. It was missing from this map entirely,
     so its gaps were not reported as zero — they were not reported at all, which is the audit lying by
     omission rather than by direction. */
  DocumentFragment:    ["core/dom/document_fragment.c", "core/dom/node.c", "core/events/event_target.c"],
  /* DOM §6. The three interfaces share §6.3's NodeFilter, and SHARING IT IS NOT INSTALLING ANY OF IT: a
     traverser CALLS the filter and installs nothing on its own prototype from node_filter.c — `filter`,
     `root`, `whatToShow` and the walks are each their own component's, which is what the cross-check said
     twice and was right about both times. A row naming a file that installs nothing this interface has is a
     belief the audit checks, and this was two false ones; §6.3's own row keeps the file, which really does
     build the object its sixteen constants go on. */
  NodeIterator:         "core/dom/node_iterator.c",
  TreeWalker:           "core/dom/tree_walker.c",
  NodeFilter:           "core/dom/node_filter.c",
  /* DOM §5. AbstractRange's five getters are installed by the shared component and INHERITED by both derived
     interfaces, so each names its own file plus that one — the same rule the BODY mixin's row states. */
  AbstractRange:        "core/dom/abstract_range.c",
  StaticRange:          "core/dom/abstract_range.c",
  Range:               ["core/dom/range.c", "core/dom/abstract_range.c"],
  /* SELECTION API §3. It inherits nothing, so its row names only its own file — the two members it shares
     with DOM §5.5 (its stringifier and deleteFromDocument) are §5.5's WALK reached through a body declared
     here, not members installed by range.c on this prototype. */
  Selection:           "core/dom/selection.c",
  NodeList:            "core/dom/collections.c",
  HTMLCollection:      "core/dom/collections.c",
  /* GEOMETRY INTERFACES §3 and §4. DOMRect's file list is its own alone because it IS its own: the eight
     attributes DOMRectReadOnly declares are installed on that prototype and reached through the chain, and the
     four DOMRect redeclares with `inherit attribute` really are installed a second time on ITS prototype —
     Web IDL §3.7.6 defines every regular attribute of an interface on that interface's own prototype, the
     inheriting ones included, which is what gives them a setter. */
  DOMRectReadOnly:      "core/geometry/dom_rect.c",
  DOMRect:              "core/geometry/dom_rect.c",
  DOMRectList:          "core/geometry/dom_rect_list.c",
  /* CSSOM VIEW §6's `partial interface Element` lands on Element.prototype, so element_view.c is one of
     Element's files — the same rule every mixin row here states. */
  /* WAI-ARIA's `Element includes ARIAMixin` is fifty-two members of Element and its own component, for the
     reason every other mixin row here names one: the members land on Element.prototype. */
  Element:             ["core/dom/element.c", "core/dom/node.c", "core/events/event_target.c",
                        "core/dom/dom_token_list.c", "core/dom/element_view.c", "core/dom/aria_mixin.c"],
  /* The HTML layer. Each interface's files are its own plus everything it INHERITS from — HTMLElement on
     Element on Node — because a member installed on a base really is reachable, and reporting it absent is the
     audit lying in the direction that gets a real gap ignored. */
  HTMLElement:         [...HTML_BASE],
  HTMLUnknownElement:  [...HTML_BASE],
  /* HTMLAnchorElement INCLUDES HTMLHyperlinkElementUtils, whose eight members live in their own component —
     and leaving that file out made the audit report href, protocol, host, hostname, port, pathname, search and
     hash ABSENT while they were installed and passing their spec tests. A false ABSENT is the audit lying in
     the direction that buries the real ones in noise. */
  HTMLAnchorElement:   [...HTML_BASE, "core/html/hyperlink.c"],
  /* §4.12.1's `async` is installed onto HTMLScriptElement's prototype by its own component, beside the `force
     async` boolean its steps read — for the same reason HTMLStyleElement's row names html_style_element.c. */
  HTMLScriptElement:   [...HTML_BASE, "core/html/html_script.c"],
  /* §4.8.3's `complete`, `currentSrc`, `naturalWidth` and `naturalHeight` are installed onto
     HTMLImageElement's prototype by its own component, beside §4.8.4.3's image requests they are computed
     from — for the same reason HTMLScriptElement's row names html_script.c. */
  HTMLImageElement:    [...HTML_BASE, "core/html/html_image.c"],
  /* §4.8.5's navigable members are their own component, for the same reason the hyperlink mixin's are. */
  HTMLIFrameElement:   [...HTML_BASE, "core/html/html_iframe.c"],
  /* §4.10's OWN COMPONENTS, which had to be withheld while the installed side was a string scan: html_form.c,
     input_value.c and constraint_validation.c are full of CONTENT ATTRIBUTE names — "required", "pattern",
     "min", "step", "size" — which are the same words as the IDL members that reflect them, so naming these
     files would have credited an interface with members nobody installs. They are named now because the
     installed side reads the install CONSTRUCT: `required` counts for HTMLInputElement because the reflection
     table declares it, and not because the string appears. §4.10.22's `submit`/`requestSubmit`/`elements` are
     html_form.c's, §4.10.5.4's `value`/`checked`/`files` are html_form.c's and input_value.c's, and
     §4.10.21.3's `willValidate` and `setCustomValidity` are constraint_validation.c's. element_internals.c is
     STILL not named, and the reason is no longer that it would lie: `validity`, `validationMessage`,
     `checkValidity`, `labels` and `form` land on ElementInternals.prototype, attribution files them there, and
     naming the file here would now change nothing but the cross-check. The row says what this interface is
     BUILT OUT OF, and ElementInternals is not part of it. */
  HTMLFormElement:     [...HTML_BASE, "core/html/html_form.c"],
  HTMLInputElement:    [...HTML_BASE, "core/html/html_form.c", "core/html/input_value.c",
                        "core/html/input_picker.c", "core/html/constraint_validation.c"],
  HTMLButtonElement:   [...HTML_BASE],
  HTMLLinkElement:     [...HTML_BASE],
  HTMLMetaElement:     [...HTML_BASE],
  HTMLDivElement:      [...HTML_BASE],
  /* §3.1.1's partial interface is installed by THREE components and the row named one, which since attribution
     no longer changes the count — it changes what the CROSS-CHECK is over. Named here so that this row states
     what Document is really built out of: §3.1.4/§3.1.5's `cookie`, `referrer`, `lastModified` and
     `readyState` are document_metadata.c's, and §7.1.1.2's `domain` is document_domain.c's, each for the
     reason its own header gives. */
  Document:            ["core/dom/document.c", "core/dom/document_metadata.c", "core/dom/document_domain.c",
                        "core/dom/node.c", "core/events/event_target.c", "core/css/style_sheet_list.c"],
  /* DOM §4.5.1 and §4.6 — the interface that BUILDS a document and the interface a doctype IS. DocumentType's
     file list carries node.c for the reason Element's does: it inherits Node, and node.c is also where the
     ChildNode mixin it INCLUDES is installed. DOMImplementation inherits nothing, so it names only its own. */
  DOMImplementation:    "core/dom/dom_implementation.c",
  /* HTML §8.5's two DOM-parsing-and-serialization interfaces. Neither inherits anything, so each names only
     its own file. XMLSerializer's row names a file that does not exist yet, which is what makes its absence
     CHECKABLE from both sides: the UNBUILT entry below says the absence is intended, and the moment
     xml_serializer.c lands the `stale` half of that pair fires and forces the entry out. */
  DOMParser:            "core/html/domparser.c",
  XMLSerializer:        "core/html/xml_serializer.c",
  DocumentType:        ["core/dom/document_type.c", "core/dom/node.c", "core/events/event_target.c"],
  HTMLTemplateElement: [...HTML_BASE],
  /* HTML §4.8.11's media elements. HTMLMediaElement is a real state machine over a modelled device, and the
     two element interfaces that INHERIT it install almost nothing of their own — which is exactly what the
     inheritance rule above is for: `play`, `paused`, `duration` and the six reflections are reachable on both
     because they are on HTMLMediaElement.prototype, and reporting them absent per tag would be the audit
     lying in the direction that buries the real gap. That real gap is the four TRACK members
     (audioTracks/videoTracks/textTracks/addTextTrack), which nothing installs and which this row is what
     makes visible. MediaError and TimeRanges are minted by the same component. */
  HTMLMediaElement:    [...HTML_BASE, "core/html/media_element.c"],
  HTMLVideoElement:    [...HTML_BASE, "core/html/media_element.c"],
  HTMLAudioElement:    [...HTML_BASE, "core/html/media_element.c"],
  MediaError:           "core/html/media_element.c",
  TimeRanges:           "core/html/media_element.c",
  /* HTML §4.13.7. ElementInternals INCLUDES ARIAMixin, whose 54 members are therefore members the audit
     expects on it — which is what makes the eight element-reflecting ones show up as the real gap they are
     rather than as nothing at all. CustomStateSet's setlike members and both iterator surfaces come from the
     shared default iterator object, so idl_iter.c is named beside THAT interface for the reason every other
     `iterable<>` interface names it — and beside this one it named a file whose members all land on
     CustomStateSet, URLSearchParams, FormData and Headers, which the cross-check said and was right about.
     §4.13.7's own interface declares no iteration. */
  ElementInternals:     "core/html/element_internals.c",
  CustomStateSet:      ["core/html/element_internals.c", "core/idl_iter.c"],
  ValidityState:        "core/html/element_internals.c",
};

/* THE IDL, PARSED AND FLATTENED ONCE, in engine/idl_members.mjs — one reader of @webref/idl for every gate
   that needs to know what the platform provides, so "is `redirected` a member of Response" and "which members
   of Response does the engine still owe" cannot come back as two different answers. */
const idl = await loadIdl();
const { byName, inheritanceOf, flatten, members } = idl;

/* The install constructs of the WHOLE engine, read once. Whole-engine and not per-row because which objects are
   install targets is a fact about the program (a prototype handed from the file that tags it to the file that
   installs on it), and because a macro or a table is followed to the header it is defined in. */
const env = loadEnvironment(HOST);

/* EVERY INSTALL IN THE PROGRAM, READ ONCE AND FILED UNDER THE INTERFACE ITS TARGET IS — not under the file it
   was written in. Which interface a member belongs to is a fact about the OBJECT it is installed on, so the
   scan is over the whole corpus and the row's file list stops being "which files to believe": it becomes a
   CROSS-CHECK, and a row naming a file whose members all land on other interfaces says so. */
const world = installedMembers([...env.sources.keys()].filter((p) => p.endsWith(".c")), env);
const installedBy = new Map(), stubbedBy = new Map(), landsIn = new Map();
const unattributed = [];
const addTo = (map, iface, name) => {
  if (!map.has(iface)) map.set(iface, new Set());
  map.get(iface).add(name);
};
/* AN OBJECT WEB IDL DECLARES IS NOT AN INTERFACE PROTOTYPE OBJECT — §3.7.4's named properties object,
   §3.7.9.2's iterator prototype object, §3.7.10.2's asynchronous iterator prototype object. What is installed
   on one is nobody's member, so it is neither credited nor reported as a target the attribution failed on; it
   is its own census, and the CHECK is that Web IDL defines the member there at all (idl_installed.mjs's
   NON_INTERFACE_FORMS names what each kind defines). A third member appearing on a §3.7.10.2 object is a
   member the spec does not put there, which is a defect and not a line to widen the list with. */
/* WHAT KIND OF PROPERTY THE IDL SAYS EACH MEMBER IS. Every category above asks whether a member is THERE;
   this asks whether what is there is what the spec builds, which is a question no gap count can hold — a
   member installed as the wrong kind of property fills its ABSENT row and reads COMPLETE while answering the
   page something else. It is the class `typeof NodeFilter` was in: Web IDL §3.11.1 constructs a built-in
   FUNCTION and this engine built an ordinary object, and nothing here could have counted it, because it is
   not a missing member — it is a wrong one. Found by reading what the spec constructs rather than which names
   were absent, and this is the same question asked mechanically wherever the two sides can be compared.
   §3.7.6 Attributes defines EVERY attribute as an ACCESSOR — `PropertyDescriptor{[[Getter]], [[Setter]],
   [[Enumerable]]: true, [[Configurable]]: configurable}`, with [LegacyUnforgeable] deciding only the last
   field — while §3.7.7's operation and §3.7.5's constant are DATA properties. The installed side states the
   same fact at the install FORM (idl_installed.mjs's `kind`), so the two are read against each other. */
const IDL_WANTS = { attribute: "accessor", operation: "data", const: "data" };
const idlKindCache = new Map();
const idlKinds = (iface) => {
  if (idlKindCache.has(iface)) return idlKindCache.get(iface);
  const out = new Map();
  const add = (n, k) => { if (!n) return; if (!out.has(n)) out.set(n, new Set()); out.get(n).add(k); };
  for (const m of flatten(iface)) {
    if (m.type === "attribute" || m.type === "operation" || m.type === "const") add(m.name, m.type);
    else if (m.type === "iterable" || m.type === "maplike" || m.type === "setlike" || m.type === "async_iterable") {
      /* §3.7.9-§3.7.12's iteration members are operations; a maplike/setlike `size` is an attribute. */
      for (const n of iterationMembers({ members: [m] })) add(n, "operation");
      if (m.type === "maplike" || m.type === "setlike") add("size", "attribute");
    }
  }
  idlKindCache.set(iface, out);
  return out;
};
const wrongKind = [], kindUndeclared = [];
const nonIface = [], nonIfaceExtra = [];
for (const r of world.records) {
  if (r.nonInterface) {
    nonIface.push(r);
    if (!r.nonInterface.members.includes(r.name)) nonIfaceExtra.push(r);
    continue;
  }
  if (!r.ifaces.length) { unattributed.push(r); continue; }
  for (const iface of r.ifaces) {
    addTo(r.stubbed ? stubbedBy : installedBy, iface, r.name);
    addTo(landsIn, r.file, iface);
    const declared = idlKinds(iface).get(r.name);
    if (!declared) continue;             /* not a member of this interface — the ABSENT side's question */
    if (!r.kind) { kindUndeclared.push({ ...r, iface }); continue; }
    if (![...declared].some((d) => IDL_WANTS[d] === r.kind))
      wrongKind.push({ ...r, iface, declared: [...declared] });
  }
}
/* A member installed on a BASE prototype really is reachable on everything that inherits it, which is what the
   hand-written file lists were spelling out one row at a time — `getBoundingClientRect` on Element.prototype is
   an HTMLInputElement member because HTMLInputElement inherits Element, not because a row named element.c. */
const chainOf = (iface) => {
  const out = [], seen = new Set();
  for (let n = iface; n && !seen.has(n); n = inheritanceOf.get(n)) { seen.add(n); out.push(n); }
  return out;
};

let totalMissing = 0;
/* The DISTINCT names, because the per-interface counts legitimately repeat an inherited gap: HTMLElement's
   getBoundingClientRect is absent on every one of the twelve HTML interfaces that inherit it, and one
   implementation fixes all twelve. The per-interface list stays exact — it is what that interface's IDL says —
   and the headline says how many things there are to BUILD. */
/* INTERFACES THIS ENGINE HAS NOT BUILT, each with the reason it is absent rather than merely the fact.
 * The row above still names where the component WILL go, which is what makes the audit's belief checkable;
 * this says the absence is intended and by whom it is named elsewhere, so a reader hitting the report is sent
 * to the crash that already describes the gap instead of concluding the auditor is broken.
 * Every entry is checked from BOTH sides at each run: an interface here whose component EXISTS is reported as
 * a stale declaration, exactly as a member exclusion is. */
const UNBUILT = {
  /* IntersectionObserver's row is GONE, and its reason was the stale-DFAIL failure this file's own two-sided
     check exists to catch: it said "no layout, so no intersection to compute", which was true about the spec
     and false about this tree. core/layout/{block_flow,used_value,flow_position}.c compute used values and
     place in-flow block-level boxes, CSSOM VIEW §6's get-the-bounding-box is written in
     core/dom/element_view.c, and every branch that still lacks a layout crashes there naming its own section.
     What was left to build was the DELIVERY seam, not the geometry. */
  ResizeObserver:       "no layout, so no box to observe — rendering.c's realm_awaits names it",
  PerformanceObserver:  "no performance timeline to observe — rendering.c's realm_awaits names it",
  Notification:         "no notification surface; nothing in the tree constructs one",
  /* HTML §7.2.6.10's REMAINING neighbours. §7.2.6.10.1's NavigateEvent and §7.2.6.10.3's NavigationDestination
     have LEFT this list — both are built, and NavigateEvent's row above reports its two operations as the
     honest ABSENT it now is. What is still unbuilt is what only INTERCEPTION creates: `intercept()` is what
     moves a NavigateEvent's interception state off "none", and until it exists no transition is ever created,
     no precommit controller is ever handed to a handler, and §7.2.6.10.4's commit switch has no arm to take.
     Each remaining one is named at the step that would build it: core/frame/history.c's shared push/replace
     state steps 7-9 and core/frame/session_history.c's §7.4.6.1 step 5 both carry a realm_awaits over
     `Navigation.prototype.onnavigate` — the observable of the navigate event being FIRED, which the interface
     merely existing is not — and §7.4.6.2 step 7's DCHECK names NavigationActivation. */
  NavigationPrecommitController: "intercept()'s precommit half; absent with the interception state",
  NavigationTransition: "reports an ONGOING navigation, which only an INTERCEPTED navigate event creates",
  NavigationActivation: "written by §7.4.6.2 step 7, whose branch is unreachable without a cross-document "
                        + "traversal or a reload — session_history.c's DCHECK names it",
  /* HTML §8.5.8. `serializeToString` IS DOM-Parsing §3.2.1's XML serialization algorithm — a different walk
     from HTML §13.3's, which core/html/fragment_serializer.c implements: §3.2.1 preserves every namespaceURI
     through a namespace prefix map copied per element, and emits an empty-element tag for a childless element
     outside the HTML namespace. Nothing in fragment_serializer.c answers either question, so this is a
     component and not a magic on that one.
     IT IS BLOCKED ON THE XML PARSER, and that is a measured fact rather than a preference: every subtest in
     wpt/domparsing/XMLSerializer-serializeToString.html builds its input with
     `new DOMParser().parseFromString(…, 'text/xml')`, which core/html/domparser.c crashes for by name. A
     serializer landed first would be a component no gate could exercise. */
  XMLSerializer:        "DOM-Parsing §3.2.1's XML serialization; its only corpus builds every fixture through "
                        + "parseFromString(…, 'text/xml'), which domparser.c's DFAIL names the parser for",
};
const unbuiltSeen = [], unmapped = [], stale = [];

const distinct = new Set();
const unresolvedAll = new Map();
/* WHICH INTERFACES ARE AUDITED IS DERIVED FROM WHAT THE CORPUS DECLARES. The map above is a FILE-LIST
   ANNOTATION on that set; it was the SET itself until this line existed, and a hand-maintained set has exactly
   the failure CLAUDE.md names in build.mjs's hand-picked list: an interface with no row reads IDENTICALLY
   whether it is complete or absent. It is in no total, contributes no ABSENT member and fails nothing — the
   excluded-test shape one level up from the members, in the very file whose whole posture is to refuse rather
   than guess. The census when this landed was EIGHTY-THREE interfaces the corpus declares and the map had no
   row for. They are not drift: IndexedDB §4.6's IDBIndex has been shipping `objectStore`, `keyPath`, `name`,
   `get`, `count` and `openCursor` since it landed and had never once been audited, and 46 of the 83 are HTML
   element interfaces — HTMLSelectElement, HTMLTextAreaElement, HTMLTableElement among them.
   A component DECLARES an interface by building its §3.7.3-tagged prototype or its §3.7.1 interface object
   (idl_installed.mjs's `declaresIface`) — the same pair of statements the row cross-check below already reads,
   so the audited set and that check now come from ONE fact instead of two lists free to disagree. An existing
   row keeps its own file list: a row names every file that INSTALLS on the interface, while a declaration
   names only the one that BUILT the prototype, and the cross-check needs the former.
   A CALLBACK INTERFACE declares itself differently and is not in this set — Web IDL §3.7.1 gives it a callback
   interface object carrying its constants and NO interface prototype object, so there is no §3.7.3 tag to
   find. DOM's NodeFilter is one, node_filter.c builds exactly that, and it stays a hand-written row because
   nothing in the corpus can state it otherwise. */
const declaringFiles = new Map();
for (const [path, names] of env.declaresIface)
  for (const n of names) {
    if (!declaringFiles.has(n)) declaringFiles.set(n, []);
    declaringFiles.get(n).push(path.replace(BROWSER + "/", ""));
  }
const AUDITED = new Map();
for (const [iface, where] of Object.entries(INTERFACES))
  AUDITED.set(iface, Array.isArray(where) ? where : [where]);
/* A NAMED PROPERTIES OBJECT IS NOT AN INTERFACE, and Web IDL §3.7.4 Named properties object is what says so:
   its class string is the interface's identifier concatenated with "Properties", so window.c tagging one
   "WindowProperties" is the spec being OBEYED and not an interface being declared, and no IDL defines it
   because none is meant to. This reads the spec's own composition rule back rather than matching a name: the
   suffix counts only when what remains is an interface this index actually carries and the whole is not
   itself one. */
const namedProps = [];
const derivedIfaces = [];
for (const [iface, files] of declaringFiles) {
  if (AUDITED.has(iface)) continue;
  const host = iface.endsWith("Properties") ? iface.slice(0, -"Properties".length) : "";
  if (host && byName.has(host) && !byName.has(iface)) { namedProps.push(`${iface} (§3.7.4 of ${host})`); continue; }
  AUDITED.set(iface, files);
  derivedIfaces.push(iface);
}
/* An interface the corpus declares that NO spec in @webref/idl defines. Its member list would be empty, so it
   would audit as complete with nothing installed to compare against — a false COMPLETE minted by the audit
   itself. Named, never skipped: either the tag misspells an interface, or the spec is one webref does not
   carry and the row has to say so. */
const noIdl = [];

for (const [iface, paths] of AUDITED) {
  const file = paths.join(" + ");
  let src = "", missing = [], present = [];
  for (const one of paths) {
    try { src += readFileSync(join(BROWSER, one), "utf8"); present.push(join(BROWSER, one)); }
    catch { missing.push(one); }
  }
  if (missing.length === paths.length) {
    /* AN INTERFACE WITH NO COMPONENT IS ACCOUNTED FOR, NOT SKIPPED. This warned once and `continue`d, so the
       interface left the audit entirely: it appeared in no total, contributed no absent members, and read the
       same whether it is deliberately unbuilt or whether the row names a path that was renamed under it. That
       is the excluded-check shape one level up from the members — the very thing the two-sided
       idl_members_excluded exists to prevent — so the same two sides apply here. */
    if (UNBUILT[iface]) unbuiltSeen.push([iface, file, UNBUILT[iface]]);
    else                unmapped.push([iface, file]);
    continue;
  }
  if (UNBUILT[iface])
    /* THE OTHER SIDE: the declaration says this interface has no component, and the component is right there.
       A stale exemption reads as an intention and hides a real audit — the interface's members would be judged
       against a file the row claims does not exist. */
    stale.push([iface, present.join(", ")]);
  if (missing.length) console.warn(`[idl-audit] ${iface}: ${missing.join(", ")} not found — audited without it`);
  /* WHAT THIS INTERFACE HAS, from the install constructs and the interface each install TARGET is — see
     engine/idl_installed.mjs. Its own members plus everything it INHERITS, because a member on a base
     prototype really is reachable on a derived object. A member wired to js_noop is STUBBED (present but does
     nothing — the banned lazy stub the audit exists to expose), and an install whose member name cannot be
     decided statically is UNRESOLVED rather than assumed either way. */
  const chain = chainOf(iface);
  const installed = new Set(), stubbed = new Set();
  for (const base of chain) {
    for (const n of installedBy.get(base) || []) installed.add(n);
    for (const n of stubbedBy.get(base) || []) stubbed.add(n);
  }
  const inRow = (list) => list.filter((x) => present.includes(x.file));
  const unresolved = inRow(world.unresolved);
  const offInstaller = inRow(world.offInstaller);
  const excluded = world.excluded;
  /* THE ROW IS NOW A CROSS-CHECK. It no longer decides what counts — it says which components this interface is
     believed to be built out of, and attribution answers whether that is true. A file whose members all land on
     other interfaces is a row that was wrong (or a component that moved), and it used to be invisible. */
  /* A FILE IS THIS INTERFACE'S EITHER BY INSTALLING A MEMBER OR BY DECLARING IT, and reading only the first
     made the check unclosable. xml_http_request.c BUILDS and §3.7.3-tags XMLHttpRequestUpload.prototype and
     installs no member on it, because §4's Upload declares no members of its own and its seven handler
     attributes are event_target.c's — so the component that IS the interface read as a stranger to it, and no
     change to the C could have made it stop. A red with no root fix is not a forcing function, it is noise
     that teaches the reader to skip the category. */
  const declares = (p) => [...(env.declaresIface.get(p) || [])];
  const strangers = present.filter((p) => {
    const lands = landsIn.get(p);
    if (lands && [...lands].some((n) => chain.includes(n))) return false;
    return !declares(p).some((n) => chain.includes(n));
  }).map((p) => {
    const lands = [...(landsIn.get(p) || [])], decl = declares(p);
    const what = [lands.length ? "installs for " + lands.join(", ") : "installs nothing this can attribute",
                  decl.length ? "declares " + decl.join(", ") : "declares no interface"];
    return `${p.replace(BROWSER + "/", "")} (${what.join("; ")})`;
  });
  /* An install in one of this row's files whose target's interface could not be decided. Neither credited to
     the row (the false COMPLETE this attribution removes) nor dropped — named, with its member and line. */
  const rowUnattributed = inRow(unattributed);
  // The g_opaque-as-prototype fallback is a BANNED shrug: it silently serves EVERY unbuilt member as an opaque
  // value, hiding a missing browser feature (it is not our choice which features to omit — a browser has them
  // all). A component must implement its real surface and DFAIL loud on an unbuilt member, never opaque-shrug it.
  const bannedShrug = /JS_SetPrototype\s*\([^)]*\bg_opaque\b/.test(src);
  const spec = members(iface);
  if (!spec.length && !byName.has(iface)) { noIdl.push([iface, file]); continue; }
  /* A CONDITIONAL member — one the component DECLARES this user agent must not have (idl_members_excluded). It
     is not a gap and it is not installed, so it is neither counted nor dropped: it is named, with the spec
     sentence that excludes it, so nobody works it off the ABSENT list and builds a member the spec forbids.
     The declaration is CHECKED here, which is what stops it being an exclusion list: a name it excludes that
     the corpus no longer carries is stale, and one the component installs anyway contradicts itself. */
  /* AN EXCLUSION INHERITS EXACTLY AS THE MEMBER DOES, and reading it only at the DECLARING interface was the
     same asymmetry as ranking by inherited totals: `installed` walks chainOf and this did not, so a member a
     base declares this user agent must not have was excluded on the base and counted as a GAP on everything
     that inherits it. media_element.c declares §4.8.11.10's `audioTracks`, `videoTracks`, `textTracks` and
     `addTextTrack` absent because the TrackList interfaces are not built — correct on HTMLMediaElement, and
     reported as four gaps each on HTMLVideoElement and HTMLAudioElement, which inherit the very same members
     that do not exist. A false ABSENT sends someone to build what the condition forbids.
     The two-sided ASSERTIONS stay bound to the declaring interface: a stale name, or one the component
     installs anyway, is a single fact about a single declaration, and re-reporting it on every inheriting
     interface would turn one wrong line into sixty-one identical reds. */
  const cond = excluded.filter((e) => chain.includes(e.iface));
  const condStale = cond.filter((e) => e.iface === iface && !spec.includes(e.name));
  const condInstalled = cond.filter((e) => e.iface === iface && installed.has(e.name));
  const condNames = new Set(cond.map((e) => e.name));
  /* AN ABSENCE THE AUDIT CANNOT TELL FROM ITS OWN BLIND SPOT IS NOT AN ABSENCE. The two halves of this were
     already computed and were printed in two places with nothing joining them, so the SAME member read as
     ABSENT on one line and UNATTRIBUTED on another and a person had to notice: FileSystemDirectoryHandle's
     §2.4.1 `entries` and `keys` ARE installed, by idl_async_iter.c's shared installer, under `if (ops->pair)`
     — a condition over the DECLARATION the caller named, which names no interface, so with two callers
     (FileSystemDirectoryHandle and ReadableStream) the target is two tagged prototypes and the site is
     honestly undecidable. Counting those as gaps to implement sends someone to build a member that is already
     there, which is a false ABSENT with the auditor's own name on it.
     THE JOIN IS EXACT AND USES ONLY WHAT IS ALREADY KNOWN: an unattributed record carries the CANDIDATES its
     target could be, so a member is UNPROVEN for this interface only when an install of that NAME landed on a
     candidate set containing this interface or one of its bases. It is neither counted as a gap nor credited
     as installed, and it is its own failing category — the work is to make the site decidable, never to
     assume it either way. */
  const maybeHere = new Map();
  for (const r of unattributed)
    if (r.candidates.some((n) => chain.includes(n)) && !maybeHere.has(r.name)) maybeHere.set(r.name, r);
  const base = inheritanceOf.get(iface);
  const inherited = base ? new Set(members(base)) : new Set();
  const ownSet = new Set(spec.filter((n) => !inherited.has(n)));
  const open = spec.filter((n) => !installed.has(n) && !condNames.has(n));
  const absent = open.filter((n) => !maybeHere.has(n));
  const unproven = open.filter((n) => maybeHere.has(n));
  const noop = spec.filter((n) => stubbed.has(n));
  totalMissing += absent.length + noop.length;
  for (const n of absent) distinct.add(n);
  for (const n of noop) distinct.add(n);
  /* PER INTERFACE, so the verdict names an AREA. An interface with nothing missing is a row too — it is what
     makes the table a census rather than a list of the loudest components. */
  /* WHAT IS THIS INTERFACE'S OWN WORK. An inherited gap is absent on every interface that inherits it, which
     is CORRECT — a page really cannot reach it on any of them — but it makes the total a measure of
     INHERITANCE DEPTH rather than of work: 45 HTML element interfaces each carry the same ~85 unbuilt
     Element/HTMLElement members, so one member built there clears 45 rows at once and a member built on
     HTMLTableElement clears one. Ranking by the total sent two lanes at the deepest rows rather than the
     largest ones. The own count is the members this interface DECLARES — its own IDL plus the partials and
     mixins that name IT — minus everything its base already declares. */
  const ownAbsent = absent.filter((n) => ownSet.has(n)).length;
  const ownNoop = noop.filter((n) => ownSet.has(n)).length;
  gapRows.push({ iface, absent: absent.length, noop: noop.length, unproven: unproven.length,
                 own: ownAbsent + ownNoop });
  defect("ABSENT members", absent.length);
  defect("UNPROVEN members — installed on a target the audit cannot attribute", unproven.length);
  defect("js_noop-STUB members", noop.length);
  defect("STALE member exclusions", condStale.length);
  defect("CONTRADICTED member exclusions", condInstalled.length);
  defect("CROSS-CHECK rows naming a file that installs nothing this interface has", strangers.length);
  if (bannedShrug) defect("BANNED g_opaque-prototype shrugs");
  for (const u of unresolved)
    unresolvedAll.set(`${u.file}:${u.line}:${u.expr}`, u);
  /* A property write with a member's NAME onto an object no installer ever names. Not counted (it may be a
     record field that happens to share the name) and not hidden (it may be an IDL member installed as a plain
     own property, which `document.title` and `screen.width` are) — reported, so both are visible. */
  const plain = [...new Set(offInstaller.filter((o) => absent.includes(o.name)).map((o) => o.name))];
  const parts = [];
  if (absent.length) parts.push(`ABSENT ${absent.length} — ${absent.join(", ")}`);
  if (unproven.length) parts.push(`UNPROVEN ${unproven.length} — ${unproven.map((n) => {
    const r = maybeHere.get(n);
    return `${n} (installed at ${r.file.replace(BROWSER + "/", "")}:${r.line}, ${r.why})`;
  }).join("; ")}`);
  if (noop.length) parts.push(`js_noop-STUB ${noop.length} — ${noop.join(", ")}`);
  if (plain.length) parts.push(`PLAIN-PROPERTY ${plain.length} — ${plain.join(", ")} (written with ` +
                               `JS_SetPropertyStr onto an object no interface declaration reaches: either a ` +
                               `member installed as a plain own property, or a record field of the same name)`);
  if (cond.length) parts.push(`CONDITIONAL ${condNames.size} — ${[...condNames].join(", ")} (${cond[0].why})`);
  if (condStale.length) parts.push(`STALE EXCLUSION ${condStale.length} — ${condStale.map((e) => e.name).join(", ")}` +
                                   ` declared excluded at ${condStale[0].file}:${condStale[0].line} but the IDL ` +
                                   `no longer carries the member — delete the exclusion`);
  if (condInstalled.length) parts.push(`CONTRADICTED EXCLUSION ${condInstalled.length} — ` +
                                       `${condInstalled.map((e) => e.name).join(", ")} is declared excluded and ` +
                                       `installed anyway`);
  if (unresolved.length) parts.push(`UNRESOLVED ${unresolved.length}`);
  if (rowUnattributed.length) {
    const named = rowUnattributed.filter((r) => spec.includes(r.name));
    parts.push(`UNATTRIBUTED ${rowUnattributed.length}${named.length ? `, ${named.length} of them members of this ` +
      `interface — ${[...new Set(named.map((r) => `${r.name} at ${r.file.replace(BROWSER + "/", "")}:${r.line}`))].join(", ")}` : ""}`);
  }
  if (strangers.length) parts.push(`CROSS-CHECK ${strangers.length} of this row's files install nothing ` +
                                   `${iface} or its bases have — ${strangers.join("; ")}`);
  if (bannedShrug) parts.push(`BANNED g_opaque-prototype shrug (silently serves unbuilt members as opaque — remove it, build the features or DFAIL)`);
  if (parts.length) console.log(`[idl-audit] ${iface} (${file}): ${parts.join(" | ")}`);
  else console.log(`[idl-audit] ${iface}: complete`);
}
if (totalMissing)
  console.log(`[idl-audit] ${distinct.size} distinct spec members not yet implemented (${totalMissing} across ` +
              `all interfaces, since an inherited gap is absent on each) — implement each at the root, never a stub.`);
/* THE INTERFACES THAT NEVER REACHED THE AUDIT, reported in the same breath as the members that did, because a
   surface the run silently declined to look at is indistinguishable in the total from one it looked at and
   found complete. The three lists are the three answers, and only the first is an acceptable steady state. */
if (unbuiltSeen.length) {
  console.log(`[idl-audit] ${unbuiltSeen.length} interface(s) declared unbuilt — no component, absence intended:`);
  for (const [iface, file, why] of unbuiltSeen) console.log(`[idl-audit]   ${iface} (${file}) — ${why}`);
}
defect("interfaces whose component is missing and whose absence is undeclared", unmapped.length);
defect("STALE UNBUILT declarations", stale.length);
/* THE SET IS DERIVED, so an interface can no longer go unaudited for want of a row — this says how many the
   derivation brought in, unconditionally, because zero is the armed state and a rising ABSENT total beside a
   nonzero count here is the audit seeing MORE, never the engine regressing. */
console.log(`[idl-audit] audited set — ${AUDITED.size} interfaces: ${AUDITED.size - derivedIfaces.length} from ` +
            `rows, ${derivedIfaces.length} derived from a §3.7.3 tag or §3.7.1 interface object with no row` +
            (derivedIfaces.length ? `: ${derivedIfaces.join(", ")}` : "") +
            (namedProps.length ? ` | not interfaces: ${namedProps.join(", ")}` : ""));
defect("interfaces the corpus declares that no spec in @webref/idl defines", noIdl.length);
for (const [iface, file] of noIdl)
  console.log(`[idl-audit] ${iface} (${file}): the corpus DECLARES this interface and no spec webref carries ` +
              `defines it — its member list would be empty, so it would read COMPLETE against nothing. Either ` +
              `the §3.7.3 tag names an interface that does not exist, or the spec is one webref does not ship.`);
for (const [iface, file] of unmapped)
  console.log(`[idl-audit] ${iface}: component ${file} not found and NOT declared unbuilt — either the row names ` +
              `a path that moved (the audit for a shipping interface is silently not running) or the interface ` +
              `is absent on purpose and belongs in UNBUILT with its reason.`);
for (const [iface, files] of stale)
  console.log(`[idl-audit] ${iface}: STALE UNBUILT declaration — it is declared to have no component and ${files} ` +
              `exists. Remove the declaration so this interface is audited against the file that implements it.`);
/* THE AUDIT'S GAP REPORT ON ITSELF. An install whose member name is decided at RUNTIME cannot be diffed against
   the IDL, and pretending either way is what this rewrite exists to stop: counted, it fills a gap that is open;
   dropped, it opens a gap that is filled. Named here with file and line, it is a work queue — make the name
   static, or teach the detector the construct. */
/* AN UNANSWERABLE QUESTION IS NOT AN ANSWER. These four categories are the audit's own gap report on itself,
   and they fail the run for the reason the report exists: a construct it cannot resolve is a member it can
   neither count nor miss, so the ABSENT number beside it is a number over a surface with holes in it. Making
   them a warning would be the gate reporting green about a question it declined to ask. */
defect("install constructs whose member name is not statically resolvable", unresolvedAll.size);
if (unresolvedAll.size) {
  console.log(`[idl-audit] ${unresolvedAll.size} install construct(s) whose member name could not be resolved ` +
              `statically — neither counted as installed nor reported as a gap:`);
  for (const u of unresolvedAll.values())
    console.log(`[idl-audit]   ${u.file.replace(BROWSER + "/", "")}:${u.line}  ${u.form}(… ${u.expr} …)`);
}
/* The same constructs in components no row names. The scan is over the whole program now, so these exist and
   are counted; hiding them behind "no row asked" would be the audit choosing what to know about itself. */
const elsewhere = world.unresolved.filter((u) => !unresolvedAll.has(`${u.file}:${u.line}:${u.expr}`));
defect("install constructs whose member name is not statically resolvable", elsewhere.length);
if (elsewhere.length) {
  const byFile = new Map();
  for (const u of elsewhere) byFile.set(u.file, (byFile.get(u.file) || 0) + 1);
  console.log(`[idl-audit] ${elsewhere.length} more unresolved install construct(s) in components no row ` +
              `names: ${[...byFile].sort((a, b) => b[1] - a[1])
                .map(([f, n]) => `${f.replace(BROWSER + "/", "")}×${n}`).join(", ")}`);
}

/* THE GAP REPORT ON THE READER, one level below the gap report on the audit. Every category above reports a
   question the ANALYSIS declined to answer; this one reports a question a PARSING PRIMITIVE could not read and
   the analysis then stood on anyway. That distinction is the whole of this reader's soundness and it has been
   broken four times — a string scan crediting a member because the word appeared in the file, `lastIndexOf`
   matching inside a longer identifier, an indirect call matching no identifier-call pattern, a `for` header
   eaten by a top-level semicolon split. None was caught by a gate; each was found by someone chasing a wrong
   number, because a primitive that guesses hands the analysis a PLAUSIBLE answer and the refusal machinery
   above it never gets to run. A primitive that cannot read its input now says so, and a refusal is counted
   only where the analysis DEPENDS on the answer — which interface a member's target is, and which interface a
   §3.7.1 interface object was built over. Zero is the armed state, not an absent check. */
defect("facts a parsing primitive could not read that the analysis then depended on", env.refusals.length);
console.log(`[idl-audit] reader refusals — ${env.refusals.length} fact(s) a parsing primitive could not read ` +
            `and an interface question then depended on`);
for (const r of env.refusals)
  console.log(`[idl-audit]   ${r.file.replace(BROWSER + "/", "")}:${r.line}  ${r.fn}()  ${r.primitive}: ${r.why}`);

/* THE OTHER HALF OF THE AUDIT'S GAP REPORT ON ITSELF — an install whose member name IS decided but whose
   TARGET's interface is not. It is the same rule one level down: counted, it credits a member to whichever row
   happened to name the file (the file-granular lie); dropped, it opens a gap that is filled. So it is named,
   grouped by the file that wrote it, and the work is either to give the prototype its §3.7.3 tag or to teach
   this detector the construct that carries the object. */
/* THE MEMBER IS THERE AND IT IS THE WRONG KIND OF PROPERTY — see IDL_WANTS. Its own category because it is
   its own defect: not a gap, a wrong answer, and one that every count above reports as COMPLETE. */
defect("members installed as the wrong kind of property for their IDL declaration", wrongKind.length);
for (const r of wrongKind)
  console.log(`[idl-audit] ${r.file.replace(BROWSER + "/", "")}:${r.line}  ${r.iface}.${r.name} is an IDL ` +
              `${r.declared.join("/")}, which Web IDL defines as ` +
              `${[...new Set(r.declared.map((d) => IDL_WANTS[d]))].join("/")} — and ${r.form} ` +
              `installs a ${r.kind} property. §3.7.6 gives every attribute a getter (and a setter unless it is ` +
              `readonly) with [LegacyUnforgeable] deciding only [[Configurable]], so a data property answers ` +
              `getOwnPropertyDescriptor wrongly and, being writable, lets a page overwrite the member`);
/* An install form whose property kind is not stated is a member nobody can ask this of. Zero is the armed
   state, exactly as it is for the reader refusals. */
defect("install forms whose property kind is not declared, over a member the IDL declares", kindUndeclared.length);
for (const r of [...new Map(kindUndeclared.map((r) => [r.form, r])).values()])
  console.log(`[idl-audit] ${r.form} declares no property kind, so ${r.iface}.${r.name} and every member it ` +
              `installs is exempt from the §3.7.6/§3.7.7/§3.7.5 check — state its kind in idl_installed.mjs`);
defect("members installed on a declared non-interface object that Web IDL does not define there",
       nonIfaceExtra.length);
for (const r of nonIfaceExtra)
  console.log(`[idl-audit] ${r.file.replace(BROWSER + "/", "")}:${r.line}  \`${r.name}\` is installed on a ` +
              `${r.nonInterface.kind}, on which Web IDL defines only ${r.nonInterface.members.join(", ")} — ` +
              `either the member belongs on the interface prototype object or the spec does not define it here`);
if (nonIface.length)
  console.log(`[idl-audit] ${nonIface.length} property(ies) installed on objects Web IDL declares are NOT ` +
              `interface prototype objects, so they are nobody's IDL member: ` +
              `${[...new Set(nonIface.map((r) => `${r.name} (${r.nonInterface.kind})`))].join(", ")}`);
defect("installed members whose target interface could not be decided", unattributed.length);
if (unattributed.length) {
  const byFile = new Map();
  for (const r of unattributed) {
    if (!byFile.has(r.file)) byFile.set(r.file, []);
    byFile.get(r.file).push(r);
  }
  console.log(`[idl-audit] ${unattributed.length} installed member(s) in ${byFile.size} file(s) could not be ` +
              `attributed to an interface — neither credited to their file nor dropped:`);
  for (const [f, rs] of [...byFile].sort((a, b) => b[1].length - a[1].length)) {
    const why = [...new Set(rs.map((r) => r.why || (r.candidates.length ? `one of ${r.candidates.join("/")}` : "?")))];
    console.log(`[idl-audit]   ${f.replace(BROWSER + "/", "")}: ${rs.length} — ` +
                `${rs.map((r) => `${r.name}@${r.line}`).join(", ")}  [${why.join(" | ")}]`);
  }
}
/* THE TAG READ BACK AGAINST THE CORPUS. A tag is a hand-written string, so it can name an interface Web IDL
   does not have — a typo, a rename, or a class string the spec states in prose without an interface behind it.
   Reading it one way only would make that silent, and the whole point of reading a declaration is that reality
   can contradict it. */
const tagged = new Set([...installedBy.keys(), ...stubbedBy.keys()]);
const unknownTags = [...tagged].filter((n) => !byName.has(n)).sort();
/* A SHARED INSTALLER WHOSE PER-CALL SUBSET COULD NOT BE COMPUTED. Neither credited (the false COMPLETE that
   does not print) nor dropped (a gap that is not there) — named with the call site, because there are exactly
   two root fixes and TEACHING THIS READER THE FILTER'S SHAPE IS NEITHER OF THEM: make the selector a C
   constant this can evaluate, so the rows the caller asked for are computable; or, where the filter is not a
   selector at all, have the C declare what the loop LEAVES ON THE TARGET (idl_install_covers_column), which
   the engine then asserts per realm against the object itself. The same category carries a declaration no
   install answers to — the other side of that pair, and the reason it is not simply believed. */
defect("install sites whose selected subset could not be computed", world.unselected.length);
for (const u of world.unselected)
  console.log(`[idl-audit] ${u.file.replace(BROWSER + "/", "")}:${u.line}  ${u.fn}() — ${u.why}`);
defect("interface tags naming something the IDL corpus does not declare", unknownTags.length);
defect("install targets whose interface tag is not statically decidable", env.tagIssues.length);
defect("interface objects whose prototype identity the corpus contradicts or cannot reach", env.tagChecks.length);
defect("installs onto an object built with no prototype", env.recordContradictions.length);
if (unknownTags.length)
  console.log(`[idl-audit] ${unknownTags.length} interface tag(s) name something the IDL corpus does not ` +
              `declare — ${unknownTags.join(", ")}`);
for (const t of env.tagIssues)
  console.log(`[idl-audit] ${t.file.replace(BROWSER + "/", "")}:${t.line}  ${t.form}(… ${t.expr} …) — the ` +
              `interface this tags cannot be decided statically, so nothing installed on it is attributed`);
for (const c of env.tagChecks)
  console.log(`[idl-audit] ${c.file.replace(BROWSER + "/", "")}:${c.line}  ` + (c.kind === "contradicted"
    ? `CONTRADICTED INTERFACE IDENTITY — §3.7.1's interface object is built for ${c.ifaces.join("/")} over a ` +
      `prototype §3.7.3 tags ${c.have.join("/")}; one of the two names the wrong interface`
    : `the prototype §3.7.1's ${c.ifaces.join("/")} interface object is built over reaches no interface tag ` +
      `from here — this detector could not follow the object, so its members are unattributed`));
/* THE CLASSIFICATION READ FROM THE OTHER END. Which objects a member can be installed on is decided by which
   objects the corpus declares an interface for; the C states the same fact at the CONSTRUCTION, and the two must
   agree. An object built with NO PROTOTYPE is reachable from nothing the page holds — it is how a component
   holds per-realm state the COW delta captures — so an install form naming one is a contradiction: either the
   object should have been built as a prototype, or the install is on the wrong object. Reported, never
   tolerated, and it is what stops the record/prototype split from becoming a list of known-OK mismatches. */
for (const c of env.recordContradictions)
  console.log(`[idl-audit] ${c.file.replace(BROWSER + "/", "")}:${c.line}  CONTRADICTED OBJECT KIND — ` +
              `${c.form}(… ${c.obj} …) installs on an object built with NO PROTOTYPE, which no page can reach`);

/* ---------------------------------------------------------------------------------------------------------
 * THE PLATFORM SURFACE — every global name a browser exposes on Window, straight out of the IDL.
 *
 * Two sources, both spec text:
 *   1. Every interface / namespace / callback-interface DECLARED [Exposed=Window] (or exposed with no
 *      restriction). Its name is a global constructor property: `Node`, `Element`, `DOMException`, `SVGElement`.
 *   2. Every member of the Window interface itself, plus the mixins Window includes
 *      (WindowOrWorkerGlobalScope, GlobalEventHandlers, WindowEventHandlers, …) — `fetch`, `setTimeout`,
 *      `onload`, `location`. members() already flattens inheritance and mixins, so it answers this directly.
 *
 * A name here is the ENGINE's to provide. A name NOT here is the server's to have injected, and reading it
 * yields the concolic unknown whose gate forks. That is the whole distinction absent.c makes. */
const platform = windowGlobals(idl);

const names = [...platform].filter((n) => /^[A-Za-z_$][\w$]*$/.test(n)).sort();
const header =
  "/* GENERATED by engine/idlgen.mjs from @webref/idl — DO NOT EDIT.\n" +
  " * Every global name Web IDL exposes on Window: the interfaces declared [Exposed=Window] and the members of\n" +
  " * the Window interface itself. absent.c reads this to tell a Web API this engine OWES (absent, so the page's\n" +
  " * ReferenceError names the component to write) from server-injected app state (unknown input, so the gate\n" +
  " * forks). Regenerate after `npm install @webref/idl webidl2`, and commit the result — the build has no\n" +
  " * network and this table is not optional. */\n" +
  "#ifndef APICLIENT_PLATFORM_NAMES_H\n#define APICLIENT_PLATFORM_NAMES_H\n\n" +
  "static const char *const PLATFORM_NAMES[] = {\n" +
  names.map((n) => `    "${n}",`).join("\n") + "\n};\n\n#endif\n";
const OUTH = join(HERE, "host", "browser", "platform_names.h");
let prev = "";
try { prev = readFileSync(OUTH, "utf8"); } catch { /* first run */ }
if (prev === header) {
  console.log(`[idl-audit] platform_names.h current — ${names.length} global names exposed on Window`);
} else if (REGEN) {
  writeFileSync(OUTH, header);
  console.log(`[idl-audit] platform_names.h REGENERATED — ${names.length} global names exposed on Window`);
} else {
  /* THE AUDIT RUN DOES NOT WRITE. What absent.c decides off this table is ReferenceError-vs-fork, so a table
     that disagrees with the corpus is a page's Web API read forking as app state (or a real injected global
     throwing) — a wrong answer, not a formatting drift, and one that a build silently rewriting the header
     would have hidden by fixing it out from under whoever was compiling. */
  defect("stale generated platform table");
  console.log(`[idl-audit] platform_names.h STALE — the corpus exposes ${names.length} global names on Window ` +
              `and the checked-in table is not that. absent.c decides ReferenceError-vs-fork off it, so a stale ` +
              `table is a wrong answer per name. Regenerate it with \`node engine/idlgen.mjs --regen\` and commit.`);
}

/* ---------------------------------------------------------------------------------------------------------
 * THE VERDICT, PER INTERFACE FIRST. §Testing: a gate reports per AREA as well as in total, because one number
 * in which the widest base answers most of the count makes every other component invisible — and here the
 * inherited members make that literal, since one absent Element member is absent on every HTML interface. */
const tot = (r) => r.absent + r.noop + r.unproven;
/* RANKED BY OWN WORK, not by total. The total counts an inherited gap once per inheriting interface, so it
   ranks by inheritance depth: sorting by it put 45 HTML element rows carrying the SAME ~85 unbuilt
   Element/HTMLElement members above every interface with real work of its own, and two lanes were scoped off
   that order. Both numbers are printed, because the total is still the honest answer to "what can a page not
   reach on this interface" — it is just not the answer to "what should I build next". */
const withGaps = gapRows.filter(tot).sort((a, b) => (b.own - a.own) || (tot(b) - tot(a)));
console.log(`[idl-audit] ── per interface, ranked by OWN work ── ${gapRows.length - withGaps.length} of ` +
            `${gapRows.length} audited interfaces install every member their IDL declares. OWN counts only the ` +
            `members this interface DECLARES; ABSENT counts those plus every inherited gap, which is what a ` +
            `page cannot reach on it — one member built on a base clears an inherited gap on every row below it`);
if (withGaps.length) {
  /* A DERIVED ROW IS MARKED, because a table that does not say so reads as if every row had been audited all
     along. These are the interfaces no row named — they were in no total until the set became derived, so
     their gaps are not new work appearing, they are existing work becoming visible, and the file that DECLARES
     each is printed because that is where the work goes. */
  const derivedSet = new Set(derivedIfaces);
  const w = Math.max(...withGaps.map((r) => r.iface.length));
  for (const r of withGaps)
    console.log(`[idl-audit]   ${r.iface.padEnd(w)}  OWN ${String(r.own).padStart(3)}` +
                `  ABSENT ${String(r.absent).padStart(3)}` +
                (r.noop ? `  js_noop-STUB ${r.noop}` : "") +
                (r.unproven ? `  UNPROVEN ${r.unproven}` : "") +
                (derivedSet.has(r.iface) ? `  NEWLY-AUDITED ${(AUDITED.get(r.iface) || []).join(" + ")}` : ""));
}
console.log("[idl-audit] ── verdict ──");
if (!defects.size) {
  console.log("[idl-audit]   PASS — every audited interface installs its whole IDL surface, every declaration " +
              "is current, and every install construct resolved.");
  process.exit(0);
}
for (const [kind, n] of [...defects].sort((a, b) => b[1] - a[1]))
  console.log(`[idl-audit]   ${String(n).padStart(5)}  ${kind}`);
console.error(`[idl-audit] FAILED — ${defects.size} category(ies) above. Each is a gap to close at the ROOT: ` +
              `implement the member in its real component (never a js_noop stub, never a g_opaque prototype), ` +
              `make the install construct resolvable, or delete the declaration that has gone stale.`);
process.exit(1);
