/* THE FIRST AVAILABLE FONT'S METRICS TABLES, TURNED INTO BYTES THE ENGINE SHIPS.
 *
 * css-fonts-4 §5.2 "Matching font styles" defines the first available font as "the first font for which the
 * character U+0020 (space) is not excluded by a unicode-range, given the font families in the font-family list
 * (or a user agent's default font if none are available)". This user agent installs no faces and loads none, so
 * that clause is the whole answer for every element of every document: ONE face, shipped in the binary.
 * core/css/font_metrics.h states why the bytes may not come from the host filesystem, the host realm's own text
 * measurement, or the network.
 *
 * WHAT THIS SCRIPT PRODUCES is core/fonts/default_font_data.c — a metrics-only sfnt, as a byte array. It is a
 * GENERATOR AND NOT A ONE-OFF: it takes the upstream release file, refuses to run on any other bytes (the
 * SHA-256 below is checked, not recorded afterwards), and emits a file whose every byte is a function of that
 * input. Re-running it on the same input reproduces the output byte for byte.
 *
 *   node engine/fontsubset.mjs <path-to-DejaVuSans.ttf>
 *
 * HOW TO OBTAIN THE INPUT — the exact command, so that "reproducible" is a thing a reader can do rather than a
 * claim they have to take:
 *
 *   curl -sSLo dejavu-fonts-ttf-2.37.zip \
 *     'https://sourceforge.net/projects/dejavu/files/dejavu/2.37/dejavu-fonts-ttf-2.37.zip/download'
 *   # sha256 7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a
 *   unzip -j dejavu-fonts-ttf-2.37.zip dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf
 *
 * WHICH FACE, AND WHY THIS ONE. DejaVu Sans 2.37. Three properties decided it and each is checkable:
 *   LICENCE. The Bitstream Vera Fonts Copyright (the release's own LICENSE file, shipped verbatim beside the
 *     generated data as core/fonts/LICENSE.dejavu) grants "the rights to use, copy, merge, publish, distribute,
 *     and/or sell copies of the Font Software" on two conditions this fork meets: the notice travels with the
 *     bytes, and a MODIFIED font must be "renamed to names not containing either the words 'Bitstream' or the
 *     word 'Vera'". The subset below drops the 'name' table outright, so it carries no font name at all; the
 *     only name attached to it is in prose, and "DejaVu Sans" contains neither reserved word.
 *   REPERTOIRE. 6253 glyphs — Latin, Greek, Cyrillic, Hebrew, Armenian, Georgian, IPA, punctuation, arrows and
 *     mathematical operators. Every codepoint a face does NOT cover is answered by .notdef (css-fonts-4 §5.2's
 *     "the missing character glyph from a default font"), and a .notdef answer is a real measurement of the
 *     wrong glyph. Breadth is therefore not a nicety: it is how many of a page's characters get measured as
 *     themselves. The other free faces to hand cover far less (Liberation Sans, 2620 glyphs).
 *   IT IS NOT MONOSPACED, and that is a REQUIREMENT and not a preference. A face whose glyphs all advance the
 *     same makes a string's width a function of its LENGTH alone, which is the input css-text-3 §5 "Line
 *     Breaking and Word Boundaries" picks a soft wrap opportunity with — so every break position and every
 *     block container's height would come out wrong while every number stayed finite and plausible. It is the
 *     exact defect core/css/font_metrics.c's crash was written to prevent, and shipping a monospaced default
 *     would reintroduce it while looking like a face.
 *     THE PRICE IS PAID AT THE 'hmtx' REPEAT RULE, and it is named here rather than discovered later. OpenType
 *     'hmtx' — Horizontal Metrics Table says "the advance width value of the last record applies to all
 *     remaining glyph IDs" and adds that this "can be useful in monospaced fonts"; DejaVu Sans has
 *     numberOfHMetrics 6238 below numGlyphs 6253, so the shipped table really is in the short form (a face
 *     with the two equal cannot even express it), but no 'cmap' entry in it reaches glyph IDs 6238..6252 —
 *     the highest mapped glyph is 5920. So the shipped face proves the parser's numberOfHMetrics <= numGlyphs
 *     validation is a real constraint, and the repeat ARITHMETIC is exercised by a synthetic face built for
 *     it. The face that would exercise it from a page is DejaVuSansMono (numberOfHMetrics 4, numGlyphs 3377),
 *     which is disqualified by the paragraph above.
 *
 * WHAT IS DROPPED AND WHY DROPPING IT IS SOUND FOR THE QUESTIONS ASKED. The engine asks a face exactly three
 * questions: what is `unitsPerEm`, which glyph does this scalar value map to, and what is that glyph's advance.
 * OpenType answers those from 'head', 'cmap', and 'hhea'+'hmtx'+'maxp', and from NOTHING ELSE — so the tables
 * kept are those five (plus 'vhea'/'vmtx' where a face has them; DejaVu Sans does not, which is why a vertical
 * advance is a NAMED unbuilt capability in core/fonts/open_type_metrics.c rather than a silent fallback to the
 * horizontal number). What goes is 'glyf'/'loca' (outlines — nothing here rasterizes), 'CFF '/'MATH'/'cvt '/
 * 'fpgm'/'prep'/'gasp' (rasterization), 'GSUB'/'GPOS'/'GDEF'/'kern' (SHAPING — a real absence and not a
 * cosmetic one: a shaping engine would need them, and until one exists the advance of a run is the sum of its
 * glyphs' advances), 'OS/2' (sTypoAscender/sTypoDescender, which CSS 2.1 §10.8.1's note recommends for `A` and
 * `D` — core/css/font_metrics.c still PICKS those two numbers, and the day it reads them off a face this list
 * grows a table), 'name'/'post'/'FFTM' (identification).
 * The kept tables are copied BYTE FOR BYTE out of the input. That is the whole justification for calling the
 * result the same face: for every question the engine asks, it is not a re-encoding of DejaVu Sans's answer, it
 * IS DejaVu Sans's bytes. The only bytes this script AUTHORS are the table directory (whose offsets necessarily
 * change) and 'head'.checksumAdjustment (a whole-file checksum, which a subset of the file invalidates) — both
 * named at their write site below.
 */
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "host", "browser", "core", "fonts", "default_font_data.c");

