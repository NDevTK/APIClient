/* WHICH FILES IN THE FROZEN MIRROR ARE PROGRAMS — TAKEN FROM THE ARTIFACT THAT OWNS THAT FACT.
 *
 * engine/absentrank.mjs and engine/nsguardrank.mjs both rank an absence against testing/corpus/mirror, and
 * both used to answer "is this file a program" from its FILENAME EXTENSION. That is the wrong artifact. The
 * mirror is built by testing/corpus/mirror.mjs, which folds a URL's query string into the saved name as a
 * `__q<sha256[0:8]>` suffix -- so a bundle fetched with a query is saved as `all.js__q54b3907e`, whose
 * extension is `.js__q54b3907e` and is in no list anybody would write. THREE genuine shipped bundles are
 * spelled that way in the committed mirror, one of them a 1.5 MB worker, and an extension filter drops all
 * three in silence. A corpus that is quietly smaller reports a smaller absence, and a smaller absence reads
 * as progress: the one direction nothing here would have caught.
 *
 * THE FACT IS STATED IN testing/corpus/provenance.json, WHICH RECORDS THE SERVER'S OWN `Content-Type` FOR
 * EVERY RESOURCE IT SAVED. A filename is this tree's guess at what bytes are; a `Content-Type` is what the
 * origin server said they are, which is also what decides whether a browser COMPILES them. So the population
 * comes from the manifest and the classification is the server's, not ours.
 *
 * IT JOINS ON CONTENT (sha256) AND NEVER ON PATH, AND THAT IS THE LOAD-BEARING CHOICE RATHER THAN A DETAIL.
 * Two separate traps sit on the path route and the second one bites:
 *   1. Re-deriving the saved name from a URL would be a SECOND COPY of a rule this tree deliberately keeps in
 *      one place. testing/corpus/serve-faithful.mjs states it and recomputes it per request "rather than
 *      keeping a second index that could disagree with the tree", and engine/pagecensus.mjs refuses to copy
 *      it in as many words: "A path mapping here would be a second copy of that rule, and the copy anyone
 *      writes first is the one that drops the query." That refusal is a DECISION and this file is consistent
 *      with it: nothing here knows what `__q` means.
 *   2. USING THE MANIFEST'S OWN `path` FIELD LOOKS LIKE IT DODGES (1) AND SILENTLY REGRESSES. mirror.mjs
 *      writes the file at `rel.replace(/[^A-Za-z0-9._\/@%+-]/g, '_')` and writes `path: rel` -- the
 *      UNSANITIZED string -- into the manifest, and recomputes that same substitution wherever it needs the
 *      name back. So the manifest's `path` is NOT the path on disk whenever the URL carries a character
 *      outside that class. MEASURED over the committed mirror: three files differ that way -- two
 *      astexplorer chunks spelled with `~` and one figma chunk spelled with `(` and `)` -- and all three are
 *      read correctly TODAY, because their sanitized names still end in `.js`. A repair that joined on `path`
 *      would have dropped three working files while adding three missing ones and reported a net of zero.
 * A digest join needs neither rule. It asks the bytes what they are, so it cannot disagree with a naming
 * convention it never consults, and it is the same move this project makes when it refuses to verify a push
 * by pointer: ask the content, never the name.
 * RETIREMENT: trap 2 goes when mirror.mjs records the name it actually WROTE rather than the one it derived
 * the write from, at which point `path` and the file on disk are the same string and a path join can no
 * longer be silently wrong. Trap 1 does not retire while the saved name is computed from a URL anywhere.
 *
 * ITS COMPLETENESS IS ASSERTED RATHER THAN CLAIMED, BECAUSE THE FAILURE THIS REPLACES WAS A SILENT SHORTFALL.
 * Swapping one selector for another buys nothing if the new one can also go quietly short, so three
 * invariants run before any caller sees a file list and each one THROWS:
 *   - EVERY file under the corpus has its digest in the manifest. A file the manifest cannot type is a file
 *     this would otherwise have to guess about, and it means the mirror and its manifest have drifted apart.
 *   - NO digest carries two different essences. If one blob is served as two types the join is ambiguous, and
 *     picking one would be this file inventing a fact the manifest does not state.
 *   - EVERY essence present is CLASSIFIED. An unclassified type is a THROW naming it, never a quiet
 *     exclusion, so mirroring a site that serves a script as some essence not seen here REDDENS this instead
 *     of shrinking the corpus by one bundle. That is the whole point: the list below is short because it is
 *     complete over what the corpus holds, and it stays complete because it cannot be outgrown in silence.
 * The parts are then summed against the file count, so none of the three can pass while the split is wrong.
 *
 * THE CLASSIFICATION IS THIS CORPUS'S OWN VOCABULARY AND IS DELIBERATELY NOT A MIME LIBRARY. The two groups
 * that matter here are both defined in MIME Sniffing §4.6 "MIME type groups", which says of the first:
 * "A JavaScript MIME type is any MIME type whose essence is one of the following:" over a ten-entry list, and
 * gives the second exactly one essence, `text/html`. This tree already states the JavaScript list once, for the CORB gate in
 * extension/lib/safe-fetch.js, which is the shipped chokepoint and the right place for it. Restating all ten
 * entries here would be a third copy of that table in which EIGHT entries no corpus file exercises sit
 * untested -- and an untested entry that is wrong is found by nobody. So each set below holds exactly the
 * essences the committed mirror contains, every one of them exercised on every run, and the assert above is
 * what makes the short list safe. Widening it is one line, and the THROW is what asks for it by name.
 *
 * WHY THE NON-PROGRAM ESSENCES ARE LISTED RATHER THAN DEFAULTED. `EXCLUDED` could be "everything else", and
 * then a new script essence would fall into it and be dropped exactly as the extension filter dropped the
 * `__q` bundles. Naming them is what converts that silent drop into a refusal. The tell that this matters:
 * 23 of the 26 `__q` files in the committed mirror are images, stylesheets and JSON, so a repair keyed on the
 * `__q` spelling rather than on the server's stated type would have swept all 23 into a JavaScript corpus. */
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { join, relative, dirname } from "node:path";

