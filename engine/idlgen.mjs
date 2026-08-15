/* Web IDL gap AUDITOR — the IDL's real job: tell us what browser logic is MISSING, not generate stub bindings.
 * For each interface we implement, it reads the canonical .idl (@webref/idl, the W3C-curated corpus browsers
 * use), parses it (webidl2), flattens inherited + mixin members, and DIFFS the spec member list against the
 * members the component actually installs (a scan of the component .c for the property names it wires). It
 * prints the missing members so we implement them at the root — never a generated noop/DCHECK stub. Runs at
 * build time (best-effort: skipped if the idl toolchain isn't installed).
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
 * optional the way the audit is. Regenerate it with `node engine/idlgen.mjs` after `npm install @webref/idl
 * webidl2`; the audit run prints a warning when the checked-in file no longer matches the corpus. */
import { listAll } from "@webref/idl";
import { parse } from "webidl2";
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { loadEnvironment, installedMembers } from "./idl_installed.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const HOST = join(HERE, "host");
const BROWSER = join(HOST, "browser");

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
  MutationObserver:     "core/dom/mutation_observer.c",
  ResizeObserver:       "core/resize_observer/resize_observer.c",
  PerformanceObserver:  "core/timing/performance_observer.c",
  /* An interface that includes the BODY mixin has its readers and `bodyUsed` installed by the shared
     component, so body.c is where the audit finds them — naming only the interface's own file reported six
     members absent that both including interfaces have. */
  Response:            ["core/fetch/response.c", "core/fetch/body.c", "core/byte_reader.c"],
  Request:             ["core/fetch/request.c", "core/fetch/body.c", "core/byte_reader.c"],
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
  UIEvent:             ["core/events/ui_event.c", "core/events/event.c"],
  FocusEvent:          ["core/events/focus_event.c", "core/events/ui_event.c", "core/events/event.c"],
  MouseEvent:          ["core/events/mouse_event.c", "core/events/ui_event.c", "core/events/event.c"],
  KeyboardEvent:       ["core/events/keyboard_event.c", "core/events/ui_event.c", "core/events/event.c"],
  /* HTML §7.2.7.2 and §7.2.7.3 — the two events a SESSION HISTORY TRAVERSAL fires. Each names its own file plus
     event.c, whose Event.prototype members really are reachable on one. */
  PopStateEvent:       ["core/events/pop_state_event.c", "core/events/event.c"],
  HashChangeEvent:     ["core/events/hash_change_event.c", "core/events/event.c"],
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
  Blob:                ["core/file/blob.c", "core/byte_reader.c"],
  File:                ["core/file/blob.c", "core/byte_reader.c"],
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
  Window:              ["core/frame/window.c", "core/dom/document.c", "core/frame/location.c",
                        "core/fetch/fetch.c", "core/events/event_target.c", "core/loader/module_loader.c",
                        "core/timing/timer.c", "core/frame/navigator.c", "core/frame/screen.c",
                        "core/dom/abort.c", "core/css/css_style_declaration.c",
                        "core/frame/navigable.c", "core/frame/bar_prop.c",
                        "core/frame/window_message.c", "core/structured_clone.c",
                        /* CSSOM VIEW §4's Window extensions — innerWidth/innerHeight, outerWidth/outerHeight,
                           scrollX/pageXOffset, scrollY/pageYOffset, screenX/screenLeft, screenY/screenTop and
                           devicePixelRatio — live with the viewport they read, not with the Window they hang
                           off, for the per-realm reason viewport.h gives. */
                        "core/frame/viewport.c", "core/frame/visual_viewport.c"],
  Navigator:            "core/frame/navigator.c",
  /* CSSOM VIEW §12. Its own seven attributes are its file's; the three event handler IDL attributes it
     declares (`onresize`, `onscroll`, `onscrollend`) and the three members it INHERITS from EventTarget come
     off the shared component, which is the same rule Node's row states. */
  VisualViewport:      ["core/frame/visual_viewport.c", "core/events/event_target.c"],
  /* HTML §6.4.4. The interface had no row at all, so the audit said nothing about it — and its two getters and
     the object Navigator's `userActivation` answers with live with §6.4's state rather than with the Navigator
     that exposes them, which is the file named here. */
  UserActivation:       "core/html/user_activation.c",
  History:              "core/frame/history.c",
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
  CSSStyleDeclaration:  "core/css/css_style_declaration.c",
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
  /* DOM §6. The three interfaces share §6.4's filter, which lives in node_filter.c, so each names its own file
     plus that one — the same rule the BODY mixin's row states: a member installed by a shared component is
     found where that component is. */
  NodeIterator:        ["core/dom/node_iterator.c", "core/dom/node_filter.c"],
  TreeWalker:          ["core/dom/tree_walker.c", "core/dom/node_filter.c"],
  NodeFilter:           "core/dom/node_filter.c",
  /* DOM §5. AbstractRange's five getters are installed by the shared component and INHERITED by both derived
     interfaces, so each names its own file plus that one — the same rule the BODY mixin's row states. */
  AbstractRange:        "core/dom/abstract_range.c",
  StaticRange:          "core/dom/abstract_range.c",
  Range:               ["core/dom/range.c", "core/dom/abstract_range.c"],
  NodeList:            "core/dom/collections.c",
  HTMLCollection:      "core/dom/collections.c",
  Element:             ["core/dom/element.c", "core/dom/node.c", "core/events/event_target.c",
                        "core/dom/dom_token_list.c"],
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
  HTMLScriptElement:   [...HTML_BASE],
  HTMLImageElement:    [...HTML_BASE],
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
  Document:            ["core/dom/document.c", "core/dom/node.c", "core/events/event_target.c"],
  /* DOM §4.5.1 and §4.6 — the interface that BUILDS a document and the interface a doctype IS. DocumentType's
     file list carries node.c for the reason Element's does: it inherits Node, and node.c is also where the
     ChildNode mixin it INCLUDES is installed. DOMImplementation inherits nothing, so it names only its own. */
  DOMImplementation:    "core/dom/dom_implementation.c",
  DocumentType:        ["core/dom/document_type.c", "core/dom/node.c", "core/events/event_target.c"],
  HTMLTemplateElement: [...HTML_BASE],
  /* HTML §4.13.7. ElementInternals INCLUDES ARIAMixin, whose 54 members are therefore members the audit
     expects on it — which is what makes the eight element-reflecting ones show up as the real gap they are
     rather than as nothing at all. CustomStateSet's setlike members and both iterator surfaces come from the
     shared default iterator object, so idl_iter.c is named beside the component for the reason every other
     `iterable<>` interface names it. */
  ElementInternals:    ["core/html/element_internals.c", "core/idl_iter.c"],
  CustomStateSet:      ["core/html/element_internals.c", "core/idl_iter.c"],
  ValidityState:        "core/html/element_internals.c",
};

