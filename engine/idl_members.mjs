/* THE WEB IDL INDEX — @webref/idl, parsed once, flattened once, read by every gate that needs to know what the
 * PLATFORM provides.
 *
 * It was inline in idlgen.mjs, and the day a second consumer needed the same answer that placement became the
 * defect CLAUDE.md names for a build's source list: a second hand-written copy is right on the day it is
 * written and silently wrong on the day the first one learns something. There is exactly one thing this
 * question can be answered from — the W3C-curated corpus browsers are written against — so there is exactly one
 * reader of it here, and a consumer that wants "is `redirected` a member of Response" gets the SAME answer as
 * the consumer that wants "which members of Response does the engine still owe".
 *
 * WHAT IT FLATTENS, AND WHY EACH IS NOT OPTIONAL. An interface's member list as spelled in its own declaration
 * is not its member list: `inheritance` puts the base's members on the prototype chain, an `includes` statement
 * puts a mixin's members on the interface itself, a `partial interface` in another spec adds more, and an
 * `iterable<>`/`maplike<>`/`setlike<>` declaration defines named members (§3.7.9-§3.7.12) that appear nowhere
 * in the members array. A consumer diffing against the unflattened list reports members that are present and
 * misses members that are not — in both directions, which is the false-COMPLETE this whole family of gates
 * exists to remove.
 *
 * THE DECLARATIONS ARE HANDED BACK RAW as well as merged, because the two answer different questions and the
 * merged map cannot answer the second. `byName` keeps the FIRST node seen for a name and folds later members
 * into it, so an interface first met as a `partial` (which carries no extended attributes) loses the real
 * declaration's `[Exposed=Window]`. A consumer asking about extended attributes must therefore walk the
 * declarations; a consumer asking about members reads the merged map. Both are here, from one parse. */
import { listAll } from "@webref/idl";
import { parse } from "webidl2";

/* Web IDL §3.7.9-§3.7.12: the members an iteration declaration defines but does not spell out. `for (const [k,
   v] of headers)` is how half the code that touches one is written, and a member list without them reports an
   interface COMPLETE while every one of them is missing.
   §3.7.10's async set is NOT §3.7.9's: a pair async declaration defines `entries`, `keys` and `values`, a value
   one defines `values` alone, and NEITHER defines `forEach` — an async iterable has no synchronous walk to hand
   a callback to, so expecting one would report a member the spec does not define. */
export function iterationMembers(node) {
  const out = [];
  for (const m of node.members) {
    if (m.type === "async_iterable") {
      out.push("values");
      if (m.idlType && m.idlType.length === 2) out.push("entries", "keys");
      continue;
    }
    if (m.type !== "iterable" && m.type !== "maplike" && m.type !== "setlike") continue;
    const pair = m.type === "maplike" || (m.idlType && m.idlType.length === 2);
    out.push("forEach", "values");
    if (pair) out.push("entries", "keys");
    if (m.type === "maplike" || m.type === "setlike") out.push("has");
    if (m.type === "maplike") out.push("get");
  }
  return out;
}