/* The essences the committed mirror actually serves. PROGRAM and DOCUMENT are the files a browser compiles or
   parses; EXCLUDED is everything else the mirror holds. Together they must cover the manifest, and the
   coverage is asserted below rather than assumed. */
const PROGRAM  = new Set(["text/javascript", "application/javascript"]);
const DOCUMENT = new Set(["text/html"]);
const EXCLUDED = new Set(["text/css", "application/json", "font/woff2", "image/svg+xml"]);

/* A MIME type's ESSENCE: the groups in MIME Sniffing §4.6 "MIME type groups" are stated over the essence,
   which MIME Sniffing §4.2 "MIME type miscellaneous" defines as the type, a solidus, and the subtype — so
   every parameter is outside it. The manifest stores the header verbatim, `text/javascript; charset=utf-8`
   included, so the parameters come off here and nowhere else. */
const essenceOf = (contentType) => (contentType || "").split(";")[0].trim().toLowerCase();

const sha256 = (buf) => createHash("sha256").update(buf).digest("hex");

/* WALK EVERY FILE, FILTER NONE. What exists on disk is a question the disk answers with no rule at all, and
   the whole defect being repaired here came from letting a filter answer it. The manifest decides what each
   file IS; the walk only decides what there is. */
const walk = (dir) => {
  const out = [];
  (function rec(d) {
    for (const e of readdirSync(d)) {
      const p = join(d, e);
      if (statSync(p).isDirectory()) { rec(p); continue; }
      out.push(p);
    }
  })(dir);
  return out;
};

/* Returns the mirror's program and document files, in walk order, with the counts a caller prints.
   `tag` is the calling instrument's name and appears in every refusal, because these throws are statements
   about the CORPUS and a reader meeting one needs to know which run stopped. */