const all = await listAll();
const byName = new Map();
const inheritanceOf = new Map();
const includes = [];
for (const spec of Object.values(all)) {
  let ast;
  try { ast = parse(await spec.text()); } catch { continue; }
  for (const n of ast) {
    if ((n.type === "interface" || n.type === "interface mixin") && n.name) {
      if (n.inheritance) inheritanceOf.set(n.name, n.inheritance);
      const prev = byName.get(n.name);
      if (prev) prev.members.push(...n.members);
      else byName.set(n.name, n);
    } else if (n.type === "includes") includes.push(n);
  }
}
for (const inc of includes) {
  const host = byName.get(inc.target), mixin = byName.get(inc.includes);
  if (host && mixin) host.members.push(...mixin.members);
}
function flatten(name, seen = new Set()) {
  const node = byName.get(name);
  if (!node || seen.has(name)) return [];
  seen.add(name);
  const base = inheritanceOf.get(name);
  return [...(base ? flatten(base, seen) : []), ...node.members];
}
/* AN `iterable<>` DECLARATION IS MEMBERS. Web IDL §3.7.10 says a pair-iterable interface gets `entries`,
   `keys`, `values`, `forEach` and @@iterator, and a value-iterable one gets the same minus `entries`/`keys` —
   they are as real as anything spelled out in the members list, and `for (const [k, v] of headers)` is how
   half the code that touches one is written. The audit read only the spelled-out members, so it reported
   Headers as COMPLETE while all five were missing: an interface could declare its iteration and the gap
   report would never mention it. `maplike`/`setlike` carry the same rule and are expanded for the same
   reason. @@iterator is left out of the diff because the scan looks for property NAMES in the component and a
   symbol-keyed one has none — the four named members are what it can honestly check. */
