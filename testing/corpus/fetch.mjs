/* FETCH THE PROGRAMS A SITE'S OWN DOCUMENT NAMES, INTO A CORPUS engine/corpus_programs.mjs CAN TYPE.
 *
 * IT EXISTS BECAUSE A MEASUREMENT OUTLIVED ITS INSTRUMENT. `engine/absentrank.mjs` and
 * `engine/nsguardrank.mjs` both rank an absence against a directory of real bundles, and the only thing that
 * ever produced one was deleted with the committed mirror. Their figures went on being quoted afterwards, and
 * CLAUDE.md §A-MEASUREMENT-CAN-OUTLIVE-ITS-INSTRUMENT rates that worse than a stale number: the figure is
 * true, the method is sound, and a reader who re-derives it finds nothing — so a measurement nobody made and
 * one whose tool is gone render identically. The tell it gives is exact and was exactly this file's absence:
 * you can state a number and cannot state the PATH, tracked, that a fresh clone would contain.
 *
 * SO THE DRIVER IS COMMITTED AND THE CORPUS IS NOT, AND THAT SPLIT IS THE WHOLE DESIGN. Other people's
 * bundles are not this project's to commit — testing/corpus/README.md is the decision and this file is built
 * to obey it, not to work around it. What a fresh clone gets is a LIST OF ADDRESSES and a way to visit them;
 * what it never gets is somebody else's site. The output therefore goes under `engine/.work/`, ignored,
 * beside the other corpora this tree fetches at run time rather than vendors.
 *
 * A CORPUS FETCHED TODAY IS A FACT ABOUT TODAY. Every record carries the instant it was fetched and the
 * manifest carries the run's, because a figure read out of this corpus is a fact about one hour of one day
 * and about the sites that answered in it — never a property of "real bundles". Two runs a week apart are two
 * corpora, and a before/after across them measures the sites as much as the engine.
 *
 * WHAT IS SAVED IS THE SERVER'S ANSWER AND NEVER A SUFFIX, AND THE SET IS IMPORTED RATHER THAN RESTATED.
 * engine/corpus_programs.mjs owns which Content-Type essences are programs and documents; it is the consumer,
 * so it is the artifact that owns the fact, and a list retyped here would be the second copy whose drift
 * nobody would ever run against reality. It is imported.
 *
 * AND THE CONSUMER'S OWN THROW CANNOT FIRE ON BYTES THAT NEVER REACH DISK, WHICH IS THE ONE WAY THIS FILE
 * CAN DEFEAT IT. corpus_programs.mjs refuses an essence it does not classify, and that refusal is what stops
 * a script type nobody listed from shrinking the corpus in silence — a smaller absence reads as progress.
 * A fetcher that declines such a response upstream moves the drop to where no throw is watching. So an
 * essence in NEITHER the program/document sets NOR the excluded set is printed by URL and THROWS at the end
 * of the run, after the corpus and its manifest are written: the corpus is still usable and its reader is
 * told, by name, that it is short and which file to widen.
 *
 * ITS LAST ACT IS TO RUN THE CONSUMER ON ITS OWN OUTPUT. A manifest shape restated in prose here would be a
 * second copy of corpus_programs.mjs's contract; calling that file is the derivation itself, and its four
 * accounting throws (every file typed, no digest typed twice, every essence classified, the parts summing to
 * the files walked) are then this file's own exit criteria. A run that writes a manifest the consumer refuses
 * has not produced a corpus, and says so rather than leaving the refusal for the next reader to meet.
 *
 * WHAT IT CANNOT SEE, STATED BECAUSE THE FIGURES DRAWN FROM IT WILL BE QUOTED:
 *   - A BUNDLE REACHED ONLY BY RUNNING THE DOCUMENT. Discovery is `<script src>`, every `<link rel=preload>`
 *     and `<link rel=modulepreload>` whatever its `as`, and the static import specifiers of the programs
 *     those name. A page whose bootstrap builds its script elements at run time contributes its document and
 *     no programs, and the per-site line says so by reporting 0 programs rather than by staying quiet.
 *   - HOW DEEP THE IMPORT WALK WENT. `--import-depth` (default 1) is a STATED limit and the specifiers left
 *     unfetched at the frontier are COUNTED and printed, so the truncation is a number a reader can quote
 *     instead of a silence.
 *   - A BARE SPECIFIER. `import x from "lodash"` names nothing a browser can resolve without an import map,
 *     so it is not a URL this can fetch; that is the web's rule and not a shortfall of this walk.
 *   - A SPECIFIER INSIDE A STRING. Import extraction is a regex over text, so a bundle that embeds source AS
 *     DATA offers specifiers no page evaluates. The cost is bounded and visible: a wrong URL is a 404 that
 *     lands in `declined` with its status, never a file in the corpus.
 *
 * NON-OK AND DECLINED RESPONSES ARE RECORDED AND ARE NOT IN `resources`. corpus_programs.mjs builds its
 * essence map from every row it can see, so a row whose bytes did not reach disk can still make a digest
 * carry two essences and refuse a file that is not ambiguous at all. Provenance is kept without manufacturing
 * that: what was saved goes in `resources`, what was not goes in `declined`, which the consumer does not read.
 *
 * NODE'S BUILT-IN FETCH IGNORES `HTTPS_PROXY` UNLESS TOLD TO READ IT, and a fetcher that silently bypasses a
 * proxy does not fail loudly — it times out, which renders as a site that is down. The mismatch is asserted
 * at startup rather than discovered as a corpus of 19 dead rows.
 *
 *   NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs [--out <dir>] [--import-depth N]
 *                                                                    [--only id,id] [--concurrency N]
 *   node engine/absentrank.mjs --corpus <out>/mirror
 */
