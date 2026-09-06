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
import { loadIdl, windowGlobals, unplacedInterfaces, iterationMembers, EXPOSED_STAR, rhsNames, extOf } from "./idl_members.mjs";
import { readDictDecls } from "./idl_dictdecl.mjs";

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
   declaration that has gone stale is itself a category.

   AND IT IS TWO LEDGERS, BECAUSE A CATEGORY THIS RUN COULD NOT EVALUATE HAS FOUND NOTHING AND SUMMING IT WITH
   ONE THAT DID IS THE THREE-STATES-BEHIND-ONE-ANSWER SHAPE ARRIVING IN THE INSTRUMENT BUILT TO END IT.
   §Testing: a gate states its FINDINGS and its BLIND SPOTS as separate verdicts, because an instrument that
   cannot see something has not found anything — and where the two must share a line, the line says which is
   which. They shared one here: every category printed under one heading, sorted together by size, beneath a
   closing sentence asserting that each "names ONE disagreement between the platform's IDL and this engine".
   That sentence is TRUE of an absent member and FALSE of an install construct whose member name this scan
   could not resolve — the second is a statement about THIS AUDIT, and a reader sent to it looking for a
   disagreement finds an unreadable line of C and no disagreement anywhere. Measured on the run that split
   them: three of seven categories, and 601 of 1095 items, were the audit's own blindness, and the largest of
   those three carried the second-largest number on the page while reading as engine work in every column
   printed.

   THE DISCRIMINATOR IS WHOSE STATE THE NUMBER IS ABOUT, and it is DECLARED AT THE SITE that raises the
   category rather than inferred from the category's wording — a rule read off the label would be a count of a
   SPELLING, which is the defect this family of gates exists to remove. `defect` is a statement about the
   ENGINE: it does not install a member the IDL declares, it installs one the IDL does not, it declares an
   absence the corpus contradicts, it cannot construct what the platform constructs. `blind` is a statement
   about THIS RUN: a construct it could not read, a target it could not attribute, a mint it could not prove —
   so it made no claim about the engine either way, and its count is the amount by which the other ledger is a
   FLOOR rather than a total. BOTH ARE RED: an audit that cannot read its subject is an unbuilt capability of
   the audit, and going green where its input is unreadable is the excluded check §Testing forbids. They are
   red about OPPOSITE WORK, which is all the split is for — a finding is closed in the engine, a blind spot is
   closed here. */
const defects = new Map();
const blinds = new Map();
const defect = (kind, n = 1) => { if (n) defects.set(kind, (defects.get(kind) || 0) + n); };
const blind = (kind, n = 1) => { if (n) blinds.set(kind, (blinds.get(kind) || 0) + n); };
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
  /* RESIZE OBSERVER §2.3's TWO RECORD INTERFACES, each its own component in the same directory, for the reason
     IntersectionObserverEntry's row above gives: each is its own interface with its own members, and what
     they are records OF belongs to other standards again — a rectangle to Geometry Interfaces §3, and each box
     size to the second of these two. */
  ResizeObserverEntry:  "core/resize_observer/resize_observer_entry.c",
  ResizeObserverSize:   "core/resize_observer/resize_observer_size.c",
  PerformanceObserver:  "core/timing/performance_observer.c",
  /* PERFORMANCE TIMELINE §4.2.2, its own component in the same directory for the reason
     IntersectionObserverEntry's row above gives: it is its own interface with its own three members, and
     what they filter is §5.5's algorithm over a list the observer hands it. */
  PerformanceObserverEntryList: "core/timing/performance_observer_entry_list.c",
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
  /* DOM §2.4 Interface CustomEvent — the one Event subclass DOM itself declares, and the one an application
     constructs for itself. Its own file plus event.c, whose Event.prototype members really are reachable on
     one; without the row the audit would find it from its §3.7.3 tag alone and report every INHERITED member
     absent, which is the audit lying in the direction that gets a real gap ignored. */
  CustomEvent:         ["core/events/custom_event.c", "core/events/event.c"],
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
  /* Streams §4.9.1 "Working with readable streams"' ReadableStreamPipeTo belongs to neither half of the
     standard — it holds a reader on one stream and a writer on another — so `pipeTo` and `pipeThrough`, the
     two §4.2.4 "Constructor, methods, and properties" members that call it, are declared in their own
     component and installed onto this prototype. Naming only readable_stream.c reported them absent while
     they were shipping. */
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
     of which stands on §13's CryptoKey. Without rows the audit would report both interfaces as
     nothing at all rather than as a member list with a reason — and the list's LENGTH is deliberately not
     written here: it is what the row prints, it changed under this comment once already when the modern-algos
     IDL added the encapsulation members, and a count in prose is the one thing a row makes redundant.
     §13's CryptoKey IS a row for the opposite reason: it ships all four of its members, so the row is what
     makes an interface that is COMPLETE distinguishable from one nobody has looked at. */
  Crypto:               "core/crypto/crypto.c",
  SubtleCrypto:         "core/crypto/subtle_crypto.c",
  CryptoKey:            "core/crypto/crypto_key.c",
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
  /* HTML §7.2.6, the navigation API. Its entry list is a view over §7.4.1.1's session history entries, and the
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
                        "core/html/input_picker.c", "core/html/constraint_validation.c",
                        "core/html/text_control_selection.c"],
  /* §4.10.20's six members are the SAME component reached from two prototypes — the section says so itself
     ("Their shared algorithms are defined here") — so the file is named by both rows, and HTMLTextAreaElement
     gets a row of its own for the first time. It had none, so the audit found it only from its §3.7.3 tag and
     credited everything it is built out of to core/html/html_element.c: §4.10.11's `value` is html_form.c's,
     §4.10.21.3's `willValidate` and `setCustomValidity` are constraint_validation.c's, and §4.10.20's
     selection members are text_control_selection.c's. */
  HTMLTextAreaElement: [...HTML_BASE, "core/html/html_form.c", "core/html/constraint_validation.c",
                        "core/html/text_control_selection.c"],
  HTMLButtonElement:   [...HTML_BASE],
  HTMLLinkElement:     [...HTML_BASE],
  HTMLMetaElement:     [...HTML_BASE],
  HTMLDivElement:      [...HTML_BASE],
  /* §4.10.13 The progress element and §4.10.14 The meter element — the two GAUGES, and the two interfaces whose
     own members are an ALGORITHM over the element's attributes rather than a mirror of any one of them. They had
     no row, so the audit found them only from their §3.7.3 tag and attributed their members to
     core/html/html_element.c, which is where the wrong answer hid: `meter.min` and `meter.max` were declared
     REFLECT_STRING in that file's per-tag table, so both names WERE installed — as strings, where §4.10.14
     declares `attribute double` and its getter steps return "this's minimum value". A member-presence diff
     cannot see that, which is why these rows name the components that own the algorithms.
     core/html/html_form.c IS NOT NAMED in either row, and the cross-check is what would say so: `labels` is
     INSTALLED by the component on its own interface's prototype, and what the form layer owns is the label
     ASSOCIATION the member calls — a file that installs nothing on these prototypes is not what they are built
     out of, the same rule DOM §6's three traverser rows state about NodeFilter. */
  HTMLMeterElement:    [...HTML_BASE, "core/html/html_meter.c"],
  HTMLProgressElement: [...HTML_BASE, "core/html/html_progress.c"],
  /* §3.1.1's partial interface is installed by THREE components and the row named one, which since attribution
     no longer changes the count — it changes what the CROSS-CHECK is over. Named here so that this row states
     what Document is really built out of: §3.1.4/§3.1.5's `cookie`, `referrer`, `lastModified` and
     `readyState` are document_metadata.c's, and §7.1.1.2's `domain` is document_domain.c's, each for the
     reason its own header gives. FULLSCREEN's `fullscreenEnabled` is a FIFTH partial interface and a fifth
     component — its answer is that standard's §7 permissions-policy feature, not anything document.c knows. */
  Document:            ["core/dom/document.c", "core/dom/document_metadata.c", "core/dom/document_domain.c",
                        "core/dom/node.c", "core/events/event_target.c", "core/css/style_sheet_list.c",
                        "core/fullscreen/fullscreen.c"],
  /* DOM §4.5.1 and §4.6 — the interface that BUILDS a document and the interface a doctype IS. DocumentType's
     file list carries node.c for the reason Element's does: it inherits Node, and node.c is also where the
     ChildNode mixin it INCLUDES is installed. DOMImplementation inherits nothing, so it names only its own. */
  DOMImplementation:    "core/dom/dom_implementation.c",
  /* HTML §8.5's two DOM-parsing-and-serialization interfaces. Neither inherits anything, so each names only
     the file that INSTALLS its members — which for XMLSerializer is not where its algorithm lives: DOM Parsing
     and Serialization §3.2.1's XML serialization is core/xml/xml_serialize.c, embedded by this member the way
     core/dom/node.c's `clone a node` is embedded by its callers, because HTML §8.5.4's fragment serializing
     algorithm steps reach the same algorithm with require well-formed TRUE. The row is a CROSS-CHECK over
     where members LAND, so it names the interface file alone and the algorithm file appears in no row at
     all. */
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
const { byName, inheritanceOf, flatten, members, dictByName, dictMembers, dictionaryTypesIn } = idl;

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

/* THE CONSTRUCTOR OPERATIONS AN INTERFACE DECLARES ITSELF — the one statement of "a constructor is not
   inherited", read by both consumers instead of spelled twice. Web IDL §3.7.1 Interface object resolves the
   overload "for constructors with identifier id on interface I", I being the interface the object is FOR and
   never a base it inherits from, and §3.7.3 Interface prototype object's own `constructor` property is a
   different thing entirely — it points AT the interface object and is not a construct step. It is a helper
   rather than a line inside memberOps because the constructor census below asks the identical question, and a
   second copy of the not-inherited rule is the copy that goes on reporting BeforeUnloadEvent with Event's
   `constructor(DOMString, optional EventInit)` after the first is fixed. */
/* NOT-DECLARED IS NOT ZERO-CONSTRUCTORS, AND A `|| { members: [] }` MADE THEM ONE ANSWER. This is
   §A-FIELD-A-CONSUMER-DEFAULTS standing inside an absence counter, which is the worst place for it: a name the
   corpus does not carry came back as an interface with an empty member list, so "this index has never heard of
   it" and "the platform declares it with no constructor" were the same value, and every caller below reads that
   value as the second. It answers `null` for the miss instead, and each caller states which of the two it
   means — the two questions §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS says must not share one bit.
   WHERE THE MISS ACTUALLY REACHES, so the split is not decoration: the row loop's call is unreachable (its
   `noIdl` guard `continue`s whenever `byName` lacks the name, because members() flattens out of byName and so a
   non-empty `spec` already implies the entry exists), and the dictionary arm's second call is guarded by an
   explicit `byName.has`. The one live path is the stray-construct filter over `world.constructs` — a name this
   ENGINE mints an interface object for, which is exactly where a typo'd identifier arrives — and its printed
   line now says which of the two readings it found rather than asserting the constructor one. */
const ownConstructors = (iface) => {
  const node = byName.get(iface);
  return node ? node.members.filter((x) => x.type === "constructor") : null;
};
/* HTML §3.2.3 "HTML element constructors" — "To support the custom elements feature, all HTML elements have
   special constructor behavior. This is indicated via the [HTMLConstructor] IDL extended attribute." */
const isHtmlConstructor = (op) => (op.extAttrs || []).some((a) => a.name === "HTMLConstructor");

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
  /* ResizeObserver's row is GONE, and its reason was the same stale-DFAIL failure IntersectionObserver's was:
     it said "no layout, so no box to observe", which was true about the spec and false about this tree.
     core/layout/used_value.c answers a box's border-edge and content-box extents on either axis and every case
     CSS 2.1 §10 defines and this engine does not compute crashes there naming its own section, so §3.4.8
     "Calculate box size, given target and observed box" had everything it needed. What was left to build was
     §3.4's DELIVERY seam and HTML §8.1.7.3 update the rendering's step 16 loop over it, not the geometry. */
  /* PerformanceObserver's row is GONE, and its reason had gone stale in exactly the way the two rows above
     record: "no performance timeline to observe" was true about this tree until PERFORMANCE TIMELINE §5.1
     Queue a PerformanceEntry landed beside §4 in core/timing/performance_observer.c, and USER TIMING
     §2.1.1 step 2 now calls it. rendering.c's realm_awaits no longer names this interface either — it
     names PerformancePaintTiming, which is the entry these steps would actually mint. */
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
};
const unbuiltSeen = [], unmapped = [], stale = [];

const distinct = new Set();
/* The two ledger categories, split from `distinct` because they are two different things to do: an ABSENT
   member is one to write, a js_noop-STUB member is one to replace. `pairs*` is the same fact per INTERFACE —
   what a page cannot reach, summed — kept beside each so the ledger can print both and neither number has to
   be recovered from the other. */
const distinctAbsent = new Set(), distinctNoop = new Set();
let pairsAbsent = 0, pairsNoop = 0;
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
/* THE CONSTRUCTOR CENSUS — see the loop, which explains why this is an axis of its own and not a member
   column. Three lists rather than two counters, because the denominator has to be printed beside the gap:
   `built + html + ordinary` is every AUDITED interface whose IDL declares a constructor operation, which is
   the only set either gap number means anything against. */
const ctorBuilt = [], htmlCtorAbsent = [], ordinaryCtorAbsent = [], ctorUnproven = [];
/* THE ASYMMETRY THAT LET SIXTY-NINE FALSE ABSENCES PRINT. The MEMBER axis has had an UNPROVEN column since it
   was written — a member whose install construct the scan cannot read is neither counted absent nor credited —
   and the CONSTRUCTOR axis had no counterpart: it asked `!world.constructs.has(iface)` and charged ABSENT, with
   nothing in the question about whether every mint had been READ. idl_installed.mjs's own section 1b already
   said which way that error runs ("an unread constructing mint would read as an interface the engine refuses to
   construct, which is the false direction"), and it ran that way: one unreadable mint reported sixty-nine built
   interfaces as unbuilt, each with an instruction to build HTML §3.2.3, which is built.
   `constructs` IS A SET OF NAMES, so ONE unread mint makes it incomplete by an unknown amount and no
   interface's absence from it proves anything — the abstention is therefore GLOBAL over the axis and not
   per-file, because an unread name could be any interface and a shared helper mints for interfaces audited
   under other components entirely. That is coarse on purpose: the alternative is a scope nothing justifies. */
const ctorUnread = world.constructsUnread;