function iterationMembers(node) {
  const out = [];
  for (const m of node.members) {
    if (m.type !== "iterable" && m.type !== "maplike" && m.type !== "setlike") continue;
    const pair = m.type === "maplike" || (m.idlType && m.idlType.length === 2);
    out.push("forEach", "values");
    if (pair) out.push("entries", "keys");
    if (m.type === "maplike" || m.type === "setlike") out.push("has");
    if (m.type === "maplike") out.push("get");
  }
  return out;
}

function members(name) {
  const out = [], seen = new Set();
  const add = (n) => { if (n && !seen.has(n)) { seen.add(n); out.push(n); } };
  for (const m of flatten(name)) {
    /* STATIC MEMBERS COUNT. They were skipped, and the audit therefore called Response's member list complete
       while `redirect` and `json` — two of its five operations — did not exist. A static lives on the
       interface OBJECT rather than the prototype, which changes where a component installs it and nothing
       about whether it is missing; the installed-name scan is by property name and reads both alike. */
    if (m.type === "attribute") add(m.name);
    else if (m.type === "operation") add(m.name);
  }
  /* the iteration members of this interface AND of everything it inherits from, which is what flatten walks */
  for (const m of flatten(name)) {
    if (m.type === "iterable" || m.type === "maplike" || m.type === "setlike")
      for (const n of iterationMembers({ members: [m] })) add(n);
  }
  return out;
}

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
for (const r of world.records) {
  if (!r.ifaces.length) { unattributed.push(r); continue; }
  for (const iface of r.ifaces) {
    addTo(r.stubbed ? stubbedBy : installedBy, iface, r.name);
    addTo(landsIn, r.file, iface);
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
const distinct = new Set();
const unresolvedAll = new Map();
for (const [iface, where] of Object.entries(INTERFACES)) {
  const paths = Array.isArray(where) ? where : [where];
  const file = paths.join(" + ");
  let src = "", missing = [], present = [];
  for (const one of paths) {
    try { src += readFileSync(join(BROWSER, one), "utf8"); present.push(join(BROWSER, one)); }
    catch { missing.push(one); }
  }
  if (missing.length === paths.length) { console.warn(`[idl-audit] ${iface}: component ${file} not found`); continue; }
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
  const strangers = present.filter((p) => {
    const lands = landsIn.get(p);
    return !lands || ![...lands].some((n) => chain.includes(n));
  }).map((p) => {
    const lands = [...(landsIn.get(p) || [])];
    return `${p.replace(BROWSER + "/", "")} (${lands.length ? "installs for " + lands.join(", ") : "installs nothing this can attribute"})`;
  });
  /* An install in one of this row's files whose target's interface could not be decided. Neither credited to
     the row (the false COMPLETE this attribution removes) nor dropped — named, with its member and line. */
  const rowUnattributed = inRow(unattributed);
  // The g_opaque-as-prototype fallback is a BANNED shrug: it silently serves EVERY unbuilt member as an opaque
  // value, hiding a missing browser feature (it is not our choice which features to omit — a browser has them
  // all). A component must implement its real surface and DFAIL loud on an unbuilt member, never opaque-shrug it.
  const bannedShrug = /JS_SetPrototype\s*\([^)]*\bg_opaque\b/.test(src);
  const spec = members(iface);
  /* A CONDITIONAL member — one the component DECLARES this user agent must not have (idl_members_excluded). It
     is not a gap and it is not installed, so it is neither counted nor dropped: it is named, with the spec
     sentence that excludes it, so nobody works it off the ABSENT list and builds a member the spec forbids.
     The declaration is CHECKED here, which is what stops it being an exclusion list: a name it excludes that
     the corpus no longer carries is stale, and one the component installs anyway contradicts itself. */
  const cond = excluded.filter((e) => e.iface === iface);
  const condStale = cond.filter((e) => !spec.includes(e.name));
  const condInstalled = cond.filter((e) => installed.has(e.name));
  const condNames = new Set(cond.map((e) => e.name));
  const absent = spec.filter((n) => !installed.has(n) && !condNames.has(n));
  const noop = spec.filter((n) => stubbed.has(n));
  totalMissing += absent.length + noop.length;
  for (const n of absent) distinct.add(n);
  for (const n of noop) distinct.add(n);
  for (const u of unresolved)
    unresolvedAll.set(`${u.file}:${u.line}:${u.expr}`, u);
  /* A property write with a member's NAME onto an object no installer ever names. Not counted (it may be a
     record field that happens to share the name) and not hidden (it may be an IDL member installed as a plain
     own property, which `document.title` and `screen.width` are) — reported, so both are visible. */
  const plain = [...new Set(offInstaller.filter((o) => absent.includes(o.name)).map((o) => o.name))];
  const parts = [];
  if (absent.length) parts.push(`ABSENT ${absent.length} — ${absent.join(", ")}`);
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
/* THE AUDIT'S GAP REPORT ON ITSELF. An install whose member name is decided at RUNTIME cannot be diffed against
   the IDL, and pretending either way is what this rewrite exists to stop: counted, it fills a gap that is open;
   dropped, it opens a gap that is filled. Named here with file and line, it is a work queue — make the name
   static, or teach the detector the construct. */
if (unresolvedAll.size) {
  console.log(`[idl-audit] ${unresolvedAll.size} install construct(s) whose member name could not be resolved ` +
              `statically — neither counted as installed nor reported as a gap:`);
  for (const u of unresolvedAll.values())
    console.log(`[idl-audit]   ${u.file.replace(BROWSER + "/", "")}:${u.line}  ${u.form}(… ${u.expr} …)`);
}
/* The same constructs in components no row names. The scan is over the whole program now, so these exist and
   are counted; hiding them behind "no row asked" would be the audit choosing what to know about itself. */
const elsewhere = world.unresolved.filter((u) => !unresolvedAll.has(`${u.file}:${u.line}:${u.expr}`));
if (elsewhere.length) {
  const byFile = new Map();
  for (const u of elsewhere) byFile.set(u.file, (byFile.get(u.file) || 0) + 1);
  console.log(`[idl-audit] ${elsewhere.length} more unresolved install construct(s) in components no row ` +
              `names: ${[...byFile].sort((a, b) => b[1] - a[1])
                .map(([f, n]) => `${f.replace(BROWSER + "/", "")}×${n}`).join(", ")}`);
}

/* THE OTHER HALF OF THE AUDIT'S GAP REPORT ON ITSELF — an install whose member name IS decided but whose
   TARGET's interface is not. It is the same rule one level down: counted, it credits a member to whichever row
   happened to name the file (the file-granular lie); dropped, it opens a gap that is filled. So it is named,
   grouped by the file that wrote it, and the work is either to give the prototype its §3.7.3 tag or to teach
   this detector the construct that carries the object. */
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
const exposedOnWindow = (node) => {
  const ext = (node.extAttrs || []).find((a) => a.name === "Exposed");
  if (!ext) return false;                       // no [Exposed] at all: not a global constructor
  if (!ext.rhs) return false;
  const v = ext.rhs.value;
  if (ext.rhs.type === "*") return true;        // [Exposed=*]
  if (typeof v === "string") return v === "Window";
  return Array.isArray(v) && v.some((x) => (x.value || x) === "Window");
};

/* Walk the DECLARATIONS, not the merged byName map: byName keeps the first node it saw for a name and merges
   later members into it, so an interface first encountered as a `partial` (which carries no [Exposed]) loses the
   real declaration's extended attributes — that is why Element and HTMLElement went missing from a table built
   off the map. A name is a global if ANY of its declarations says [Exposed=Window]. */
const platform = new Set();
for (const spec of Object.values(all)) {
  let ast;
  try { ast = parse(await spec.text()); } catch { continue; }
  for (const n of ast)
    if ((n.type === "interface" || n.type === "namespace" || n.type === "callback interface")
        && n.name && exposedOnWindow(n))
      platform.add(n.name);
}
for (const m of members("Window")) platform.add(m);

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
if (prev !== header) {
  writeFileSync(OUTH, header);
  console.log(`[idl-audit] platform_names.h REGENERATED — ${names.length} global names exposed on Window`);
} else {
  console.log(`[idl-audit] platform_names.h current — ${names.length} global names exposed on Window`);
}
