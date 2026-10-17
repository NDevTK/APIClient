/* The interface names standards have removed, read from each standard's own removed-interface text, minus
 * every name a live vocabulary holds — the fourth table solver/absent.c answers a global miss from.
 *
 * A removed name is neither a component this engine owes nor app state: a browser answers
 * `typeof MutationEvent` with "undefined", so a feature detect on it takes the false arm and is not a fork.
 * A standard that removed an interface another standard still defines (CSSOM View's DOMRectList) has not
 * removed it from the platform, hence the subtraction. Each source is read by its own rule, at its block.
 *
 * Run: node engine/obsoletegen.mjs   (writes engine/host/browser/obsolete_names.h) */
import { readFileSync, writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const BROWSER = join(ENGINE, "host", "browser");
const OUT = join(BROWSER, "obsolete_names.h");

/* curl rather than node's fetch, for citegen's reason: curl is what carries this environment's proxy. */
const curl = (url) => execFileSync("curl", ["-sSL", "--fail", "--max-time", "180", url],
                                   { encoding: "utf8", maxBuffer: 96 * 1024 * 1024 });
const lastModified = (url) => {
  const h = execFileSync("curl", ["-sSIL", "--fail", "--max-time", "60", url], { encoding: "utf8" });
  const m = [...h.matchAll(/^last-modified:\s*(.+?)\s*$/gim)].pop();
  if (!m) throw new Error(`${url} answers no Last-Modified, and it carries no edition stamp of its own`);
  return m[1];
};
const text = (html) => html.replace(/<[^>]*>/g, " ").replace(/&[a-z]+;|&#[0-9]+;/g, " ").replace(/\s+/g, " ").trim();
const IDENT = /^[A-Z][A-Za-z0-9]*$/;
const dtUpdated = (url, html) => {
  const m = /<time class="dt-updated"[^>]*>([^<]+)/.exec(html);
  if (!m) throw new Error(`${url} carries no <time class="dt-updated"> — its edition cannot be stamped`);
  return m[1].trim();
};
/* The section a heading id opens, up to the next heading of the same level, with its title asserted. */
const section = (url, html, id, title) => {
  const re = new RegExp(`<h2[^>]*id="${id}"[^>]*>([\\s\\S]*?)</h2>`);
  const m = re.exec(html);
  if (!m) throw new Error(`${url} has no <h2 id="${id}">`);
  const got = text(m[1]).replace(/^[0-9.]+\s*/, "");
  if (got !== title) throw new Error(`${url}#${id} is titled "${got}", not "${title}" — the document has changed shape`);
  const end = html.indexOf("<h2", m.index + m[0].length);
  return html.slice(m.index + m[0].length, end < 0 ? html.length : end);
};

const found = new Map();   /* name -> source key */
const add = (name, key) => {
  if (!IDENT.test(name)) throw new Error(`${key} yielded "${name}", which is not an interface name`);
  if (!found.has(name)) found.set(name, key);
};
const sources = [];

/* DOM §11 Historical: the list after "These interfaces have been removed:". */
{
  const url = "https://dom.spec.whatwg.org/";
  const html = curl(url);
  const sec = section(url, html, "historical", "Historical");
  const at = sec.indexOf("These interfaces have been removed:");
  if (at < 0) throw new Error(`${url}#historical no longer says "These interfaces have been removed:"`);
  const ul = /<ul[^>]*>([\s\S]*?)<\/ul>/.exec(sec.slice(at));
  if (!ul) throw new Error(`${url}#historical has no list after its removed-interfaces sentence`);
  const names = [...ul[1].matchAll(/<code>([^<]+)<\/code>/g)].map((m) => m[1]);
  if (!names.length) throw new Error(`${url}#historical's removed-interfaces list is empty`);
  for (const n of names) add(n, "dom");
  sources.push(`DOM §11 Historical, ${url}, ${dtUpdated(url, html)}: ${names.length}`);
}

/* SVG 2 Appendix K Changes from SVG 1.1: the SVG-prefixed words before "interface(s)" in each "Removed the"
   item, and the "'X' interface is removed." item. A linked name is a live interface the item mentions.
   `SVGPathSeg*` is expanded against SVG 1.1's IDL appendix, which defined those interfaces. */
{
  const url = "https://svgwg.org/svg2-draft/changes.html";
  const idlUrl = "https://www.w3.org/TR/SVG11/idl.html";
  const html = curl(url);
  if (!/<title>Changes from SVG 1\.1/.test(html)) throw new Error(`${url} is no longer Changes from SVG 1.1`);
  let idl = null, n0 = found.size;
  for (const m of html.matchAll(/<li>([\s\S]*?)<\/li>/g)) {
    const live = m[1].replace(/<a class="idlinterface"[\s\S]*?<\/a>/g, " ");
    const t = text(live);
    let head = null;
    const removed = /^Removed the (.*?)\binterfaces?\b/.exec(t);
    if (removed) head = removed[1];
    const single = /^'(SVG[A-Za-z0-9]+)' interface is removed\.$/.exec(t);
    if (single) head = single[1];
    if (head === null) continue;
    for (const [tok] of head.matchAll(/\bSVG[A-Za-z0-9]*\*?/g)) {
      if (!tok.endsWith("*")) { add(tok, "svg2"); continue; }
      idl ??= text(curl(idlUrl));
      const prefix = tok.slice(0, -1);
      const hits = [...idl.matchAll(new RegExp(`\\binterface (${prefix}[A-Za-z0-9]*)`, "g"))].map((x) => x[1]);
      if (!hits.length) throw new Error(`${idlUrl} defines no interface matching ${tok}`);
      for (const h of hits) add(h, "svg11-idl");
    }
  }
  if (found.size === n0) throw new Error(`${url} yielded no removed interface — its item shape has changed`);
  sources.push(`SVG 2 Appendix K Changes from SVG 1.1, ${url}, Last-Modified ${lastModified(url)}: ` +
               `${found.size - n0} (SVGPathSeg* expanded against ${idlUrl})`);
}

/* CSSOM §11 Change History and CSSOM View §17 Changes: in each item saying "removed", the unlinked <code>
   words that start with a capital and carry no "()" (a member is lowercase or parenthesised; a linked
   <code> is a live definition). */
for (const [key, url, id, title, label] of [
  ["cssom", "https://drafts.csswg.org/cssom/", "change-history", "Change History", "CSSOM §11 Change History"],
  ["cssomview", "https://drafts.csswg.org/cssom-view/", "changes", "Changes", "CSSOM View §17 Changes"],
]) {
  const html = curl(url);
  const sec = section(url, html, id, title);
  let n = 0;
  for (const item of sec.split(/<li\b/)) {
    if (!/\bremoved\b/.test(text(item))) continue;
    for (const m of item.matchAll(/<code>([A-Z][A-Za-z0-9]*)<\/code>/g)) { add(m[1], key); n++; }
  }
  if (!n) throw new Error(`${url}#${id} yielded no removed interface — its item shape has changed`);
  sources.push(`${label}, ${url}, ${dtUpdated(url, html)}: ${n}`);
}

/* Minus every live vocabulary: the three generated tables absent.c already reads. */
const live = new Set();
for (const f of ["platform_names.h", "language_names.h", "i18n_names.h"])
  for (const m of readFileSync(join(BROWSER, f), "utf8").matchAll(/^\s*"([^"]+)",/gm)) live.add(m[1]);
if (live.size < 1000) throw new Error(`the live vocabularies hold ${live.size} names — a table did not parse`);
const dropped = [...found.keys()].filter((n) => live.has(n)).sort();
const names = [...found.keys()].filter((n) => !live.has(n)).sort();

const body = names.map((n) => `    ${JSON.stringify(n)},   /* ${found.get(n)} */`).join("\n");
writeFileSync(OUT,
`/* GENERATED by engine/obsoletegen.mjs — DO NOT EDIT.
 * Every interface name a standard's own removed-interface text names, minus every name a live vocabulary
 * (browser/platform_names.h, language_names.h, i18n_names.h) holds. solver/absent.c answers a global read of
 * one of these as a browser does, absent, and neither as a component this engine owes nor as app state.
${sources.map((s) => ` * ${s}`).join("\n")}
 * Still defined by a live standard, so not here: ${dropped.join(", ") || "none"} */
#ifndef APICLIENT_OBSOLETE_NAMES_H
#define APICLIENT_OBSOLETE_NAMES_H

static const char *const OBSOLETE_NAMES[] = {
${body}
};

#endif
`);
process.stdout.write(`obsolete_names.h: ${names.length} name(s); dropped as live: ${dropped.join(", ") || "none"}\n`);