/* -----------------------------------------------------------------------------------------------------------
 * WEB IDL §3.3.7 [Exposed] — A MEMBER NO REALM THIS ENGINE BUILDS MAY CARRY IS NOT A GAP, AND CHARGING IT AS
 * ONE NAMES AN ACTION THE SPEC FORBIDS.
 *
 * §3.7.7 Operations: "For each unique identifier of an exposed operation defined on the interface, there exist
 * a corresponding property." A member that is NOT exposed in a realm has no property there — so a component
 * that installed one would be wrong, and the verdict's standing instruction to "implement the member in its
 * real component" is not merely unhelpful for it, it is SPEC-WRONG. That is §AN-ASSERT-THAT-NAMES-A-REMEDY
 * living inside an instrument: the finding is right about the FACT (the member really is not installed) and
 * wrong about the ACTION, and a reader who obeys it builds a conformance violation.
 *
 * IT IS DERIVED AND NOT DECLARED, WHICH IS THE WHOLE POINT. `idl_members_excluded` is the engine-side
 * declaration for a member the IDL says exists and a spec's PROSE excludes — its own comment in
 * core/idl_args.c says so: "neither the IDL corpus nor a reader of this prototype can tell that state apart
 * from a member nobody has written yet." [Exposed] is the opposite case: the corpus carries it, in the member's
 * own extended attributes, so a C declaration restating it would be the second copy of a fact whose first copy
 * is the artifact this tool exists to read — the hand-kept table §Browser half bans. Both halves come from
 * artifacts: WHICH GLOBAL NAMES EXIST from the corpus's [Global] annotations, and WHICH OF THEM THIS ENGINE
 * BUILDS from this audit's own interface census, so the day a worker global gets a component the answer moves
 * with it and nothing here is edited.
 *
 * THE SOUND DIRECTION IS TO STAY SILENT. A member whose exposure this index cannot decide — an iteration
 * member minted by an `iterable<>` declaration, which has no named declaration node at all — keeps its place
 * in ABSENT. Absence of evidence must never remove a member from the gap list, because that is the false
 * COMPLETE this whole file refuses. */
/* `EXPOSED_STAR`, `rhsNames` and `extOf` WERE DEFINED HERE AND NOW COME FROM idl_members.mjs, which is a move
   UP rather than a tidy-up: idl_members.mjs's `windowGlobals` needs the same reader to derive §3.4.11's
   identifiers, this file imports THAT file, and a helper kept here is unreachable from it — so the choice was
   between moving the one spelling up and writing a second copy down there. Two of those copies already
   existed: `windowGlobals` open-coded the string/array split for [Exposed] and accepted only a string rhs for
   [LegacyFactoryFunction], and the §3.4.11 source it was missing is what that per-source spelling cost. The
   reader is identical; only its address changed. */
/* §3.3.7's "own exposure set", as a Set of global names or EXPOSED_STAR; null when the construct carries no
   [Exposed] at all, which is the case the algorithm answers by walking outward rather than by defaulting. */
const ownExposure = (node) => {
  const v = rhsNames(extOf(node, "Exposed"));
  return v === null ? null : v === EXPOSED_STAR ? EXPOSED_STAR : new Set(v);
};
/* THE GLOBAL NAMES THE CORPUS DEFINES, per §3.3.8 [Global] "[Global]": "The [Global] extended attribute also
   defines the global names for the interface" — the given identifier, or the identifier list, and a bare
   [Global] takes the interface's own identifier.
   THE SENTENCE THAT USED TO BE QUOTED HERE IS §3.3.7's, NOT §3.3.8's. "Each of the identifiers mentioned must
   be a global name of some interface and be unique" is a constraint [Exposed] places on ITS identifiers; it is
   real, it is quoted correctly, and it was attributed to the section one along — the mis-aimed citation this
   project rates as the harder half of a fabrication, since the words check out and the section does not
   govern them. */
const globalNamesOf = new Map();
for (const n of idl.declarations) {
  if (n.type !== "interface" || !n.name) continue;
  const v = rhsNames(extOf(n, "Global"));
  if (v === null || v === EXPOSED_STAR) continue;
  const names = v.length ? v : [n.name];
  globalNamesOf.set(n.name, new Set([...(globalNamesOf.get(n.name) || []), ...names]));
}
/* WHICH OF THEM THIS ENGINE BUILDS — the [Global] interfaces this audit is auditing. An interface with a row
   is one this engine has a component for, which is exactly the question, and it is the same census every other
   number in this file is drawn from rather than a second opinion beside it. */
const BUILT_GLOBALS = new Set();
for (const [g, names] of globalNamesOf) if (AUDITED.has(g)) for (const n of names) BUILT_GLOBALS.add(n);
/* AN EMPTY SET WOULD SILENTLY EMPTY THE GAP LIST — every member's exposure set would fail to meet it and the
   audit would report a clean bill for a program that installs nothing. That is the largest false COMPLETE this
   file could mint, so it CRASHES here instead of degrading: a run with no global is a run that cannot ask the
   question, never a run whose answer is "nothing is owed". */
if (!BUILT_GLOBALS.size)
  throw new Error("[idl-audit] no [Global] interface is in this audit's census, so Web IDL §3.3.7's exposure " +
                  "question has no realm to be asked about — the engine builds no global, or the census lost " +
                  "the interface that is one");
/* §3.3.7's "exposure set intersection of a construct C and interface-or-null H". */
const exposureIntersect = (c, h) => {
  if (h === null) return c;
  if (c === EXPOSED_STAR) return h;
  if (h === EXPOSED_STAR) return c;
  return new Set([...c].filter((x) => h.has(x)));
};
/* THE ORIGINAL DEFINITION, WHICH `byName` IS NOT — and reading [Exposed] off the wrong one fails in the ONE
   direction an exposure answer must never fail in. `byName` keeps the FIRST node it saw for a name and folds
   later members into it (idl_members.mjs says so), and across a corpus of many specs that first node is often
   a PARTIAL, which carries no [Exposed] of its own. §3.3.7's own walk ends at "set C to the original
   interface, interface mixin or namespace definition of C", so the ORIGINAL is what the algorithm asks about.
   Measured before this map existed: `Window` and `HTMLImageElement` both answered [Exposed=*] — exposed in
   every realm, from a declaration that says Window. */
const originalOf = new Map();
for (const n of idl.declarations)
  if (!n.partial && n.name && !originalOf.has(n.name) &&
      (n.type === "interface" || n.type === "interface mixin" || n.type === "callback interface" ||
       n.type === "namespace"))
    originalOf.set(n.name, n);
const ifaceExposure = (name) => {
  const node = originalOf.get(name) || byName.get(name);
  const own = node ? ownExposure(node) : null;
  return own === null ? EXPOSED_STAR : own;   // no [Exposed] anywhere: this index has nothing to narrow with
};
/* §3.3.7's "get the exposure set of a construct C", for a member declaration `m` written on container `cont`,
   with `host` the including interface when `cont` is an interface mixin and null otherwise. The three walks
   the algorithm makes — member to container, partial to original, mixin to host — are the three arms here. */
const memberExposure = (m, cont, host) => {
  const h = host ? ifaceExposure(host) : null;
  let own = ownExposure(m);
  if (own !== null) return exposureIntersect(own, h);
  own = ownExposure(cont);
  if (cont.partial || cont.type === "interface mixin") {
    if (own !== null) return exposureIntersect(own, h);
    if (cont.type === "interface mixin") return h === null ? EXPOSED_STAR : h;
    return ifaceExposure(cont.name);          // a partial with no [Exposed]: the original definition's
  }
  return own === null ? EXPOSED_STAR : own;
};
/* EVERY DECLARATION OF EVERY MEMBER, filed under the interface it lands on. A name can be declared more than
   once — a base, a partial, a mixin — so the exposure sets are kept as a LIST and a member is exposed here if
   ANY of its declarations is. §3.3.7 requires the overloads of one operation to agree, and says nothing about
   a name two separate declarations both define; taking the union is the direction that cannot manufacture an
   exclusion out of a declaration that does not narrow. */
const memberExposures = new Map();
const fileExposure = (iface, name, set) => {
  if (!memberExposures.has(iface)) memberExposures.set(iface, new Map());
  const per = memberExposures.get(iface);
  if (!per.has(name)) per.set(name, []);
  per.get(name).push(set);
};
const includedBy = new Map();
for (const n of idl.declarations)
  if (n.type === "includes") includedBy.set(n.includes, [...(includedBy.get(n.includes) || []), n.target]);
const namedMember = (m) => (m.type === "attribute" || m.type === "operation" || m.type === "const") && m.name;
for (const n of idl.declarations) {
  if (!n.name) continue;
  if (n.type === "interface" || n.type === "callback interface" || n.type === "namespace") {
    for (const m of n.members || []) if (namedMember(m)) fileExposure(n.name, m.name, memberExposure(m, n, null));
  } else if (n.type === "interface mixin") {
    for (const host of includedBy.get(n.name) || [])
      for (const m of n.members || []) if (namedMember(m)) fileExposure(host, m.name, memberExposure(m, n, host));
  }
}
/* The one question a row asks: is this member exposed in some realm this engine builds. `null` means the index
   has no declaration for the name — see the sound-direction paragraph above; the caller keeps such a member. */
