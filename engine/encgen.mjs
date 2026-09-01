/* THE ENCODING STANDARD'S TABLES, generated from the standard's own machine-readable data.
 *
 * WHY GENERATED AND COMMITTED, the same reasons idnagen.mjs is. Two of the Encoding Standard's normative
 * artifacts are DATA rather than prose: §4.2's label table — 228 labels naming 40 encodings, including the ones
 * a page actually writes (`utf8`, `latin1`, `ansi_x3.4-1968`) — and §9.1's single-byte indexes, one 128-entry
 * map per legacy encoding. Hand-transcribing either is a guarantee of being subtly wrong somewhere nobody
 * checks, and "we support the labels we thought of" is not what a browser does with `<meta charset>`.
 *
 * The header is COMMITTED so the build needs no network. Re-run this deliberately when re-pinning, the same as
 * re-pinning lexbor, test262 or the wpt corpus.
 *
 * PINNED TWO WAYS. encodings.json carries no hash, so its SHAPE is asserted — the encoding count and the label
 * count, which is what would change if the registry did, and the generator stops rather than silently re-pinning
 * a table under a build checked against the old one. Each index file DOES carry the standard's own content
 * identifier, and each is EMITTED into the header beside its table: a moved index is then a visible diff in a
 * committed file rather than a decode that quietly changed.
 *
 * THE MULTI-BYTE CJK INDEXES are here too — gb18030 and its ranges, jis0208, jis0212, Big5 and EUC-KR — each
 * beside the decoder that reads it, because a table with no algorithm is a table nothing can be wrong about.
 * They are emitted in the narrowest type that holds them, which for Big5 is 32 bits: its index reaches past
 * U+FFFF, and the generator FAILS rather than truncating if any other one starts to.
 *
 * Usage:  node engine/encgen.mjs
 */
import { writeFileSync } from "node:fs";
import { join } from "node:path";

const ENGINE = import.meta.dirname;
const OUT = join(ENGINE, "host", "browser", "core", "encoding", "encoding_table.h");
const BASE = "https://encoding.spec.whatwg.org/";

/* The registry's shape, asserted rather than trusted — see the file comment. */
const N_ENCODINGS = 40;
const N_LABELS = 228;

async function get(path) {
  const res = await fetch(BASE + path);
  if (!res.ok) { console.error(`[encgen] ${BASE}${path}: HTTP ${res.status}`); process.exit(1); }
  return res.text();
}

const registry = JSON.parse(await get("encodings.json"));
/* THE REGISTRY'S OWN ENTRIES, FLATTENED — not copies of them. This used to build `{ ...e, group: g.heading }`,
   a wrapper whose one added field was written here and read NOWHERE, in this file or anywhere else: the
   write-with-no-reader half of the contract fieldgate.mjs audits, in a record that is built and consumed
   entirely in JavaScript and so declares no name it can see (its per-area note says a zero in those columns is
   silent about exactly this boundary, not clean about it). The group each encoding belongs to is still read
   where it is actually used — `singleByte` below asks the registry for the group by heading. */
const encodings = registry.flatMap((g) => g.encodings);
const labels = encodings.flatMap((e) => e.labels);
if (encodings.length !== N_ENCODINGS || labels.length !== N_LABELS) {
  console.error(`[encgen] the registry now has ${encodings.length} encodings and ${labels.length} labels, and ` +
                `this generator is pinned to ${N_ENCODINGS}/${N_LABELS} — re-pin deliberately, as a commit of ` +
                `its own, so the diff in results has one cause.`);
  process.exit(1);
}

/* The single-byte group, read from the registry rather than listed here: the standard decides which encodings
   are single-byte, and a second list would be a second place for that to be true. */
const singleByte = registry.find((g) => g.heading === "Legacy single-byte encodings").encodings;

/* Parse an index file: `pointer \t code point \t # comment`, and every single-byte index has 128 entries. A
   pointer with no code point is absent, which the standard writes as a missing ROW rather than as a value —
   so the table is filled with 0 (U+0000 is never an index entry) and the decoder reads that as an error. */
function parseIndex(text) {
  const out = new Uint16Array(128);
  for (const line of text.split("\n")) {
    const m = /^\s*(\d+)\s+(0x[0-9A-Fa-f]+)/.exec(line);
    if (!m) continue;
    const p = Number(m[1]);
    if (p >= 128) { console.error(`[encgen] a single-byte index has pointer ${p}`); process.exit(1); }
    out[p] = Number(m[2]);
  }
  return out;
}

