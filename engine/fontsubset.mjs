/* THE INSTALLED FACES' TABLES, TURNED INTO BYTES THE ENGINE SHIPS.
 *
 * css-fonts-4 §5 "Font Matching Algorithm" leaves "the set of installed fonts available to the Font Matching
 * Algorithm" to the user agent. This one's set is the four faces in FACES below, shipped in the binary, and
 * nothing else; which family and generic each answers is core/fonts/font_matching.c's table, and
 * core/css/font_metrics.h states why the bytes may not come from the host filesystem, the host realm's own text
 * measurement, or the network.
 *
 * WHAT THIS SCRIPT PRODUCES is one core/fonts/*_data.c per face — an sfnt carrying exactly the tables REQUIRED
 * names, as a byte array. WHICH SHAPE THAT IS, THE EMITTED HEADER DERIVES AND THIS SENTENCE DOES NOT: the
 * phrase here read `a metrics-only sfnt` while REQUIRED held five tables, and one edit to that list made it
 * false in a file nobody re-reads, which is why the emitted line now asks `keep` instead of restating it.
 * It is a
 * GENERATOR AND NOT A ONE-OFF: it takes the upstream release file, refuses to run on any other bytes (the
 * SHA-256 below is checked, not recorded afterwards), and emits a file whose every byte is a function of that
 * input. Re-running it on the same input reproduces the output byte for byte.
 *
 *   node engine/fontsubset.mjs <face> <path-to-ttf>      (face: one of FACES' keys below)
 *
 * HOW TO OBTAIN THE INPUTS — the exact commands, so that "reproducible" is a thing a reader can do rather than a
 * claim they have to take. Each archive's SHA-256 is stated, and each face's own SHA-256 is checked by this script:
 *
 *   curl -sSLo dejavu-fonts-ttf-2.37.zip \
 *     https://sourceforge.net/projects/dejavu/files/dejavu/2.37/dejavu-fonts-ttf-2.37.zip/download
 *   # sha256 7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a
 *   unzip -j dejavu-fonts-ttf-2.37.zip dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf
 *
 *   curl -sSLo fonts-liberation_2.1.5-3_all.deb \
 *     http://archive.ubuntu.com/ubuntu/pool/main/f/fonts-liberation/fonts-liberation_2.1.5-3_all.deb
 *   # sha256 065c2ab1abc9108b17d401016dc594b79750904390f095845c93bb06e1153acc
 *   dpkg-deb -x fonts-liberation_2.1.5-3_all.deb fonts-liberation
 *   # fonts-liberation/usr/share/fonts/truetype/liberation/Liberation{Serif,Sans}-Regular.ttf
 *   # fonts-liberation/usr/share/doc/fonts-liberation/copyright is core/fonts/LICENSE.liberation, verbatim
 *
 *   curl -sSLo fonts-dejavu-mono_2.37-8_all.deb \
 *     http://archive.ubuntu.com/ubuntu/pool/main/f/fonts-dejavu/fonts-dejavu-mono_2.37-8_all.deb
 *   # sha256 8a599d6553307db7ecb795d2f0e5a301e03234afc75c7358b0ba43466454c89a
 *   dpkg-deb -x fonts-dejavu-mono_2.37-8_all.deb fonts-dejavu-mono
 *   # fonts-dejavu-mono/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
 *
 * The three family faces come from Ubuntu 24.04's packages rather than the upstream archives because those are
 * the bytes the reference browser on this project's hosts renders the generic families with, so a comparison
 * against it is face for face. DejaVu Sans Mono's licence is DejaVu Sans's, so core/fonts/LICENSE.dejavu
 * covers both.
 *
 * WHICH FALLBACK FACE, AND WHY THIS ONE. DejaVu Sans 2.37. Three properties decided it and each is checkable:
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
 *     themselves. The other free faces to hand cover far less (Liberation Sans, 2620 glyphs), which is why it is
 *     the per-character fallback behind them rather than a family of its own.
 *   IT IS NOT MONOSPACED, because it is the face a codepoint no family covers falls to, and a monospaced
 *     fallback would give every such character one advance whatever its shape.
 * OpenType 'hmtx' — Horizontal Metrics Table's repeat rule ("the advance width value of the last record
 * applies to all remaining glyph IDs") is reached from a page through DejaVu Sans Mono, whose short-form table
 * gives most mapped glyphs the last record's advance; each generated file states its own face's case.
 *
 * WHAT IS DROPPED AND WHY DROPPING IT IS SOUND. The engine asks a face for `unitsPerEm`, a scalar value's glyph,
 * its advance, its SHAPE and a pair's kerning, which OpenType answers from 'head', 'cmap', 'hhea'+'hmtx'+'maxp',
 * 'glyf'+'loca' and 'GPOS' with 'GDEF''s classes (core/fonts/pair_kerning.h): those, plus 'vhea'/'vmtx' where
 * a face has them (none does, so a vertical advance is a NAMED unbuilt capability in open_type_metrics.c).
 *
 * A TABLE IS KEPT WHEN THE ENGINE ASKS A QUESTION ONLY THAT TABLE ANSWERS, and the list grows when the engine
 * learns to ask, never when a table looks useful.
 *
 * SO WHAT STILL GOES IS 'CFF '/'MATH'/'cvt '/'fpgm'/'prep'/'gasp' — the first because outlines that are not
 * TrueType are a form the sfntVersion pin above refuses outright, and the rest because they are HINTING:
 * instructions that grid-fit a rasterized result at a particular size, which this engine does not do and
 * which core/fonts/glyph_outline.c reads past by name. And 'GSUB' (substitution, a real absence: a run's
 * glyphs are its characters' own), 'kern' (HarfBuzz reads it only for a face with no 'GPOS'), 'OS/2'
 * (sTypoAscender/sTypoDescender, which CSS 2.1 §10.8.1's note recommends for `A` and `D` —
 * core/css/font_metrics.c still PICKS those two numbers, and the day it reads them off a face this list grows
 * a table), 'name'/'post'/'FFTM' (identification).
 *
 * WHAT THIS EDIT COSTS IS NOT STATED HERE, IT IS DERIVED. A size written into a comment is a number nobody
 * can re-run; the generator PRINTS the kept tables and their lengths on every run and the generated file's own
 * header carries them, so the cost of this decision is read off the artifact rather than taken on trust.
 * WHAT IT MAY NOT DO IS SHRINK THE GLYPH SET TO PAY FOR ITSELF. Every kept table here is copied BYTE FOR BYTE,
 * which is the whole justification for calling the result the same face; dropping glyphs would mean
 * renumbering glyph IDs and REWRITING 'cmap', 'hmtx', 'loca' and 'glyf', at which point the output is this
 * script's encoding of a face rather than the face, and every measurement the engine takes off it is this
 * script's arithmetic. If the size is unaffordable the answer is a different FACE or a different delivery,
 * never a mutilated one.
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

/* THE INPUTS, PINNED BY CONTENT. A generator that accepts whatever file it is pointed at produces output nobody
   can re-derive, which is the same defect as a measurement quoted without its revision. Each face names its
   output, the symbol core/fonts/installed_font_data.h declares its bytes under, and its licence. */
