/* Web IDL gap AUDITOR — the IDL's real job: tell us what browser logic is MISSING, not generate stub bindings.
 * For each interface we implement, it reads the canonical .idl (@webref/idl, the W3C-curated corpus browsers
 * use), parses it (webidl2), flattens inherited + mixin members, and DIFFS the spec member list against the
 * members the component actually installs (a scan of the component .c for the property names it wires). It
 * prints the missing members so we implement them at the root — never a generated noop/DCHECK stub. Runs at
 * build time (best-effort: skipped if the idl toolchain isn't installed). */
import { listAll } from "@webref/idl";
import { parse } from "webidl2";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const BROWSER = join(HERE, "host", "browser");

// interface -> the component .c that implements it. Add an interface here to audit its coverage against the spec.
const INTERFACES = {
  AbortSignal:          "core/dom/abort.c",
  AbortController:      "core/dom/abort.c",
  IntersectionObserver: "core/intersection_observer/intersection_observer.c",
  MutationObserver:     "core/dom/mutation_observer.c",
  ResizeObserver:       "core/resize_observer/resize_observer.c",
  PerformanceObserver:  "core/timing/performance_observer.c",
  Blob:                 "core/fileapi/blob.c",
  Response:             "core/loader/response.c",
  Notification:         "modules/notification.c",
  Navigator:            "core/frame/navigator.c",
  History:              "core/frame/history.c",
  Screen:               "core/frame/screen.c",
  URLSearchParams:      "platform/urlobj.c",
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
for (const [iface, file] of Object.entries(INTERFACES)) {
  let src;
  try { src = readFileSync(join(BROWSER, file), "utf8"); } catch { console.warn(`[idl-audit] ${iface}: component ${file} not found`); continue; }
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
  const parts = [];
  if (absent.length) parts.push(`ABSENT ${absent.length} — ${absent.join(", ")}`);
  if (noop.length) parts.push(`js_noop-STUB ${noop.length} — ${noop.join(", ")}`);
  if (bannedShrug) parts.push(`BANNED g_opaque-prototype shrug (silently serves unbuilt members as opaque — remove it, build the features or DFAIL)`);
  if (parts.length) console.log(`[idl-audit] ${iface} (${file}): ${parts.join(" | ")}`);
  else console.log(`[idl-audit] ${iface}: complete`);
}
if (totalMissing) console.log(`[idl-audit] ${totalMissing} spec members not yet implemented — implement each at the root (never a stub).`);