const indexes = [];
for (const e of singleByte) {
  const file = `index-${e.name.toLowerCase()}.txt`;
  /* ISO-8859-8-I shares ISO-8859-8's index — the standard gives it no file of its own, because the difference
     between the two is bidi handling and not the byte mapping. */
  const src = await get(e.name === "ISO-8859-8-I" ? "index-iso-8859-8.txt" : file);
  const id = /^#\s*Identifier:\s*(\S+)/m.exec(src);
  indexes.push({ name: e.name, table: parseIndex(src), id: id ? id[1] : "?" });
}

/* §11's INDEX gb18030 — 23940 pointers, one code point each, and unlike the single-byte indexes it is DENSE
   enough that a missing row is a real absence rather than a formatting accident. U+0000 is never an entry, so 0
   reads as absent exactly as it does above. */
function parseWideIndex(text, expect) {
  const rows = [];
  for (const line of text.split("\n")) {
    const m = /^\s*(\d+)\s+(0x[0-9A-Fa-f]+)/.exec(line);
    if (!m) continue;
    rows.push([Number(m[1]), Number(m[2])]);
  }
  const max = Math.max(...rows.map((r) => r[0]));
  const out = new Uint16Array(max + 1);
  for (const [ptr, cp] of rows) {
    if (cp > 0xFFFF) {
      console.error(`[encgen] index gb18030 pointer ${ptr} is U+${cp.toString(16)}, past what a uint16 holds`);
      process.exit(1);
    }
    out[ptr] = cp;
  }
  if (out.length !== expect) {
    console.error(`[encgen] index gb18030 now has ${out.length} pointers and this generator is pinned to ` +
                  `${expect} — re-pin deliberately, as a commit of its own.`);
    process.exit(1);
  }
  return out;
}

/* §5's "index gb18030 ranges code point" walks these in order, so they are emitted in the standard's order and
   the decoder does not sort them. The pairs are (pointer, code point) and the code points DO pass U+FFFF —
   the whole point of the ranges is the astral planes — so this one is uint32. */
function parseRanges(text) {
  const out = [];
  for (const line of text.split("\n")) {
    const m = /^\s*(\d+)\s+(0x[0-9A-Fa-f]+)/.exec(line);
    if (m) out.push([Number(m[1]), Number(m[2])]);
  }
  return out;
}

/* The multi-byte indexes, each a dense pointer -> code point map. 0 is absent, as everywhere here. */
function parseDense(text) {
  const rows = [];
  for (const line of text.split("\n")) {
    const m = /^\s*(\d+)\s+(0x[0-9A-Fa-f]+)/.exec(line);
    if (m) rows.push([Number(m[1]), Number(m[2])]);
  }
  const out = new Uint32Array(Math.max(...rows.map((r) => r[0])) + 1);
  for (const [ptr, cp] of rows) out[ptr] = cp;
  return out;
}

const wide = [];
for (const [name, file, expect, bits] of [
  ["JIS0208", "index-jis0208.txt", 11104, 16],
  ["JIS0212", "index-jis0212.txt", 7211, 16],
  ["BIG5",    "index-big5.txt",    19782, 32],
  ["EUC_KR",  "index-euc-kr.txt",  23750, 16],
]) {
  const src = await get(file);
  const table = parseDense(src);
  const id = /^#\s*Identifier:\s*(\S+)/m.exec(src);
  const max = table.reduce((a, b) => (b > a ? b : a), 0);
  if (table.length !== expect) {
    console.error(`[encgen] ${file} now has ${table.length} pointers and this generator is pinned to ` +
                  `${expect} — re-pin deliberately, as a commit of its own.`);
    process.exit(1);
  }
  if (bits === 16 && max > 0xFFFF) {
    console.error(`[encgen] ${file} reaches U+${max.toString(16)}, past the 16 bits this emits it in`);
    process.exit(1);
  }
  wide.push({ name, table, id: id ? id[1] : "?", bits });
}

const gbSrc = await get("index-gb18030.txt");
const gbRangesSrc = await get("index-gb18030-ranges.txt");
const gbId = /^#\s*Identifier:\s*(\S+)/m.exec(gbSrc);
const gbRangesId = /^#\s*Identifier:\s*(\S+)/m.exec(gbRangesSrc);
const gb = parseWideIndex(gbSrc, 23940);
const gbRanges = parseRanges(gbRangesSrc);