export async function loadIdl() {
  const all = await listAll();
  /* Every top-level declaration of the whole corpus, in spec order, kept so a consumer that needs the
     UNMERGED node (extended attributes, per-declaration exposure) has it without a second parse. */
  const declarations = [];
  for (const spec of Object.values(all)) {
    let ast;
    try { ast = parse(await spec.text()); } catch { continue; }
    declarations.push(...ast);
  }

  const byName = new Map();
  const inheritanceOf = new Map();
  const includes = [];
  for (const n of declarations) {
    /* A CALLBACK INTERFACE IS COLLECTED. Web IDL §3.7.1 gives one a callback interface OBJECT carrying its
       constants — `NodeFilter.SHOW_ELEMENT` is a real property of a real object a page reads.
       A NAMESPACE IS COLLECTED. §3.13 gives a namespace a real object on the global whose properties are its
       operations and attributes — `console.log`, `CSS.supports`. Skipping either kind means a consumer diffs
       against an EMPTY member list and reads COMPLETE against nothing at all. */
    if ((n.type === "interface" || n.type === "interface mixin" || n.type === "callback interface" ||
         n.type === "namespace") && n.name) {
      if (n.inheritance) inheritanceOf.set(n.name, n.inheritance);
      const prev = byName.get(n.name);
      if (prev) prev.members.push(...n.members);
      else byName.set(n.name, n);
    } else if (n.type === "includes") includes.push(n);
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

  /* The member NAMES of one interface, flattened. §3.7.5's constants and the STATIC members count: a static
     lives on the interface object rather than the prototype, which changes where a component installs it and
     nothing about whether a page can read it. */
  function members(name) {
    const out = [], seen = new Set();
    const add = (n) => { if (n && !seen.has(n)) { seen.add(n); out.push(n); } };
    for (const m of flatten(name)) {
      if (m.type === "attribute") add(m.name);
      else if (m.type === "operation") add(m.name);
      else if (m.type === "const") add(m.name);
    }
    for (const m of flatten(name)) {
      if (m.type === "iterable" || m.type === "maplike" || m.type === "setlike" || m.type === "async_iterable")
        for (const n of iterationMembers({ members: [m] })) add(n);
    }
    return out;
  }

  return { declarations, byName, inheritanceOf, flatten, members };
}

/* EVERY GLOBAL NAME WEB IDL EXPOSES ON WINDOW. Three sources, all spec text, and each was missing from an
 * earlier answer to this question with a measured cost:
 *   1. Every interface / namespace / callback interface DECLARED `[Exposed=Window]` (or `[Exposed=*]`). Web IDL
 *      §3.7 puts its interface OBJECT on the global, so `Node`, `Element`, `DOMException`, `WebSocket` are
 *      properties of Window that Window's own member list does not mention.
 *   2. Every member of the Window interface itself, flattened through its mixins — `fetch`, `setTimeout`,
 *      `onload`, `location`.
 *   3. §3.7.2's legacy factory functions: `[LegacyFactoryFunction=Image(…)]` puts `Image` on the global as a
 *      built-in function object under a name that is neither the interface's nor a member of Window. The corpus
 *      has three — Image, Option, Audio — and each is a name a page reaches for by writing `new Image()`.
 * Taken only where the interface carrying it is itself exposed on Window, because that is what decides whether
 * the function object is created in this global at all. */
const exposedOnWindow = (node) => {
  const ext = (node.extAttrs || []).find((a) => a.name === "Exposed");
  if (!ext || !ext.rhs) return false;           // no [Exposed] at all: not a global constructor
  const v = ext.rhs.value;
  if (ext.rhs.type === "*") return true;        // [Exposed=*]
  if (typeof v === "string") return v === "Window";
  return Array.isArray(v) && v.some((x) => (x.value || x) === "Window");
};

const legacyFactoryNames = (node) =>
  (node.extAttrs || [])
    .filter((a) => a.name === "LegacyFactoryFunction" && a.rhs && typeof a.rhs.value === "string")
    .map((a) => a.rhs.value);

/* Walk the DECLARATIONS, not the merged map: `byName` keeps the first node it saw for a name and folds later
   members into it, so an interface first met as a `partial` (which carries no [Exposed]) loses the real
   declaration's extended attributes — that is why Element and HTMLElement went missing from a table built off
   the map. A name is a global if ANY of its declarations says [Exposed=Window]. */
export function windowGlobals(idl) {
  const out = new Set();
  for (const n of idl.declarations) {
    if (!((n.type === "interface" || n.type === "namespace" || n.type === "callback interface")
          && n.name && exposedOnWindow(n)))
      continue;
    out.add(n.name);
    for (const f of legacyFactoryNames(n)) out.add(f);
  }
  for (const m of idl.members("Window")) out.add(m);
  return out;
}
