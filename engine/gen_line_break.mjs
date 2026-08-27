/* [UAX14]'s Line_Break PROPERTY, TURNED INTO THE TABLE core/layout/line_break.c READS.
 *
 * css-text-3 §5 "Line Breaking and Word Boundaries" makes a soft wrap opportunity a property of the CONTENT
 * ("valid soft wrap opportunities depend on the content language and writing system"), and its own note names
 * where the baseline comes from: "Unicode Standard Annex #14: Unicode Line Breaking Algorithm defines a
 * baseline behavior for line breaking for all scripts in Unicode, which is expected to be further tailored.
 * [UAX14]". §5.5 "Line Breaking Details" then makes four parts of that baseline MANDATORY for a CSS UA — BK
 * and NL are forced line breaks "regardless of the white-space value", the behavior defined for WJ, ZW, GL and
 * ZWJ "must be honored", and "CSS never allows soft wrap opportunities within typographic character units.
 * Thus, the CM and SG Unicode line breaking classes must always be honored."
 * So the Line_Break property is not optional data for this engine, and it is a table over EVERY Unicode scalar
 * value that no library in this tree carries: quickjs's libunicode has ECMAScript's binary properties and
 * General_Category and no Line_Break at all, and lexbor's unicode module is IDNA normalisation. It comes from
 * where a browser's comes from, which is the UCD, generated the way libunicode's own tables are — from the
 * file, checked in beside the generator, never hand-typed.
 *
 *   node engine/gen_line_break.mjs <dir-holding-the-four-UCD-files>
 *
 * HOW TO OBTAIN THE INPUT — the exact commands, so that "reproducible" is a thing a reader can do rather than a
 * claim they have to take. The VERSIONED urls are pinned and not `Public/UCD/latest/`: `latest` is a moving
 * target, so a generator pointed at it produces output nobody can re-derive once the next Unicode ships.
 *
 *   U=https://www.unicode.org/Public/17.0.0/ucd
 *   curl -sSLO $U/LineBreak.txt
 *   curl -sSLO $U/EastAsianWidth.txt
 *   curl -sSLO $U/extracted/DerivedGeneralCategory.txt
 *   curl -sSLO $U/emoji/emoji-data.txt
 *
 * WHY FOUR FILES AND NOT ONE, which is the whole of this generator's scope argument. UAX14's rules are not
 * stated over the Line_Break property alone: five of them name a SECOND Unicode property, and a rule whose
 * second property is missing cannot be implemented at all — it can only be replaced by a crash, and the
 * characters those five rules are about are the ones ordinary pages are full of.
 *   LB1 resolves SA (South East Asian) to CM or AL "Only Mn or Mc" / "Any except Mn and Mc" — General_Category.
 *     That is Thai, Lao, Khmer and Myanmar: a page in any of them.
 *   LB15a and LB15b are stated over `[\p{Pi}&QU]` and `[\p{Pf}&QU]`, and LB19 over `[QU - \p{Pi}]` and
 *     `[QU - \p{Pf}]` — Initial_Punctuation and Final_Punctuation. That is “ and ”, which is every piece of
 *     English prose a word processor has been near.
 *   LB19a and LB30 exclude `$EastAsian` — East_Asian_Width in {Fullwidth, Wide, Halfwidth}. That is CJK.
 *   LB30b's second production is stated over `[\p{Extended_Pictographic}&\p{Cn}]` — emoji-data.txt's
 *     Extended_Pictographic intersected with the UNASSIGNED code points, which is how the rule reaches code
 *     points reserved for future emoji.
 * Each of those sets is SMALL once intersected with the class the rule asks about (QU&Pi is 12 code points,
 * QU&Pf is 10, Extended_Pictographic&Cn is 36 ranges, East_Asian_Width F|W|H is 129), so the price of the
 * closure is a few hundred rows and the price of NOT paying it is a crash on curly quotes.
 *
 * WHAT IS EMITTED IS THE CLASS AFTER LB1, AND THAT IS A DECISION WITH A CONDITION ON IT. UAX14 LB1 says
 * "Assign a line breaking class to each code point of the input. Resolve AI, CB, CJ, SA, SG, and XX into other
 * line breaking classes depending on criteria outside the scope of this algorithm. In the absence of such
 * criteria all characters with a specific combination of original class and General_Category property value
 * are resolved as follows" — AI, SG and XX to AL for any General_Category, SA to CM when Mn or Mc and AL
 * otherwise, CJ to NS for any. (CB is NOT in that table: LB20 "Break before and after unresolved CB" is its
 * own default, so CB survives into the emitted class and is a rule's business rather than this file's.)
 * The "criteria outside the scope" a CSS UA has are css-text-3 §5.1's `word-break` and §5.2's `line-break` —
 * `line-break: loose` is what turns CJ into ID rather than NS. This engine's cascade derives a computed value
 * for NEITHER property, so there are no criteria, so the default table IS the answer and applying it here
 * costs the runtime a General_Category table it would otherwise need for SA alone. core/layout/line_break.c
 * ASSERTS that condition rather than recording it: the day css_computed_models answers for `line-break` or
 * `word-break`, it crashes and names this file as the thing that has to stop pre-resolving.
 *
 * THE OUTPUT IS RUNS THAT PARTITION THE WHOLE CODE POINT SPACE and not a list of the assignments the file
 * happens to state. LineBreak.txt's `@missing: 0000..10FFFF; XX` is part of the data: an unlisted code point
 * is XX, which LB1 resolves to AL, and leaving it to a fallthrough in the consumer would put a second copy of
 * that resolution somewhere it can disagree with this one. So every code point is covered by exactly one run
 * and the lookup is a search that cannot miss. */
import { readFileSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { join } from "node:path";

const MAX = 0x110000;

/* THE INPUTS, PINNED BY CONTENT. A generator that accepts whatever files it is pointed at produces output
   nobody can re-derive, which is the same defect as a measurement quoted without its revision. The version
   number in each file's own first line is checked too, because two files from different Unicode versions
   intersect into a set that is neither version's. */
const UNICODE_VERSION = "17.0.0";
const BASE = `https://www.unicode.org/Public/${UNICODE_VERSION}/ucd`;
const INPUTS = {
  lineBreak: {
    file: "LineBreak.txt",
    url: `${BASE}/LineBreak.txt`,
    sha256: "e6a18fa91f8f6a6f8e534b1d3f128c21ada45bfe152eb6b1bcc5e15fd8ac92e6",
    stamp: `# LineBreak-${UNICODE_VERSION}.txt`,
  },
  eastAsianWidth: {
    file: "EastAsianWidth.txt",
    url: `${BASE}/EastAsianWidth.txt`,
    sha256: "ea7ce50f3444a050333448dffef1cadd9325af55cbb764b4a2280faf52170a33",
    stamp: `# EastAsianWidth-${UNICODE_VERSION}.txt`,
  },
  generalCategory: {
    file: "DerivedGeneralCategory.txt",
    url: `${BASE}/extracted/DerivedGeneralCategory.txt`,
    sha256: "d62e5bab70ca74f099343f71224fa051cb1fdd61a1ab45c0488c44cfc0b6102e",
    stamp: `# DerivedGeneralCategory-${UNICODE_VERSION}.txt`,
  },
  emoji: {
    file: "emoji-data.txt",
    url: `${BASE}/emoji/emoji-data.txt`,
    sha256: "2cb2bb9455cda83e8481541ecf5b6dfda66a3bb89efa3fa7c5297eccf607b72b",
    /* emoji-data.txt's first line carries no version; its `# Version: 17.0` line does. */
    stamp: "# emoji-data.txt",
  },
};

const dir = process.argv[2];
if (!dir) {
  console.error("usage: node engine/gen_line_break.mjs <dir-holding-the-four-UCD-files>");
  console.error("       see this file's header for the exact curl commands that obtain them");
  process.exit(1);
}

function load(spec) {
  const path = join(dir, spec.file);
  const bytes = readFileSync(path);
  const got = createHash("sha256").update(bytes).digest("hex");
  if (got !== spec.sha256) {
    console.error(`[gen_line_break] ${path} is not the pinned ${spec.file}`);
    console.error(`[gen_line_break]   expected sha256 ${spec.sha256}`);
    console.error(`[gen_line_break]   got             ${got}`);
    console.error(`[gen_line_break]   obtain it from ${spec.url}`);
    process.exit(1);
  }
  const text = bytes.toString("utf8");
  if (!text.startsWith(spec.stamp)) {
    console.error(`[gen_line_break] ${path} does not begin "${spec.stamp}" — a file from another Unicode`);
    console.error("[gen_line_break]   version would intersect into a set that is neither version's.");
    process.exit(1);
  }
  return text;
}

/* ONE UCD DATA LINE: `<code point>[..<code point>] ; <value>`, with `#` starting a comment. Every one of the
   four files is in this format (UAX44 §4.2.1 "Data Files"), which is why one parser reads all four. A line
   this does not match is a comment or blank; a line that matches with a malformed range is a CRASH rather than
   a skip, because a silently dropped range is a class this engine would then answer wrongly for. */
function* rows(text, file) {
  let lineno = 0;
  for (const raw of text.split("\n")) {
    lineno++;
    const line = raw.split("#")[0].trim();
    if (line === "") continue;
    const m = /^([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*([A-Za-z_0-9]+)\s*$/.exec(line);
    if (!m) {
      console.error(`[gen_line_break] ${file}:${lineno}: not a code point range and a value: ${JSON.stringify(line)}`);
      process.exit(1);
    }
    const lo = parseInt(m[1], 16);
    const hi = m[2] === undefined ? lo : parseInt(m[2], 16);
    if (!(lo <= hi) || hi >= MAX) {
      console.error(`[gen_line_break] ${file}:${lineno}: range ${m[1]}..${m[2] ?? m[1]} is empty or out of range`);
      process.exit(1);
    }
    yield { lo, hi, value: m[3] };
  }
}

/* A property's value for every code point, as a dense array. `fill` is the value an unlisted code point has —
   for Line_Break that is XX, which the file states in its own `@missing` line and which this refuses to guess:
   the line is READ and compared. */
function dense(text, file, fill) {
  const out = new Array(MAX).fill(fill);
  for (const r of rows(text, file)) out.fill(r.value, r.lo, r.hi + 1);
  return out;
}

const lineBreakText = load(INPUTS.lineBreak);
const missing = /^# @missing: 0000\.\.10FFFF; (\w+)$/m.exec(lineBreakText);
if (!missing || missing[1] !== "XX") {
  console.error("[gen_line_break] LineBreak.txt's @missing line is not `0000..10FFFF; XX`. The default for an");
  console.error("[gen_line_break]   unlisted code point is part of the data and this generator reads it rather");
  console.error("[gen_line_break]   than assuming it; a changed default is a changed table.");
  process.exit(1);
}

const lb = dense(lineBreakText, INPUTS.lineBreak.file, "XX");
const gc = dense(load(INPUTS.generalCategory), INPUTS.generalCategory.file, "Cn");
const eaw = dense(load(INPUTS.eastAsianWidth), INPUTS.eastAsianWidth.file, "N");

/* emoji-data.txt lists BINARY properties: the value column is the property name, and a code point not listed
   under a name does not have it. */
const emojiText = load(INPUTS.emoji);
if (!/^# Version: 17\.0$/m.test(emojiText)) {
  console.error("[gen_line_break] emoji-data.txt does not carry `# Version: 17.0`.");
  process.exit(1);
}
const extPict = new Uint8Array(MAX);
for (const r of rows(emojiText, INPUTS.emoji.file))
  if (r.value === "Extended_Pictographic") extPict.fill(1, r.lo, r.hi + 1);

/* ---- LB1 ------------------------------------------------------------------------------------------------ */

const resolved = new Array(MAX);
for (let cp = 0; cp < MAX; cp++) {
  const v = lb[cp];
  if (v === "AI" || v === "SG" || v === "XX") resolved[cp] = "AL";
  else if (v === "CJ") resolved[cp] = "NS";
  else if (v === "SA") resolved[cp] = gc[cp] === "Mn" || gc[cp] === "Mc" ? "CM" : "AL";
  else resolved[cp] = v;
}

/* THE CLASSES THAT SURVIVE LB1 ARE THE ONLY ONES THE ENUM HOLDS, which is a structural guarantee and not a
   comment: a rule in core/layout/line_break.c cannot name AI, SA, SG, XX or CJ, because there is no constant
   for them to name. */
const classes = [...new Set(resolved)].sort();
for (const gone of ["AI", "SA", "SG", "XX", "CJ"])
  if (classes.includes(gone)) {
    console.error(`[gen_line_break] LB1 left ${gone} in the resolved table — the resolution above is incomplete.`);
    process.exit(1);
  }

const runs = [];
for (let i = 0; i < MAX; ) {
  let j = i;
  while (j < MAX && resolved[j] === resolved[i]) j++;
  runs.push({ end: j - 1, cls: resolved[i] });
  i = j;
}

/* A SET, as sorted disjoint inclusive ranges. */
function ranges(pred) {
  const out = [];
  for (let i = 0; i < MAX; ) {
    if (!pred(i)) { i++; continue; }
    let j = i;
    while (j < MAX && pred(j)) j++;
    out.push([i, j - 1]);
    i = j;
  }
  return out;
}

const quInitial = ranges((cp) => resolved[cp] === "QU" && gc[cp] === "Pi");
const quFinal = ranges((cp) => resolved[cp] === "QU" && gc[cp] === "Pf");
const eastAsian = ranges((cp) => eaw[cp] === "F" || eaw[cp] === "W" || eaw[cp] === "H");
const unassignedPictographic = ranges((cp) => extPict[cp] === 1 && gc[cp] === "Cn");

/* U+25CC DOTTED CIRCLE is written into LB28a as a LITERAL code point ("the class [◌] contains the single
   character U+25CC DOTTED CIRCLE"), so it is emitted as a name rather than left for the consumer to spell —
   a magic number in the rules is a number nobody can check against the annex. */
const DOTTED_CIRCLE = 0x25cc;

const hex = (n) => `0x${n.toString(16).toUpperCase().padStart(4, "0")}`;

/* TWO FILES, WHICH IS core/fonts/default_font_data.{h,c}'S SHAPE AND IS NOT A STYLE CHOICE. The data is ~15 KB
   and the header is included by every consumer of core/layout/line_break.h; `static const` arrays in a header
   are a COPY PER TRANSLATION UNIT, so the declarations go in the .h and the definitions in a .c the build
   compiles like any other. The one difference from default_font_data is that BOTH files here are generated:
   that file's header is hand-written because it declares two symbols, and this one declares an enum whose
   members are a function of the input. */

const H = [];
const C = [];
const banner = (what) => [
  `/* GENERATED BY engine/gen_line_break.mjs from the Unicode Character Database ${UNICODE_VERSION} — DO NOT EDIT.`,
  `   Re-run the generator instead; its header carries the four curl commands that obtain the input, the`,
  `   sha256 each is pinned to, and why the rules need four files rather than one.`,
  ``,
  `   ${what}`,
  ``,
  `   [UAX14] Unicode Line Breaking Algorithm's Line_Break property, AFTER §6.1 rule LB1's resolution of AI,`,
  `   SA, SG, XX and CJ — see the generator for why that resolution is a generation-time fact for this engine,`,
  `   and core/layout/text_run.c's \`tr_require_no_line_break_tailoring\` for the assert that fires the day it`,
  `   stops being one. CB is NOT resolved here: LB20 "Break before and after unresolved CB" is its own default`,
  `   and is a rule. */`,
];

H.push(...banner("The DECLARATIONS. The data is in core/layout/line_break_class.c."));
H.push(`#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BREAK_CLASS_H`);
H.push(`#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BREAK_CLASS_H`);
H.push(``);
H.push(`#include <stdint.h>`);
H.push(``);
H.push(`/* The ${classes.length} Line_Break values that survive LB1. There is deliberately no constant for AI, SA, SG, XX or`);
H.push(`   CJ: a rule cannot name a class the resolution above has already removed. */`);
H.push(`typedef enum {`);
classes.forEach((c, i) => H.push(`    LB_CLASS_${c} = ${i},`));
H.push(`} LineBreakClass;`);
H.push(`#define LINE_BREAK_CLASS_COUNT ${classes.length}`);
H.push(``);
H.push(`/* The classes as runs that PARTITION the code point space: run i covers`);
H.push(`   (i ? line_break_class_run_end[i - 1] + 1 : 0) .. line_break_class_run_end[i], so every code point is in`);
H.push(`   exactly one and a search over them cannot miss. LineBreak.txt's own \`@missing: 0000..10FFFF; XX\` is`);
H.push(`   materialized as part of that rather than left to a fallthrough in the consumer, which is where a second`);
H.push(`   copy of LB1's resolution would otherwise live. */`);
H.push(`#define LINE_BREAK_CLASS_RUN_COUNT ${runs.length}`);
H.push(`extern const uint32_t line_break_class_run_end[LINE_BREAK_CLASS_RUN_COUNT];`);
H.push(`extern const uint8_t line_break_class_run[LINE_BREAK_CLASS_RUN_COUNT];`);
H.push(``);
H.push(`/* THE FOUR SETS UAX14 NAMES A SECOND UNICODE PROPERTY FOR, each already intersected with the Line_Break`);
H.push(`   class its rule asks about — so the consumer never needs the second property in general, only where a`);
H.push(`   rule quotes it. Sorted, disjoint, inclusive. */`);
H.push(`typedef struct { uint32_t lo, hi; } LineBreakRange;`);
H.push(``);

C.push(...banner("The DATA. Its declarations are in core/layout/line_break_class.h."));
C.push(`#include "core/layout/line_break_class.h"`);
C.push(``);
C.push(`const uint32_t line_break_class_run_end[LINE_BREAK_CLASS_RUN_COUNT] = {`);
for (let i = 0; i < runs.length; i += 8)
  C.push(`    ${runs.slice(i, i + 8).map((r) => hex(r.end)).join(", ")},`);
C.push(`};`);
C.push(``);
C.push(`const uint8_t line_break_class_run[LINE_BREAK_CLASS_RUN_COUNT] = {`);
for (let i = 0; i < runs.length; i += 8)
  C.push(`    ${runs.slice(i, i + 8).map((r) => `LB_CLASS_${r.cls}`).join(", ")},`);
C.push(`};`);
C.push(``);

const emitSet = (name, note, set) => {
  const N = `${name.toUpperCase()}_COUNT`;
  H.push(`/* ${note} */`);
  H.push(`#define ${N} ${set.length}`);
  H.push(`extern const LineBreakRange ${name}[${N}];`);
  H.push(``);
  C.push(`const LineBreakRange ${name}[${N}] = {`);
  for (let i = 0; i < set.length; i += 4)
    C.push(`    ${set.slice(i, i + 4).map(([lo, hi]) => `{ ${hex(lo)}, ${hex(hi)} }`).join(", ")},`);
  C.push(`};`);
  C.push(``);
};
emitSet("line_break_qu_initial", "LB15a's and LB19's `[\\p{Pi}&QU]` — General_Category Initial_Punctuation.", quInitial);
emitSet("line_break_qu_final", "LB15b's and LB19's `[\\p{Pf}&QU]` — General_Category Final_Punctuation.", quFinal);
emitSet("line_break_east_asian",
        "LB19a's and LB30's `$EastAsian` — East_Asian_Width Fullwidth, Wide or Halfwidth. NOT intersected with\n" +
        "   a class: LB19a asks it of the character on the OTHER side of a QU, which is any class at all.",
        eastAsian);
emitSet("line_break_unassigned_pictographic",
        "LB30b's `[\\p{Extended_Pictographic}&\\p{Cn}]` — the code points reserved for future emoji.",
        unassignedPictographic);

H.push(`/* LB28a writes U+25CC DOTTED CIRCLE into its rules as a literal: "the class [◌] contains the single`);
H.push(`   character U+25CC DOTTED CIRCLE". */`);
H.push(`#define LINE_BREAK_DOTTED_CIRCLE ${hex(DOTTED_CIRCLE)}`);
H.push(``);
H.push(`#endif`);

const dir_out = join(process.cwd(), "engine", "host", "browser", "core", "layout");
writeFileSync(join(dir_out, "line_break_class.h"), H.join("\n") + "\n");
writeFileSync(join(dir_out, "line_break_class.c"), C.join("\n") + "\n");
console.error(`[gen_line_break] ${join(dir_out, "line_break_class.h")} + .c`);
console.error(`[gen_line_break]   ${runs.length} class runs over ${classes.length} classes`);
console.error(`[gen_line_break]   QU&Pi ${quInitial.length} ranges, QU&Pf ${quFinal.length}, ` +
              `East_Asian ${eastAsian.length}, ExtPict&Cn ${unassignedPictographic.length}`);
