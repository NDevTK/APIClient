/* WHAT EACH COMPONENT-RECORD CAPTURE SITE COSTS THE LAYOUT CHECK — the per-site half of a price whose other
 * half is a count nobody can derive from the tree.
 *
 * cow_capture_host_record_at validates the CowRecord layout it is handed BEFORE the flow-private skip and
 * before the dedup, so the check runs once per ASK rather than once per captured entry. Its shape is
 * `3*k + k*(k-1)/2` comparisons for a layout of k owned values: two bounds/alignment tests per entry plus a
 * pairwise duplicate-offset scan. `k` is not uniform across the platform and the spread is the whole question
 * — this tree's layouts run from 0 to twenty-odd, which is a 250x spread in what one ask costs.
 *
 * THE COUNT IS NOT HERE AND CANNOT BE. How many asks each site supplies is a fact about a RUN, and the engine
 * publishes it per site (`CowHostRecSite`). This instrument publishes the other factor, which is a fact about
 * the TREE, so the price of a site is this table's `compares` times that census's `asks` — and neither half
 * means anything alone. Quote them joined or not at all.
 *
 * IT DERIVES EVERY FIGURE AND STATES NONE. There is no expected layout count, no expected `k` and no list of
 * components: a record added tomorrow appears by existing. The one thing it does assert is agreement with the
 * subject — where a component states its own `n_val` as a literal, this must reproduce it exactly, and a
 * single mismatch THROWS rather than printing a breakdown nobody can trust. That is the calibration a second
 * implementation of somebody else's selector owes before any of its numbers are worth reading; the first
 * version of this parser split on every comma, which `offsetof(T, f)` supplies one of, and it was the
 * published literals that caught it. */

import { readFileSync } from "node:fs";
import { execFileSync } from "node:child_process";

const ROOT = new URL("..", import.meta.url).pathname.replace(/\/$/, "");
const git = (...a) => execFileSync("git", a, { cwd: ROOT, encoding: "utf8" });

/* Comments and string literals hold braces, commas and parentheses, so they are removed before any structure
   is read out of the source. Nothing below re-reads the original text. */
const strip = (s) =>
  s.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/\/\/[^\n]*/g, " ").replace(/"(\\.|[^"\\])*"/g, '""');

/* Top-level terms of a brace body: `offsetof(T, f)` is ONE term carrying a comma of its own, so the split is
   by nesting depth and never by the character. A trailing comma closes no term. */
function terms(body) {
  const out = [];
  let depth = 0, cur = "";
  for (const c of body) {
    if (c === "(" || c === "[") depth++;
    else if (c === ")" || c === "]") depth--;
    if (c === "," && depth === 0) { out.push(cur); cur = ""; continue; }
    cur += c;
  }
  out.push(cur);
  return out.filter((t) => t.trim() !== "");
}

/* A term may itself be a macro that expands to SEVERAL offsets (node_filter.h's TRAVERSER_VALS names two).
   Its arity is read from its own definition rather than known here, so a second such macro needs no edit. */