const exposureHere = (chain, name) => {
  const sets = [];
  for (const c of chain) {
    const per = memberExposures.get(c);
    if (per && per.has(name)) sets.push(...per.get(name));
  }
  if (!sets.length) return null;
  const reach = sets.some((s) => s === EXPOSED_STAR || [...s].some((g) => BUILT_GLOBALS.has(g)));
  return { reach, sets };
};
const showExposure = (s) => (s === EXPOSED_STAR ? "*" : `(${[...s].join(",")})`);

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
  /* CAN A PAGE `new` THIS INTERFACE — THE MEMBER CENSUS ABOVE CANNOT ASK, AND ITS SILENCE READ AS A YES.
     `spec` is idl_members.mjs's member() list, which collects `attribute`, `operation` and `const`, so a
     `constructor` operation is in NO denominator anywhere in this file: an interface whose every member is
     installed and whose constructor is Web IDL §3.7.1 Interface object's shared "Illegal constructor"
     TypeError printed `complete`. That is a false clean bill on ordinary page code — `new Comment("x")` is
     DOM §4.14 Interface Comment and it throws — and an entire member kind outside the denominator is the
     largest form of the coverage figure §Testing says must state what it is a fraction of.
     IT IS ITS OWN AXIS AND IS NOT FOLDED INTO THE MEMBER COUNTS, deliberately. A constructor is not a property
     of the interface prototype object: §3.7.1 gives the interface OBJECT [[Construct]] steps, and §3.7.3's
     `constructor` property points at that object rather than being one. Adding it to `spec` would make the
     audit hunt for an installed property called "constructor", which is a different fact and one every
     prototype already satisfies — so it would answer a question nobody asked and silence this one. It is one
     bit per interface (the overloads are one question, "does `new` run anything"), it is NOT inherited
     (ownConstructors), and it enters neither `distinctAbsent`, `pairsAbsent` nor `totalMissing`. */
  const ctors = ownConstructors(iface);
  /* The `noIdl` guard above already `continue`d for every name this index does not carry, so `null` here would
     mean that guard stopped holding — asserted rather than defaulted past, because the default it replaces is
     what made an unknown interface read as one with no constructor. */
  if (ctors === null)
    throw new Error(`[idl-audit] ${iface} reached the constructor census and the IDL index does not carry it — ` +
                    `the noIdl guard above is meant to make that unreachable`);
  /* NOT IN `constructs` IS TWO ANSWERS AND THIS ASKS WHICH. With every mint read it means the engine refuses;
     with one unread it means this scan does not know, and saying "absent" then is a claim the evidence does not
     carry. See ctorUnread above for why the second reading is global rather than per-site. */
  const ctorMissing = ctors.length && !world.constructs.has(iface);
  const ctorAbsent = ctorMissing && !ctorUnread.length;
  /* HTML §3.2.3's population is counted apart, because it is ONE piece of work and not sixty-nine. §3.2.3 says
     interfaces annotated with [HTMLConstructor] "have the following overridden constructor steps" — one
     algorithm, shared, parameterised by the interface, which is why custom_elements.c's single
     custom_elements_html_constructor already serves HTMLElement. Summing them with the ordinary constructors
     would report seventy-three things to build where there are four plus one mechanism, and a count that mixes
     two questions is the defect this audit keeps finding in itself. */
  const ctorIsHtml = ctors.some(isHtmlConstructor);
  if (ctorAbsent) (ctorIsHtml ? htmlCtorAbsent : ordinaryCtorAbsent).push(iface);
  else if (ctorMissing) ctorUnproven.push(iface);
  else if (ctors.length) ctorBuilt.push(iface);
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
  const noop = spec.filter((n) => stubbed.has(n));
  /* A js_noop-STUB IS PRESENT ON THE OBJECT, SO IT IS NOT ABSENT — and it was counted in both, because
     `stubbed` and `installed` are the two halves of ONE split (idl_installed.mjs files each record under
     exactly one of them) and only `installed` was subtracted here. The two categories are declared distinct
     where they are minted — "an ABSENT member is one to write, a js_noop-STUB member is one to replace" — so a
     member in both is ONE thing to do reported as two, inflating `totalMissing`, the ledger's two distinct
     sets, and the OWN and ABSENT columns together. Nothing in the row can be asked about it: the member's name
     simply appears in two lists a reader takes to be alternatives. It is removed at the ROOT rather than
     asserted about, because the categories being disjoint is what the labels below promise — subtract the
     stubs here and the promise is true by construction rather than by a check somebody keeps passing. */
  const stubSet = new Set(noop);
  /* WEB IDL §3.3.7 — see the derivation above. A member whose exposure set meets no global this engine builds
     leaves the gap list: there is nothing to implement, because §3.7.7 Operations gives a property only to an
     EXPOSED member and installing this one would be the violation. It is NAMED rather than dropped, with its
     exposure set printed, so nobody works it off a shorter list and nobody re-derives the same question.
     THE OTHER SIDE IS CHECKED, which is what makes this a derivation and not a filter: a member the engine
     INSTALLS that this computation says is not exposed here is a contradiction between the corpus and the
     program, and it is loud. Without it the exposure rule could only ever remove work, and a rule that can
     only subtract is one no evidence can refute. */
  const notExposed = [], installedNotExposed = [];
  for (const n of spec) {
    const e = exposureHere(chain, n);
    if (!e || e.reach) continue;
    if (installed.has(n) || stubSet.has(n)) installedNotExposed.push([n, e.sets]);
    else if (!condNames.has(n)) notExposed.push([n, e.sets]);
  }
  const notExposedSet = new Set(notExposed.map(([n]) => n));
  defect("members installed that Web IDL §3.3.7 [Exposed] does not expose in any global this engine builds",
         installedNotExposed.length);
  const open = spec.filter((n) => !installed.has(n) && !stubSet.has(n) && !condNames.has(n) &&
                                  !notExposedSet.has(n));
  const absent = open.filter((n) => !maybeHere.has(n));
  const unproven = open.filter((n) => maybeHere.has(n));
  totalMissing += absent.length + noop.length;
  for (const n of absent) { distinct.add(n); distinctAbsent.add(n); pairsAbsent++; }
  for (const n of noop) { distinct.add(n); distinctNoop.add(n); pairsNoop++; }
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
  /* THE SURFACE EACH COLUMN IS A GAP IN, carried with the gap. A bare count is read against whatever set its
     label names, so a gap column named after a member set is read AS that member set — see the ranking
     header below, which is where that reading was available and was taken. `spec.length` is the whole
     flattened surface ABSENT is drawn from and `ownSet.size` is the declared-here surface OWN is drawn from;
     the two differ by exactly the inherited members. */
  gapRows.push({ iface, absent: absent.length, noop: noop.length, unproven: unproven.length,
                 own: ownAbsent + ownNoop, spec: spec.length, ownSpec: ownSet.size });
  /* THE LEDGER COUNTS MEMBERS, NOT INTERFACE-MEMBER PAIRS, AND THAT IS NOT THE SAME NUMBER — see the two
     comments above and the ranking below, which both already say that a per-interface sum measures
     INHERITANCE DEPTH. `absent.length` summed over rows charges ONE unbuilt member once per interface that
     inherits it: `checkVisibility` is one thing to write in element_view.c and it was 46 of this ledger's
     count. The verdict's own closing sentence commands "implement the member in its real component", so the
     number beside it has to be a count of members somebody implements — a count of pairs names no set of
     actions, and being ten times larger it is also what buried SubtleCrypto's 17 and MouseEvent's 2 in it.
     Nothing is hidden by this: the pair total is printed in the category's own label and again in the
     distinct-members line above, and the exit code is unchanged, because a category is non-empty either way.
     The per-interface rows stay exact — each still names every member a page cannot reach on that interface,
     which is the honest answer to a different question. */
  blind("UNPROVEN members — installed on a target the audit cannot attribute", unproven.length);
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
  /* THE ROW SAYS IT, not only the ledger. A row is what a person reads when they pick an interface up, and
     `${iface}: complete` is the sentence that has to stop being printed for an interface a page cannot `new` —
     the ledger totals below are the falsifiable check, the row is the thing that stops sending someone away. */
  if (ctorAbsent)
    parts.push(`ABSENT CONSTRUCTOR — its IDL declares ${ctors.map((op) => `constructor(${(op.arguments || [])
      .map((a) => `${a.optional ? "optional " : ""}${a.idlType && a.idlType.idlType ? (typeof a.idlType.idlType === "string" ? a.idlType.idlType : "…") : "…"} ${a.name}`)
      .join(", ")})`).join(" / ")}${ctorIsHtml ? " [HTMLConstructor]" : ""} and no mint in this engine gives ` +
      `its Web IDL §3.7.1 Interface object [[Construct]] steps, so \`new ${iface}()\` is the shared "Illegal ` +
      `constructor" TypeError${ctorIsHtml ? " — HTML §3.2.3 \"HTML element constructors\", whose steps are " +
      "OVERRIDDEN and shared, so this is one mechanism and not one job per interface" : ""}`);
  /* THE ROW SAYS THE THIRD ANSWER TOO, in the same place and for the same reason: `${iface}: complete` and
     `ABSENT CONSTRUCTOR` are both wrong sentences for an interface whose mint this scan could not read. */
  else if (ctorMissing)
    parts.push(`UNPROVEN CONSTRUCTOR — its IDL declares a constructor${ctorIsHtml ? " [HTMLConstructor]" : ""} ` +
               `and no mint this scan READ names it, but ${ctorUnread.length} constructing mint(s) in this ` +
               `engine name an interface the scan could not read (see the verdict), so whether \`new ${iface}()\` ` +
               `runs anything is not decided here`);
  if (unproven.length) parts.push(`UNPROVEN ${unproven.length} — ${unproven.map((n) => {
    const r = maybeHere.get(n);
    return `${n} (installed at ${r.file.replace(BROWSER + "/", "")}:${r.line}, ${r.why})`;
  }).join("; ")}`);
  if (noop.length) parts.push(`js_noop-STUB ${noop.length} — ${noop.join(", ")}`);
  if (plain.length) parts.push(`PLAIN-PROPERTY ${plain.length} — ${plain.join(", ")} (written with ` +
                               `JS_SetPropertyStr onto an object no interface declaration reaches: either a ` +
                               `member installed as a plain own property, or a record field of the same name)`);
  if (cond.length) parts.push(`CONDITIONAL ${condNames.size} — ${[...condNames].join(", ")} (${cond[0].why})`);
  if (notExposed.length) parts.push(`NOT-EXPOSED ${notExposed.length} — ${notExposed.map(([n, sets]) =>
    `${n} [Exposed=${sets.map(showExposure).join(" | ")}]`).join(", ")} (Web IDL §3.3.7 [Exposed]: the ` +
    `member's exposure set meets no global this engine builds — ${[...BUILT_GLOBALS].join(", ")} — and ` +
    `§3.7.7 Operations gives a property only "for each unique identifier of an exposed operation defined on ` +
    `the interface", so this is a member to NOT install and there is nothing here to write. Derived from the ` +
    `corpus's own extended attributes, so it needs no declaration and must not be given one)`);
  if (installedNotExposed.length) parts.push(`INSTALLED BUT NOT EXPOSED ${installedNotExposed.length} — ` +
    `${installedNotExposed.map(([n, sets]) => `${n} [Exposed=${sets.map(showExposure).join(" | ")}]`).join(", ")}` +
    ` is on this prototype and Web IDL §3.3.7 exposes it in no global this engine builds — remove the install ` +
    `or build the global its exposure set names`);
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
/* WHAT THIS CATEGORY HAS RULED OUT, AND THE ONE THING IT CANNOT — because the verdict's closing sentence used
   to name ONE action for it ("implement the member in its real component") and that action is WRONG for part
   of the population, which is §AN-ASSERT-THAT-NAMES-A-REMEDY inside an instrument: right about the FACT, wrong
   about the ACTION, and a reader who obeys it builds the wrong thing. Measured: of three findings examined in
   one session, TWO were correct absences whose declared remedy was wrong, one of them because the spec states
   the member has no getter steps at all — so the count was honest and the instruction beside it was not.
   FOUR of the five states a not-installed member can be in are decided mechanically and are subtracted before
   this count: installed elsewhere on the chain, js_noop-stubbed, declared CONDITIONAL by idl_members_excluded,
   and §3.3.7-unexposed. The fifth is a spec's PROSE — steps that are stated to do nothing, or a condition the
   IDL cannot carry — and no reading of the corpus can see it, so this label states BOTH actions rather than
   picking the commoner one. Naming the declaration as an equal outcome is what stops a reader working the list
   off by writing members the spec forbids. */
defect(`ABSENT members (distinct; ${pairsAbsent} interface-member pairs a page cannot reach) — each is ` +
       `EITHER a member to implement in its real component OR, where the spec's prose states the member has ` +
       `no steps or excludes it under a condition this user agent does not meet, an idl_members_excluded ` +
       `declaration to make at the component's prototype build; this audit cannot tell those two apart and ` +
       `does not claim to`, distinctAbsent.size);
defect(`js_noop-STUB members (distinct; ${pairsNoop} interface-member pairs)`, distinctNoop.size);
/* THE TWO CONSTRUCTOR CATEGORIES, EACH WITH THE SET IT IS A FRACTION OF IN ITS OWN LABEL. They are separate
   because the WORK is separate, not because the count looks better split: HTML §3.2.3's are one shared
   overridden algorithm reached from sixty-odd interface objects, and the ordinary ones are that many distinct
   construct steps to write. One count over both would name neither job. */
const ctorDeclaring = ctorBuilt.length + htmlCtorAbsent.length + ordinaryCtorAbsent.length + ctorUnproven.length;
blind(`interfaces whose \`new\` this audit CANNOT DECIDE — ${ctorUnread.length} constructing mint(s) name an ` +
       `interface this scan could not read, so the set of names it minted is incomplete by an unknown amount ` +
       `and no interface's absence from it proves a thing (of ${ctorDeclaring} audited interfaces declaring a ` +
       `constructor). Closed by making the mint READABLE — a name the scan can resolve at the site or through ` +
       `the callers of the helper that was handed it — never by charging these ABSENT, which is the false ` +
       `direction that reports a built interface as unbuilt`, ctorUnproven.length);
for (const u of ctorUnread)
  console.log(`[idl-audit] UNREADABLE MINT ${u.file.replace(BROWSER + "/", "")}:${u.line} — \`${u.form}\` is ` +
              `handed \`${u.expr}\`, which this scan cannot resolve to an interface identifier. While this ` +
              `stands, the whole constructor axis abstains.`);
defect(`interfaces a page cannot \`new\` — §3.7.1 construct steps absent (of ${ctorDeclaring} audited ` +
       `interfaces whose IDL declares a constructor operation, ${ctorBuilt.length} of which this engine mints; ` +
       `HTML §3.2.3's [HTMLConstructor] population is its own category in this ledger)`, ordinaryCtorAbsent.length);
defect(`[HTMLConstructor] interfaces a page cannot \`new\` — HTML §3.2.3 "HTML element constructors", ONE ` +
       `shared overridden algorithm (of ${ctorDeclaring} audited interfaces declaring a constructor)`,
       htmlCtorAbsent.length);
if (ordinaryCtorAbsent.length)
  console.log(`[idl-audit] ${ordinaryCtorAbsent.length} interface(s) whose own construct steps are unbuilt — ` +
              `${ordinaryCtorAbsent.join(", ")}. Each is its interface's own algorithm to write in its ` +
              `component and to mint with idl_step_constructor; a page that writes \`new X()\` gets a TypeError.`);
if (htmlCtorAbsent.length)
  console.log(`[idl-audit] ${htmlCtorAbsent.length} [HTMLConstructor] interface(s) a page cannot \`new\` — ` +
              `HTML §3.2.3's steps are OVERRIDDEN and shared, and custom_elements.c already runs them for ` +
              `HTMLElement, so this is ONE mechanism (route each element interface object through it) rather ` +
              `than ${htmlCtorAbsent.length} algorithms: ${htmlCtorAbsent.join(", ")}`);
/* THE OTHER SIDE, so neither list can go stale in silence: a name this engine gives [[Construct]] that the
   platform declares no constructor operation for. Web IDL §3.7.2 "Legacy factory functions" is the one legitimate
   shape — `[LegacyFactoryFunction=Image(…)]` puts a constructible `Image` on the global under a name that is
   no interface's — so those are recognised from the IDL rather than excused by name, and anything left is a
   mint naming something the corpus does not declare, which is a typo the run must not swallow. */
const legacyFactories = new Set();
/* THE DECLARATION, KEPT WITH THE INTERFACE IT IS ON — see the presence audit below, which is why this is a Map
   and not only the Set the stray filter needs. */
const legacyFactoryOf = new Map();
for (const n of idl.declarations)
  for (const a of n.extAttrs || [])
    if (a.name === "LegacyFactoryFunction" && a.rhs && typeof a.rhs.value === "string") {
      legacyFactories.add(a.rhs.value);
      if (!legacyFactoryOf.has(a.rhs.value)) legacyFactoryOf.set(a.rhs.value, n.name);
    }

/* WEB IDL §3.7.2 "Legacy factory functions" — THE PRESENCE QUESTION, WHICH NOTHING IN THIS TREE ASKED. The set
   above was computed to SUBTRACT: it excused a name in `constructs` that the corpus declares no interface for,
   and was read in that one direction only. A set you already derive and only ever subtract from is a set you
   can also ASSERT over, and the cost of not doing so is measurable rather than theoretical — `Audio` was
   declared by the platform, absent from this engine, and reported by no gate in the tree until a person read
   the IDL by hand. That is the same shape as a read-with-no-writer: the fact was computed, and consumed by
   nothing that could fail.
   IT IS ITS OWN AXIS, and the axis is PRESENCE OF A GLOBAL NAME — not members, not arity, not what the factory
   accepts. §3.7.2: "A legacy factory function that exists due to one or more [LegacyFactoryFunction] extended
   attributes with a given identifier is a built-in function object." It is a name on the global that
   CONSTRUCTS, so `world.constructs` is exactly the right evidence and the same evidence the constructor axis
   uses — which is why it inherits that axis's abstention: while a mint is unread, absence proves nothing here
   either.
   AN INTERFACE THIS ENGINE DOES NOT BUILD OWES NO FACTORY: the extended attribute is on the interface, so the
   factory's absence is the interface's absence and is already accounted for by UNBUILT / the unmapped list.
   Reporting it twice would charge one gap to two axes. */
const legacyFactoryAbsent = [], legacyFactoryUnproven = [];
for (const [id, iface] of [...legacyFactoryOf].sort()) {
  if (!AUDITED.has(iface) || UNBUILT[iface]) continue;
  if (world.constructs.has(id)) continue;
  (ctorUnread.length ? legacyFactoryUnproven : legacyFactoryAbsent).push([id, iface]);
}
defect(`Web IDL §3.7.2 "Legacy factory functions" the platform declares that this engine does not install (of ` +
       `${[...legacyFactoryOf].filter(([, i]) => AUDITED.has(i) && !UNBUILT[i]).length} declared on an ` +
       `interface this engine builds). Each is a GLOBAL NAME that constructs, minted by the component that ` +
       `owns the interface and hung on the global beside the interface object — never on the interface ` +
       `object's own prototype, since §3.7.2's function has no \`constructor\` back-pointer`,
       legacyFactoryAbsent.length);
for (const [id, iface] of legacyFactoryAbsent)
  console.log(`[idl-audit] ${id}: the corpus declares \`[LegacyFactoryFunction=${id}(…)]\` on ${iface} and no ` +
              `mint in this engine gives the global name \`${id}\` [[Construct]] steps, so a page writing ` +
              `\`new ${id}(…)\` gets a TypeError no browser gives it. §3.7.2's own steps are the job: overload ` +
              `resolution, internally create a new object implementing ${iface}, run the constructor steps, ` +
              `and a non-writable non-configurable \`prototype\` pointing at ${iface}'s interface prototype ` +
              `object.`);
blind(`Web IDL §3.7.2 legacy factory functions whose presence this audit CANNOT DECIDE — see the unreadable ` +
       `mint(s) above`, legacyFactoryUnproven.length);