import { writeFileSync, mkdirSync, rmSync, existsSync, readdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve, relative } from "node:path";
import { siteList } from "./list.mjs";
import { corpusPrograms, PROGRAM, DOCUMENT, EXCLUDED, essenceOf } from "../../engine/corpus_programs.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..", "..");

const argOf = (flag, dflt) => {
  const i = process.argv.indexOf(flag);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt;
};
const say = (s) => console.log(`[corpus-fetch] ${s}`);
const sha256 = (buf) => createHash("sha256").update(buf).digest("hex");

/* A NAME ON DISK IS FOR A HUMAN OPENING THE SITE BEHIND A ROW, AND FOR NOTHING ELSE. corpus_programs.mjs
   joins the manifest to the files by CONTENT, so nothing downstream parses this string and no rule here can
   disagree with one over there. The manifest records the name that was WRITTEN rather than the one it was
   derived from, which is the condition corpus_programs.mjs's own header names for retiring its second trap. */
const saneName = (u) => {
  const url = new URL(u);
  let s = `${url.host}${url.pathname}${url.search}`.replace(/[^A-Za-z0-9._/@%+-]/g, "_");
  if (s.endsWith("/")) s += "index";
  return s.replace(/^\/+/, "").slice(0, 180);
};

/* `<script src>` AND EVERY PRELOAD, WITH A REGEX ENGINE AND NEVER A LINE-ORIENTED GREP. A `<script>` tag may
   put its `src=` on a line of its own — testing/corpus/apps.tsv records a page whose five real scripts are
   spelled across seven lines each, for which a `grep -oE` answered a confident ZERO and a whole row carried
   the wrong verdict because of it. In a JS regex `[^>]` already crosses newlines; the tell that it matters is
   that the same pattern in a line-oriented tool does not.
   EVERY PRELOAD, WHATEVER ITS `as`: apps.tsv records that clause being written as `as=script` while the
   matcher never read `as` at all, and keeping the fonts and stylesheets is right — what decides whether a
   response is saved is the type the server states for it, decided after it answers and not before. */
const TAG_SCRIPT = /<script\b[^>]*\bsrc\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))[^>]*>/gi;
const TAG_LINK = /<link\b[^>]*>/gi;
const LINK_REL = /\brel\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))/i;
const LINK_HREF = /\bhref\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))/i;
const pick = (m) => (m ? (m[1] ?? m[2] ?? m[3] ?? "") : "");

function documentRefs(html) {
  const out = [];
  for (const m of html.matchAll(TAG_SCRIPT)) { const v = pick(m); if (v) out.push(v); }
  for (const m of html.matchAll(TAG_LINK)) {
    const rel = pick(LINK_REL.exec(m[0])).trim().toLowerCase();
    if (rel !== "preload" && rel !== "modulepreload") continue;
    const href = pick(LINK_HREF.exec(m[0])); if (href) out.push(href);
  }
  return out;
}

