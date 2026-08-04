/* Web IDL gap AUDITOR — the IDL's real job: tell us what browser logic is MISSING, not generate stub bindings.
 * For each interface we implement, it reads the canonical .idl (@webref/idl, the W3C-curated corpus browsers
 * use), parses it (webidl2), flattens inherited + mixin members, and DIFFS the spec member list against the
 * members the component actually installs (a scan of the component .c for the property names it wires). It
 * prints the missing members so we implement them at the root — never a generated noop/DCHECK stub. Runs at
 * build time (best-effort: skipped if the idl toolchain isn't installed).
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

const HERE = dirname(fileURLToPath(import.meta.url));
const BROWSER = join(HERE, "host", "browser");

// interface -> the component .c that implements it, or the componentS: one interface's surface can be split
// across components when the spec puts unrelated capabilities on one object. Window is the case that forces it —
// its members are installed by window.c (browsing context), document.c, location.c, fetch.c and
// event_target.c — and scanning only one of them reported every member the others install as ABSENT, which is
// the audit lying in the direction that gets a real gap ignored.
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
  Blob:                 "core/fileapi/blob.c",
  Response:             "core/fetch/response.c",
  Notification:         "modules/notification.c",
  Window:              ["core/frame/window.c", "core/dom/document.c", "core/frame/location.c",
                        "core/fetch/fetch.c", "core/events/event_target.c", "core/loader/module_loader.c",
                        "core/timing/timer.c", "core/frame/navigator.c", "core/frame/screen.c",
                        "core/dom/abort.c", "core/css/css_style_declaration.c"],
  Navigator:            "core/frame/navigator.c",
  History:              "core/frame/history.c",
  Screen:               "core/frame/screen.c",
  URLSearchParams:      "platform/urlobj.c",
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
  NodeList:            "core/dom/collections.c",
  HTMLCollection:      "core/dom/collections.c",
  Element:             ["core/dom/element.c", "core/dom/node.c", "core/events/event_target.c",
                        "core/dom/dom_token_list.c"],
  /* The HTML layer. Each interface's files are its own plus everything it INHERITS from — HTMLElement on
     Element on Node — because a member installed on a base really is reachable, and reporting it absent is the
     audit lying in the direction that gets a real gap ignored. */
  HTMLElement:         [...HTML_BASE],
  HTMLUnknownElement:  [...HTML_BASE],
  HTMLAnchorElement:   [...HTML_BASE],
  HTMLScriptElement:   [...HTML_BASE],
  HTMLImageElement:    [...HTML_BASE],
  HTMLIFrameElement:   [...HTML_BASE],
  HTMLFormElement:     [...HTML_BASE],
  HTMLInputElement:    [...HTML_BASE],
  HTMLButtonElement:   [...HTML_BASE],
  HTMLLinkElement:     [...HTML_BASE],
  HTMLMetaElement:     [...HTML_BASE],
  HTMLDivElement:      [...HTML_BASE],
  Document:            ["core/dom/document.c", "core/dom/node.c", "core/events/event_target.c"],
  HTMLTemplateElement: [...HTML_BASE],
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
function members(name) {
  const out = [], seen = new Set();
  for (const m of flatten(name)) {
    if (m.special === "static") continue;
    if (m.type === "attribute" && !seen.has(m.name)) { seen.add(m.name); out.push(m.name); }
    else if (m.type === "operation" && m.name && !seen.has(m.name)) { seen.add(m.name); out.push(m.name); }
  }
  return out;
}

let totalMissing = 0;
/* The DISTINCT names, because the per-interface counts legitimately repeat an inherited gap: HTMLElement's
   getBoundingClientRect is absent on every one of the twelve HTML interfaces that inherit it, and one
   implementation fixes all twelve. The per-interface list stays exact — it is what that interface's IDL says —
   and the headline says how many things there are to BUILD. */
const distinct = new Set();
for (const [iface, where] of Object.entries(INTERFACES)) {
  const paths = Array.isArray(where) ? where : [where];
  const file = paths.join(" + ");
  let src = "", missing = [];
  for (const one of paths) {
    try { src += readFileSync(join(BROWSER, one), "utf8"); } catch { missing.push(one); }
  }
  if (missing.length === paths.length) { console.warn(`[idl-audit] ${iface}: component ${file} not found`); continue; }
  if (missing.length) console.warn(`[idl-audit] ${iface}: ${missing.join(", ")} not found — audited without it`);
  // The property names the component actually installs appear as string literals (JS_SetPropertyStr / JS_NewAtom
  // / def_getset(..., "name", ...)). A member absent from every literal is unimplemented; a member wired to
  // js_noop is STUBBED (present but does nothing — the banned lazy stub the audit exists to expose).
  const installed = new Set([...src.matchAll(/"([A-Za-z_$][\w$]*)"/g)].map((m) => m[1]));
  // js_noop reaches a member two ways: the JS_SetPropertyStr form (`"name", JS_NewCFunction(ctx, js_noop`) and
  // the IDL member-TABLE form (`{ "name", IDL_METHOD, js_noop, ... }`). Catch BOTH — a table-form stub is just
  // as banned and was previously invisible to the name-scan.
  const stubbed = new Set([
    ...src.matchAll(/"([A-Za-z_$][\w$]*)"\s*,\s*JS_NewCFunction\w*\(\s*ctx\s*,\s*js_noop\b/g),
    ...src.matchAll(/"([A-Za-z_$][\w$]*)"\s*,\s*IDL_(?:METHOD|ATTRIBUTE)\s*,\s*js_noop\b/g),
  ].map((m) => m[1]));
  // The g_opaque-as-prototype fallback is a BANNED shrug: it silently serves EVERY unbuilt member as an opaque
  // value, hiding a missing browser feature (it is not our choice which features to omit — a browser has them
  // all). A component must implement its real surface and DFAIL loud on an unbuilt member, never opaque-shrug it.
  const bannedShrug = /JS_SetPrototype\s*\([^)]*\bg_opaque\b/.test(src);
  const absent = members(iface).filter((n) => !installed.has(n));
  const noop = members(iface).filter((n) => stubbed.has(n));
  totalMissing += absent.length + noop.length;
  for (const n of absent) distinct.add(n);
  for (const n of noop) distinct.add(n);
  const parts = [];
  if (absent.length) parts.push(`ABSENT ${absent.length} — ${absent.join(", ")}`);
  if (noop.length) parts.push(`js_noop-STUB ${noop.length} — ${noop.join(", ")}`);
  if (bannedShrug) parts.push(`BANNED g_opaque-prototype shrug (silently serves unbuilt members as opaque — remove it, build the features or DFAIL)`);
  if (parts.length) console.log(`[idl-audit] ${iface} (${file}): ${parts.join(" | ")}`);
  else console.log(`[idl-audit] ${iface}: complete`);
}
if (totalMissing)
  console.log(`[idl-audit] ${distinct.size} distinct spec members not yet implemented (${totalMissing} across ` +
              `all interfaces, since an inherited gap is absent on each) — implement each at the root, never a stub.`);

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