/* THE LIVE PATH FOR `ownConstructors`'s MISS — see its own comment. A name here is one the ENGINE mints, so it
   need not be an interface at all, and the two readings are told apart rather than merged: the corpus carries
   the interface and it declares no constructor, or the corpus carries no such name. Both belong in this one
   category (the engine gives [[Construct]] to something the platform does not construct either way) and each
   row says WHICH, because the fix differs — write the mint's justification against the IDL, or fix a typo. */
const strayAll = [...world.constructs]
  .filter((n) => { const c = ownConstructors(n); return !legacyFactories.has(n) && !(c && c.length); }).sort();
/* AND THE ROW'S THIRD EXPLANATION IS A REASON NOT TO CHARGE THE NAME AT ALL, WHICH THE ROW SAID AND THIS
   COUNTER DID NOT. The message below has always offered a forwarded name three readings, the third being that
   "the C's own row filter removes this row and this scan cannot evaluate it" — and then counted the name as a
   DEFECT anyway, which is a verdict the audit's own sentence says it cannot reach. That is the three-states-
   behind-one-answer shape arriving inside the instrument built to end it: a typo, a mint that needs its
   justification written, and a construct this scan cannot see are three findings, and only the first two are
   the engine's.
   MEASURED, AND IT WAS THE WHOLE OF THIS CATEGORY. `HTMLUnknownElement` is one cell of the element-interface
   table's `iface` column, which core/html/html_element.c's install loop hands to a shared minting helper — and
   that loop's FIRST statement is `if (iface_is_base(HTML_IFACE[i].iface)) continue;`, so the name never reaches
   the mint. Its interface object is installed two lines above the loop by node_install_interface, whose
   constructor is the shared Illegal-constructor throw, so `new HTMLUnknownElement()` already does exactly what
   Web IDL §3.7.1 Interface object requires — which is what the HTML Standard's own IDL asks for, declaring the
   interface under `// Note: intentionally no [HTMLConstructor]`. The audit was right about the column and wrong
   about the engine, and it named a fidelity bug that is not there.
   WHY THE ABSTENTION IS PER-NAME HERE AND GLOBAL ON THE CONSTRUCTOR AXIS. `ctorUnread` abstains over the whole
   axis because an unread mint could name ANY interface, so no interface's ABSENCE from `constructs` proves
   anything. This is the opposite direction: the name is PRESENT, and what is unproven is only whether the
   filtered loop reaches THAT cell — a question about one column, answerable for the names in it and nobody
   else's. So the two abstentions have different scopes because they are abstaining about different things.
   WHAT WOULD DECIDE ONE. Nothing in this tree lets a filtered loop state which of its column's names it gave
   [[Construct]] to: `idl_install_covers_column` asserts every name of a column is an OWN PROPERTY of the
   target, which is the PRESENCE axis and answers the same for an interface object that constructs and one that
   throws. A constructor-axis counterpart — a declaration the C makes at the minting loop, asserting of each
   covered name that its interface object's [[Construct]] is §3.2.3's rather than the shared throw — is what
   would move these names out of this band, and it does not exist yet. Until it does, ABSTAIN: a wrong
   accusation here costs a reader a hunt for a bug that is not there, and this one already did. */
const strayForwarded = (n) => world.constructsForwarded.has(n) && !world.constructsDirect.has(n);
const strayConstructs = strayAll.filter((n) => !strayForwarded(n));
const strayUnproven = strayAll.filter(strayForwarded);
defect("interface objects this engine gives [[Construct]] that the platform declares no constructor for",
       strayConstructs.length);
blind("names a filtered install loop's COLUMN carries whose mint this scan cannot prove reaches them — " +
       "neither charged as a stray construct nor credited as constructing", strayUnproven.length);
/* THE ROW STATES HOW THE NAME WAS READ, because the two readings do not support the same accusation and the
   difference decides which of three things a reader should go and do. A name read as a LITERAL beside the mint
   is evidence about THAT interface and the row's original two branches are the whole answer. A name read out of
   a table COLUMN handed to a shared minting helper is one CELL of the set that helper's caller loops over, and a
   C loop may `continue` past a row for a reason no static reader evaluates — so the column proves the mint
   REACHES the name, and a row filter this scan cannot see is the third explanation. Printing the first two
   alone for a forwarded name is the same defect this diff is closing, one category over: right about the
   observable, wrong about the cause, with an instruction ("fix a typo") that does not fit what happened. */
const strayWhat = (n) => byName.has(n) ? `the corpus declares \`${n}\` with no constructor operation`
                                       : `NO IDL in the corpus declares \`${n}\` at all`;
for (const n of strayConstructs)
  console.log(`[idl-audit] ${n}: this engine mints a CONSTRUCTING interface object for it and ` +
              `${strayWhat(n)} — either the identifier beside the mint misspells an interface, or the mint is ` +
              `a Web IDL §3.7.2 "Legacy factory functions" name this index does not carry. Web IDL §3.7.1's ` +
              `construct steps throw a TypeError for an interface not declared ` +
              `with one, so a page reaching this gets behaviour no browser has`);
/* THE ABSTENTION'S OWN ROWS — a work queue for the reader and never an accusation, which is the difference the
   counter above now keeps. Each says exactly what was read and where, so the one command that settles it (open
   the loop and look at its `continue`) is obvious, and so that a name whose loop really does mint it is still
   visible rather than dropped. */
for (const n of strayUnproven) {
  const via = world.constructsForwarded.get(n);
  console.log(`[idl-audit] ${n}: ${strayWhat(n)}, and this engine's mint for it CANNOT BE DECIDED from the ` +
              `source: the name was not written beside the mint — ` +
              `${via.mint.file.replace(BROWSER + "/", "")}:${via.mint.line}'s \`${via.mint.form}\` is handed a ` +
              `parameter, and this name is one cell of \`${via.read.expr}\` at ` +
              `${via.read.file.replace(BROWSER + "/", "")}:${via.read.line}. The column proves the mint could ` +
              `reach the name; a \`continue\` in that loop is C this scan does not evaluate, so the readings ` +
              `are three and not two — the identifier misspells an interface, the mint needs its justification ` +
              `written against the IDL, or the loop's row filter removes this row and the engine is already ` +
              `right. Deciding it needs the C to state the CONSTRUCTOR-axis counterpart of ` +
              `idl_install_covers_column, which asserts presence and answers the same for an interface object ` +
              `that constructs and one that throws.`);
}
if (totalMissing)
  console.log(`[idl-audit] ${distinct.size} distinct spec members this engine does not install (${totalMissing} ` +
              `across all interfaces, since an inherited gap is absent on each) — see the ABSENT category in ` +
              `the verdict for the two outcomes that population holds; never a stub either way.`);
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
blind("install constructs whose member name is not statically resolvable", unresolvedAll.size);
if (unresolvedAll.size) {
  console.log(`[idl-audit] ${unresolvedAll.size} install construct(s) whose member name could not be resolved ` +
              `statically — neither counted as installed nor reported as a gap:`);
  for (const u of unresolvedAll.values())
    console.log(`[idl-audit]   ${u.file.replace(BROWSER + "/", "")}:${u.line}  ${u.form}(… ${u.expr} …)`);
}
/* The same constructs in components no row names. The scan is over the whole program now, so these exist and
   are counted; hiding them behind "no row asked" would be the audit choosing what to know about itself. */
const elsewhere = world.unresolved.filter((u) => !unresolvedAll.has(`${u.file}:${u.line}:${u.expr}`));
blind("install constructs whose member name is not statically resolvable", elsewhere.length);
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
blind("facts a parsing primitive could not read that the analysis then depended on", env.refusals.length);
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
blind("install forms whose property kind is not declared, over a member the IDL declares", kindUndeclared.length);
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
blind("installed members whose target interface could not be decided", unattributed.length);
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
blind("install sites whose selected subset could not be computed", world.unselected.length);
for (const u of world.unselected)
  console.log(`[idl-audit] ${u.file.replace(BROWSER + "/", "")}:${u.line}  ${u.fn}() — ${u.why}`);
defect("interface tags naming something the IDL corpus does not declare", unknownTags.length);
blind("install targets whose interface tag is not statically decidable", env.tagIssues.length);
/* ONE CATEGORY HELD BOTH ANSWERS, AND THEY TAKE OPPOSITE WORK. A prototype the corpus CONTRADICTS is a
   statement about the engine; an interface object this detector could not REACH is a statement about this run,
   so a single count of the two cannot be filed in either ledger without being wrong about half of it — and it
   was the one category here that could not be classified at all until it was split. The split reads the
   producer's OWN `kind`, which idl_installed.mjs already stamps on every record and which the row printer
   below already branches on; it does not re-derive the distinction, which would be the second copy. A kind in
   neither arm is FATAL rather than filed under neither: a third answer landing in no ledger is exactly the
   silent drop this split exists to remove, and the run that adds one is the run that must classify it. */
const TAGCHECK_KINDS = new Set(["contradicted", "unreached"]);
const tagCheckUnclassified = env.tagChecks.filter((c) => !TAGCHECK_KINDS.has(c.kind));
if (tagCheckUnclassified.length)
  throw new Error(`[idl-audit] idl_installed.mjs raised ${tagCheckUnclassified.length} tagCheck(s) of a kind ` +
                  `this verdict does not classify (${[...new Set(tagCheckUnclassified.map((c) => c.kind))]
                    .join(", ")}) — file each as a FINDING about the engine or a BLIND SPOT of this audit at ` +
                  `this site; a kind in neither ledger is counted nowhere and reported as nothing.`);
defect("interface objects whose prototype identity the corpus CONTRADICTS",
       env.tagChecks.filter((c) => c.kind === "contradicted").length);
blind("interface objects whose prototype identity this run could not REACH",
      env.tagChecks.filter((c) => c.kind === "unreached").length);
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
 * WEB IDL §2.7 Dictionaries — WHAT A PAGE MAY HAND THE PLATFORM, AGAINST WHAT THE ENGINE DECLARES IT READS.
 *
 * EVERY CATEGORY ABOVE ASKS WHETHER A MEMBER IS THERE. This asks what a member ACCEPTS, and it is the half the
 * audit was structurally unable to see: a dictionary has no interface object, no prototype and no installed
 * member, so nothing about it reaches the install scan — a spec type made ENTIRELY of the thing this file looks
 * at was invisible to it, in both directions at once. The cost is not coverage for its own sake. A dictionary
 * is how a page hands the platform an instruction — `{once: true}`, `{signal}`, `{credentials: "include"}`,
 * `{window: null}` — so a member the engine never reads is an instruction SILENTLY IGNORED: nothing is absent,
 * there is no js_noop stub to point at, and the page's own behaviour diverges with nothing to say so. That is
 * §NO STUBS' failure with no stub in it. §3.2.17 Dictionary types makes the same absence a CONVERSION THAT
 * NEVER RAN — its ES-to-IDL list reads each member with `? Get(jsDict, key)` and converts what it got by the
 * member's own type, both of which are the page's code, so an unread member is also a getter that did not fire
 * and a `toString` that did not throw.
 *
 * WHAT THE RIGHT-HAND SIDE IS, AND WHY IT IS NOT THE INSTALL SCAN. There is nothing installed to diff against;
 * what the engine states about a dictionary is core/idl_args.h's `IdlDictMember` list — the member's name, its
 * type, whether it is `required`, and which level of the inheritance chain declares it. That declaration IS the
 * engine's answer to "what does this operation accept", so the diff is against it and against nothing else.
 *
 * WHICH DICTIONARIES ARE AUDITED IS DERIVED, exactly as the interface set is: the corpus declares 900-odd and
 * this engine can never receive most of them, so a gap in one is not a gap — it is an unimplemented API, which
 * the ABSENT ledger above already reports as the missing MEMBER that would have taken it. The audited set is
 * therefore the dictionaries reachable from the arguments of the members this engine actually installs, walked
 * through unions, sequences, records and nullables because that is where a dictionary hides: an options bag
 * spelled `(AddEventListenerOptions or boolean)` is one level down, and reading only the top of the type would
 * report every such member COMPLETE.
 *
 * HOW A C DECLARATION IS MATCHED TO A DICTIONARY, AND THE ONE THING THAT IS BELIEVED. An `IdlDictDecl` NAMES
 * its dictionary, so those are matched exactly and checked in full — membership both ways, `required`, and
 * §3.2.17's READ ORDER, which is observable through a getter and which the engine's own init asserts. A bare
 * `IdlDictMember` array (the form a declared ARGUMENT position takes) names nothing, and this audit does not
 * guess which dictionary it is: it uses the one thing that is TRUE of every candidate — an array can only be a
 * declaration of a dictionary that CONTAINS ALL OF ITS MEMBERS — and credits a dictionary with the names of
 * every array that could be its declaration. That direction never sends anyone to build a member that is
 * already there. It can under-report, and the residual says exactly how.
 *
 * NAMED RESIDUAL — THE ARRAY IS NOT PAIRED WITH ITS DICTIONARY, ONLY CONSTRAINED BY IT. Two dictionaries whose
 * member sets nest (`EventListenerOptions` inside `AddEventListenerOptions`) each get credited with the smaller
 * array, so a member missing from the LARGER one's declaration reads as covered when some other array supplies
 * the name. The next diff pairs an array with its dictionary through the site that USES it — the
 * `idl_method_id_dict` / `idl_method_id_step` call, whose member's own IDL argument list is where the
 * dictionary type is stated and the only place it is stated — which turns every subset credit into an identity
 * and lets the `required`, order and level checks run on the bare form too. ITS ABSENCE SHOWS as a dictionary
 * reported complete whose component declares no array of its own: the credit came from a neighbour. */
const dictHave = new Map();          /* dictionary name -> the member names some C declaration could give it */
const dictNamed = new Map();         /* dictionary name -> the named IdlDictDecl that states it */
const dictUnknownName = [];

/* THE C-SIDE READ IS engine/idl_dictdecl.mjs's AND NOT THIS FILE'S. It was written here and moved out unchanged
   when the dictionary-member TYPE axis needed the same `IdlDictMember` initialiser lists: two readers of one
   construct are two answers to "what does this engine declare about this dictionary", and the one that drifts
   is the copy whose consumer runs less often. What stays HERE is the question this audit asks of the read —
   membership, `required` and §3.2.17's read order — and the classification of a named declaration's IDENTIFIER,
   which is an IDL-side fact the reader has no business holding: it reports the identifier the C states, and
   whether any spec defines a dictionary by that name is this file's question and not the reader's. */