/* STATIC IMPORT SPECIFIERS. A regex over text, which is what this file's header declares it to be: the cost
   of a false one is a 404 in `declined`, and the cost of a missed one is a program the frontier count says
   was left. A BARE specifier is skipped because no browser resolves one without an import map.
   IT ANCHORS ON `from` AND NOT ON `import`, WHICH IS WHAT REMOVES THE ONLY MAGIC NUMBER THIS FILE HAD. The
   obvious pattern bridges `import`/`export` to its `from` across some span of text, and a minified
   re-export's clause has no bound anybody can name — so whatever span is chosen, a longer clause is a
   specifier this walk silently does not see, which is the quietly-smaller-corpus direction. `from`
   IMMEDIATELY followed by a string literal is the shape both forms end in, it needs no bridge, and it
   cannot be spelled by anything else that parses: `x.from"s"` is not a program. */
const FROM_SPEC = /\bfrom\s*(?:"([^"\n]*)"|'([^'\n]*)'|`([^`\n]*)`)/g;
const DYNAMIC_IMPORT = /\bimport\s*\(\s*(?:"([^"\n]*)"|'([^'\n]*)'|`([^`\n]*)`)/g;
const BARE_IMPORT = /\bimport\s*(?:"([^"\n]*)"|'([^'\n]*)'|`([^`\n]*)`)/g;

function importRefs(src) {
  const out = new Set();
  for (const re of [FROM_SPEC, DYNAMIC_IMPORT, BARE_IMPORT]) {
    re.lastIndex = 0;
    for (const m of src.matchAll(re)) {
      const s = m[1] ?? m[2] ?? m[3] ?? "";
      if (!s) continue;
      if (/^(?:\.{1,2}\/|\/)/.test(s) || /^https?:\/\//i.test(s)) out.add(s);
    }
  }
  return [...out];
}

const TIMEOUT_MS = Number(argOf("--timeout", "30000"));

async function get(url) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), TIMEOUT_MS);
  try {
    /* SAFE METHOD ONLY. RFC 9110 §9.2.1 Safe Methods' safe set is what a corpus fetch is entitled to, and
       this file never composes another: it asks for documents and programs a page's own markup names. */
    const r = await fetch(url, { method: "GET", redirect: "follow", signal: ctl.signal,
                                 headers: { "accept": "*/*" } });
    const body = Buffer.from(await r.arrayBuffer());
    return { ok: r.ok, status: r.status, contentType: r.headers.get("content-type") || "",
             finalUrl: r.url || url, body };
  } catch (e) {
    return { ok: false, status: 0, contentType: "", finalUrl: url, body: Buffer.alloc(0),
             error: String((e && e.message) || e) };
  } finally { clearTimeout(t); }
}

async function pool(items, n, fn) {
  const out = new Array(items.length);
  let i = 0;
  await Promise.all(Array.from({ length: Math.min(n, items.length) }, async () => {
    for (;;) { const k = i++; if (k >= items.length) return; out[k] = await fn(items[k], k); }
  }));
  return out;
}

