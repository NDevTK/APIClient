/* The Public Suffix List, generated from Chromium's `net/base/registry_controlled_domains/effective_tld_names.dat`
 * at a pinned Chromium release into core/url/public_suffix_table.h and engine/public_suffix_table.mjs. Real
 * Chrome is the oracle for the sites and cookies a page sees, so the engine carries the list Chrome carries,
 * matched as Chrome matches it (core/url/public_suffix.c); testing/psl_agreement.mjs measures the two against
 * the running browser. HTML §7.1.1.2's `document.domain` setter and the cookie jar ask it, and a hand-written
 * TLD table gets exactly their cases wrong (`compute.amazonaws.com`, `github.io`, `*.ck`).
 *
 * The list changes several times a week and a build has no network, so it is pinned by a Chromium release tag
 * and the file's SHA-256, generated into committed files, and re-pinned deliberately as a commit of its own,
 * as idnagen.mjs pins Unicode and wpt.mjs pins WPT_REV. Rules are canonicalized to A-labels at generation
 * (engine/psl_list.mjs), since every host reaching the lookup has been through URL §3.3's domain-to-ASCII.
 *
 * Usage:  node engine/pslgen.mjs
 */
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { parseList } from "./psl_list.mjs";

const ENGINE = import.meta.dirname;
const OUT = join(ENGINE, "host", "browser", "core", "url", "public_suffix_table.h");
/* The native host's copy, for engine/psl.mjs: the same three sorted arrays, written by the same run. A process
   host has no browser to ask, so engine/trusted.mjs answers its zone's site questions from the engine's own
   list; the extension's zone asks the browser (extension/lib/site.js). */
const JS_OUT = join(ENGINE, "public_suffix_table.mjs");

/* THE PIN: Chromium's copy of the list at the first release that exposes chrome.publicSuffix (153, the
   extension's minimum_chrome_version), read from Chromium's own repository at that tag, and the SHA-256 of
   its bytes. Chromium's copy carries no VERSION or COMMIT line, so the hash is what this generator asserts;
   bytes that differ STOP it. */
const CHROMIUM_TAG = "153.0.8010.52";
const LIST_PATH = "net/base/registry_controlled_domains/effective_tld_names.dat";
const URL_SRC = `https://chromium.googlesource.com/chromium/src/+/refs/tags/${CHROMIUM_TAG}/${LIST_PATH}`;
const SHA256 = "4155611645690529d1bbea0cfe653b0c45f63b028e0ba039de29a97edc3204ca";

/* `--js-from-header` writes the native host's table from the committed header without re-pinning: it parses the three
   arrays back, and refuses unless re-rendering them reproduces the header byte for byte. */
if (process.argv.includes("--js-from-header")) {
  const h = readFileSync(OUT, "utf8");
  const v = /#define PSL_VERSION "([^"]+)"/.exec(h), c = /#define PSL_SHA256  "([^"]+)"/.exec(h);
  const n = /\* (\d+) rules in §Divisions' ICANN section and (\d+) in its PRIVATE/.exec(h);
  const arr = (name) => JSON.parse("[" + new RegExp("static const char \\*const " + name +
    "\\[\\] = \\{([^}]*)\\};").exec(h)[1].trim().replace(/,$/, "") + "]");
  const [N, W, E] = [arr("PSL_NORMAL"), arr("PSL_WILDCARD"), arr("PSL_EXCEPTION")];
  if (headerText(N, W, E, v[1], c[1], { ICANN: +n[1], PRIVATE: +n[2] }) !== h) {
    console.error("[pslgen] the committed header does not re-render from the arrays parsed out of it — the " +
                  "parse missed something, so a table written from it would not be the engine's list");
    process.exit(1);
  }
  writeFileSync(JS_OUT, jsText(N, W, E, v[1], c[1]));
  console.log(`[pslgen] ${N.length} ordinary + ${W.length} wildcard + ${E.length} exception rules from ` +
              OUT.replace(ENGINE + "/", "engine/") + " -> engine/public_suffix_table.mjs");
  process.exit(0);
}

/* Gitiles serves a file's bytes base64-encoded under `?format=TEXT`. */
const res = await fetch(URL_SRC + "?format=TEXT");
if (!res.ok) { console.error(`[pslgen] ${URL_SRC}: HTTP ${res.status}`); process.exit(1); }
const bytes = Buffer.from(await res.text(), "base64");
const sha = createHash("sha256").update(bytes).digest("hex");
if (sha !== SHA256) {
  console.error(`[pslgen] ${LIST_PATH} at ${CHROMIUM_TAG} hashes to ${sha} and this generator is pinned to ` +
                `${SHA256} — re-pin deliberately, as a commit of its own, so the diff in results has one cause.`);
  process.exit(1);
}
const text = bytes.toString("utf8");
const version = [null, "Chromium " + CHROMIUM_TAG];
const commit = [null, SHA256];