const LIBERATION_DEB = "http://archive.ubuntu.com/ubuntu/pool/main/f/fonts-liberation/fonts-liberation_2.1.5-3_all.deb";
const DEJAVU_MONO_DEB = "http://archive.ubuntu.com/ubuntu/pool/main/f/fonts-dejavu/fonts-dejavu-mono_2.37-8_all.deb";
const DEJAVU_LICENCE =
`Licence: core/fonts/LICENSE.dejavu, verbatim. Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved.
   Bitstream Vera is a trademark of Bitstream, Inc. DejaVu changes are in public domain. Glyphs imported from
   Arev fonts are (c) Tavmjong Bah. The 'name' table is dropped, so these bytes carry no font name.`;
const LIBERATION_LICENCE =
`Licence: core/fonts/LICENSE.liberation, the package's copyright file verbatim — SIL Open Font License 1.1.
   Copyright (c) 2010 Google Corporation, (c) 2012 Red Hat, Inc., Reserved Font Name Liberation. A subset is a
   Modified Version, which may not use a Reserved Font Name: the 'name' table is dropped, so these bytes carry none.`;
const FACES = {
  "dejavu-sans": {
    name: "DejaVu Sans 2.37 (dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf)",
    url: "https://sourceforge.net/projects/dejavu/files/dejavu/2.37/dejavu-fonts-ttf-2.37.zip/download",
    sha256: "7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954",
    out: "dejavu_sans_data.c", symbol: "DEJAVU_SANS_SFNT",
    licence: DEJAVU_LICENCE,
  },
  "dejavu-sans-mono": {
    name: "DejaVu Sans Mono 2.37 (Ubuntu fonts-dejavu-mono 2.37-8, DejaVuSansMono.ttf)",
    url: DEJAVU_MONO_DEB,
    sha256: "c805f9436dbc268644c1d9584f01a601a653e028e08fd74b9b949f6cf8304d88",
    out: "dejavu_sans_mono_data.c", symbol: "DEJAVU_SANS_MONO_SFNT",
    licence: DEJAVU_LICENCE,
  },
  "liberation-serif": {
    name: "Liberation Serif 2.1.5 (Ubuntu fonts-liberation 1:2.1.5-3, LiberationSerif-Regular.ttf)",
    url: LIBERATION_DEB,
    sha256: "705903ae1382f0150f115d6f10a30f5b2f3d6ade649c474afe6366c39174fb65",
    out: "liberation_serif_data.c", symbol: "LIBERATION_SERIF_SFNT",
    licence: LIBERATION_LICENCE,
  },
  "liberation-sans": {
    name: "Liberation Sans 2.1.5 (Ubuntu fonts-liberation 1:2.1.5-3, LiberationSans-Regular.ttf)",
    url: LIBERATION_DEB,
    sha256: "4659bc0c58c5028dd488ec928d41d9265db43d9b669fc14ca8b0832daca7b144",
    out: "liberation_sans_data.c", symbol: "LIBERATION_SANS_SFNT",
    licence: LIBERATION_LICENCE,
  },
};