const files = git("ls-files", "--", "*.c", "*.h").trim().split("\n").filter(Boolean);
const macroArity = new Map();
for (const f of files) {
  const src = strip(readFileSync(`${ROOT}/${f}`, "utf8"));
  for (const m of src.matchAll(/^[ \t]*#define[ \t]+(\w+)\(([^)]*)\)[ \t]+(.+)$/gm))
    macroArity.set(m[1], terms(m[3]).length);
}
function entriesOf(body) {
  let n = 0;
  for (const t of terms(body)) {
    const call = t.trim().match(/^(\w+)\s*\(/);
    const arity = call && call[1] !== "offsetof" ? macroArity.get(call[1]) : 1;
    if (arity === undefined)
      throw new Error(`cowlayout: term '${t.trim()}' names a macro this tree does not define — the entry ` +
                      `count would silently be a floor`);
    n += arity ?? 1;
  }
  return n;
}

/* THE LAYOUTS, AND THE CALIBRATION AGAINST THE LITERALS THE COMPONENTS PUBLISH. */
const arrays = new Map(), recs = new Map();
for (const f of files.filter((f) => f.endsWith(".c"))) {
  const src = strip(readFileSync(`${ROOT}/${f}`, "utf8"));
  for (const m of src.matchAll(/const\s+uint16_t\s+(\w+)\s*\[\s*\]\s*=\s*\{([\s\S]*?)\}\s*;/g))
    arrays.set(m[1], { file: f, n: entriesOf(m[2]) });
  for (const m of src.matchAll(/const\s+CowRecord\s+(\w+)\s*=\s*\{\s*sizeof\((\w+)\)\s*,\s*(\w+)\s*,\s*([^}]*?)\}/g))
    recs.set(m[1], { file: f, type: m[2], vals: m[3], stated: m[4].trim() });
}
if (recs.size === 0) throw new Error("cowlayout: no CowRecord found — the selector no longer matches the tree");

const bad = [];
for (const [name, r] of recs) {
  r.k = r.vals === "NULL" ? 0 : arrays.get(r.vals)?.n;
  if (r.k === undefined) throw new Error(`cowlayout: ${name} names ${r.vals}, which no array defines`);
  if (/^\d+$/.test(r.stated) && Number(r.stated) !== r.k)
    bad.push(`${name} (${r.file}): states n_val=${r.stated}, its array holds ${r.k}`);
}
if (bad.length)
  throw new Error("cowlayout: CALIBRATION FAILED against the components' own n_val literals — this parser " +
                  "and the tree disagree, so no figure below is worth reading:\n  " + bad.join("\n  "));
const calibrated = [...recs.values()].filter((r) => /^\d+$/.test(r.stated)).length;

/* THE SITES. The address the engine's census keys on is the CALL, not the record: one record is passed from
   several sites and one site passes a record chosen per derived class, so the two cannot be joined by record
   name. This prints the call's own file:line, which is what `CowHostRecSite` files. */
const compares = (k) => 3 * k + (k * (k - 1)) / 2;
const sites = [];
for (const f of files.filter((f) => f.endsWith(".c") && !f.endsWith("solver/cow.c"))) {
  readFileSync(`${ROOT}/${f}`, "utf8").split("\n").forEach((line, i) => {
    const m = line.match(/cow_capture_host_record\s*\(\s*[^,]+,\s*[^,]+,\s*&?([\w>.-]+)\s*\)/);
    if (!m) return;
    const ref = m[1].replace(/^&/, "");
    const r = recs.get(ref);
    sites.push({ at: `${f}:${i + 1}`, rec: ref, k: r ? r.k : null, indirect: !r });
  });
}

const w = (s, n) => String(s).padEnd(n);
console.log(`cow layout check — ${recs.size} layouts, ${sites.length} capture sites` +
            `  [calibrated against ${calibrated} published n_val literals]`);
console.log(`cost model: compares/ask = 3k + k(k-1)/2, run BEFORE the flow-private skip and the dedup\n`);
console.log(`${w("CAPTURE SITE", 52)}${w("LAYOUT", 18)}${w("k", 5)}compares/ask`);
for (const s of sites.sort((a, b) => (b.k ?? -1) - (a.k ?? -1)))
  console.log(`${w(s.at, 52)}${w(s.rec, 18)}${w(s.k ?? "?", 5)}` +
              (s.k === null ? "chosen per derived class at run time — join by site, never by record"
                            : compares(s.k)));

const ks = sites.filter((s) => s.k !== null).map((s) => s.k).sort((a, b) => a - b);
const c = ks.map(compares);
console.log(`\nover the ${ks.length} sites whose layout is statically known:`);
console.log(`  k            min=${ks[0]}  median=${ks[ks.length >> 1]}  max=${ks[ks.length - 1]}`);
console.log(`  compares/ask min=${Math.min(...c)}  median=${c.sort((a, b) => a - b)[c.length >> 1]}  ` +
            `max=${Math.max(...c)}`);
console.log(`\nA PRICE NEEDS THE OTHER FACTOR. Multiply a site's compares by that site's ask count from the\n` +
            `engine's own per-site census; this table cannot say which site is hot and neither can any\n` +
            `reading of the tree. The spread above is why the aggregate ask count does not settle it.`);
