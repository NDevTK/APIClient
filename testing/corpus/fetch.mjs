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
 *   - THE DOCUMENT BASE URL OF A DOCUMENT THAT SHIPS A `<base href>`. Finding the first one in tree order is a
 *     question about a PARSED tree and this walk is a matcher over text, so a relative reference whose two
 *     candidate bases disagree is REFUSED rather than resolved under a guess. The refusal is per reference and
 *     is argued at `baseCandidates`.
 *   - A CHARACTER REFERENCE OUTSIDE THE FIVE THIS FILE CARRIES. An attribute value is decoded before it is a
 *     URL, and the full named-character-references table is what decides a run this subset does not hold, so
 *     such a reference is REFUSED. Argued at `decodeAttrRefs`, with what the next diff builds.
 *
 * AND A REFERENCE THAT NEVER BECAME A REQUEST IS RECORDED TOO, WHICH IS THE ONE SHORTFALL THIS FILE USED TO
 * BE UNABLE TO NAME. The frontier was built by a `catch` that returned null into a `.filter(Boolean)`, so a
 * reference this walk could not resolve read as a reference the document did not have — the defaulted-field
 * shape, where the absence becomes a plausible datum and nothing counts it. Such a reference is now a row in
 * `declined` carrying `requested: false`, and the per-site line prints how many there were.
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
const ATTR_HREF = /\bhref\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))/i;
const pick = (m) => (m ? (m[1] ?? m[2] ?? m[3] ?? "") : "");

function documentRefs(html) {
  const out = [];
  for (const m of html.matchAll(TAG_SCRIPT)) { const v = pick(m); if (v) out.push(v); }
  for (const m of html.matchAll(TAG_LINK)) {
    const rel = pick(LINK_REL.exec(m[0])).trim().toLowerCase();
    if (rel !== "preload" && rel !== "modulepreload") continue;
    const href = pick(ATTR_HREF.exec(m[0])); if (href) out.push(href);
  }
  return out;
}

/* A REFERENCE IS AN ATTRIBUTE VALUE, SO THE BYTES IN IT ARE NOT THE BYTES A URL PARSER SEES. The tokenizer
   resolves character references inside an attribute value before any URL exists, so `src="/a&amp;b"` names
   `/a&b`, and a fetcher that asks for the literal `&amp;` asks a question the page never asked. THE HAZARD IS
   NOT THAT THE REQUEST FAILS — IT IS THAT IT ANSWERS: a query whose parameter name is `amp;authorization`
   rather than `authorization` is a well-formed request, so the reply is a real status over real bytes, and a
   row stored under that URL's name is a measurement of an error page wearing a real address.
   WHAT DECIDES IT IS A SPEC ALGORITHM AND NOT A REPLACE CHAIN. HTML §13.2.5.77 "Character reference state"
   sends an alphanumeric run to §13.2.5.78 "Named character reference state", which consumes the longest
   identifier in the named character references table of §13.5 "Named character references"; a run matching
   nothing reaches §13.2.5.79 "Ambiguous ampersand state", whose own arms EMIT those characters and whose only
   complaint is that "This is an unknown-named-character-reference parse error." — so an `&` beginning no
   reference is ORDINARY TEXT, and a refusal on `&` would fire on every healthy query string. §13.2.5.78 also
   states the attribute-only arm honoured below: where a match's last character is not a semicolon "and the
   next input character is either a U+003D EQUALS SIGN character (=) or an ASCII alphanumeric, then, for
   historical reasons" it is NOT decoded, which is why `?a=1&ampere=2` keeps its text while `&amp;` and a
   trailing `&amp` do not. Its munch is "Consume the maximum number of characters possible, where the consumed
   characters are one of the identifiers in the first column of the named character references table.", which
   is why a run no table entry is a prefix of is not a reference however much it looks like one.
   NAMED RESIDUAL — WHAT IS NOT COVERED: this carries the five references HTML attribute escaping exists to
   produce and none of the table's other rows, so a SEMICOLON-TERMINATED run outside it is UNDECIDABLE here.
   The full table is what says whether such a run is a reference at all, and either guess stores a URL the page
   never named, so it is REFUSED rather than left silently wrong — a subset that merely under-decodes CERTIFIES
   its survivors, the common case ceasing to show an ampersand run while the rare one stays wrong with nothing
   left to say so. ALSO NOT COVERED: a semicolon-LESS run outside the subset, which §13.2.5.78 decodes when the
   next character is neither `=` nor alphanumeric. It is left alone and NOT refused, because `&b` at the end of
   a query has that exact shape and refusing it would fire on healthy sites.
   WHAT THE NEXT DIFF BUILDS: the named character references table itself, derived from
   engine/lexbor/source/lexbor/html/tokenizer/res.h, which already holds it, rather than typed out a second
   time — and the refusal retires with it. HOW ITS ABSENCE SHOWS: a `declined` row reading
   `undecidable-character-reference`, and a reference the table would have resolved reaching no request. */