/* ---- emit ---------------------------------------------------------------------------------------------- */

const enumName = (n) => "ENC_" + n.toUpperCase().replace(/[^A-Z0-9]/g, "_");

let h = `/* GENERATED BY engine/encgen.mjs — DO NOT EDIT.
 *
 * The Encoding Standard's §4.2 label table and §9.1 single-byte indexes, from the standard's own
 * machine-readable data. Re-run the generator to update; see its comment for why this is committed.
 *
 * Registry: ${encodings.length} encodings, ${labels.length} labels.
 * Single-byte indexes: ${indexes.length}.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_ENCODING_TABLE_H
#define ENGINE_HOST_BROWSER_CORE_ENCODING_TABLE_H
#include <stdint.h>

/* Every encoding the standard names, whether or not this engine decodes it yet — the LABEL table must answer
   for all of them, because §4.2's "get an encoding" is what decides between an unknown label (a RangeError the
   page sees) and a known encoding this engine has not built (a capability to add). Collapsing the two would
   report a browser-supported encoding as invalid. */
typedef enum {
`;
for (const e of encodings) h += `    ${enumName(e.name)},\n`;
h += `    ENC_COUNT\n} EncodingId;\n\n`;

h += `/* §4.2's LABEL TABLE. Labels are compared after stripping leading/trailing ASCII whitespace and\n` +
     `   ASCII-lowercasing, which is what the standard's "get an encoding" does, so every label here is already\n` +
     `   lowercase and the lookup does the stripping. */\ntypedef struct { const char *label; EncodingId id; } EncodingLabel;\n` +
     `static const EncodingLabel ENCODING_LABELS[] = {\n`;
/* Sorted, so the lookup is a binary search rather than a walk of 228 strcmps per <meta charset>. */
const rows = encodings.flatMap((e) => e.labels.map((l) => [l, enumName(e.name)])).sort((a, b) => (a[0] < b[0] ? -1 : 1));
for (const [l, id] of rows) h += `    { ${JSON.stringify(l)}, ${id} },\n`;
h += `};\n#define ENCODING_LABEL_N ((int)(sizeof ENCODING_LABELS / sizeof ENCODING_LABELS[0]))\n\n`;

h += `/* §4.2 Names and labels' NAME COLUMN, in the standard's own case — "UTF-8", "Shift_JIS", "ISO-8859-8-I".\n` +
     `   It is a SECOND column beside the labels and not a spelling of one: DOM §4.5 Interface Document's\n` +
     `   characterSet/charset/inputEncoding answer "this's encoding's name", which is this, while Encoding §7.1\n` +
     `   Interface mixin TextDecoderCommon's \`encoding\` answers "this's encoding's name, ASCII lowercased",\n` +
     `   which is the row below. */\nstatic const char *const ENCODING_NAMES[ENC_COUNT] = {\n`;
for (const e of encodings) h += `    ${JSON.stringify(e.name)},\n`;
h += `};\n\n`;

/* §7.1's ASCII-lowercased name is NOT emitted as a second array of strings, because the standard already says
   where it lives: §4.2 Names and labels — "For each encoding, ASCII-lowercasing its name yields one of its
   labels." So the lowercased name is a LABEL, and what an encoding needs is WHICH ROW of the label table its
   own is. Asserted here rather than assumed: the day the registry gains an encoding the sentence stops holding
   for, this generator stops instead of emitting an index into the wrong row. */
const nameLabelRow = encodings.map((e) => {
  const lower = e.name.toLowerCase();
  const row = rows.findIndex(([l, id]) => l === lower && id === enumName(e.name));
  if (row < 0) {
    console.error(`[encgen] §4.2 says ASCII-lowercasing an encoding's name yields one of its labels, and ` +
                  `${e.name} lowercases to ${JSON.stringify(lower)}, which is not one of its labels — the ` +
                  `standard's own invariant no longer holds, so §7.1's ASCII-lowercased name needs a column ` +
                  `of its own rather than an index into the labels.`);
    process.exit(1);
  }
  return row;
});
h += `/* WHICH LABEL each encoding's §7.1 ASCII-lowercased name IS. §4.2 Names and labels: "For each encoding,\n` +
     `   ASCII-lowercasing its name yields one of its labels" — so Encoding §7.1 Interface mixin\n` +
     `   TextDecoderCommon's \`encoding\` getter has no string of its own to store, and storing one would be a\n` +
     `   second place for a name to be true. The generator FAILS rather than emitting this if the standard's\n` +
     `   sentence ever stops holding. */\nstatic const short ENCODING_NAME_LABEL[ENC_COUNT] = {\n`;
