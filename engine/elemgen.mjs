/* THE ELEMENT-INTERFACE TABLE — GENERATED FROM THE HTML STANDARD, AUDITED WITHOUT IT.
 *
 * WHAT THIS ANSWERS. HTML §3.2.2 Elements in the DOM defines "the element interface for an element with name
 * name in the HTML namespace" as a seven-step algorithm, and four of those steps are DATA: three explicit name
 * lists the section states itself, and "if this specification defines an interface appropriate for the element
 * type corresponding to the local name name", which is §4's element-interface index plus the six elements §16.3
 * keeps interfaces for. core/html/html_element.c owns the ALGORITHM; this owns the rows it walks.
 *
 * WHY IT IS GENERATED. The rows are a fact the STANDARD states and this codebase does not, so a hand-kept copy
 * of them is a second answer that is right on the day it is typed and silently wrong afterwards — which is what
 * happened: the table had 71 of the 141 names, so `<section>`, `<nav>`, `<main>`, `<article>` and thirty-five
 * more answered HTMLUnknownElement where every browser answers HTMLElement, and `<progress>`, `<datalist>`,
 * `<menu>` and `<selectedcontent>` answered it where the spec names a dedicated interface. Nothing in the tree
 * could see it: idlgen.mjs audits which MEMBERS an interface installs and knows nothing about which TAG wears
 * which interface, which is a different axis with no instrument on it until this one.
 *
 * WHAT IS NOT GENERATED, AND WHY THE SPLIT IS THERE. The REFLECTIONS stay hand-written in html_element.c. A
 * reflection is a pair of names AND a type AND a range AND a default — behaviour the element index does not
 * state and the IDL states in a form no two-column table carries — so generating it would be fabricating it.
 * The join is by interface NAME at init, and the audit below checks it in the one direction that can rot: a
 * reflection set written for an interface no row wears is dead code the compiler cannot see.
 *
 * THE AUDIT RUNS WITH NO NETWORK, and it is not a re-derivation — the committed header IS the corpus's answer,
 * so there is nothing offline to re-derive it from. What it checks instead are the three things a wrong header
 * would break, each against a source already in this tree:
 *   1. @webref/idl — every interface a row names must be a real `[Exposed=Window]` interface. A typo, or an
 *      interface the standard retired, reddens here rather than installing a global name nothing else knows.
 *   2. WPT `html/semantics/interfaces.html`'s own element list — an INDEPENDENT oracle, written by the people
 *      who wrote the spec, checked out and pinned in this repository, covering 151 names across three creation
 *      variants. It is the reason this file does not carry a second census of the mapping: an oracle you have
 *      beats one you build. It is consulted where the corpus is checked out and reported as UNCHECKED where it
 *      is not, because a check that silently skips itself is a clean bill from a run that asked nothing.
 *   3. html_element.c — every interface with a reflection set must be a row's interface.
 * `--regen` is the one command that reaches the network. It curls, like engine/citegen.mjs, because curl is
 * what carries this environment's proxy configuration.
 *
 * Usage:  node engine/elemgen.mjs [--regen]
 */
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { loadIdl, windowGlobals } from "./idl_members.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "host", "browser", "core", "html", "element_interfaces.h");
const COMPONENT = join(HERE, "host", "browser", "core", "html", "html_element.c");
const WPT_LIST = join(HERE, ".work", "wpt", "html", "semantics", "interfaces.js");
const REGEN = process.argv.includes("--regen");

let defects = 0;
const defect = (s) => { defects++; console.log(`[elemgen] DEFECT  ${s}`); };

const strip = (h) => h.replace(/<[^>]+>/g, "")
                      .replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"')
                      .replace(/&amp;/g, "&").replace(/\s+/g, " ").trim();

/* ---------------------------------------------------------------------------------------------------------
 * --regen: the three pages of the HTML Standard that state the rows.
 */
function curl(url) {
  const r = spawnSync("curl", ["-sSL", "--fail", url], { encoding: "utf8", maxBuffer: 1 << 28 });
  if (r.status !== 0) { console.error(`[elemgen] curl ${url} failed: ${r.stderr}`); process.exit(1); }
  return r.stdout;
}