const { arrays: dictArrays, named: dictNameds, unreadable: dictUnreadable } = readDictDecls(env);
for (const n of dictNameds) {
  if (!dictByName.has(n.name)) { dictUnknownName.push({ file: n.file, line: n.line, name: n.name }); continue; }
  dictNamed.set(n.name, { ...n.arr, decl: n.decl, file: n.file, line: n.line });
}
/* THE SUBSET CREDIT, stated once: an array can only be the declaration of a dictionary that has every member it
   names, so its names are what SOME declaration of that dictionary would contribute. Computed over the audited
   set only, so an array is never weighed against a dictionary the engine can never be handed. */
const creditSubsets = (name) => {
  const spec = new Set(dictMembers(name).map((m) => m.name));
  const have = new Set();
  for (const a of dictArrays)
    if (a.members.length && a.members.every((m) => spec.has(m.name)))
      for (const m of a.members) have.add(m.name);
  return have;
};

/* THE AUDITED SET — every dictionary an installed member's IDL can hand this engine. A record names the member
   and the interface its target is, which is what the whole audit above already established; the IDL then states
   what that member ACCEPTS. A record whose NAME is itself an interface is an interface object install, and
   §3.7.1's interface object is what a page calls `new` on, so its constructor's arguments count too — that is
   how every `FooEventInit` in the platform is reached, since no operation anywhere takes one. TWO THINGS THAT
   SENTENCE DOES NOT SAY, both of which it was read as saying: the constructor counted is the interface's OWN
   (see memberOps), and it counts only where THIS ENGINE mints one (see world.constructs at the loop below).
   An interface object exists for every exposed interface either way, so its install is evidence of neither. */
const opsOf = new Map();
const memberOps = (iface) => {
  if (opsOf.has(iface)) return opsOf.get(iface);
  /* AN INTERFACE THIS INDEX DOES NOT CARRY HAS NO OPERATION SURFACE TO READ, and answering an EMPTY one is the
     silent half of the default `ownConstructors` used to hold: `flatten` returns [] for an unknown name too, so
     the whole map would come back empty and every dictionary that name's members accept would go uncounted
     with nothing said. Both call sites below establish the name is declared before asking, and this is the
     assertion that keeps them doing so. */
  if (!byName.has(iface))
    throw new Error(`[idl-audit] memberOps was asked for ${iface}, which no spec in @webref/idl declares — its ` +
                    `callers must establish that first, because an empty answer here is indistinguishable ` +
                    `from an interface with no operations`);
  const m = new Map();
  /* AN OPERATION IS INHERITED AND A CONSTRUCTOR IS NOT, so the two are read from different member lists.
     Web IDL §3.7.1 Interface object's construct steps open "If I was not declared with a constructor
     operation, then throw a TypeError" and resolve the overload "for constructors with identifier id on
     interface I" — I being the interface the object is FOR, never a base it inherits from. Reading the ctor off
     the flattened list makes every derived interface answer with its base's: BeforeUnloadEvent reports Event's
     `constructor(DOMString, optional EventInit)` and FileSystemWritableFileStream reports WritableStream's
     `constructor(optional object, optional QueuingStrategy)`, neither of which its own IDL declares and
     neither of which any browser lets a page call. It was invisible while the only thing read off a ctor was
     which dictionaries it takes AND those dictionaries were declared anyway — a wrong reachability that
     happened to reach a true row. */
  const own = new Set(ownConstructors(iface));   /* non-null: the assertion above established the name */
  for (const x of flatten(iface)) {
    if ((x.type !== "operation" && x.type !== "constructor") || (x.type === "operation" && !x.name)) continue;
    if (x.type === "constructor" && !own.has(x)) continue;        /* a base's constructor is not this one's */
    const key = x.type === "constructor" ? " ctor" : x.name;
    if (!m.has(key)) m.set(key, []);
    m.get(key).push(x);
  }
  opsOf.set(iface, m);
  return m;
};
/* A CONSTRUCTOR THE IDL DECLARES THAT THIS ENGINE REFUSES, recorded where it costs a DICTIONARY its only way
   in. The whole population of absent constructors is wider than this — every §3.7.1 [[Construct]] the engine
   answers with a TypeError — and none of it is reported anywhere, because idl_members.mjs's members() collects
   `attribute`, `operation` and `const` and no `constructor`, so the ABSENT census cannot see one. That is a
   NAMED RESIDUAL and not this diff: what belongs to the DICTIONARY audit is the constructor whose absence makes
   a dictionary unreachable, because that is the one an UNDECLARED row would otherwise mis-diagnose. The rest
   shows as `new Range()` answering "Illegal constructor" while Range's row reads COMPLETE, and it closes when
   §3.7.1's constructor becomes a row of that census. */
const ctorAbsent = new Map();
const noteCtorAbsent = (iface, ctors, r) => {
  const dicts = new Set();
  for (const op of ctors)
    for (const a of op.arguments || [])
      for (const d of dictionaryTypesIn(a.idlType)) dicts.add(d);
  if (!dicts.size) return;               /* an absent constructor taking no dictionary is the residual above */
  if (!ctorAbsent.has(iface)) ctorAbsent.set(iface, { file: r.file, line: r.line, dicts: new Set() });
  for (const d of dicts) ctorAbsent.get(iface).dicts.add(d);
};
const dictSites = new Map();
const noteSite = (d, site) => {
  if (!dictSites.has(d)) dictSites.set(d, []);
  const list = dictSites.get(d);
  if (!list.some((s) => s.what === site.what)) list.push(site);
};
for (const r of world.records) {
  if (r.nonInterface) continue;
  const asks = [];
  /* A NAMED RESIDUAL — THE OPERATION ARM, WHICH IS THE OTHER HALF OF THE SAME QUESTION. WHAT IS NOT COVERED: an
     operation's dictionary argument is counted reachable from the IDL alone, so a position this engine declares
     with a type that cannot carry a dictionary still reports that dictionary's members UNDECLARED, under the
     same "declare the member" instruction the constructor arm below no longer gives. WHAT THE NEXT DIFF BUILDS:
     this arm reads the declared IdlArgType row for the argument's POSITION through idl_argdecl.mjs's
     declarations() — the one parse of `idl_method_id*` that argaudit.mjs and argtypegate.mjs already share, so
     no second copy is created — and counts the dictionary only where the declared type can produce one. HOW ITS
     ABSENCE SHOWS: JsonWebKey's twenty members report UNDECLARED while §14.3.9's `(BufferSource or JsonWebKey)
     keyData` position is declared IDL_BUFFERSOURCE, a narrowing that file names at the declaration and whose
     jwk arm throws a TypeError by name — so the members could not be read however they were declared. */
  for (const iface of r.ifaces || []) {
    /* An interface tag naming something the corpus does not declare is ALREADY its own category above
       ("interface tags naming something the IDL corpus does not declare"), so this arm skips it rather than
       asking memberOps for a surface that does not exist — which is what makes that assertion satisfiable and
       keeps one wrong tag from being reported here a second time as a missing dictionary declaration. */
    if (!byName.has(iface)) continue;
    const ops = memberOps(iface).get(r.name);
    if (ops) asks.push([`${iface}.${r.name}`, ops]);
  }
  if (byName.has(r.name)) {
    const ctors = memberOps(r.name).get(" ctor");
    /* ONLY WHERE THIS ENGINE ACTUALLY CONSTRUCTS. An interface OBJECT is installed for every exposed
       interface, and §3.7.1 gives it [[Construct]] steps only where the interface declares a constructor —
       which this engine states with idl_step_constructor and states nowhere else (idl_installed.mjs's
       CONSTRUCTING_FORM). Reading the property's existence as the constructor's is the audit deriving a fact
       about THIS ENGINE from the IDL alone, and it does not fail quietly: it reported a dictionary as reachable
       through a constructor whose every call is `idl_illegal_ctor`'s TypeError, under an instruction to declare
       three members that no conversion could ever have read. The absent constructor is reported below instead,
       which is the true instruction and the one an engineer can act on. */
    if (ctors && world.constructs.has(r.name)) asks.push([`new ${r.name}`, ctors]);
    /* THE SAME ABSTENTION AS THE CONSTRUCTOR AXIS, and for the same reason — this arm reads the same
       `world.constructs`, so an unread mint makes its ELSE unsound in exactly the way section 1b of
       idl_installed.mjs names by example: "it would make a dictionary that IS reachable report as unreachable".
       Repairing the verdict category and leaving this reading the incomplete set would have left the defect
       alive in the axis the header used to describe it. */
    else if (ctors && !ctorUnread.length) noteCtorAbsent(r.name, ctors, r);
  }
  for (const [what, ops] of asks)
    for (const op of ops)
      for (const a of op.arguments || [])
        for (const d of dictionaryTypesIn(a.idlType))
          noteSite(d, { what, file: r.file, line: r.line });
}

const dictUndeclared = [], dictStranger = [], dictRequired = [], dictOrder = [];
for (const [d, sites] of [...dictSites].sort()) {
  const spec = dictMembers(d);
  if (!spec.length) continue;                  /* a dictionary with no members states nothing to read */
  const named = dictNamed.get(d);
  const have = named ? new Set(named.members.map((m) => m.name)) : creditSubsets(d);
  dictHave.set(d, have);
  const missing = spec.filter((m) => !have.has(m.name));
  if (missing.length) dictUndeclared.push({ d, sites, missing, spec, named: !!named });
  if (!named) continue;
  /* THE NAMED FORM IS CHECKED IN FULL, because its dictionary is stated rather than inferred. */
  const specByName = new Map(spec.map((m) => [m.name, m]));
  for (const m of named.members)
    if (!specByName.has(m.name)) dictStranger.push({ d, named, name: m.name });
  for (const m of named.members) {
    const s = specByName.get(m.name);
    if (s && m.required !== null && m.required !== s.required) dictRequired.push({ d, named, name: m.name, want: s.required });
  }
  const order = named.members.map((m) => m.name).filter((n) => specByName.has(n));
  const want = spec.map((m) => m.name).filter((n) => have.has(n));
  if (order.join(",") !== want.join(",")) dictOrder.push({ d, named, order, want });
}

/* THE COUNT IS OF MEMBERS, WHICH IS WHAT THE LABEL ALREADY SAID. It was `dictUndeclared.length` — the number of
   DICTIONARIES holding at least one undeclared member — printed under a label naming MEMBERS, so the verdict
   answered a different question from the rows above it and the two could not be reconciled by reading them. A
   coverage figure states what it is a fraction of, in the same line, or it is not a coverage figure. */
defect("dictionary members the platform declares that no IdlDictMember declaration names",
       dictUndeclared.reduce((n, u) => n + u.missing.length, 0));
defect("dictionaries a page cannot reach because the constructor that takes one is absent", ctorAbsent.size);
defect("IdlDictMember entries naming a member the dictionary the declaration names does not have", dictStranger.length);
defect("dictionary members whose declared `required` contradicts the IDL", dictRequired.length);
defect("named dictionary declarations whose member order is not §3.2.17's", dictOrder.length);
defect("IdlDictDecl identifiers naming a dictionary no spec in @webref/idl defines", dictUnknownName.length);
blind("IdlDictMember declarations this audit could not read", dictUnreadable.length);
console.log(`[idl-audit] ── Web IDL §2.7 dictionaries ── ${dictSites.size} reachable from the members this ` +
            `engine installs; ${dictArrays.length} IdlDictMember declaration(s) read, ${dictNamed.size} of ` +
            `them naming their dictionary`);
for (const u of dictUndeclared)
  console.log(`[idl-audit]   ${u.d}: ${u.missing.length} of ${u.spec.length} member(s) UNDECLARED — ` +
              `${u.missing.map((m) => m.name + (m.required ? " (required)" : "")).join(", ")}. Reached by ` +
              `${u.sites.map((s) => `${s.what} at ${s.file.replace(BROWSER + "/", "")}:${s.line}`).join(", ")}. ` +
              `No IdlDictMember declaration in this engine ${u.named ? "of this dictionary " : "that could be " +
              "this dictionary's "}names ${u.missing.length > 1 ? "them" : "it"}, so §3.2.17's conversion never ` +
              `reads ${u.missing.length > 1 ? "them" : "it"} — either the option is silently ignored, or a ` +
              `component reads it with a dictionary walk of its own, which is the second copy of §3.2.17 ` +
              `core/idl_args.h's own header forbids. Both are the same fix: declare the member.`);
for (const [iface, c] of [...ctorAbsent].sort())
  console.log(`[idl-audit]   ${iface}: its IDL declares a constructor taking ${[...c.dicts].sort().join(", ")}, ` +
              `and the interface object installed at ${c.file.replace(BROWSER + "/", "")}:${c.line} is named by ` +
              `no idl_step_constructor in this engine — so Web IDL §3.7.1 Interface object's [[Construct]] is ` +
              `the shared "Illegal constructor" TypeError, \`new ${iface}()\` throws before any argument is ` +
              `converted, and the dictionary is reachable through nothing. The fix is the CONSTRUCTOR: ` +
              `declaring them here would write rows §3.2.17 never reaches. Build the constructor's steps as a ` +
              `declared member and mint its interface object with idl_step_constructor, and the dictionary ` +
              `becomes reachable and is audited like every other`);
for (const s of dictStranger)
  console.log(`[idl-audit]   ${s.named.file.replace(BROWSER + "/", "")}:${s.named.line}  ${s.named.decl} declares ` +
              `\`${s.name}\`, which ${s.d} does not have — a member no page can pass, so the [[Get]] §3.2.17 ` +
              `step 4.1.3.1 runs for it is a property read the spec never performs`);
for (const s of dictRequired)
  console.log(`[idl-audit]   ${s.named.file.replace(BROWSER + "/", "")}:${s.named.line}  ${s.d}.${s.name} is ` +
              `${s.want ? "required" : "optional"} in the IDL and declared ${s.want ? "optional" : "required"} — ` +
              `§3.2.17 step 4.1.6 throws a TypeError for an absent required member, so this ` +
              `${s.want ? "accepts a call the spec rejects" : "rejects a call the spec accepts"}`);
for (const s of dictOrder)
  console.log(`[idl-audit]   ${s.named.file.replace(BROWSER + "/", "")}:${s.named.line}  ${s.d} is declared in the ` +
              `order ${s.order.join(", ")} and §3.2.17 steps 3 and 4.1 read it ${s.want.join(", ")} — inherited ` +
              `dictionaries least-derived first, each level lexicographic. The order is OBSERVABLE: every ` +
              `member is a [[Get]] on the page's object, so two getters see which ran first`);
for (const s of dictUnknownName)
  console.log(`[idl-audit]   ${s.file.replace(BROWSER + "/", "")}:${s.line}  IdlDictDecl names "${s.name}", which ` +
              `no spec in @webref/idl declares — either the identifier is misspelt, or the spec is one webref ` +
              `does not ship and the declaration cannot be checked against anything`);