for (let i = 0; i < encodings.length; i++)
  h += `    ${nameLabelRow[i]},  /* ${encodings[i].name} -> ${JSON.stringify(rows[nameLabelRow[i]][0])} */\n`;
h += `};\n\n`;

h += `/* §9.1's SINGLE-BYTE INDEXES: byte 0x80..0xFF maps to index[byte - 0x80], and a 0 entry means the byte\n` +
     `   maps to nothing, which is a decoder ERROR rather than U+0000.\n`;
for (const ix of indexes) h += ` * ${ix.name}: ${ix.id}\n`;
h += ` */\n`;
h += `static const uint16_t ENCODING_SINGLE_BYTE[][128] = {\n`;
for (const ix of indexes) {
  h += `    { /* ${ix.name} */\n`;
  for (let i = 0; i < 128; i += 8)
    h += "        " + Array.from(ix.table.slice(i, i + 8)).map((v) => "0x" + v.toString(16).padStart(4, "0")).join(", ") + ",\n";
  h += `    },\n`;
}
h += `};\n\n`;

h += `/* WHICH ROW of the table above an encoding uses, or -1 when it is not single-byte. */\n` +
     `static const signed char ENCODING_SINGLE_BYTE_ROW[ENC_COUNT] = {\n`;
for (const e of encodings) {
  const row = indexes.findIndex((ix) => ix.name === e.name);
  h += `    ${row}, /* ${e.name} */\n`;
}
h += `};\n\n`;

h += `/* §11's INDEX gb18030 — pointer to code point, 0 meaning the pointer maps to nothing.\n` +
     ` * ${gbId ? gbId[1] : "?"}\n */\n` +
     `static const uint16_t ENCODING_GB18030[${gb.length}] = {\n`;
for (let i = 0; i < gb.length; i += 12)
  h += "    " + Array.from(gb.slice(i, i + 12)).map((v) => "0x" + v.toString(16).padStart(4, "0")).join(", ") + ",\n";
h += `};\n#define ENCODING_GB18030_N ((int)(sizeof ENCODING_GB18030 / sizeof ENCODING_GB18030[0]))\n\n`;

h += `/* §5's INDEX gb18030 RANGES, in the standard's own order because the lookup walks them in it. The code\n` +
     ` * points reach past U+FFFF, which is what the ranges exist for.\n` +
     ` * ${gbRangesId ? gbRangesId[1] : "?"}\n */\n` +
     `typedef struct { uint32_t pointer, code_point; } EncodingRange;\n` +
     `static const EncodingRange ENCODING_GB18030_RANGES[] = {\n`;
for (const [ptr, cp] of gbRanges) h += `    { ${ptr}, 0x${cp.toString(16)} },\n`;
h += `};\n#define ENCODING_GB18030_RANGES_N ` +
     `((int)(sizeof ENCODING_GB18030_RANGES / sizeof ENCODING_GB18030_RANGES[0]))\n\n`;

for (const ix of wide) {
  const digits = ix.bits === 32 ? 8 : 4, per = ix.bits === 32 ? 8 : 12;
  h += `/* The Encoding Standard's INDEX ${ix.name.toLowerCase().replace(/_/g, "-")}: pointer to code point, 0\n` +
       ` * meaning the pointer maps to nothing.\n * ${ix.id}\n */\n` +
       `static const uint${ix.bits}_t ENCODING_${ix.name}[${ix.table.length}] = {\n`;
  for (let i = 0; i < ix.table.length; i += per)
    h += "    " + Array.from(ix.table.slice(i, i + per))
                       .map((v) => "0x" + v.toString(16).padStart(digits, "0")).join(", ") + ",\n";
  h += `};\n#define ENCODING_${ix.name}_N ((int)(sizeof ENCODING_${ix.name} / sizeof ENCODING_${ix.name}[0]))\n\n`;
}

h += `#endif\n`;

writeFileSync(OUT, h);
console.log(`[encgen] ${OUT}: ${encodings.length} encodings, ${rows.length} labels, ${indexes.length} ` +
            `single-byte indexes, gb18030 ${gb.length} pointers + ${gbRanges.length} ranges, ` +
            wide.map((w) => `${w.name} ${w.table.length}`).join(", "));