/* THE INPUT, PINNED BY CONTENT. A generator that accepts whatever file it is pointed at produces output nobody
   can re-derive, which is the same defect as a measurement quoted without its revision. */
const SOURCE_NAME = "DejaVu Sans 2.37 (dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf)";
const SOURCE_URL = "https://sourceforge.net/projects/dejavu/files/dejavu/2.37/dejavu-fonts-ttf-2.37.zip/download";
const SOURCE_SHA256 = "7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954";

/* THE TABLES THE ENGINE READS. 'vhea'/'vmtx' are kept WHEN PRESENT and are not required: OpenType makes them
   optional, and css-writing-modes-4 §5.1.1 "Vertical Typesetting and Font Features" says the UA "must
   synthesize vertical font metrics for fonts that lack them" while defining no heuristic for doing so — so
   their absence is a capability to build, stated at the lookup, not a hole to paper over here. */
const REQUIRED = ["cmap", "head", "hhea", "hmtx", "maxp"];
const OPTIONAL = ["vhea", "vmtx"];

const input = process.argv[2];
if (!input) {
  console.error("usage: node engine/fontsubset.mjs <path-to-DejaVuSans.ttf>");
  console.error("       see this file's header for the exact curl+unzip that obtains it");
  process.exit(1);
}
const src = readFileSync(input);
const got = createHash("sha256").update(src).digest("hex");
if (got !== SOURCE_SHA256) {
  console.error(`[fontsubset] ${input} is not ${SOURCE_NAME}`);
  console.error(`[fontsubset]   expected sha256 ${SOURCE_SHA256}`);
  console.error(`[fontsubset]   got             ${got}`);
  console.error(`[fontsubset]   obtain it from ${SOURCE_URL} — see this file's header.`);
  process.exit(1);
}

const u16 = (b, o) => b.readUInt16BE(o);
const u32 = (b, o) => b.readUInt32BE(o);

/* OpenType "The OpenType Font File" §Organization of an OpenType Font — Table Directory: `sfntVersion`,
   `numTables`, `searchRange`, `entrySelector`, `rangeShift`, then one TableRecord{`tableTag`, `checksum`,
   `offset`, `length`} per table. */
const sfntVersion = u32(src, 0);
if (sfntVersion !== 0x00010000) {
  console.error(`[fontsubset] sfntVersion ${sfntVersion.toString(16)} — this generator reads the TrueType-outline`);
  console.error("[fontsubset]   form (0x00010000) it pins above; an 'OTTO' input would need its own pin.");
  process.exit(1);
}
const numTables = u16(src, 4);
const dir = new Map();
for (let i = 0; i < numTables; i++) {
  const o = 12 + 16 * i;
  dir.set(src.subarray(o, o + 4).toString("latin1"), { offset: u32(src, o + 8), length: u32(src, o + 12) });
}