const NAMED_REF = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'" };
const ATTR_REF = /&(#[xX][0-9a-fA-F]+|#[0-9]+|[A-Za-z][A-Za-z0-9]*)(;?)/g;

function decodeAttrRefs(raw) {
  let undecidable = null;
  const value = String(raw).replace(ATTR_REF, (m, body, semi, at, whole) => {
    if (body[0] === "#") {
      /* §13.2.5.84 "Numeric character reference end state" answers every out-of-range form with U+FFFD rather
         than with a throw: a surrogate is a surrogate-character-reference parse error, a value above 0x10FFFF
         is character-reference-outside-unicode-range, and both become the replacement character. */
      const hex = body[1] === "x" || body[1] === "X";
      const cp = parseInt(hex ? body.slice(2) : body.slice(1), hex ? 16 : 10);
      return Number.isFinite(cp) && cp > 0 && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)
        ? String.fromCodePoint(cp) : "\ufffd";
    }
    const v = NAMED_REF[body];
    if (v === undefined) {
      /* A run this subset cannot decide is a refusal only where a SEMICOLON makes it a reference outright;
         without one it is the §13.2.5.79 shape an ordinary query string produces. */
      if (semi) undecidable ??= `&${body};`;
      return m;
    }
    /* `body` is the maximal alphanumeric run, so the character after it is never alphanumeric and the
       historical arm reduces to the one test §13.2.5.78 still leaves open. */
    if (!semi && whole[at + m.length] === "=") return m;
    return v;
  });
  return { value, undecidable };
}

/* THE DOCUMENT BASE URL IS NOT THE DOCUMENT'S ADDRESS, AND A MATCHER OVER TEXT CANNOT TELL YOU WHICH IT IS.
   HTML §2.4.3 "Document base URLs": "If document has no descendant base element that has an href attribute,
   then return document's fallback base URL." — "Otherwise, return the frozen base URL of the first base
   element in document that has an href attribute, in tree order." The fallback IS the address, so resolving
   against `finalUrl` is exactly right for a document with no `<base href>` and wrong for one that has one.
   WHY THIS DOES NOT READ THE BASE AND RESOLVE AGAINST IT. The FIRST such element in TREE ORDER is a fact about
   a parsed tree: a `<base>` inside a comment, inside `<script>` or `<textarea>` text, or inside a `<template>`
   — whose contents are a separate fragment and so no descendant of the document — is not the document's base,
   and a matcher over text takes all three. Resolving against one of those would move a CORRECT answer to a
   wrong one on every document that merely mentions a base, a larger population than the documents that ship
   one. §4.2.3 "The base element" does say "A base element, if it has an href attribute, must come before any
   other elements in the tree that have attributes defined as taking URLs." — and an authoring requirement is
   not a thing a fetcher of other people's documents may assume.
   SO THE ANSWER IS A REFUSAL, AND IT IS PER REFERENCE RATHER THAN PER DOCUMENT. A candidate is computed the way
   §4.2.3 computes a frozen base URL — the href parsed against the document's fallback base URL, "Thus, the base
   element isn't affected by itself.", with a parse failure and with a `data:` or `javascript:` scheme alike
   taking the arm that says "then set element's frozen base URL to document's fallback base URL and return."
   — and a reference is fetched only where it resolves to the SAME URL under the
   address and under every candidate. That asks no question of the bases themselves, which is what makes it
   exact: an absolute reference is base-independent by construction, and a `<base href="/">` on a document
   already at `/` changes no resolution, so neither costs anything.
   RETIREMENT: this goes when the document is PARSED rather than matched, because the first `<base href>` in
   tree order is then a fact and no refusal is owed. HOW ITS ABSENCE SHOWS: a `declined` row reading
   `base-element-ambiguous` carrying both resolutions. */
const TAG_BASE = /<base\b[^>]*>/gi;