export function corpusPrograms(corpusDir, tag) {
  const die = (s) => { throw new Error(`[${tag}] CORPUS CALIBRATION FAILED — ${s}`); };

  const manifestPath = join(dirname(corpusDir), "provenance.json");
  if (!existsSync(manifestPath))
    die(`no provenance.json beside ${corpusDir} (looked at ${manifestPath}). The corpus population is taken ` +
        `from the manifest's recorded Content-Type, so without it this cannot say which files are programs — ` +
        `and guessing from the filename is the defect this replaces. Point --corpus at a mirror whose ` +
        `manifest sits beside it.`);
  let manifest;
  try { manifest = JSON.parse(readFileSync(manifestPath, "utf8")); }
  catch (e) { die(`${manifestPath} did not parse as JSON (${e.message}).`); }
  if (!Array.isArray(manifest) || !manifest.length)
    die(`${manifestPath} is not a non-empty array of site records — a manifest that is not there reads as a ` +
        `corpus with no programs in it.`);

  /* digest -> the essence(s) the manifest states for those bytes, from the site DOCUMENTS and from every
     resource that carries one. mirror.mjs writes the document as index.html and gives it no `path` in the
     manifest, so the document's type is on the SITE record and is read there; deriving a path the builder
     never used is the trap the header names. A SET rather than a value because the disagreement it would
     hide is asserted below, and only for the digests that actually reach disk. */
  const essence = new Map();
  let rows = 0;
  const note = (digest, ct, where) => {
    if (!digest) return;
    rows++;
    if (!essence.has(digest)) essence.set(digest, new Map());
    essence.get(digest).set(essenceOf(ct), where);
  };
  for (const site of manifest) {
    note(site.sha256, site.contentType, `document of ${site.id}`);
    for (const r of site.resources || []) note(r.sha256, r.contentType, `${site.id} ${r.url || "(no url)"}`);
  }
  if (!essence.size) die(`${manifestPath} records no sha256 for any document or resource, so nothing on disk ` +
                         `can be typed from it.`);

  const all = walk(corpusDir);
  if (!all.length) die(`no files at all under ${corpusDir} — a corpus that is not there reads as a corpus ` +
                       `with no uses.`);

  /* BOTH ASSERTS BELOW ARE SCOPED TO THE BYTES THAT REACH DISK, and that scope is a finding rather than a
     convenience. Asserting over every row the manifest holds was written first and FIRED IMMEDIATELY on its
     author: a manifest row records what was FETCHED, and mirror.mjs records rows whose bytes it did NOT
     save, so the manifest names image, video and font essences that no file on disk carries. (Measured when
     this landed, over the then-committed mirror: 206 such rows and fifteen such essences. The figures move
     with the corpus; the mechanism does not, which is why the assert is scoped and not tuned.) Classifying
     those would put entries in the sets below that NO RUN EXERCISES, which is
     the third-copy-of-a-MIME-table problem the header refuses one paragraph earlier, arriving through the
     assert instead of through the list. A row with no bytes on disk cannot cause a file to be dropped, so it
     is not what these guard. */
  const seen = [];
  const orphans = [];
  for (const p of all) {
    const buf = readFileSync(p);
    const m = essence.get(sha256(buf));
    if (m === undefined) { orphans.push(p); continue; }
    seen.push([p, m]);
  }
  /* A FILE THE MANIFEST CANNOT TYPE IS A FILE THIS WOULD HAVE TO GUESS ABOUT. It means the mirror and its
     manifest have drifted — bytes written by hand, or a manifest regenerated without its files — and either
     way a guess here would be the filename filter coming back under a new name. */
  if (orphans.length)
    die(`${orphans.length} file(s) under ${corpusDir} have no matching sha256 in ${manifestPath}, so the ` +
        `manifest cannot say what they are: ` +
        `${orphans.slice(0, 5).map((p) => relative(corpusDir, p)).join(", ")}` +
        `${orphans.length > 5 ? `, and ${orphans.length - 5} more` : ""}. The mirror and its manifest have ` +
        `drifted; re-run testing/corpus/mirror.mjs rather than typing these from their names.`);
  for (const [p, m] of seen)
    if (m.size > 1)
      die(`${relative(corpusDir, p)} has bytes the manifest types two ways — ` +
          `${[...m.keys()].sort().map((e) => JSON.stringify(e)).join(" and ")} ` +
          `(${[...m.values()].join("; ")}). The join is by content, so one blob must have one type; picking ` +
          `either would be this file inventing a fact the manifest does not state.`);

  /* EVERY ESSENCE THAT REACHES DISK IS CLASSIFIED, checked before anything is partitioned so the message
     names the TYPE to decide about rather than a file that happens to carry it. */
  const unclassified = new Map();
  for (const [p, m] of seen) {
    const e = [...m.keys()][0];
    if (!PROGRAM.has(e) && !DOCUMENT.has(e) && !EXCLUDED.has(e) && !unclassified.has(e))
      unclassified.set(e, relative(corpusDir, p));
  }
  if (unclassified.size)
    die(`${unclassified.size} Content-Type essence(s) on files under ${corpusDir} that this file does not ` +
        `classify: ${[...unclassified].sort().map(([e, p]) => `${JSON.stringify(e)} (e.g. ${p})`).join(", ")}. ` +
        `This is a THROW and not a quiet exclusion BY DESIGN — an unclassified script type would otherwise ` +
        `drop out of the corpus in silence and report a smaller absence, which reads as progress. Add each ` +
        `one to PROGRAM, DOCUMENT or EXCLUDED in engine/corpus_programs.mjs, and read the file before ` +
        `deciding which.`);

  const files = [];
  let bytes = 0, nProgram = 0, nDocument = 0, nExcluded = 0;
  for (const [p, m] of seen) {
    const e = [...m.keys()][0];
    if (PROGRAM.has(e))  { nProgram++;  files.push(p); bytes += statSync(p).size; continue; }
    if (DOCUMENT.has(e)) { nDocument++; files.push(p); bytes += statSync(p).size; continue; }
    nExcluded++;
  }
  if (nProgram + nDocument + nExcluded !== all.length)
    die(`the split does not sum: ${nProgram} program + ${nDocument} document + ${nExcluded} excluded = ` +
        `${nProgram + nDocument + nExcluded}, over ${all.length} file(s) walked.`);
  if (!files.length)
    die(`${all.length} file(s) under ${corpusDir} and the manifest calls none of them a program or a ` +
        `document — a corpus with no programs in it ranks nothing.`);

  return { files, bytes, onDisk: all.length, nProgram, nDocument, nExcluded, manifestRows: rows };
}