let N, W, E, SECTIONS;
try { ({ N, W, E, sections: SECTIONS } = parseList(text)); }
catch (e) { console.error("[pslgen] " + e.message); process.exit(1); }

writeFileSync(OUT, headerText(N, W, E, version[1], commit[1], SECTIONS));
writeFileSync(JS_OUT, jsText(N, W, E, version[1], commit[1]));
console.log(`[pslgen] ${version[1]} ${LIST_PATH} (sha256 ${commit[1]}): ${N.length} ordinary + ${W.length} wildcard + ` +
            `${E.length} exception rules -> ` + OUT.replace(ENGINE + "/", "engine/") +
            " and engine/public_suffix_table.mjs");

function emitC(name, rules) {
  const out = [`static const char *const ${name}[] = {`];
  for (let i = 0; i < rules.length; i += 4)
    out.push("    " + rules.slice(i, i + 4).map((r) => JSON.stringify(r) + ",").join(" "));
  out.push("};");
  return out;
}

function headerText(N, W, E, version, commit, sections) {
  const lines = [];
  lines.push("/* GENERATED by engine/pslgen.mjs — DO NOT EDIT. The Public Suffix List as " + version + " carries it,");
  lines.push(" * sha256 " + commit + ", from " + URL_SRC + " —");
  lines.push(` * ${sections.ICANN} rules in §Divisions' ICANN section and ${sections.PRIVATE} in its PRIVATE` +
             " section, unified here");
  lines.push(" * because URL §3.2 runs the algorithm over the list and HTML §7.1.1.2's own table turns on a");
  lines.push(" * PRIVATE entry (`*.compute.amazonaws.com`).");
  lines.push(" *");
  lines.push(" * §Format's three rule shapes, each as its own SORTED array of the rule's matchable BODY — the");
  lines.push(" * leading `!` of an exception rule and the leading `*.` of a wildcard rule are the rule's KIND and");
  lines.push(" * are carried by which array it is in, so a lookup never has to re-parse a rule at run time. The");
  lines.push(" * bodies are A-labels: the PSL's formal algorithm requires both sides of a comparison to be");
  lines.push(" * lower-case Punycode, and the host side arrives that way from URL §3.3 IDNA's domain parser.");
  lines.push(" *");
  lines.push(" * Re-pin DELIBERATELY, as a commit of its own — see engine/pslgen.mjs. */");
  lines.push("#ifndef ENGINE_HOST_BROWSER_CORE_URL_PUBLIC_SUFFIX_TABLE_H");
  lines.push("#define ENGINE_HOST_BROWSER_CORE_URL_PUBLIC_SUFFIX_TABLE_H");
  lines.push("");
  lines.push(`#define PSL_VERSION "${version}"`);
  lines.push(`#define PSL_SHA256  "${commit}"`);
  lines.push("");
  lines.push("/* An ordinary rule: it matches a domain whose right-most labels are exactly these. */");
  lines.push(...emitC("PSL_NORMAL", N));
  lines.push("");
  lines.push("/* A wildcard rule `*.BODY`, stored as Chromium stores it, as the entry BODY with a wildcard tag: a");
  lines.push("   host that is BODY is itself a public suffix, and a longer one's public suffix is BODY and the");
  lines.push("   label before it (core/url/public_suffix.c). */");
  lines.push(...emitC("PSL_WILDCARD", W));
  lines.push("");
  lines.push("/* An exception rule `!BODY`, stored as the entry BODY with an exception tag: when it is the longest");
  lines.push("   entry a host ends in, the public suffix is BODY without its leftmost label. */");
  lines.push(...emitC("PSL_EXCEPTION", E));
  lines.push("");
  lines.push("#endif");
  return lines.join("\n") + "\n";
}

function jsText(N, W, E, version, commit) {
  const arr = (name, rules) => {
    const out = [`export const ${name} = [`];
    for (let i = 0; i < rules.length; i += 6) out.push("  " + rules.slice(i, i + 6).map((r) => JSON.stringify(r) + ",").join(" "));
    out.push("];");
    return out;
  };
  return [
    `// GENERATED by engine/pslgen.mjs — DO NOT EDIT. The Public Suffix List as ${version} carries it, sha256 ${commit}.`,
    "// The same three sorted arrays engine/host/browser/core/url/public_suffix_table.h carries (see its",
    "// header for what each kind of rule matches); engine/psl.mjs answers registrable domains over them.",
    `export const PSL_VERSION = ${JSON.stringify(version)};`,
    `export const PSL_SHA256 = ${JSON.stringify(commit)};`,
    ...arr("PSL_NORMAL", N), ...arr("PSL_WILDCARD", W), ...arr("PSL_EXCEPTION", E), "",
  ].join("\n");
}