function baseCandidates(html, fallbackBaseUrl) {
  const bases = new Set();
  let unreadable = 0;
  for (const m of html.matchAll(TAG_BASE)) {
    const hm = ATTR_HREF.exec(m[0]);
    if (!hm) continue;                     /* §2.4.3 counts only a base element that HAS an href attribute */
    const { value, undecidable } = decodeAttrRefs(pick(hm));
    if (undecidable) { unreadable++; continue; }
    let u;
    try { u = new URL(value, fallbackBaseUrl); } catch { bases.add(fallbackBaseUrl); continue; }
    bases.add(u.protocol === "data:" || u.protocol === "javascript:" ? fallbackBaseUrl : u.href);
  }
  return { bases: [...bases], unreadable };
}

/* THE RESOLUTION, AND THE REASON IT RETURNS A REFUSAL RATHER THAN A NULL. What stood here was
   `try { return new URL(h, doc.finalUrl).href } catch { return null }` behind a `.filter(Boolean)`, which
   turned "this reference could not be resolved" into "there was no reference" — the one shortfall of this walk
   that no line of the per-site report could name, beside the depth frontier it counts and the statuses it
   declines. A reference this cannot resolve soundly is now a DECLARED ABSENCE, which is what `declined` is. */
function resolveDocumentRef(h, fallbackBaseUrl, base) {
  const { value, undecidable } = decodeAttrRefs(h);
  if (undecidable)
    return { reason: "undecidable-character-reference",
             note: `${JSON.stringify(h)} carries ${undecidable}, which is not one of the named character `
                 + `references this file can decide; HTML §13.5's table is what says whether it is one at all` };
  /* An absolute reference resolves to itself under every base, so no base question reaches it. */
  try { return { url: new URL(value).href }; } catch { /* relative: the base decides it */ }
  let own;
  try { own = new URL(value, fallbackBaseUrl).href; }
  catch { return { reason: "unresolvable-reference",
                   note: `${JSON.stringify(value)} does not resolve against ${fallbackBaseUrl}` }; }
  if (base.unreadable)
    return { reason: "base-element-ambiguous",
             note: `${base.unreadable} apparent <base href> carries a character reference this file cannot `
                 + `decide, so the base this relative reference resolves against is unknown` };
  for (const b of base.bases) {
    let under = null;
    try { under = new URL(value, b).href; } catch { /* a base this reference cannot resolve against at all */ }
    if (under !== own)
      return { reason: "base-element-ambiguous",
               note: `${own} under the document's own address, ${under === null ? "unresolvable" : under} `
                   + `under an apparent <base href> whose frozen base URL is ${b}` };
  }
  return { url: own };
}

/* A MODULE SPECIFIER IS NEITHER OF THE TWO QUESTIONS ABOVE, AND IS KEPT APART SO NEITHER ANSWER CAN BE READ
   FOR THE OTHER. It is JS source rather than an attribute value, so no character reference was ever resolved
   in it; and it resolves against the MODULE'S OWN URL rather than against any document base. What changes here
   is only that a specifier this cannot resolve is now declared instead of dropped. */
function resolveImportSpec(s, moduleUrl) {
  try { return { url: new URL(s, moduleUrl).href }; }
  catch { return { reason: "unresolvable-specifier",
                   note: `${JSON.stringify(s)} does not resolve against ${moduleUrl}` }; }
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
    let nRefused = 0;

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
    /* A REFUSAL IS NOT A ZERO-STATUS RESPONSE, AND IT MAY NOT BORROW ONE'S SHAPE. `get`'s catch arm already
       records `status: 0` for a request that WAS made and threw, so reusing that row would merge "the network
       failed" with "no request was ever composed" — two facts behind one answer, which is the shape this
       file's header refuses a few paragraphs up for an unclassified essence. A refusal carries the REFERENCE
       as the document spelled it, says it was not requested, and carries nothing a response would have. */
    const refuse = (reference, reason, note) => {
      rec.declined.push({ reference, requested: false, reason, note });
      nRefused++;
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
    const html = doc.body.toString("utf8");
    const base = baseCandidates(html, doc.finalUrl);
    let frontier = [];
    for (const h of documentRefs(html)) {
      const got = resolveDocumentRef(h, doc.finalUrl, base);
      if (got.url) frontier.push(got.url); else refuse(h, got.reason, got.note);
    }
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
            const refs = [];
            for (const s of importRefs(r.body.toString("utf8"))) {
              const got = resolveImportSpec(s, r.finalUrl);
              if (got.url) refs.push(got.url); else refuse(s, got.reason, got.note);
            }
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
        + `+ ${nDocSaved} document(s), declined ${rec.declined.length} (${nRefused} reference(s) `
        + `refused unresolved, never requested), `
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