/* §3.2.2's steps 1-3. The three lists are read out of the algorithm's own <ol>, by TOP-LEVEL <li> with list
   depth tracked — a step holding a nested list is ONE step, and a flat <li> count promotes its sub-items to
   peers. Seven top-level steps is the whole algorithm; anything else means the section was restructured and
   the rows below would be a guess. */
function stepLists(domHtml) {
  const at = domHtml.indexOf("for an element with name");
  if (at < 0) throw new Error("HTML §3.2.2's element-interface algorithm was not found in dom.html");
  const algo = domHtml.slice(domHtml.lastIndexOf("<p>", at), domHtml.indexOf("</ol>", at) + 5);
  let depth = 0;
  const tops = [];
  for (const m of algo.matchAll(/<(\/?)(ol|ul|dl|li)\b[^>]*>/g)) {
    const close = m[1] === "/", tag = m[2];
    if (tag !== "li") depth += close ? -1 : 1;
    else if (!close && depth === 1) tops.push(m.index);
  }
  if (tops.length !== 7)
    throw new Error(`HTML §3.2.2's element-interface algorithm has ${tops.length} top-level steps, not 7 — ` +
                    `the section was restructured and this parse no longer knows which step is which`);
  const text = tops.map((p, i) => strip(algo.slice(p, i + 1 < tops.length ? tops[i + 1] : algo.indexOf("</ol>", p))));
  /* "If name is a, b, or c, then return X." — the names, in the standard's own order. */
  const names = (t) => {
    const m = t.match(/If name is (.+?), then return/);
    if (!m) throw new Error(`HTML §3.2.2 step text is not the expected shape: ${t}`);
    return m[1].split(/,\s*(?:or\s+)?|\s+or\s+/).map((s) => s.trim()).filter((s) => /^[a-z][a-z0-9-]*$/.test(s));
  };
  const iface = (t) => {
    const m = t.match(/then return (HTML[A-Za-z]+)\./);
    if (!m) throw new Error(`HTML §3.2.2 step names no interface: ${t}`);
    return m[1];
  };
  return [1, 2, 3].map((i) => ({ step: i, names: names(text[i - 1]), iface: iface(text[i - 1]) }));
}

/* §4's element-interface index — the tag column against the FIRST interface of the inheritance chain in the
   interface column ("HTMLAnchorElement : HTMLElement"). Several tags share a row, which is what the spec says
   and why the interface is read per tag rather than derived from the tag. */
function indexRows(indicesHtml) {
  const a = indicesHtml.indexOf("<h3 id=element-interfaces");
  const b = indicesHtml.indexOf("<h3 id=all-interfaces");
  if (a < 0 || b < a) throw new Error("HTML §4's element-interface index was not found in indices.html");
  const body = indicesHtml.slice(a, b);
  const rows = [];
  for (const r of body.slice(body.indexOf("<tbody>")).split(/<tr>/).slice(1)) {
    const cells = r.split(/<td>/).slice(1);
    if (cells.length < 2) continue;
    const code = (c) => [...c.matchAll(/<code[^>]*>(?:<a[^>]*>)?([^<]+)/g)].map((m) => m[1].trim());
    const tags = code(cells[0]), ifaces = code(cells[1]);
    for (const t of tags) rows.push([t, ifaces[0]]);
  }
  if (rows.length < 100) throw new Error(`HTML §4's element-interface index parsed to ${rows.length} rows`);
  return rows;
}

/* §16.3's six. Each is stated as one sentence — "The marquee element must implement the HTMLMarqueeElement
   interface." — which is why it is matched as one rather than by walking the section's IDL blocks. */
function obsoleteRows(obsoleteHtml) {
  const rows = [...strip(obsoleteHtml).matchAll(/The ([a-z]+) element must implement the (HTML[A-Za-z]+) interface/g)]
    .map((m) => [m[1], m[2]]);
  if (!rows.length) throw new Error("HTML §16.3 states no element-implements-interface sentence");
  return rows;
}