for (const s of dictUnreadable)
  console.log(`[idl-audit]   ${s.file.replace(BROWSER + "/", "")}:${s.line}  ${s.sym}: ${s.why} — the list is ` +
              `neither credited nor counted, because a member list read in part is a list of unknown length`);

/* ---------------------------------------------------------------------------------------------------------
 * THE PLATFORM SURFACE — every global name a browser exposes on Window, straight out of the IDL.
 *
 * FOUR sources, all spec text, and this banner said TWO for as long as it had three — which is worth more
 * than the correction, because an under-claiming comment is the one direction nothing here reports. A reader
 * who believed it would have concluded that §3.7.2's `Image` was absent from the table and gone looking; the
 * ones who believed it about §3.4.11 concluded there was nothing to look for. The list is derived in
 * idl_members.mjs's `windowGlobals`, which is where the four are enumerated and where each is read through the
 * one rhs reader, so this is a pointer and not a second copy:
 *   1. Every interface / namespace / callback-interface DECLARED [Exposed=Window] (or exposed with no
 *      restriction). Its name is a global constructor property: `Node`, `Element`, `DOMException`, `SVGElement`.
 *   2. Every member of the Window interface itself, plus the mixins Window includes
 *      (WindowOrWorkerGlobalScope, GlobalEventHandlers, WindowEventHandlers, …) — `fetch`, `setTimeout`,
 *      `onload`, `location`. members() already flattens inheritance and mixins, so it answers this directly.
 *   3. §3.7.2 Legacy factory functions' names — `Image`, `Option`, `Audio`.
 *   4. §3.4.11 [LegacyWindowAlias]'s identifiers — `webkitURL`, `SVGPoint`, `SVGRect`, `SVGMatrix`,
 *      `WebKitCSSMatrix`. IDL_EXPOSURE below carries the same five, and carries a SET with each because its
 *      rows answer a per-realm question; this table answers a Window-only membership one, so the two
 *      derivations agree on the names and differ on what they say about them.
 *
 * A name here is the ENGINE's to provide. A name NOT here is the server's to have injected, and reading it
 * yields the concolic unknown whose gate forks. That is the whole distinction absent.c makes. */
const platform = windowGlobals(idl);

const names = [...platform].filter((n) => /^[A-Za-z_$][\w$]*$/.test(n)).sort();
const header =
  "/* GENERATED by engine/idlgen.mjs from @webref/idl — DO NOT EDIT.\n" +
  " * Every global name Web IDL exposes on Window, from FOUR sources: the interfaces / namespaces / callback\n" +
  " * interfaces declared [Exposed=Window], the members of the Window interface itself, Web IDL 3.7.2's legacy\n" +
  " * factory function names (Image, Option, Audio) and Web IDL 3.4.11 [LegacyWindowAlias]'s identifiers\n" +
  " * (webkitURL, SVGPoint, SVGRect, SVGMatrix, WebKitCSSMatrix). This sentence named the first two only, for\n" +
  " * as long as the derivation had three and then four.\n" +
  " * MINUS what Web IDL 3.8 Platform objects implementing interfaces' step 3.1 refuses to place: an\n" +
  " * interface declared [LegacyNoInterfaceObject] or [LegacyNamespace] gets NO property on any global, so\n" +
  " * its identifier is not a name this engine owes. The corpus has 53 — every WebGL extension, and\n" +
  " * WebAssembly's Module/Instance/Memory/Table/Global/Tag/Exception, which 3.4.4 puts on the WebAssembly\n" +
  " * namespace instead. While they were here, absent.c answered `window.Module` with a concrete undefined\n" +
  " * and `var Module = window.Module || {}` took its {} arm without forking.\n" +
  " * absent.c reads this to tell a Web API this engine OWES from server-injected app state. It does NOT throw\n" +
  " * on a name it finds here — it LEAVES THE READ ALONE, so an unguarded `new EventSource(u)` gets the\n" +
  " * ReferenceError that names the component to write and a guarded `if (window.EventSource)` gets\n" +
  " * ECMAScript 10.1.8.1 OrdinaryGet step 2.b's `undefined`. A name MISSING from this table takes the other\n" +
  " * arm: absent.c mints an example-free concolic for it and the guard FORKS, which is a world in which this\n" +
  " * engine holds an API it has not built. So a stale table is a wrong answer per name in both directions.\n" +
  " * Regenerate after `npm install @webref/idl webidl2`, and commit the result — the build has no\n" +
  " * network and this table is not optional. */\n" +
  "#ifndef APICLIENT_PLATFORM_NAMES_H\n#define APICLIENT_PLATFORM_NAMES_H\n\n" +
  "static const char *const PLATFORM_NAMES[] = {\n" +
  names.map((n) => `    "${n}",`).join("\n") + "\n};\n\n#endif\n";
/* THE AUDIT RUN DOES NOT WRITE — see the file header. A checked-in table that no longer matches the corpus is a
   FAILING category like any other gap, because what each table decides is an ANSWER and not a formatting
   detail; `--regen` is the one command that writes. One emitter for both tables: a second hand-written copy of
   this three-way is right on the day it is written and silently diverges on the day the first one learns
   something, which is the reason idl_members.mjs exists one layer up. */
const emitGenerated = (file, text, what, cost) => {
  const out = join(HERE, "host", "browser", file);
  let prev = "";
  try { prev = readFileSync(out, "utf8"); } catch { /* first run */ }
  if (prev === text) { console.log(`[idl-audit] ${file} current — ${what}`); return; }
  if (REGEN) { writeFileSync(out, text); console.log(`[idl-audit] ${file} REGENERATED — ${what}`); return; }
  defect(`stale generated table (${file})`);
  console.log(`[idl-audit] ${file} STALE — the corpus says ${what} and the checked-in table is not that. ` +
              `${cost} Regenerate it with \`node engine/idlgen.mjs --regen\` and commit.`);
};
emitGenerated("platform_names.h", header, `${names.length} global names exposed on Window`,
              "absent.c decides ReferenceError-vs-fork off it, so a stale table is a wrong answer per name.");

/* ---------------------------------------------------------------------------------------------------------
 * §3.7.3's PROTO STEP, GENERATED, BECAUSE THE AUDIT ABOVE STANDS ON IT AND CANNOT CHECK IT.
 *
 * `chainOf` credits a BASE's installed members to every interface that inherits it — `addEventListener` counts
 * for HTMLSpanElement because HTMLSpanElement inherits EventTarget — and that is right only while the ENGINE's
 * prototype chain is the IDL's. Nothing established that. The IDL side is read from the corpus and the
 * installed side is read from the C, and the LINK between them was believed: a component that built its
 * prototype over the wrong parent would still be credited with every member of the parent the IDL names, so
 * ~64 members a page cannot reach would read COMPLETE on one row and ~2900 across the HTML family. That is the
 * false COMPLETE this whole file exists to refuse, at the largest scale in it, minted by the auditor itself.
 *
 * It is not checkable HERE: which object a prototype is built over is a RUNTIME fact, per realm, and a static
 * approximation of it would be a second plausible answer beside the first. So the corpus states the expectation
 * and the ENGINE asserts it, at the one call every interface prototype object already makes
 * (core/idl_args.c's idl_interface_tag, Web IDL §3.7.3's own class string) — the two-sided shape every
 * declaration in this file already has.
 *
 * WHAT EACH ROW SAYS is the §3.7.3 class string the object at the other end of the [[Prototype]] link must
 * carry, taken straight from §3.7.3 Interface prototype object's four proto arms — the named properties object
 * for a [Global] interface that supports named properties, the inherited interface's prototype, %Error.prototype%
 * for DOMException, %Object.prototype% otherwise — plus §3.7.4 Named properties object's own two arms for the
 * object that arm names. A CALLBACK INTERFACE, a MIXIN and a NAMESPACE get no row: §3.7.1 gives a callback
 * interface no interface prototype object at all, and the other two are never objects. */
const namedPropsGlobals = new Set();
for (const n of idl.declarations) {
  if (n.type !== "interface" || !n.name) continue;
  if (!(n.extAttrs || []).some((a) => a.name === "Global")) continue;
  /* §3.7.4: "For every interface declared with the [Global] extended attribute that SUPPORTS NAMED
     PROPERTIES" — which §3.7.4's own definition makes a NAMED PROPERTY GETTER, not any getter: an indexed
     getter takes an integer type and defines §3.9's indexed properties instead. */
  if ((n.members || []).some((m) => m.type === "operation" && m.special === "getter" && m.arguments &&
                                    m.arguments.length === 1 && m.arguments[0].idlType &&
                                    m.arguments[0].idlType.idlType === "DOMString"))
    namedPropsGlobals.add(n.name);
}
const inheritRows = [];
const named = (n) => [`"${n}"`, "IDL_PROTO_INHERITS"];
const OBJ_PROTO = ["NULL", "IDL_PROTO_OBJECT"], ERR_PROTO = ["NULL", "IDL_PROTO_ERROR"];
for (const [iface, node] of byName) {
  if (node.type !== "interface") continue;
  const base = inheritanceOf.get(iface);
  inheritRows.push([iface, ...(namedPropsGlobals.has(iface) ? named(`${iface}Properties`)
                              : base ? named(base) : iface === "DOMException" ? ERR_PROTO : OBJ_PROTO)]);
  if (namedPropsGlobals.has(iface))
    inheritRows.push([`${iface}Properties`, ...(base ? named(base) : OBJ_PROTO)]);
}
/* strcmp order, because the engine reaches a row with bsearch. Web IDL identifiers are ASCII, so JS's
   code-unit comparison and strcmp's byte comparison are the same order and there is nothing to keep in step. */
inheritRows.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
const ifaceW = Math.max(...inheritRows.map((r) => r[0].length));
const inheritH =
  "/* GENERATED by engine/idlgen.mjs from @webref/idl — DO NOT EDIT.\n" +
  " * Web IDL §3.7.3 Interface prototype object's PROTO STEP and §3.7.4 Named properties object's, per\n" +
  " * interface: the §3.7.3 class string the object at the other end of this interface prototype object's\n" +
  " * [[Prototype]] link must carry, or one of the two INTRINSIC spellings §3.7.3 itself uses for an object\n" +
  " * that carries no class string at all. core/idl_args.c asserts it per realm, at the one call every\n" +
  " * interface prototype object already makes; engine/idlgen.mjs's own gap audit credits a base's installed\n" +
  " * members to everything that inherits it and can only do that soundly because this is asserted.\n" +
  " * Regenerate after `npm install @webref/idl webidl2`, and commit the result — the build has no network\n" +
  " * and this table is not optional. */\n" +
  "#ifndef APICLIENT_IDL_INHERITANCE_H\n#define APICLIENT_IDL_INHERITANCE_H\n\n" +
  "/* WHICH OF §3.7.3's PROTO ARMS DECIDED THE ROW. Two of them name an INTRINSIC rather than an interface —\n" +
  "   \"set proto to realm.[[Intrinsics]].[[%Object.prototype%]]\" and, for DOMException,\n" +
  "   \"realm.[[Intrinsics]].[[%Error.prototype%]]\" — and neither object carries a §3.7.3 class string of its\n" +
  "   own, so those two are checked by OBJECT IDENTITY against the realm's intrinsic and the rest by the tag\n" +
  "   the inherited interface's prototype object already carries. */\n" +
  "enum { IDL_PROTO_INHERITS = 0, IDL_PROTO_OBJECT = 1, IDL_PROTO_ERROR = 2 };\n\n" +
  "typedef struct IdlInherits {\n" +
  "    const char *iface;   /* the §3.7.3 class string of the interface prototype object */\n" +
  "    const char *proto;   /* the class string its [[Prototype]] carries; NULL on an intrinsic arm */\n" +
  "    int         arm;     /* which §3.7.3 (or §3.7.4) proto arm decided it */\n" +
  "} IdlInherits;\n\n" +
  "static const IdlInherits IDL_INHERITS[] = {\n" +
  inheritRows.map(([i, p, a]) =>
    `    { "${i}",${" ".repeat(ifaceW - i.length)} ${p}, ${a} },`).join("\n") +
  "\n};\n\n#endif\n";
emitGenerated("idl_inheritance.h", inheritH,
              `${inheritRows.length} interface prototype objects and the §3.7.3 [[Prototype]] each must have`,
              "idl_args.c asserts each realm's chain against it, and this file's own gap audit credits a " +
              "base's members to everything that inherits it only because that assertion holds.");

/* ---------------------------------------------------------------------------------------------------------
 * WEB IDL §3.3.7 [Exposed]'s STEP 1, GENERATED — the two sets that step intersects, and NEITHER of them is
 * this engine's to state.
 *
 * Step 1 is "If construct's exposure set is not `*`, and realm.[[GlobalObject]] does not implement an
 * interface that is in construct's exposure set, then return false". It has a CONSTRUCT side (a set of global
 * names, from the construct's own [Exposed]) and a REALM side (the global names the realm's global object's
 * interface is declared with, from that interface's §3.3.8 [Global]). Both are extended attributes in the
 * corpus, so both are read from it: a C table restating either would be the third copy of a fact whose first
 * copy is the artifact this tool exists to read, and it would go stale the way a hand-kept column always does.
 *
 * IT IS THE SAME DERIVATION THE AUDIT ABOVE ALREADY RUNS, and that is deliberate rather than convenient. The
 * NOT-EXPOSED category is computed from `ifaceExposure`/`memberExposure` and `globalNamesOf`; emitting the
 * engine's runtime answer from those same functions means the audit and the engine cannot disagree about what
 * §3.3.7 says. Two derivations of one algorithm is the shape that drifts.
 *
 * WHY THE ENGINE NEEDS IT AT ALL: until this landed, `IdlExposure` modelled step 2 ([SecureContext]) and
 * idl_args.h recorded step 1 as honestly absent, on the argument that this engine has exactly one global kind
 * — no WorkerGlobalScope — so every member's exposure set was trivially satisfied. That argument was true and
 * it was also the blocker: with no way to build a realm that gets the [Exposed=Worker] surface and not
 * Window's, a worker script could only be run in a Window realm, where `document` exists.
 *
 * A NAME WITH NO ROW IS EXPOSED, which is this file's own sound direction stated for a consumer rather than a
 * count: absence of evidence must never REMOVE something, so a name the corpus does not declare (a legacy
 * factory function of an interface webref does not ship, an engine-only name) keeps the property it has today.
 * The rows that carry information are the ones that can EXCLUDE, and every one of them is a corpus fact. */
const exposureBitNames = [...new Set([...globalNamesOf.values()].flatMap((s) => [...s]))].sort();
/* §3.3.7: "each of the identifiers mentioned must be a global name of some interface". A name in an [Exposed]
   that no [Global] declares would get no bit, and a set of no bits is how `*` is spelled below — so it would
   read as EXPOSED EVERYWHERE, which is the one direction this table must never fail in. It crashes instead. */