export async function run() {
  if (process.env.HTTPS_PROXY && !process.env.NODE_USE_ENV_PROXY)
    throw new Error("[corpus-fetch] HTTPS_PROXY is set and NODE_USE_ENV_PROXY is not. Node's built-in fetch "
      + "does not read the environment proxy without it, so every request would go direct and TIME OUT — a "
      + "corpus of dead rows that reads as sites being down rather than as this process asking the wrong "
      + "way. Re-run as `NODE_USE_ENV_PROXY=1 node testing/corpus/fetch.mjs ...`.");

  const OUT = resolve(argOf("--out", join(ROOT, "engine", ".work", "sitecorpus")));
  const MIRROR = join(OUT, "mirror");
  const DEPTH = Number(argOf("--import-depth", "1"));
  const CONC = Number(argOf("--concurrency", "6"));
  const only = argOf("--only", null);
  const onlyIds = only ? new Set(only.split(",").map((s) => s.trim()).filter(Boolean)) : null;

  const list = siteList();
  const rows = onlyIds ? list.rows.filter((r) => onlyIds.has(r.id)) : list.rows;
  if (!rows.length) throw new Error(`[corpus-fetch] --only ${only} names no row of ${list.path}.`);

  /* A RECURSIVE DELETE OF A CALLER-SUPPLIED PATH IS THE BANNED DESTROY WEARING A `--out`. A fresh corpus
     must replace the old one whole — a half-overwritten mirror is a manifest that disagrees with its files,
     which is exactly the drift corpus_programs.mjs refuses — and CLAUDE.md is unambiguous that a command
     which erases somebody else's bytes with no record is not one this project writes. So the delete is
     GATED on the directory being one THIS tool made, evidenced by the two files it always writes, and an
     absent or empty directory needs no gate at all. Anything else THROWS with the path in it: naming a
     fresh `--out` costs the caller nothing and reconstructing what was there costs them everything. */
  if (existsSync(OUT)) {
    const mine = existsSync(join(OUT, "provenance.json")) && existsSync(join(OUT, "run.json"));
    const empty = readdirSync(OUT).length === 0;
    if (!mine && !empty)
      throw new Error(`[corpus-fetch] ${OUT} already exists and is not a corpus this tool wrote (it carries `
        + `no provenance.json + run.json pair). Refusing to delete it recursively: a --out that lands on `
        + `somebody's directory would erase it with no record, which is the one operation CLAUDE.md bans `
        + `outright. Pass a --out that is empty, absent, or a previous corpus from this tool.`);
    rmSync(OUT, { recursive: true, force: true });
  }
  mkdirSync(MIRROR, { recursive: true });

  const startedAt = new Date().toISOString();
  say(`list ${list.path} — ${rows.length} row(s)${onlyIds ? ` (--only ${only})` : ""}`);
  say(`out ${OUT} — import depth ${DEPTH}, concurrency ${CONC}, timeout ${TIMEOUT_MS}ms`);
  say(`started ${startedAt} — every figure taken from this corpus is a fact about this instant and about `
      + `the sites that answered in it`);

  const manifest = [];
  const unclassified = new Map();

  for (const row of rows) {
    const siteDir = join(MIRROR, row.id);
    mkdirSync(siteDir, { recursive: true });
    const rec = { id: row.id, url: row.url, fetchedAt: new Date().toISOString(),
                  resources: [], declined: [], frontierUnfetched: [] };
    const seenUrl = new Set();
    const written = new Map();          /* savedPath -> sha256, so one name is never two bodies */

    const save = (r, url) => {
      const e = essenceOf(r.contentType);
      const d = sha256(r.body);
      /* ONE NAME IS NEVER TWO BODIES. Two URLs can sanitize to one string, and the digest is what separates
         them: distinct bytes have distinct digests, so a single suffix always resolves it. */
      let name = saneName(url);
      if (written.has(name) && written.get(name) !== d) name = `${name}~${d.slice(0, 8)}`;
      written.set(name, d);
      const p = join(siteDir, name);
      mkdirSync(dirname(p), { recursive: true });
      writeFileSync(p, r.body);
      return { url, finalUrl: r.finalUrl, status: r.status, contentType: r.contentType, essence: e,
               sha256: d, bytes: r.body.length, savedPath: relative(MIRROR, p),
               fetchedAt: new Date().toISOString() };
    };
    const decline = (r, url, reason) => {
      rec.declined.push({ url, finalUrl: r.finalUrl, status: r.status, contentType: r.contentType,
                          essence: essenceOf(r.contentType), bytes: r.body.length,
                          sha256: r.body.length ? sha256(r.body) : null, reason,
                          ...(r.error ? { error: r.error } : {}) });
      if (reason === "unclassified-essence" && !unclassified.has(essenceOf(r.contentType)))
        unclassified.set(essenceOf(r.contentType), url);
    };

    /* THE DOCUMENT. It is the site record's own sha256/contentType — corpus_programs.mjs reads the document's
       type off the SITE and not off a resource row, so it is recorded there and nowhere else. */
    seenUrl.add(row.url);
    const doc = await get(row.url);
    if (!doc.ok) {
      decline(doc, row.url, "status");
      rec.status = doc.status; rec.contentType = doc.contentType;
      manifest.push(rec);
      say(`  ${row.id.padEnd(12)} document HTTP ${doc.status || "ERR"}${doc.error ? ` (${doc.error})` : ""} `
          + `— no programs; the block is an observation, not an absence`);
      continue;
    }
    const docEssence = essenceOf(doc.contentType);
    if (!DOCUMENT.has(docEssence) && !PROGRAM.has(docEssence)) {
      decline(doc, row.url, EXCLUDED.has(docEssence) ? "excluded-essence" : "unclassified-essence");
      rec.status = doc.status; rec.contentType = doc.contentType;
      manifest.push(rec);
      say(`  ${row.id.padEnd(12)} document served ${JSON.stringify(doc.contentType)}, which is not a document `
          + `type — nothing saved`);
      continue;
    }
    const docSaved = save(doc, row.url);
    Object.assign(rec, { status: doc.status, finalUrl: doc.finalUrl, contentType: doc.contentType,
                         essence: docEssence, sha256: docSaved.sha256, bytes: docSaved.bytes,
                         savedPath: docSaved.savedPath });

    /* THE FRONTIER: the document's own script and preload references, then the static imports of whatever
       those turn out to be, to the stated depth. */
    let frontier = documentRefs(doc.body.toString("utf8"))
      .map((h) => { try { return new URL(h, doc.finalUrl).href; } catch { return null; } })
      .filter(Boolean);
    let nProgSaved = 0, nDocSaved = 0;

    for (let depth = 0; depth <= DEPTH; depth++) {
      const todo = [...new Set(frontier)].filter((u) => !seenUrl.has(u) && /^https?:/i.test(u));
      for (const u of todo) seenUrl.add(u);
      if (!todo.length) break;
      const got = await pool(todo, CONC, (u) => get(u));
      const next = [];
      for (let k = 0; k < todo.length; k++) {
        const u = todo[k], r = got[k];
        if (!r.ok) { decline(r, u, "status"); continue; }
        const e = essenceOf(r.contentType);
        if (PROGRAM.has(e) || DOCUMENT.has(e)) {
          const row2 = save(r, u);
          rec.resources.push(row2);
          if (PROGRAM.has(e)) nProgSaved++; else nDocSaved++;
          if (PROGRAM.has(e)) {
            const refs = importRefs(r.body.toString("utf8"))
              .map((s) => { try { return new URL(s, r.finalUrl).href; } catch { return null; } })
              .filter(Boolean);
            if (depth < DEPTH) next.push(...refs);
            else for (const f of refs) if (!seenUrl.has(f)) rec.frontierUnfetched.push(f);
          }
          continue;
        }
        decline(r, u, EXCLUDED.has(e) ? "excluded-essence" : "unclassified-essence");
      }
      frontier = next;
    }

    manifest.push(rec);
    const bytes = rec.resources.reduce((a, b) => a + b.bytes, rec.bytes || 0);
    say(`  ${row.id.padEnd(12)} doc ${doc.status} ${JSON.stringify(docEssence)} — saved ${nProgSaved} program(s) `
        + `+ ${nDocSaved} document(s), declined ${rec.declined.length}, `
        + `${[...new Set(rec.frontierUnfetched)].length} specifier(s) left at the depth-${DEPTH} frontier, `
        + `${bytes} byte(s)`);
  }

  writeFileSync(join(OUT, "provenance.json"), JSON.stringify(manifest, null, 1));
  const finishedAt = new Date().toISOString();
  writeFileSync(join(OUT, "run.json"), JSON.stringify(
    { startedAt, finishedAt, list: list.path, sites: rows.map((r) => r.id), importDepth: DEPTH,
      argv: process.argv.slice(2) }, null, 1));

  /* THE CONSUMER IS THE CALIBRATION. Nothing above restates corpus_programs.mjs's contract, so the only
     statement this file can make about having met it is to hand it the output and print what it answers. */
  const cp = corpusPrograms(MIRROR, "corpus-fetch");
  console.log("");
  say(`corpus ${MIRROR}`);
  say(`engine/corpus_programs.mjs accepts it — ${cp.files.length} file(s), ${cp.bytes} byte(s): `
      + `${cp.nProgram} program + ${cp.nDocument} document, ${cp.nExcluded} other, ${cp.onDisk} on disk, `
      + `${cp.manifestRows} manifest row(s)`);
  say(`fetched ${startedAt} .. ${finishedAt} over ${rows.map((r) => r.id).join(" ")}`);
  const shown = relative(ROOT, MIRROR);
  say(`next: node engine/absentrank.mjs --corpus ${shown.startsWith("..") ? MIRROR : shown}`);

  if (unclassified.size) {
    say("");
    for (const [e, u] of unclassified)
      say(`UNCLASSIFIED ESSENCE ${JSON.stringify(e)} — first at ${u}`);
    throw new Error(`[corpus-fetch] ${unclassified.size} Content-Type essence(s) this corpus met are in none `
      + `of engine/corpus_programs.mjs's three sets, so they were not saved. That file's own THROW cannot `
      + `fire on bytes that never reach disk, which is the one way a fetcher can shrink a corpus in silence `
      + `— a smaller absence reads as progress. The corpus and its manifest ARE written and are usable; they `
      + `are SHORT by the responses named above. Read each one and add its essence to PROGRAM, DOCUMENT or `
      + `EXCLUDED in engine/corpus_programs.mjs, then re-run.`);
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url))
  run().catch((e) => { console.error(String((e && e.message) || e)); process.exit(1); });
