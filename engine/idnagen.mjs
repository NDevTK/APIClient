/* THE UTS-46 MAPPING TABLE, generated from Unicode's own IdnaMappingTable.txt.
 *
 * WHY GENERATED AND COMMITTED. idna.c shipped without it and said so: full case folding plus NFC is most of
 * the table's content, but the DISALLOWED set is not decidable without it, so a code point UTS-46 refuses was
 * accepted. wpt measured exactly that — 1126 of IdnaTestV2's cases are `new URL(...)` expecting a THROW that
 * never came. The fix is the real table, not a bigger approximation.
 *
 * It is generated the way platform_names.h is: a script reads the authoritative source, writes a C header, and
 * the header is COMMITTED so the build needs no network. Re-run this deliberately when re-pinning, the same as
 * re-pinning lexbor, test262 or the wpt corpus.
 *
 * THE TABLE IS SPECIALIZED FOR THIS CALLER, and that is honest rather than lossy: WHATWG URL calls UTS-46 with
 * UseSTD3ASCIIRules=false and Transitional=false, which collapses seven statuses onto four outcomes —
 * `disallowed_STD3_valid` is KEEP, `disallowed_STD3_mapped` is MAP, and `deviation` is KEEP. Emitting all seven
 * and re-deriving the collapse at every lookup would be a second place for the configuration to live.
 *
 * Usage:  node engine/idnagen.mjs
 */
import { writeFileSync } from "node:fs";
import { join } from "node:path";

const ENGINE = import.meta.dirname;
const OUT = join(ENGINE, "host", "browser", "core", "url", "idna_table.h");

/* PINNED, like every other oracle in this tree. Unicode 17.0.0 has no versioned directory yet, so the pin is
   the version ASSERTION below rather than the URL — if `latest` moves, the generator stops instead of silently
   re-pinning the table under a build that was checked against the old one. */
const VERSION = "17.0.0";
const URL_SRC = "https://www.unicode.org/Public/idna/latest/IdnaMappingTable.txt";

const res = await fetch(URL_SRC);
if (!res.ok) { console.error(`[idnagen] ${URL_SRC}: HTTP ${res.status}`); process.exit(1); }
const text = await res.text();
const ver = /^#\s*Version:\s*(\S+)/m.exec(text);
if (!ver || ver[1] !== VERSION) {
  console.error(`[idnagen] the source is Unicode ${ver ? ver[1] : "?"} and this generator is pinned to ` +
                `${VERSION} — re-pin deliberately, as a commit of its own, so the diff in results has one cause.`);
  process.exit(1);
}

/* The four outcomes idna.c acts on. Their numbering is the header's contract. */
const KEEP = 0, REMOVE = 1, MAP = 2, ERROR = 3;

const rows = [];
for (const raw of text.split("\n")) {
  const line = raw.split("#")[0].trim();
  if (!line) continue;
  const f = line.split(";").map((x) => x.trim());
  const [lo, hi] = f[0].includes("..") ? f[0].split("..") : [f[0], f[0]];
  const status = f[1];
  let outcome, mapping = [];
  switch (status) {
    /* UseSTD3ASCIIRules=false makes the two STD3 statuses ordinary, and Transitional=false makes a deviation
       valid — the three places this configuration shows up, and they show up only here. */
    case "valid": case "disallowed_STD3_valid": case "deviation": outcome = KEEP; break;
    case "ignored": outcome = REMOVE; break;
    case "mapped": case "disallowed_STD3_mapped":
      outcome = MAP;
      mapping = (f[2] || "").split(/\s+/).filter(Boolean).map((h) => parseInt(h, 16));
      /* A mapped range maps the WHOLE range to ONE sequence, which is what lets a range carry a single
         mapping — `0132..0133 ; mapped ; 0069 006A` is both IJ ligatures to "ij". */
      break;
    case "disallowed": outcome = ERROR; break;
    default:
      console.error(`[idnagen] unknown status ${JSON.stringify(status)} — a status this generator does not ` +
                    `collapse would be silently dropped, so it stops instead`);
      process.exit(1);
  }
  rows.push({ lo: parseInt(lo, 16), hi: parseInt(hi, 16), outcome, mapping });
}
rows.sort((a, b) => a.lo - b.lo);

/* Merge adjacent ranges that act the same. A MAP only merges with an identical mapping, which is rare and is
   the point: two code points that map to different sequences are two ranges however adjacent they are. */
const merged = [];
for (const r of rows) {
  const p = merged[merged.length - 1];
  const same = p && p.hi + 1 === r.lo && p.outcome === r.outcome &&
               p.mapping.length === r.mapping.length && p.mapping.every((v, i) => v === r.mapping[i]);
  if (same) p.hi = r.hi;
  else merged.push({ ...r });
}

/* One shared pool for the mapping sequences; identical sequences share an offset. */
const pool = [];
const poolIndex = new Map();
for (const r of merged) {
  if (r.outcome !== MAP) { r.off = 0; continue; }
  const key = r.mapping.join(",");
  if (!poolIndex.has(key)) { poolIndex.set(key, pool.length); pool.push(...r.mapping); }
  r.off = poolIndex.get(key);
}
if (pool.length > 0xffff) { console.error("[idnagen] the mapping pool outgrew a uint16 offset"); process.exit(1); }