const exposureBit = new Map(exposureBitNames.map((n, i) => [n, 1 << i]));
if (exposureBitNames.length > 31)
  throw new Error(`[idl-audit] the corpus declares ${exposureBitNames.length} §3.3.8 [Global] names and the ` +
                  `generated set is a 32-bit mask — the mask has to grow before the table can be emitted`);
const exposureMask = (set, what) => {
  if (set === EXPOSED_STAR) return null;
  let m = 0;
  for (const n of set) {
    const b = exposureBit.get(n);
    if (b === undefined)
      throw new Error(`[idl-audit] ${what} is [Exposed=…${n}…] and no interface in the corpus is declared ` +
                      `[Global=${n}] — Web IDL §3.3.7 requires every identifier in an exposure set to be a ` +
                      `global name of some interface, and a name with no bit would silently read as \`*\``);
    m |= b;
  }
  return m;
};
const maskSpelling = (m) => (m === null ? "IDL_EXPOSED_STAR"
  : exposureBitNames.filter((n) => m & exposureBit.get(n)).map((n) => `IDL_GLOBAL_${n.toUpperCase()}`)
      .join(" | "));
/* WEB IDL §3.4.11 [LegacyWindowAlias]'s IDENTIFIERS, per interface — the FOURTH kind of name §3.8 `define the
   global property references` puts on a global, and the one this table was keyed without.
   Web IDL §3.4.11 [LegacyWindowAlias]: "If the [LegacyWindowAlias] extended attribute appears on an interface,
   it indicates that the Window interface will have a property for each identifier mentioned in the extended
   attribute, whose value is the interface object for the interface." Web IDL §3.7 Interfaces says the same one
   step down — "for each identifier in [LegacyWindowAlias]'s identifiers there exists a corresponding property
   on the Window global object" — and Web IDL §3.8 Platform objects implementing interfaces' step 3.1.4 is
   where the condition is written as an algorithm: "If the interface is declared with a [LegacyWindowAlias]
   extended attribute, and target implements the Window interface".
   THE SET IS `Window` AND IT IS NOT THE INTERFACE'S OWN, which is the whole reason this cannot be spelled like
   the [LegacyFactoryFunction] line below it. §3.8 step 3.2 has NO realm condition, so a factory function goes
   wherever its interface goes and `exposureMask(ifaceExposure(iface))` is exactly right for it; step 3.1.4 adds
   "and target implements the Window interface", so an alias's set is Window INTERSECT the interface's — and
   §3.4.11 requires the interface to include Window ("The [LegacyWindowAlias] extended attribute must not be
   specified on an interface that does not include the Window interface in its exposure set"), which makes that
   intersection Window. Copying the factory line's shape would have been measurably wrong rather than pedantic:
   three of the corpus's four aliased interfaces are [Exposed=(Window,Worker)], so `SVGPoint`, `SVGRect`,
   `SVGMatrix` and `WebKitCSSMatrix` would each have got a WORKER bit and appeared in every worker realm — the
   exact defect a Window-only alias exists to not have.
   THE CONFORMANCE REQUIREMENTS ARE CHECKED AND NOT ASSUMED, because each of them is what makes the emitted row
   mean what it says, and a corpus that breaks one would emit a row that is silently a different claim. */
const legacyWindowAliasOf = new Map();   /* alias identifier -> the interface whose interface object it names */
for (const n of idl.declarations) {
  if (!n.name || n.partial) continue;    /* read off the ORIGINAL definition, for ifaceExposure's own reason */
  const ext = extOf(n, "LegacyWindowAlias");
  if (!ext) continue;
  const v = rhsNames(ext);
  /* §3.4.11: "The [LegacyWindowAlias] extended attribute must either take an identifier or take an identifier
     list." A bare form gives [] and a `*` gives EXPOSED_STAR; neither is an identifier, so neither can name a
     property, and a row emitted from one would carry an empty name. */
  if (v === EXPOSED_STAR || !Array.isArray(v) || !v.length)
    throw new Error(`[idl-audit] ${n.name} carries a [LegacyWindowAlias] that is neither an identifier nor an ` +
                    `identifier list — Web IDL §3.4.11 [LegacyWindowAlias] requires one of the two, and there ` +
                    `is no property name to define without it`);
  /* §3.4.11: "An interface must not have more than one [LegacyWindowAlias] extended attributes specified." */
  if ((n.extAttrs || []).filter((a) => a.name === "LegacyWindowAlias").length > 1)
    throw new Error(`[idl-audit] ${n.name} carries more than one [LegacyWindowAlias] extended attribute, which ` +
                    `Web IDL §3.4.11 forbids — which one names the aliases is then this generator's guess`);
  /* §3.4.11: "The [LegacyWindowAlias] extended attribute must not be specified on an interface that does not
     include the Window interface in its exposure set." Emitting IDL_GLOBAL_WINDOW for an interface that is NOT
     on Window would put a name on the Window global pointing at an interface object that realm never built. */
  const e = ifaceExposure(n.name);
  if (e !== EXPOSED_STAR && !e.has("Window"))
    throw new Error(`[idl-audit] ${n.name} is [LegacyWindowAlias=…] and its exposure set is ` +
                    `${showExposure(e)}, which does not include Window — Web IDL §3.4.11 forbids that, and ` +
                    `§3.8 step 3.1.4's alias property would name an interface object no Window realm has`);
  for (const id of v) legacyWindowAliasOf.set(id, n.name);
}
/* EVERY IDENTIFIER §3.8 CAN DEFINE ON A GLOBAL: an interface, a namespace, a callback interface — §3.7.1 and
   §3.13.1 each put one property on the global under the construct's own identifier — every §3.7.2 LEGACY
   FACTORY FUNCTION, whose name is on the global too and is no interface's, so a table keyed by interface name
   alone would leave `Image`, `Audio` and `Option` in a worker — and every §3.4.11 [LegacyWindowAlias]
   IDENTIFIER, which is the one kind whose exposure set is NOT its interface's (see the derivation above). */
const exposureRows = [];
/* §3.8 STEP 3.1's CONDITION, WHICH THIS TABLE IS KEYED BY AND WAS NOT ASKING. Every row here claims to be
   "the identifier §3.8 defines on the global" — that is this header's own words and core/realm.c's walk
   rests its whole classification on it — and step 3.1 defines NO identifier for an interface declared
   [LegacyNoInterfaceObject] or [LegacyNamespace]. The corpus has 53, so 53 rows asserted a placement the
   algorithm they cite refuses. It is the same derivation browser/platform_names.h needs, so it is the same
   reader: idl_members.mjs's `unplacedInterfaces`, never a second spelling of the two attribute names. */
const unplaced = unplacedInterfaces(idl);
for (const [name, node] of byName) {
  const kind = (originalOf.get(name) || node).type;
  if (!["interface", "namespace", "callback interface"].includes(kind)) continue;
  if (unplaced.has(name)) continue;
  exposureRows.push([name, exposureMask(ifaceExposure(name), name)]);
}
for (const [factory, iface] of legacyFactoryOf)
  if (byName.has(iface))
    exposureRows.push([factory, exposureMask(ifaceExposure(iface), `${iface}'s [LegacyFactoryFunction=${factory}]`)]);
/* §3.8 step 3.1.4's OWN CONDITION, as the row's set — `Window`, from the algorithm and not from the interface.
   `exposureMask` is still what spells it, so a corpus in which no interface is [Global=Window] crashes here
   with the message that entry already carries rather than emitting a set of no bits, which is how `*` is
   spelled and would read as EXPOSED EVERYWHERE. */
for (const [alias, iface] of legacyWindowAliasOf)
  /* AND STEP 3.1.4 IS NESTED INSIDE STEP 3.1, so an alias of an unplaced interface is not defined either —
     its define sits at step 3.1.4.1.1, under the guard. The factory-function loop above is NOT gated,
     because step 3.2 is a SIBLING of step 3.1 and states no such condition. Neither arm is separable by
     measurement today (no corpus interface is both unplaced and aliased, or both unplaced and a factory);
     they are told apart by the algorithm's list nesting, and writing them the same way would make one of
     them silently wrong on the day webref ships the pairing. */
  if (byName.has(iface) && !unplaced.has(iface))
    exposureRows.push([alias, exposureMask(new Set(["Window"]),
                                           `${iface}'s [LegacyWindowAlias=${alias}]`)]);
/* THE NAMES MUST BE UNIQUE, AND THAT IS THE CONSUMER'S REQUIREMENT RATHER THAN A TIDINESS ONE: core/idl_args.c
   and core/realm.c both reach this table with `bsearch`, which over duplicate keys returns AN element and not a
   determinate one — so two rows under one name is a construct whose exposure answer depends on the array's
   length. Web IDL §3.4.11 already forbids the three collisions that could produce one (an alias identifier
   "must not be the same as one used by a [LegacyWindowAlias] extended attribute on this interface or another
   interface", "must not be the same as the identifier used by a [LegacyFactoryFunction] extended attribute on
   this interface or another interface", and "must not be the same as an identifier of an interface that has an
   interface object"), so this asserts the corpus obeys its own rule at the one place the answer would go
   quietly wrong. It is checked over the WHOLE array rather than per source, because the collisions §3.4.11
   names are BETWEEN the three kinds and a per-kind check cannot see any of them. */
{
  const seen = new Map();
  for (const [n] of exposureRows) {
    if (seen.has(n))
      throw new Error(`[idl-audit] two §3.8 global property references are both named \`${n}\` — Web IDL ` +
                      `§3.4.11 [LegacyWindowAlias] and §3.7.2 forbid an identifier collision between an ` +
                      `interface, a legacy factory function and a legacy window alias, and IDL_EXPOSURE is ` +
                      `read with bsearch, which answers with an unspecified one of two rows sharing a key`);
    seen.set(n, true);
  }
}
exposureRows.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
const globalRows = [...globalNamesOf].map(([iface, names]) =>
  [iface, [...names].reduce((m, n) => m | exposureBit.get(n), 0)]);
globalRows.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));

/* WEB IDL §3.3.7 [Exposed] STEP 1's OTHER VOCABULARY — THE MEMBER, WHICH THE TABLE ABOVE CANNOT KEY AND
 * WHICH §3.7.6 AND §3.7.7 EACH ASK ONCE PER MEMBER.
 *
 * §3.3.7 declares [Exposed] on "an individual interface member, interface mixin member, or namespace member",
 * and the two sections that place a member ask step 1 of each: §3.7.6 Attributes' define the attributes reads
 * "If attr is not exposed in realm, then continue." and §3.7.7 Operations' define the operations reads "If op
 * is not exposed in realm, then continue." IDL_EXPOSURE cannot answer either — it is keyed by the identifier
 * §3.8 puts on a global, a member has no row there, and a name with no row is exposed — so this is the second
 * table rather than a second lookup into the first.
 *
 * THE UNION IS OVER THE INHERITANCE CHAIN AND NOT OVER THE [Global] INTERFACE ALONE, AND THAT IS THE ONE
 * DESIGN DECISION IN THIS TABLE. §3.7.6's own opening says a regular attribute is "exposed on the interface
 * prototype object, unless the attribute is unforgeable or if the interface was declared with the [Global]
 * extended attribute, in which case they are exposed on every object that implements the interface" — so
 * `WorkerGlobalScope`, which is NOT [Global], puts `self` and `fetch` on ITS PROTOTYPE while
 * `DedicatedWorkerGlobalScope`, which is, puts `postMessage` on the global. A table keyed by the [Global]
 * interfaces alone would therefore give `fetch` the set (Window) — Window is [Global] and WorkerGlobalScope is
 * not — and a consumer would REMOVE `fetch` from a worker realm, where a browser has it one link up the
 * prototype chain. Removing a name that is reachable is the one direction this table must never fail in, so
 * the row states where the name is reachable FROM A GLOBAL: every interface a [Global] one inherits, which is
 * exactly `chainOf`. It is a SOUND OVER-APPROXIMATION of §3.7.6's per-attribute ask, and the consumer says so.
 *
 * A NAME WITH NO ROW IS EXPOSED, for IDL_EXPOSURE's reason and with one more source of no-row here: a member
 * whose union is `*` carries no exclusion and is omitted, and so is one whose union is EMPTY. Empty is not
 * hypothetical — §3.3.7's note says "the exposure set of its members is a function of the interface that
 * includes them", so a mixin member [Exposed=Window] included into a Worker-only host intersects to nothing —
 * and it cannot be SPELLED either, because a set of no bits is how `*` is written. Omitting it is the sound
 * reading of both facts at once. */
const memberExposureUnion = new Map();   /* member identifier -> mask, or null for `*` */
for (const g of globalNamesOf.keys())
  for (const iface of chainOf(g)) {
    const per = memberExposures.get(iface);
    if (!per) continue;
    for (const [name, sets] of per)
      for (const s of sets) {
        const m = exposureMask(s, `\`${iface}\`'s member \`${name}\``);
        const prev = memberExposureUnion.get(name);
        memberExposureUnion.set(name, prev === undefined ? m
                                    : prev === null || m === null ? null : prev | m);
      }
  }
const memberRows = [...memberExposureUnion].filter(([, m]) => m !== null && m !== 0);
memberRows.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
/* THE SAME UNIQUENESS THE TABLE ABOVE ASSERTS, AND HERE IT IS THE MAP THAT ENFORCES IT rather than a check:
   the union is BUILT keyed by name, so two declarations of one identifier are one row by construction and
   there is no bsearch-over-duplicates hazard to guard. What the map cannot make true is that the emitted
   array is SORTED, which bsearch needs and which the sort above is; a corpus identifier with a non-ASCII code
   unit would order differently under strcmp than under this comparison, so it is refused rather than emitted
   into an array the consumer would search wrongly. */
for (const [n] of memberRows)
  if (!/^[\x20-\x7e]+$/.test(n))
    throw new Error(`[idl-audit] the member identifier \`${n}\` is not ASCII, and IDL_MEMBER_EXPOSURE is ` +
                    `sorted here with a JavaScript code-unit comparison and read with strcmp — the two orders ` +
                    `agree only over ASCII, and a bsearch over an array sorted the other way answers wrongly`);