const keep = REQUIRED.concat(OPTIONAL.filter((t) => dir.has(t)));
for (const t of REQUIRED) {
  if (!dir.has(t)) {
    console.error(`[fontsubset] the input has no '${t}' table — it is not a face this engine can measure`);
    process.exit(1);
  }
}
/* "The records in the array must be sorted in ascending order by tag" — OpenType, §Organization of an OpenType
   Font. Sorting by the TAG BYTES (not by a locale collation) is what that sentence means, and the parser
   asserts the same order on the way back in, so the two statements are one. */
keep.sort();

/* THE LAYOUT. Table data is 4-byte aligned and zero padded, which is the assumption OpenType's own checksum
   routine states ("this function assumes that the length of any table is a multiple of four bytes, or that
   tables are padded with zero to four-byte aligned offsets"); the RECORDED length is the true length, "actual
   table lengths recorded in the table directory should not include padding". */
const pad4 = (n) => (n + 3) & ~3;
const headerLen = 12 + 16 * keep.length;
let cursor = headerLen;
const placed = [];
for (const tag of keep) {
  const { offset, length } = dir.get(tag);
  if (offset + length > src.length) {
    console.error(`[fontsubset] the input's '${tag}' record runs past the end of the file`);
    process.exit(1);
  }
  placed.push({ tag, at: cursor, length, bytes: src.subarray(offset, offset + length) });
  cursor += pad4(length);
}
const out = Buffer.alloc(cursor, 0);

/* searchRange / entrySelector / rangeShift are DERIVED, never copied — the spec's own recommendation, and it
   gives the reason: "incorrect values could potentially be used as an attack vector against some
   implementations. Since these values can be derived from the numTables field when the file is parsed, it is
   strongly recommended that parsing implementations not rely on [them] ... but derive them independently".
   A generator that derives them and a parser that derives them are the same statement made twice. */
const entrySelector = Math.floor(Math.log2(keep.length));
const searchRange = 2 ** entrySelector * 16;
out.writeUInt32BE(sfntVersion, 0);
out.writeUInt16BE(keep.length, 4);
out.writeUInt16BE(searchRange, 6);
out.writeUInt16BE(entrySelector, 8);
out.writeUInt16BE(keep.length * 16 - searchRange, 10);

/* OpenType §Organization of an OpenType Font — "Calculating Checksums": sum the table's bytes as big-endian
   uint32, modulo 2^32, over a length padded to a multiple of four. */
function checksum(buf, at, len) {
  let sum = 0;
  for (let i = 0; i < pad4(len); i += 4) sum = (sum + buf.readUInt32BE(at + i)) >>> 0;
  return sum;
}
for (const p of placed) p.bytes.copy(out, p.at);

/* THE ONE FIELD OF A KEPT TABLE THIS SCRIPT AUTHORS, AND THE ORDER OpenType STATES FOR WRITING IT. 'head' —
   Font Header Table's `checksumAdjustment` is defined over the WHOLE file, so the input's value is a statement
   about a file this is a subset of and is false of these bytes. Nothing in this engine reads it — a checksum
   is not a security control, since an attacker supplying a face computes it too — but leaving a field that is
   knowably wrong is how a plausible datum gets born, so it is recomputed, and the shipped blob is then a
   self-consistent sfnt an independent tool can validate.
   §Calculating Checksums gives the sequence and every step of it matters: "Set the checksumAdjustment field to
   0. Calculate the checksum for all tables including the 'head' table and enter the value for each table into
   the corresponding record in the table directory. Calculate the checksum for the entire font. Subtract that
   value from 0xB1B0AFBA. Store the result in the 'head' table checksumAdjustment field." Zeroing FIRST is what
   makes the recorded 'head' checksum the one a verifier recomputes — the same section says a verifier
   "should calculate the checksum for that table assuming that the checksumAdjustment value is zero, rather
   than the actual value in the font". Compute it before the zeroing and every tool reports the shipped face's
   'head' as corrupt. */