/* ---- RFC 5892's CONTEXTJ data, which CheckJoiners needs ---------------------------------------------------
 *
 * WHATWG calls UTS-46 with CheckJoiners=true, and A.1/A.2 are the two rules that make a ZWNJ or a ZWJ valid
 * only in the right neighbourhood. Deciding them needs Canonical_Combining_Class=Virama and Joining_Type,
 * neither of which libunicode exposes — so they come from the UCD, from the same pinned version, and are
 * emitted as two more range tables rather than as a second source of Unicode truth. */
async function ucd(file) {
  const u = `https://www.unicode.org/Public/${VERSION}/ucd/extracted/${file}`;
  const r = await fetch(u);
  if (!r.ok) { console.error(`[idnagen] ${u}: HTTP ${r.status}`); process.exit(1); }
  const out = [];
  for (const raw of (await r.text()).split("\n")) {
    const line = raw.split("#")[0].trim();
    if (!line) continue;
    const f = line.split(";").map((x) => x.trim());
    const [lo, hi] = f[0].includes("..") ? f[0].split("..") : [f[0], f[0]];
    out.push({ lo: parseInt(lo, 16), hi: parseInt(hi, 16), value: f[1] });
  }
  return out;
}

function rangesWhere(rows, pred) {
  const sel = rows.filter((r) => pred(r.value)).sort((a, b) => a.lo - b.lo);
  const m = [];
  for (const r of sel) {
    const p = m[m.length - 1];
    if (p && p.hi + 1 >= r.lo) p.hi = Math.max(p.hi, r.hi);
    else m.push({ lo: r.lo, hi: r.hi });
  }
  return m;
}

const ccc = await ucd("DerivedCombiningClass.txt");
const jt  = await ucd("DerivedJoiningType.txt");
const viramas = rangesWhere(ccc, (v) => v === "9" || v === "Virama");
/* Joining_Type is emitted as three sets, which is all A.1's regex distinguishes: {L,D}, T, and {R,D}. */
const jtLD = rangesWhere(jt, (v) => v === "L" || v === "D");
const jtT  = rangesWhere(jt, (v) => v === "T");
const jtRD = rangesWhere(jt, (v) => v === "R" || v === "D");

function emitRanges(name, rs) {
  const out = [`static const uint32_t ${name}[][2] = {`];
  for (const r of rs) out.push(`    { 0x${r.lo.toString(16)}, 0x${r.hi.toString(16)} },`);
  out.push("};");
  return out;
}

const lines = [];
lines.push("/* GENERATED by engine/idnagen.mjs from Unicode " + VERSION + "'s IdnaMappingTable.txt.");
lines.push("   Do not edit — re-run the generator, which is also where the WHATWG configuration this table is");
lines.push("   specialized for (UseSTD3ASCIIRules=false, Transitional=false) is applied. */");
lines.push("#ifndef ENGINE_HOST_BROWSER_CORE_URL_IDNA_TABLE_H");
lines.push("#define ENGINE_HOST_BROWSER_CORE_URL_IDNA_TABLE_H");
lines.push("#include <stdint.h>");
lines.push("");
lines.push('#define IDNA_UNICODE_VERSION "' + VERSION + '"');
lines.push("");
lines.push("/* The four outcomes, after the collapse. */");
lines.push("enum { IDNA_KEEP = 0, IDNA_REMOVE = 1, IDNA_MAP = 2, IDNA_ERROR = 3 };");
lines.push("");
lines.push("typedef struct { uint32_t lo, hi; uint8_t outcome; uint16_t off, len; } IdnaRange;");
lines.push("");
lines.push("static const uint32_t IDNA_MAP_POOL[] = {");
for (let i = 0; i < pool.length; i += 12)
  lines.push("    " + pool.slice(i, i + 12).map((v) => "0x" + v.toString(16)).join(", ") + ",");
lines.push("};");
lines.push("");
lines.push("static const IdnaRange IDNA_RANGES[] = {");
for (const r of merged)
  lines.push(`    { 0x${r.lo.toString(16)}, 0x${r.hi.toString(16)}, ${r.outcome}, ${r.off}, ${r.mapping.length} },`);
lines.push("};");
lines.push("");
lines.push("/* RFC 5892 A.1/A.2's data: Canonical_Combining_Class=Virama, and Joining_Type as the three sets");
lines.push("   A.1's regex distinguishes. */");
lines.push(...emitRanges("IDNA_VIRAMA", viramas));
lines.push("");
lines.push(...emitRanges("IDNA_JT_LD", jtLD));
lines.push("");
lines.push(...emitRanges("IDNA_JT_T", jtT));
lines.push("");
lines.push(...emitRanges("IDNA_JT_RD", jtRD));
lines.push("");
lines.push("#endif");
writeFileSync(OUT, lines.join("\n") + "\n");
console.log(`[idnagen] Unicode ${VERSION}: ${merged.length} mapping ranges, ${pool.length} mapping code ` +
            `points, ${viramas.length} virama + ${jtLD.length}/${jtT.length}/${jtRD.length} joining-type ` +
            `ranges -> ` + OUT.replace(ENGINE + "/", "engine/"));