/* THE TABLES THE ENGINE READS. 'GPOS'/'GDEF' are kept WHEN PRESENT: a face without them kerns nothing, which
   is what HarfBuzz does with it too. 'vhea'/'vmtx' are kept WHEN PRESENT and are not required: OpenType makes them
   optional, and css-writing-modes-4 §5.1.1 "Vertical Typesetting and Font Features" says the UA "must
   synthesize vertical font metrics for fonts that lack them" while defining no heuristic for doing so — so
   their absence is a capability to build, stated at the lookup, not a hole to paper over here.
   'glyf' AND 'loca' ARE REQUIRED AND NOT OPTIONAL, which is a statement about this generator's input rather
   than about faces in general: the sfntVersion pin above accepts only the TrueType-outline form, and a face
   of that form with no outline table is not a face with outlines somewhere else, it is a broken one. A face
   whose outlines are CFF would have to be pinned separately and would need a second decoder, so refusing it
   here is the honest arm and not a narrowing. */
const REQUIRED = ["cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp"];
const OPTIONAL = ["GDEF", "GPOS", "vhea", "vmtx"];

const FACE = FACES[process.argv[2]];
const input = process.argv[3];
if (!FACE || !input) {
  console.error(`usage: node engine/fontsubset.mjs <${Object.keys(FACES).join("|")}> <path-to-ttf>`);
  console.error("       see this file's header for the exact commands that obtain each input");
  process.exit(1);
}
const OUT = join(HERE, "host", "browser", "core", "fonts", FACE.out);
const SOURCE_NAME = FACE.name, SOURCE_URL = FACE.url, SOURCE_SHA256 = FACE.sha256;
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
  /* 'head' — Font Header Table's indexToLocFormat, at byte 50 of that table: 0 selects the 16-bit offset
     array and 1 the 32-bit one. It is published because it is the ONE field of this face a decoder cannot
     infer and cannot fail loudly on — read the wrong one and the offset array still parses, still looks
     self-consistent, and addresses bytes that are not a glyph. core/fonts/glyph_outline.h takes it as an
     argument for exactly that reason, so a reader of the C side needs the face to have stated it somewhere,
     and this is that somewhere. Read back out of the OUTPUT like its three neighbours, so a subset that lost
     a table cannot describe itself as one that kept it. */
  indexToLocFormat: out.readInt16BE(headAt + 50),
};
/* THE HIGHEST GLYPH ID ANY CHARACTER REACHES, over the 'cmap' subtable core/fonts/open_type_metrics.c selects
   (its OT_CMAP_PREFERENCE order), so the generated file can say whether a page reaches the 'hmtx' repeat rule. */
