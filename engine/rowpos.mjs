/* WHERE A FIXTURE ROW SITS IN THE DOCUMENT, SO A ZERO CAN BE ASKED AGAINST REACH BEFORE IT IS ASKED ABOUT A
 * MECHANISM.
 *
 * `smokerows.mjs` says WHICH rows are still 0 and `rowsplit.mjs` partitions them by a condition of the run.
 * Neither can say the thing that turned out to explain a third of them: a row whose statement the run never
 * REACHED reads exactly like a row whose mechanism is missing, and twelve unrelated families reading 0
 * together is not twelve broken mechanisms, it is one boundary upstream of all of them.
 *
 *   node engine/rowpos.mjs [--after <srcline>] [--rows a,b,c]
 *
 * With no arguments it prints every row it can place, in document order, with the character offset of the
 * first fixture line that mentions the row's KEY. `--after` splits the list at a source line.
 *
 * IT PLACES A ROW BY ITS KEY AND NOT BY ITS NAME. A row is declared `{ "name", pred, "KEY", SESS_… }` and the
 * KEY — an endpoint path, a global, a marker — is what the DOCUMENT contains; the row's own name usually
 * appears nowhere in it. A tool that searched for the name would place almost nothing and would say so as a
 * small honest-looking number, which is the failure this file exists downstream of.
 *
 * THE OFFSET IS CALIBRATED AGAINST A FIGURE DERIVED INDEPENDENTLY, and that is the only reason to trust it.
 * The first hand-derivation of this map accumulated its document length in a loop that exited early and
 * reported a total ~27% short, which made a correct cut POSITION render as a wrong FRACTION in a published
 * report. This prints the total so the next reader can check it the same way, and it decodes the C escapes
 * that a naive byte count of the source would get wrong.
 *
 * IT PRINTS NO VERDICT AND NO EXPECTED SET. Which rows exist and where the document ends are properties of a
 * fixture at a revision; both move, and a build failing on either would be failing on an edit somebody made.
 */
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const SRC = join(dirname(fileURLToPath(import.meta.url)), "host", "test_forced.c");
const text = fs.readFileSync(SRC, "utf8");
const lines = text.split("\n");

/* The fixture document is one C string concatenation. Find its opening declaration and the line that closes
   it, rather than a hardcoded range — the literal grows every time somebody adds a statement. */
/* THERE ARE FOUR DOCUMENTS AND NOT ONE, WHICH IS WHY THIS TOOL EXISTS RATHER THAN A GREP.
   test_forced.c's own DCHECK asserts every probe key occurs in `HTML`, `HTML_MIN`, `HTML_COLD` or
   `HTML_POPOVER`, and its comments record the same five rows reading 1 over one and 0 over another at one
   commit. So "how far through the document" is a question with four answers, a row placed in one has NO
   position in the others, and a percentage computed against a single total — or against their sum — answers
   something nobody asked. Both figures published for this cut before the four were noticed were of that kind:
   one counted a single literal, one a larger assembly, and the cut POSITION they agreed on was the part that
   was real. Place per document, report per document, and let the reader see which. */
const DOCS = ["HTML", "HTML_MIN", "HTML_COLD", "HTML_POPOVER"];
const docs = [];
for (const name of DOCS) {
  const open = lines.findIndex((l) => new RegExp(`^static const char \\*${name}\\s*=\\s*$`).test(l));
  if (open < 0) continue;
  let close = -1;
  for (let i = open + 1; i < lines.length; i++) if (/";\s*$/.test(lines[i])) { close = i; break; }
  if (close < 0) throw new Error(`${name}'s literal has no closing \`";\` after its declaration`);
  docs.push({ name, open, close });
}
if (!docs.length) throw new Error("no fixture document literal found in test_forced.c — every one of " +
  DOCS.join(", ") + " is absent, so this tool would place nothing and say so as a small honest-looking zero.");

/* Character offset per source line, decoding the escapes a raw byte count would miscount. Only the quoted
   run of each line contributes — a `/* … *\/` comment between segments is source and not document. */
for (const d of docs) {
  d.off = new Map();
  d.total = 0;
  for (let i = d.open + 1; i <= d.close; i++) {
    d.off.set(i + 1, d.total);                 /* 1-based source line -> offset BEFORE this line's bytes */
    for (const m of lines[i].matchAll(/"((?:[^"\\]|\\.)*)"/g))
      d.total += m[1].replace(/\\x[0-9a-fA-F]{2}/g, "x").replace(/\\./g, "x").length;
  }
}

const argv = process.argv.slice(2);
const optOf = (n) => { const i = argv.indexOf(n); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : null; };
const after = optOf("--after") ? Number(optOf("--after")) : null;
const only = (optOf("--rows") || "").split(",").filter(Boolean);

/* Row declarations, anywhere in the file: { "name", <pred>, "KEY", SESS_… }. The KEY is the third field. */
const placed = [], unplaced = [];
for (const m of text.matchAll(/\{\s*"([a-z][a-z0-9-]*)"\s*,\s*[^,]+,\s*"([^"]*)"\s*,\s*SESS_/g)) {
  const [, name, key] = m;
  if (only.length && !only.includes(name)) continue;
  let hit = null;
  for (const d of docs) {
    for (let i = d.open + 1; i <= d.close; i++)
      if (key && lines[i].includes(key)) { hit = { doc: d.name, line: i + 1, off: d.off.get(i + 1) ?? -1 }; break; }
    if (hit) break;                            /* FIRST document that mentions it — a key may occur in several */
  }
  (hit ? placed : unplaced).push({ name, key, ...(hit || {}) });
}
placed.sort((a, b) => (a.doc === b.doc ? a.line - b.line : a.doc.localeCompare(b.doc)));

for (const d of docs)
  console.log(`${d.name}: source lines ${d.open + 2}..${d.close + 1}, ${d.total} characters, ` +
              `${placed.filter((r) => r.doc === d.name).length} row(s) placed here`);
console.log(`${placed.length} row(s) placed by KEY across ${docs.length} document(s), ${unplaced.length} unplaced`);
if (after !== null) {
  /* A SOURCE LINE BELONGS TO ONE DOCUMENT, so a split is reported against that one and the others are stated
     as untouched rather than silently counted on either side. */
  const owner = docs.find((d) => after > d.open + 1 && after <= d.close + 1);
  if (!owner) console.log(`source line ${after} is in none of the document literals — no split made`);
  else {
    const mine = placed.filter((r) => r.doc === owner.name);
    const before = mine.filter((r) => r.line < after), at = mine.filter((r) => r.line >= after);
    console.log(`split at source line ${after}, which is inside ${owner.name}: ${before.length} before, ` +
                `${at.length} at or after, and ${placed.length - mine.length} row(s) in the other ` +
                `document(s) are on NEITHER side of it`);
    const cut = owner.off.get(after);
    if (cut !== undefined) console.log(`  that line begins at character ${cut} of ${owner.name}'s ${owner.total} ` +
                                       `(${((cut / owner.total) * 100).toFixed(1)}% through THAT document, ` +
                                       `which is not a fraction of the fixture)`);
  }
}
for (const r of placed) {
  if (after !== null && r.line < after) continue;
  console.log(`  ${r.doc.padEnd(13)} ${String(r.line).padStart(5)}  ${String(r.off).padStart(7)}  ` +
              `${r.name.padEnd(24)} ${r.key}`);
}
if (unplaced.length)
  console.log(`\nUNPLACED (no fixture line contains the KEY) — these are not "late", they are UNLOCATED, and a ` +
              `reach reading says nothing about them: ${unplaced.map((r) => r.name).join(", ")}`);