const headAt = placed.find((p) => p.tag === "head").at;
out.writeUInt32BE(0, headAt + 8);
placed.forEach((p, i) => {
  const o = 12 + 16 * i;
  out.write(p.tag, o, 4, "latin1");
  out.writeUInt32BE(checksum(out, p.at, p.length), o + 4);
  out.writeUInt32BE(p.at, o + 8);
  out.writeUInt32BE(p.length, o + 12);
});
let whole = 0;
for (let i = 0; i < out.length; i += 4) whole = (whole + out.readUInt32BE(i)) >>> 0;
out.writeUInt32BE((0xb1b0afba - whole) >>> 0, headAt + 8);

/* WHAT THE FACE ANSWERS, RESTATED IN THE GENERATED FILE so that the C side's asserts have something to be
   checked against by a reader who is not holding the font. These are read back out of the OUTPUT, not carried
   over from the input, so a subset that lost a table cannot describe itself as one that kept it. */
const maxpAt = placed.find((p) => p.tag === "maxp").at;
const hheaAt = placed.find((p) => p.tag === "hhea").at;
const facts = {
  unitsPerEm: u16(out, headAt + 18),
  numGlyphs: u16(out, maxpAt + 4),
  numberOfHMetrics: u16(out, hheaAt + 34),
};

const dropped = [...dir.keys()].filter((t) => !keep.includes(t)).sort();
const lines = [];
for (let i = 0; i < out.length; i += 16) {
  lines.push("    " + [...out.subarray(i, i + 16)].map((b) => "0x" + b.toString(16).padStart(2, "0")).join(", ") + ",");
}

const text = `/* GENERATED BY engine/fontsubset.mjs — DO NOT EDIT. Re-run the generator instead; that file's header
   carries the curl+unzip that obtains the input, the licence, and why each dropped table is sound to drop.

   THE FIRST AVAILABLE FONT (css-fonts-4 §5.2 "Matching font styles") of every document this engine presents,
   as a metrics-only sfnt: ${keep.map((t) => `'${t}'`).join(", ")}, copied byte for byte out of
   ${SOURCE_NAME}
     ${SOURCE_URL}
     sha256 ${SOURCE_SHA256}
   Dropped: ${dropped.map((t) => `'${t}'`).join(", ")}.

   WHAT IT ANSWERS, so a reader can check the parser without the font in front of them:
     'head' — Font Header Table    unitsPerEm       ${facts.unitsPerEm}
     'maxp' — Maximum Profile      numGlyphs        ${facts.numGlyphs}
     'hhea' — Horizontal Header    numberOfHMetrics ${facts.numberOfHMetrics}
   numberOfHMetrics is BELOW numGlyphs, so this 'hmtx' is in the short form OpenType 'hmtx' — Horizontal
   Metrics Table's "the advance width value of the last record applies to all remaining glyph IDs" is about:
   glyph IDs ${facts.numberOfHMetrics}..${facts.numGlyphs - 1} take the last record's advance. NO 'cmap' ENTRY IN THIS FACE REACHES THEM
   (the highest mapped glyph is 5920), so the repeat arithmetic is exercised by a synthetic face and not by a
   page — engine/fontsubset.mjs's header says why a face that WOULD reach it is disqualified.
   ${keep.includes("vmtx") ? "" : "There is no 'vmtx' — the face carries no vertical advances, and css-writing-modes-4 §5.1.1\n   \"Vertical Typesetting and Font Features\" makes synthesizing them a named capability rather than a\n   substitution: core/fonts/open_type_metrics.c crashes there instead of handing back the horizontal number."}

   THE LICENCE travels with the bytes, which is a condition of it: core/fonts/LICENSE.dejavu is the release's
   own LICENSE file, verbatim. Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is a
   trademark of Bitstream, Inc. DejaVu changes are in public domain. Glyphs imported from Arev fonts are
   (c) Tavmjong Bah. The 'name' table is one of the dropped ones, so these bytes carry no font name at all. */
#include "core/fonts/default_font_data.h"

const unsigned char DEFAULT_FONT_SFNT[${out.length}] = {
${lines.join("\n")}
};

const unsigned int DEFAULT_FONT_SFNT_LEN = ${out.length};
`;

mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, text);
console.log(`[fontsubset] ${OUT}`);
console.log(`[fontsubset]   ${out.length} bytes, tables ${keep.join(" ")}, dropped ${dropped.join(" ")}`);
console.log(`[fontsubset]   unitsPerEm ${facts.unitsPerEm} numGlyphs ${facts.numGlyphs} numberOfHMetrics ${facts.numberOfHMetrics}`);