function highestMappedGlyph() {
  const cmapAt = placed.find((p) => p.tag === "cmap").at;
  const n = u16(out, cmapAt + 2);
  for (const [platform, encoding, format] of [[3, 10, 12], [0, 4, 12], [3, 1, 4], [0, 3, 4]]) {
    for (let i = 0; i < n; i++) {
      const rec = cmapAt + 4 + 8 * i;
      if (u16(out, rec) !== platform || u16(out, rec + 2) !== encoding) continue;
      const sub = cmapAt + u32(out, rec + 4);
      if (u16(out, sub) !== format) continue;
      let top = 0;
      if (format === 12) {
        for (let g = 0, groups = u32(out, sub + 12); g < groups; g++) {
          const at = sub + 16 + 12 * g;
          top = Math.max(top, u32(out, at + 8) + u32(out, at + 4) - u32(out, at));
        }
        return top;
      }
      const segs = u16(out, sub + 6) / 2;
      const ends = sub + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs, ranges = deltas + 2 * segs;
      for (let k = 0; k < segs; k++) {
        const start = u16(out, starts + 2 * k), end = u16(out, ends + 2 * k);
        const delta = u16(out, deltas + 2 * k), range = u16(out, ranges + 2 * k);
        for (let c = start; c <= end && c !== 0xffff; c++) {
          let g = 0;
          if (range === 0) g = (c + delta) & 0xffff;
          else {
            g = u16(out, ranges + 2 * k + range + 2 * (c - start));
            if (g !== 0) g = (g + delta) & 0xffff;
          }
          top = Math.max(top, g);
        }
      }
      return top;
    }
  }
  console.error("[fontsubset] the face has none of the Unicode 'cmap' subtables the engine selects");
  process.exit(1);
}
const highestMapped = highestMappedGlyph();
const repeatReach = highestMapped >= facts.numberOfHMetrics
  ? "a page's characters reach the repeat rule through this face."
  : "no character reaches the repeat rule; a synthetic face exercises it.";

/* WHAT THE OUTLINE TABLES COST, IN THE ARTIFACT'S OWN BYTES. A size in a comment is a number nobody can
   re-run; these are measured on the way past and printed on every run, so the price of keeping them is read
   off the thing that was built rather than taken on trust. */
const outlineBytes = placed.filter((p) => p.tag === "glyf" || p.tag === "loca")
                           .reduce((a, p) => a + p.length, 0);

const dropped = [...dir.keys()].filter((t) => !keep.includes(t)).sort();
const lines = [];
for (let i = 0; i < out.length; i += 16) {
  lines.push("    " + [...out.subarray(i, i + 16)].map((b) => "0x" + b.toString(16).padStart(2, "0")).join(", ") + ",");
}

const hmtxForm = facts.numberOfHMetrics === facts.numGlyphs
  ? "numberOfHMetrics equals numGlyphs, so 'hmtx' is in the long form."
  : `numberOfHMetrics is below numGlyphs: glyph IDs ${facts.numberOfHMetrics}..${facts.numGlyphs - 1} take 'hmtx''s last record; the
   highest glyph a 'cmap' entry reaches is ${highestMapped}, so ${repeatReach}`;
const text = `/* GENERATED BY engine/fontsubset.mjs — DO NOT EDIT. Re-run the generator, whose header carries the commands
   that obtain the input, the licence, and why each dropped table is sound to drop.
   ${FACE.name.split(" (")[0]}, an installed face (core/fonts/installed_font.h): ${keep.includes("glyf") ? "an outline-carrying" : "a metrics-only"} sfnt of
   ${keep.map((t) => `'${t}'`).join(", ")}, copied byte for byte out of
   ${SOURCE_NAME}
     ${SOURCE_URL}
     sha256 ${SOURCE_SHA256}
   Dropped: ${dropped.map((t) => `'${t}'`).join(", ")}.
   'head' unitsPerEm ${facts.unitsPerEm}, indexToLocFormat ${facts.indexToLocFormat} (${facts.indexToLocFormat ? "32" : "16"}-bit offsets); 'maxp' numGlyphs ${facts.numGlyphs}; 'hhea' numberOfHMetrics ${facts.numberOfHMetrics}.
   ${hmtxForm}
   The outline tables are ${outlineBytes} of the ${out.length} bytes below.${keep.includes("vmtx") ? "" : " No 'vmtx', so a vertical advance crashes."}
   ${FACE.licence} */
#include "core/fonts/installed_font_data.h"

const unsigned char ${FACE.symbol}[${out.length}] = {
${lines.join("\n")}
};

const unsigned int ${FACE.symbol}_LEN = ${out.length};
`;

mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, text);
console.log(`[fontsubset] ${OUT}`);
console.log(`[fontsubset]   ${out.length} bytes, tables ${keep.join(" ")}, dropped ${dropped.join(" ")}`);
console.log(`[fontsubset]   unitsPerEm ${facts.unitsPerEm} numGlyphs ${facts.numGlyphs} numberOfHMetrics ${facts.numberOfHMetrics} indexToLocFormat ${facts.indexToLocFormat}`);
/* THE COST OF THE OUTLINE TABLES, PRINTED RATHER THAN ESTIMATED — bytes, and the C source lines they become,
   which is the number a reviewer of the generated file actually meets. The emitter that built `lines`
   above writes sixteen bytes per line, and the generated header is the rest of the file. */
console.log(`[fontsubset]   outlines ${outlineBytes} bytes of ${out.length}; ` +
            `${Math.ceil(out.length / 16)} data lines of C, ${Math.ceil(outlineBytes / 16)} of them outlines`);
