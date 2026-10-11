/* The provisioning notices' field tables for the trusted zone, derived from the engine's own declaration.
 *
 * core/frame/navigable_notice.h declares, as X-macros, the fields of `navigable.create` and of the provision
 * record `navigable.swap` and `navigable.navigate` share. This module parses those lists and answers each field's
 * index in the record split on TAB (the operation name is index 0), `parts`, the split length of every
 * record, and `encoding`, each field's encoding as the header states it (`token` or `base64`). It also
 * renders `navigableNoticeField`, the one reader that turns a field into its value by that encoding.
 * It also carries the sandboxing flag set's vocabulary, from core/frame/sandboxing.h, which the create
 * notice's flag-set field is spelled in.
 *
 * Two consumers. A node reader (trusted.mjs, route.mjs, renderer_host_gate.mjs) imports NAVIGABLE_NOTICE and
 * navigableNoticeField from here, and importing asserts that extension/lib/navigable-notice.js is exactly what the header renders, so a
 * stale extension copy stops every node-driven run. The extension (bridge.js) loads that copy as a classic
 * script. `node engine/noticegen.mjs` writes it; `--check` exits 1 when it is stale. */
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const ENGINE = import.meta.dirname;
const HEADER = join(ENGINE, "host", "browser", "core", "frame", "navigable_notice.h");
/* The create notice's sandboxing flag set is spelled in §7.1.5's own flag names, which sandboxing.h lists. */
const SANDBOX_HEADER = join(ENGINE, "host", "browser", "core", "frame", "sandboxing.h");
const JS_OUT = join(ENGINE, "..", "extension", "lib", "navigable-notice.js");