const expW = Math.max(...exposureRows.map((r) => r[0].length + 2));
const memW = memberRows.length ? Math.max(...memberRows.map((r) => r[0].length + 2)) : 0;
const globW = Math.max(...globalRows.map((r) => r[0].length + 2));
const exposureH =
  "/* GENERATED by engine/idlgen.mjs from @webref/idl — DO NOT EDIT.\n" +
  " * Web IDL §3.3.7 [Exposed]'s STEP 1 — \"If construct's exposure set is not `*`, and realm.[[GlobalObject]]\n" +
  " * does not implement an interface that is in construct's exposure set, then return false\" — as the two\n" +
  " * sets that step intersects, both straight out of the corpus's own extended attributes.\n" +
  " *\n" +
  " * IDL_EXPOSURE is the CONSTRUCT side, keyed by the identifier Web IDL §3.8 `define the global property\n" +
  " * references` puts on a global: every interface, namespace and callback interface, every §3.7.2 legacy\n" +
  " * factory function name, and every §3.4.11 [LegacyWindowAlias] identifier. An interface §3.8 step 3.1\n" +
  " * REFUSES has no row, because step 3.1 defines no identifier for one declared [LegacyNoInterfaceObject]\n" +
  " * or [LegacyNamespace] — 53 in this corpus — and a row for it would claim a placement the algorithm this\n" +
  " * table is keyed by does not make. Their §3.4.11 aliases go with them (step 3.1.4 is NESTED in 3.1);\n" +
  " * their §3.7.2 factory functions would NOT (step 3.2 is a sibling), and no corpus interface is both.\n" +
  " * IDL_GLOBALS is the REALM side:\n" +
  " * each §3.3.8 [Global] interface and the global names its global object implements. core/idl_args.c\n" +
  " * intersects them; core/realm.c is where a realm states which of the IDL_GLOBALS rows its global object\n" +
  " * is.\n" +
  " *\n" +
  " * IDL_MEMBER_EXPOSURE IS THE CONSTRUCT SIDE AGAIN OVER §3.3.7's OTHER VOCABULARY — a MEMBER of a [Global]\n" +
  " * interface rather than an identifier §3.8 defines. §3.7.6 Attributes and §3.7.7 Operations each ask step 1\n" +
  " * per member, and no key of IDL_EXPOSURE answers for one; see idlgen.mjs for why a row is a union over the\n" +
  " * [Global] interface's whole INHERITANCE CHAIN and why that makes it a sound over-approximation.\n" +
  " *\n" +
  " * A NAME WITH NO ROW IS EXPOSED. Absence of evidence must not remove a property, so an identifier the\n" +
  " * corpus does not declare keeps the global property it would have had — the rows that carry information\n" +
  " * are the ones that can EXCLUDE. Regenerate after `npm install @webref/idl webidl2`, and commit the\n" +
  " * result — the build has no network and this table is not optional. */\n" +
  "#ifndef APICLIENT_IDL_EXPOSURE_H\n#define APICLIENT_IDL_EXPOSURE_H\n\n" +
  "/* WEB IDL §3.3.8 [Global]'s GLOBAL NAMES, one bit each — \"The [Global] extended attribute also defines\n" +
  "   the global names for the interface\", so this vocabulary is exactly the corpus's [Global] annotations.\n" +
  "   §3.3.7 [Exposed] requires every identifier in an exposure set to be one of them (\"Each of the\n" +
  "   identifiers mentioned must be a global name of some interface and be unique\"), so a name outside this\n" +
  "   enum is a corpus error the generator refuses rather than a bit nobody set. */\n" +
  "enum {\n" +
  exposureBitNames.map((n, i) =>
    `    IDL_GLOBAL_${n.toUpperCase()}${" ".repeat(Math.max(...exposureBitNames.map((x) => x.length)) - n.length)}` +
    ` = 1u << ${i},`).join("\n") + "\n};\n\n" +
  "/* `*` — §3.3.7's wildcard own exposure set, which step 1 tests for BEFORE it looks at the realm. Zero is\n" +
  "   the right spelling of it and not a hole: an exposure set of no global names is a construct exposed\n" +
  "   nowhere, which §3.3.7 forbids (the generator crashes on one), so the value cannot mean anything else. */\n" +
  "#define IDL_EXPOSED_STAR 0u\n\n" +
  "/* AN ALIAS ROW\'S SET IS §3.8 STEP 3.1.4\'S OWN CONDITION AND NOT ITS INTERFACE\'S EXPOSURE SET. Step 3.2\n" +
  "   defines a §3.7.2 legacy factory function with no realm condition, so that row carries the interface\'s\n" +
  "   set; step 3.1.4 reads \"If the interface is declared with a [LegacyWindowAlias] extended attribute, and\n" +
  "   target implements the Window interface\", so an alias row carries IDL_GLOBAL_WINDOW however widely the\n" +
  "   interface itself is exposed — DOMPoint is [Exposed=(Window,Worker)] and `SVGPoint` is Window-only. */\n" +
  "typedef struct IdlExposureRow {\n" +
  "    const char *name;   /* the identifier §3.8 defines on the global */\n" +
  "    unsigned    set;    /* §3.3.7's exposure set of the construct that identifier names */\n" +
  "} IdlExposureRow;\n\n" +
  "typedef struct IdlGlobalRow {\n" +
  "    const char *iface;  /* a §3.3.8 [Global] interface */\n" +
  "    unsigned    names;  /* the global names its global object implements */\n" +
  "} IdlGlobalRow;\n\n" +
  "static const IdlExposureRow IDL_EXPOSURE[] = {\n" +
  exposureRows.map(([n, m]) =>
    `    { ${`"${n}",`.padEnd(expW + 1)} ${maskSpelling(m)} },`).join("\n") + "\n};\n\n" +
  "static const IdlGlobalRow IDL_GLOBALS[] = {\n" +
  globalRows.map(([n, m]) =>
    `    { ${`"${n}",`.padEnd(globW + 1)} ${maskSpelling(m)} },`).join("\n") + "\n};\n\n" +
  "/* WEB IDL §3.7.6 Attributes' \"If attr is not exposed in realm, then continue.\" and §3.7.7 Operations'\n" +
  "   \"If op is not exposed in realm, then continue.\", as the one fact those two steps need that no\n" +
  "   identifier states: the global names on which a MEMBER of a [Global] interface may stand.\n" +
  "   A ROW SAYS WHERE THE NAME IS REACHABLE FROM A GLOBAL, not which interface declares it. §3.7.6 puts a\n" +
  "   member of a non-[Global] interface on that interface's PROTOTYPE, so `fetch` is a WorkerGlobalScope\n" +
  "   prototype property in a worker and a Window own property on a page; a row keyed to the [Global]\n" +
  "   interfaces alone would carry Window only and remove it from a worker realm. The set is therefore the\n" +
  "   union over each [Global] interface and every interface it inherits — a SOUND OVER-APPROXIMATION of the\n" +
  "   per-attribute ask, which refuses only a name no interface the realm's global implements declares here.\n" +
  "   A NAME WITH NO ROW IS EXPOSED, as above; a member whose union is `*` or empty is omitted rather than\n" +
  "   emitted, because neither can EXCLUDE and a set of no bits is how `*` is spelled. */\n" +
  "typedef struct IdlMemberExposureRow {\n" +
  "    const char *name;   /* a §3.7.6 attribute or §3.7.7 operation of some §3.3.8 [Global] interface */\n" +
  "    unsigned    set;    /* the global names on which some declaration of that member is exposed */\n" +
  "} IdlMemberExposureRow;\n\n" +
  "static const IdlMemberExposureRow IDL_MEMBER_EXPOSURE[] = {\n" +
  memberRows.map(([n, m]) =>
    `    { ${`"${n}",`.padEnd(memW + 1)} ${maskSpelling(m)} },`).join("\n") + "\n};\n\n#endif\n";
emitGenerated("idl_exposure.h", exposureH,
              `${exposureRows.length} identifiers §3.8 can define on a global, the ${globalRows.length} ` +
              `[Global] interfaces §3.3.7 step 1 measures them against, and the ${memberRows.length} of ` +
              `their members whose exposure set can EXCLUDE a realm`,
              "idl_args.c answers §3.3.7 step 1 off it at every global property reference and at every member " +
              "installed on a global, so a stale table is a name present in a realm the standard says it is " +
              "absent from, or absent from one it is in.");

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
/* A COLUMN LABELLED BY THE SET IT IS DRAWN FROM READS AS THAT SET'S SIZE. This line named one column "the
   members this interface DECLARES" and the other "those plus every inherited gap": both sentences describe the
   SETS the columns are filtered out of, and neither describes the NUMBERS printed, which are what is MISSING
   from each set. §Testing's rule that a coverage figure states what it is a fraction OF is the same rule a
   bare gap count needs, and for the same reason — with no denominator beside it a reader supplies whichever
   reading makes the label true, and the label said SURFACE.
   Measured, on a NAMESPACE row, which is the shape that offers no other clue: a namespace inherits nothing, so
   its two columns are equal by construction, and `OWN n ABSENT n` was read as its whole member surface charged
   as absent. That sent a lane hunting an over-charge in the resolver — the reading where the audit cannot see
   the component's install construct and credits it nothing — when the construct resolves, both its operations
   are attributed to the namespace object the component declares per Web IDL §3.13.1 Namespace object, and the
   count was the honest gap all along. The denominator is what removes that reading; a sentence arguing
   against it would not, because the sentence is what was misread. Print both, and the row answers the
   question a reader actually has. */
console.log(`[idl-audit] ── per interface, ranked by OWN work ── ${gapRows.length - withGaps.length} of ` +
            `${gapRows.length} audited interfaces install every member their IDL declares. BOTH COLUMNS COUNT ` +
            `MEMBERS THAT ARE MISSING, printed as missing/surface: OWN is the missing members this interface ` +
            `DECLARES — its own IDL plus the partials and mixins naming IT, minus everything its base declares ` +
            `— out of every member it declares; ABSENT is everything a page cannot reach on it at all, out of ` +
            `its whole flattened surface, own plus inherited, so one member built on a base clears an ` +
            `inherited gap on every row below it. Neither numerator counts a member this run reported ` +
            `CONDITIONAL, NOT-EXPOSED, UNPROVEN or js_noop-STUB above: those are not installed and are not ` +
            `gaps either.`);
if (withGaps.length) {
  /* A DERIVED ROW IS MARKED, because a table that does not say so reads as if every row had been audited all
     along. These are the interfaces no row named — they were in no total until the set became derived, so
     their gaps are not new work appearing, they are existing work becoming visible, and the file that DECLARES
     each is printed because that is where the work goes.
     THE LABEL SAYS WHAT IT MEANS RATHER THAN WHEN IT HAPPENED. It read NEWLY-AUDITED, which is temporal, and
     the fact is not: nothing about it changes from run to run, and a reader who takes it at its word looks for
     what this run added and finds the same rows the last one printed. What it states is that the interface has
     no hand-written row in INTERFACES above — the audit found it in the corpus, from the §3.7.3 class string
     the named file tags its prototype with, and audited it on that evidence alone. That is a claim about WHERE
     THE ROW CAME FROM, and it is worth printing for exactly one reason: a derived row's file list is the set
     of files that DECLARE the interface, which is narrower than a hand-written row's list of files that build
     it, so a gap here may belong in a component the annotation does not name. */
  const derivedSet = new Set(derivedIfaces);
  const w = Math.max(...withGaps.map((r) => r.iface.length));
  for (const r of withGaps)
    console.log(`[idl-audit]   ${r.iface.padEnd(w)}  OWN ${String(r.own).padStart(3)}/${String(r.ownSpec).padEnd(4)}` +
                ` ABSENT ${String(r.absent).padStart(3)}/${String(r.spec).padEnd(4)}` +
                (r.noop ? `  js_noop-STUB ${r.noop}` : "") +
                (r.unproven ? `  UNPROVEN ${r.unproven}` : "") +
                (derivedSet.has(r.iface)
                   ? `  NO-ROW — found from its §3.7.3 tag in ${(AUDITED.get(r.iface) || []).join(" + ")}`
                   : ""));
}
console.log("[idl-audit] ── verdict ──");
if (!defects.size && !blinds.size) {
  console.log("[idl-audit]   PASS — every audited interface installs its whole IDL surface, every declaration " +
              "is current, and every install construct resolved.");
  process.exit(0);
}
const ledgerItems = (m) => [...m.values()].reduce((a, b) => a + b, 0);
const ledgerRows = (m) => [...m].sort((a, b) => b[1] - a[1]);
/* BOTH BLOCKS PRINT, INCLUDING THE EMPTY ONE. A block that appears only when it is non-empty is one nobody
   learns to look for, so its absence reads as the absence of the QUESTION rather than as an answer to it — and
   the direction that costs something is the blind block going quiet, which is the only evidence that the
   findings above it are a total rather than a floor. */
console.log(`[idl-audit] ── FINDINGS ── ${defects.size} category(ies), ${ledgerItems(defects)} item(s). Each ` +
            `names ONE disagreement between the platform's IDL and this engine, and each states its own ` +
            `outcome on its own line.`);
for (const [kind, n] of ledgerRows(defects))
  console.log(`[idl-audit]   ${String(n).padStart(5)}  ${kind}`);
console.log(`[idl-audit] ── BLIND SPOTS ── ${blinds.size} category(ies), ${ledgerItems(blinds)} item(s). Each ` +
            `names something THIS RUN COULD NOT READ, so it found nothing about the engine there and the ` +
            `FINDINGS above are a FLOOR by at most this much: an unresolved construct is neither charged as a ` +
            `gap (which reports a built member unbuilt) nor dropped (which reports it complete). Each is ` +
            `closed HERE — by making the construct readable at its site, or by declaring to this audit what ` +
            `it cannot see — never in the engine.`);
for (const [kind, n] of ledgerRows(blinds))
  console.log(`[idl-audit]   ${String(n).padStart(5)}  ${kind}`);
/* THE ACTION IS ON THE CATEGORY'S OWN LINE, AND THIS SENTENCE NO LONGER GUESSES ONE FOR ALL OF THEM. It used
   to offer three actions for however many categories printed above it, with nothing saying which belonged to
   which — and its first action, "implement the member in its real component", is affirmatively WRONG for a
   member whose spec states no steps and for one §3.3.7 exposes in no global this engine builds. A remedy that
   names an action with no object is a crash nobody can act on; one that names the wrong object is worse,
   because a reader who obeys it lands a conformance violation and the instrument that told them to goes on
   printing. Each category above states its own outcome; what is common to all of them is only that a category
   is non-empty, and that is all this line is entitled to say. */
console.error(`[idl-audit] FAILED — ${defects.size} FINDING category(ies) and ${blinds.size} BLIND-SPOT ` +
              `category(ies) above, never summed. A finding is closed at the ROOT IN THE ENGINE by the action ` +
              `ITS line names — they are not all members to write, and never a js_noop stub and never a ` +
              `g_opaque prototype, whichever line sent you. A blind spot is closed IN THIS AUDIT, and until ` +
              `it is, the finding count above it is a floor.`);
process.exit(1);