function generate() {
  const dom = curl("https://html.spec.whatwg.org/multipage/dom.html");
  const indices = curl("https://html.spec.whatwg.org/multipage/indices.html");
  const obsolete = curl("https://html.spec.whatwg.org/multipage/obsolete.html");
  const groups = [];
  for (const g of stepLists(dom))
    groups.push({ step: `${g.step}`, why: `HTML §3.2.2 Elements in the DOM step ${g.step}`,
                  rows: g.names.map((t) => [t, g.iface]) });
  groups.push({ step: "4", why: "HTML §3.2.2 step 4 — HTML §4's element-interface index", rows: indexRows(indices) });
  groups.push({ step: "4", why: "HTML §3.2.2 step 4 — HTML §16.3 Requirements for implementations",
                rows: obsoleteRows(obsolete) });

  const seen = new Map();
  let body = "";
  let n = 0;
  for (const g of groups) {
    body += `    /* ${g.why} */ \\\n`;
    for (const [tag, iface] of g.rows) {
      if (seen.has(tag))
        throw new Error(`the standard decides "${tag}" twice — ${seen.get(tag)} and ${iface} — so the table's ` +
                        `first-match lookup would answer one of them with nothing to say which`);
      seen.set(tag, iface);
      body += `    X("${tag}", "${iface}", ${g.step}) \\\n`;
      n++;
    }
  }
  return { n, text:
"/* GENERATED by engine/elemgen.mjs from the HTML Standard — DO NOT EDIT.\n" +
" * HTML §3.2.2 Elements in the DOM's \"element interface\", as ROWS: every local name the standard decides an\n" +
" * interface for in the HTML namespace, with the step of §3.2.2's seven that decided it. Steps 1-3 are the\n" +
" * three name lists §3.2.2 states itself; step 4's rows are HTML §4's element-interface index and the six\n" +
" * elements HTML §16.3 Requirements for implementations keeps an interface for.\n" +
" * The ALGORITHM is core/html/html_element.c's — including steps 5, 6 and 7, which decide names no row names.\n" +
" * The REFLECTIONS are html_element.c's too, joined onto these rows by interface name: a reflection is a\n" +
" * behaviour the index does not state, so generating one would be inventing it.\n" +
" * Regenerate with `node engine/elemgen.mjs --regen`, and commit the result — the build has no network and\n" +
" * this table is not optional. */\n" +
"#ifndef APICLIENT_ELEMENT_INTERFACES_H\n#define APICLIENT_ELEMENT_INTERFACES_H\n\n" +
"/* X(local name, interface name, the §3.2.2 step that decided the row) */\n" +
"#define HTML_ELEMENT_INTERFACES(X) \\\n" + body.replace(/ \\\n$/, "\n") +
"\n#endif\n" };
}

/* ---------------------------------------------------------------------------------------------------------
 * The audit — the committed header against three sources already in this tree.
 */
function committedRows() {
  if (!existsSync(OUT)) { defect(`${OUT} does not exist — run \`node engine/elemgen.mjs --regen\``); return []; }
  const h = readFileSync(OUT, "utf8");
  return [...h.matchAll(/X\("([^"]+)",\s*"([^"]+)",\s*(\d)\)/g)].map((m) => [m[1], m[2], +m[3]]);
}

const rows = committedRows();
const byTag = new Map();
for (const [tag, iface] of rows) {
  if (byTag.has(tag)) defect(`the committed table decides "${tag}" twice (${byTag.get(tag)} and ${iface})`);
  byTag.set(tag, iface);
}
console.log(`[elemgen] ${rows.length} rows, ${new Set(rows.map((r) => r[1])).size} distinct interfaces`);

/* 1. Every interface a row names is a real global interface. */
{
  const idl = await loadIdl();
  const globals = windowGlobals(idl);
  for (const iface of new Set(rows.map((r) => r[1])))
    if (!globals.has(iface))
      defect(`no interface named ${iface} is [Exposed=Window] in @webref/idl, and ${
             rows.filter((r) => r[1] === iface).map((r) => `<${r[0]}>`).join(" ")} is given it`);
}