/* The X(ID, jsName, ENCODING) entries of one `#define <name>(X)` list, in order. */
function parseList(h, name) {
  const at = h.indexOf(`#define ${name}(X)`);
  if (at < 0) throw new Error(`[noticegen] ${HEADER} declares no ${name}(X) list`);
  const lines = [];
  for (const line of h.slice(at).split("\n")) {
    lines.push(line.replace(/\/\*.*?\*\//g, ""));
    if (!line.trimEnd().endsWith("\\")) break;
  }
  const entry = /\bX\(\s*([A-Z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*,\s*(TOKEN|BASE64)\s*\)/g;
  const ids = [...lines.join("\n").matchAll(entry)];
  if (ids.length === 0) throw new Error(`[noticegen] ${name} lists no X(ID, name, ENCODING) entry`);
  /* An entry this pattern does not match would be skipped and every later index would shift, so every `X(` in
     the list must be one it matched. */
  if ((lines.join("\n").match(/\bX\(/g) || []).length !== ids.length)
    throw new Error(`[noticegen] ${name} has an X(...) entry that is not X(ID, name, TOKEN|BASE64)`);
  const table = {}, encoding = {};
  ids.forEach((m, i) => {
    if (m[2] in table || m[2] === "parts" || m[2] === "encoding")
      throw new Error(`[noticegen] ${name} names \`${m[2]}\` twice or with a reserved name`);
    table[m[2]] = 1 + i;
    encoding[m[2]] = m[3].toLowerCase();
  });
  table.parts = 1 + ids.length;
  table.encoding = encoding;
  return table;
}

/* HTML §7.1.5's flag names in sandboxing.h's order, and the serialized empty set. Every `X(` in the list must be
   an entry this pattern matched, as for the field tables; an entry's name and text may span lines. */
function sandboxVocabulary() {
  const h = readFileSync(SANDBOX_HEADER, "utf8");
  const at = h.indexOf("#define SANDBOX_FLAG_LIST(X)");
  if (at < 0) throw new Error(`[noticegen] ${SANDBOX_HEADER} declares no SANDBOX_FLAG_LIST(X)`);
  const lines = [];
  for (const line of h.slice(at).split("\n")) {
    lines.push(line);
    if (!line.trimEnd().endsWith("\\")) break;
  }
  const body = lines.join("\n").replace(/\\\n/g, " ");
  const names = [...body.matchAll(/\bX\(\s*[A-Z0-9_]+\s*,\s*"([^"]+)"\s*\)/g)].map((m) => m[1]);
  if (names.length === 0 || (body.match(/\bX\(/g) || []).length !== names.length)
    throw new Error("[noticegen] SANDBOX_FLAG_LIST has an entry that is not X(NAME, \"text\")");
  const none = /#define SANDBOX_FLAGS_SERIALIZED_NONE "([^"]+)"/.exec(h);
  if (!none) throw new Error(`[noticegen] ${SANDBOX_HEADER} defines no SANDBOX_FLAGS_SERIALIZED_NONE`);
  return { flags: names, none: none[1] };
}

function headerTable() {
  const h = readFileSync(HEADER, "utf8");
  return { create: parseList(h, "NAVIGABLE_CREATE_FIELDS"), provision: parseList(h, "NAVIGABLE_PROVISION_FIELDS"),
           request: parseList(h, "NAVIGABLE_REQUEST_FIELDS"), sandbox: sandboxVocabulary() };
}

/* A field's value: a `token` as written, a `base64` field's bytes (RFC 4648 §4, the engine's JS_Base64Encode)
   decoded as UTF-8, the encoding the notice channel carries its text in. `table` is NAVIGABLE_NOTICE.create,
   .provision or .request and `f` a record split on TAB. Its source is rendered into extension/lib/navigable-notice.js, so the
   extension and the node readers run the same reader.
   Named residual. Not covered: a CSP byte outside UTF-8 decodes to U+FFFD, where CSP's parse isomorphic-decodes
   header bytes. Next diff builds: the inherited CSP texts as `array<uint8>` parameters of Init and Join, relayed
   undecoded. Absence shows as: a report's original-policy differing from the delivered header's bytes. */
function navigableNoticeField(table, f, name) {
  const at = table[name], encoding = table.encoding[name];
  if (typeof at !== "number" || typeof encoding !== "string")
    throw new Error(`[navigable-notice] the table declares no field \`${name}\``);
  if (f.length !== table.parts || typeof f[at] !== "string")
    throw new Error(`[navigable-notice] a record of ${f.length} part(s) was read where the table has ` +
                    `${table.parts} — the engine and this reader were built from different tables`);
  if (encoding === "token") return f[at];
  if (encoding !== "base64") throw new Error(`[navigable-notice] \`${name}\` has the encoding \`${encoding}\``);
  const bin = atob(f[at]), bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new TextDecoder().decode(bytes);
}

function jsText(t) {
  const enc = (o) => "Object.freeze({\n" + Object.entries(o).map(([k, v]) => `      ${k}: "${v}",`).join("\n") +
    "\n    })";
  const obj = (o) => "Object.freeze({\n" + Object.entries(o).map(([k, v]) =>
    k === "encoding" ? `    encoding: ${enc(v)},` : `    ${k}: ${v},`).join("\n") + "\n  })";
  return "// GENERATED by engine/noticegen.mjs from engine/host/browser/core/frame/navigable_notice.h — DO NOT " +
    "EDIT.\n" +
    "// Each field's index in a provisioning notice split on TAB (index 0 is the operation name), `parts`,\n" +
    "// the split length of a complete record, and `encoding`, each field's encoding. `create` is\n" +
    "// navigable.create; `provision` is navigable.swap and navigable.navigate; `request` is\n" +
    "// navigable.request. `sandbox` is the create notice's sandboxing flag set vocabulary: HTML §7.1.5's flag\n" +
    "// names in core/frame/sandboxing.h's order, and the word for the empty set. A `var`, so a vm context's\n" +
    "// global object carries it. navigableNoticeField reads a field by its encoding.\n" +
    "var NAVIGABLE_NOTICE = Object.freeze({\n" +
    `  create: ${obj(t.create)},\n` +
    `  provision: ${obj(t.provision)},\n` +
    `  request: ${obj(t.request)},\n` +
    "  sandbox: Object.freeze({\n" +
    "    flags: Object.freeze([\n" + t.sandbox.flags.map((n) => `      ${JSON.stringify(n)},`).join("\n") +
    "\n    ]),\n" +
    `    none: ${JSON.stringify(t.sandbox.none)},\n` +
    "  }),\n" +
    "});\n" +
    `var navigableNoticeField = ${navigableNoticeField.toString()};\n`;
}

const TABLE = headerTable();
const TEXT = jsText(TABLE);

/* Whether this module is the entry point, which only the runtime knows: an argv comparison would also match a
   script that merely passes this file's path as its first argument. */
if (typeof import.meta.main !== "boolean")
  throw new Error("[noticegen] this node states no import.meta.main, so it cannot tell a run from an import");
if (import.meta.main) {
  let have = null;
  try { have = readFileSync(JS_OUT, "utf8"); } catch (e) { if (e.code !== "ENOENT") throw e; }
  if (process.argv.includes("--check")) {
    if (have !== TEXT) {
      console.error("[noticegen] extension/lib/navigable-notice.js is not what navigable_notice.h renders — " +
                    "run `node engine/noticegen.mjs`");
      process.exit(1);
    }
    console.log("[noticegen] extension/lib/navigable-notice.js matches navigable_notice.h");
    process.exit(0);
  }
  writeFileSync(JS_OUT, TEXT);
  console.log(`[noticegen] create ${TABLE.create.parts} parts, provision ${TABLE.provision.parts} parts, ` +
              `request ${TABLE.request.parts} parts -> extension/lib/navigable-notice.js`);
} else if (readFileSync(JS_OUT, "utf8") !== TEXT) {
  throw new Error("[noticegen] extension/lib/navigable-notice.js is not what navigable_notice.h renders, so the " +
                  "extension would index notices by a table the engine no longer writes — run " +
                  "`node engine/noticegen.mjs` and commit the result");
}

export const NAVIGABLE_NOTICE = TABLE;
export { navigableNoticeField };