/* 2. WPT's own element list. It carries the interface SUFFIX ("Anchor" for HTMLAnchorElement, "" for
      HTMLElement), and it deliberately includes names no row names — `foo-bar`, `xxx`, `å-bar` — because those
      are what §3.2.2's steps 6 and 7 decide. Those are checked by the C, not here: what this can check is that
      no row DISAGREES with the corpus, and that every name the corpus expects an interface for has one. */
if (!existsSync(WPT_LIST)) {
  console.log(`[elemgen] UNCHECKED against WPT html/semantics/interfaces.js — the corpus is not checked out ` +
              `at ${WPT_LIST}, so the independent oracle asked nothing this run`);
} else {
  const src = readFileSync(WPT_LIST, "utf8");
  const list = [...src.matchAll(/\[\s*"((?:[^"\\]|\\u[0-9a-fA-F]{4})+)",\s*"([A-Za-z]*)"\s*\]/g)]
    .map((m) => [JSON.parse(`"${m[1]}"`), `HTML${m[2]}Element`]);
  let agreed = 0, deferred = 0;
  for (const [tag, want] of list) {
    const got = byTag.get(tag);
    if (got === undefined) {
      /* No row: §3.2.2 steps 6/7 decide it, and the only answers those two can give are HTMLElement (a valid
         custom element name) and HTMLUnknownElement. A corpus entry expecting anything else is a row this
         table is MISSING, which is exactly the defect this file was written for. */
      if (want !== "HTMLElement" && want !== "HTMLUnknownElement")
        defect(`WPT html/semantics/interfaces.js expects <${tag}> to be ${want} and the table has no row for ` +
               `it — §3.2.2's steps 6 and 7 can only answer HTMLElement or HTMLUnknownElement`);
      else deferred++;
    } else if (got !== want) {
      defect(`WPT html/semantics/interfaces.js expects <${tag}> to be ${want}; the table says ${got}`);
    } else agreed++;
  }
  console.log(`[elemgen] WPT html/semantics/interfaces.js: ${list.length} names, ${agreed} agree with a row, ` +
              `${deferred} left to §3.2.2 steps 6/7`);
}

/* 3. Every reflection set html_element.c declares belongs to an interface some row wears. */
if (!existsSync(COMPONENT)) {
  defect(`${COMPONENT} does not exist, so the reflection sets could not be checked against the rows`);
} else {
  const c = readFileSync(COMPONENT, "utf8");
  const tbl = c.slice(c.indexOf("IFACE_REFL[] = {"), c.indexOf("#define IFACE_REFL_N"));
  const declared = [...tbl.matchAll(/\{\s*"(HTML[A-Za-z]+)"/g)].map((m) => m[1]);
  if (!declared.length)
    defect(`html_element.c declares no IFACE_REFL rows — either the join was renamed, in which case this ` +
           `check now silently passes for everything, or the reflections are gone`);
  const worn = new Set(rows.map((r) => r[1]));
  for (const iface of declared)
    if (!worn.has(iface))
      defect(`html_element.c declares reflections for ${iface} and no element-interface row wears it, so ` +
             `those members are installed on a prototype no element can have`);
  console.log(`[elemgen] ${declared.length} reflection sets, all borne by a row`);
}

/* ---------------------------------------------------------------------------------------------------------
 * The emit. Same contract as engine/idlgen.mjs's: the audit run NEVER writes, because a build that rewrites a
 * committed source is a gate measuring a tree that no longer exists.
 */
if (REGEN) {
  const { n, text } = generate();
  let prev = "";
  try { prev = readFileSync(OUT, "utf8"); } catch { /* first run */ }
  if (prev === text) console.log(`[elemgen] element_interfaces.h current — ${n} rows`);
  else { writeFileSync(OUT, text); console.log(`[elemgen] element_interfaces.h REGENERATED — ${n} rows`); }
}

if (defects) {
  console.log(`[elemgen] ${defects} defect${defects === 1 ? "" : "s"} — the element-interface table and the ` +
              `sources that can contradict it disagree`);
  process.exit(1);
}
console.log("[elemgen] clean");
