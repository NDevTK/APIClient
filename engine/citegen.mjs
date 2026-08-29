/* Spec-citation gap AUDITOR — the same job engine/idlgen.mjs does for Web IDL, done for the section numbers
 * this tree cites. It reads the REAL spec text, builds an index of what the standard actually numbers and
 * names, and DIFFS every citation in the tree against it. It PRINTS what disagrees; it does not rewrite a
 * comment and it does not generate one.
 *
 *   node engine/citegen.mjs [path …]   audit (default: engine/host); --all prints every finding
 *   node engine/citegen.mjs --regen        fetch the standard, rewrite engine/specindex/html.json
 *
 * WHY THIS EXISTS. CLAUDE.md §Browser half: a named spec with no number cannot be looked up, so it cannot
 * be checked, so it is indistinguishable from a recollection — and a WRONG number is worse than none,
 * because it reads as authoritative and sends the next reader to a section that does not say what the code
 * claims. That failure mode has NO SYMPTOM: nothing crashes, no gate goes red, and the citation looks exactly
 * like a correct one. The only thing that can catch it is the spec text itself, so this reads the spec text.
 *
 * THE CHECK THAT MATTERS IS TERM ATTRIBUTION, NOT NUMBER VALIDITY. A wrong number is almost never a number the
 * standard does not have — it is a REAL section cited for an algorithm that lives in a DIFFERENT one, and
 * a checker that only asks "does §7.3.1 exist?" answers yes and reports nothing. `determine the origin`
 * was cited as §7.3.1 "Navigables" across five files; the standard defines it under §7.3.2.1
 * "Creating browsing contexts", one heading over, and both numbers are real. So the index is keyed by the
 * standard's own <dfn>s — every term the spec defines, filed under the section whose heading it sits
 * beneath — and a citation that names an algorithm is checked against where that algorithm IS.
 *
 * WHAT IS CHECKABLE OFFLINE. The index is COMMITTED and the audit never touches the network, for idlgen.mjs's
 * reason: a build must work with no network, and fetching 57 spec pages per run is a gate measuring the
 * WHATWG's uptime. --regen is the one command that fetches, and it curls rather than using node's
 * fetch, because curl is what carries this environment's proxy configuration and is what an agent
 * verifying a section number is already required to use. The cost of vendoring is STALENESS, and staleness is
 * then a CHECKABLE fact rather than an invisible one: the index records the standard's own "Last Updated"
 * line and the date it was fetched, the audit prints both in its header, and --regen prints every section
 * whose NUMBER MOVED since the committed index — which is exactly the renumbering hazard §Browser
 * half names as the reason a citation carries its title beside its number.
 *
 * REPORT, NOT FAIL — a departure from idlgen.mjs, argued rather than inherited. idlgen's finding is a
 * MISSING BROWSER CAPABILITY: red is correct, because the tree cannot be right until someone writes C. A
 * citation is prose. A build that fails on a comment is a build in which no lane can land a spelling fix, and
 * it turns every stale line into a stop-the-world event for whoever next touches that directory — which is
 * how a checker gets muted, and a muted checker is worse than none. So the exit code is 0 and the findings are
 * the output. What keeps that honest is §Testing's discipline for any gate: the count is reported
 * beside the revision it was measured at, so it can only go down.
 *
 * ONE SPEC, WELL. HTML only — it carries the most citations here by a wide margin and every measured error
 * so far has been an HTML one; a checker covering eight standards badly reports noise nobody reads. A citation
 * anchored to another standard is SKIPPED AND COUNTED, so what this auditor is blind to is printed rather than
 * assumed to be zero.
 *
 * THE THREE THINGS THAT MADE THIS REPORT NOISE, EACH FIXED AT ITS ROOT AND EACH WORTH KEEPING AS A SHAPE:
 *   — A dfn whose whole content is a link OUT to another standard is an IMPORT, not a definition.
 *     §2.1.9 "Dependencies" re-exports several hundred CSS and DOM terms that way, and indexing them made
 *     one HTML section the answer to every CSS citation in the tree.
 *   — A number-does-not-exist check fires on ABSENCE, which is what a MIS-RESOLVED citation produces, so it
 *     is asked only of an explicitly anchored citation. Term attribution fires on a positive match against a
 *     phrase the standard defines, which is its own evidence that the comment is about that standard.
 *   — A parse is checked against the PAGE, never against a floor. The first version of the TOC parse
 *     dropped every section whose title wraps across a line — §4.8.5 "The iframe element",
 *     §7.4, §4.13.2 — and sailed past a "did we get at least 500" guard while the audit reported
 *     "HTML has no §4.8.5" for 65 correct citations. A floor a broken parse passes is not a check. */
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, mkdirSync, readdirSync, statSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, relative } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);
const INDEX_DIR = join(HERE, "specindex");
const INDEX_FILE = join(INDEX_DIR, "html.json");
const SPEC_BASE = "https://html.spec.whatwg.org/multipage/";

/* ---- shared text normalization -------------------------------------------------------------------------- */

const ENTITIES = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ", "#39": "'", "#x27": "'" };

function decodeEntities(s) {
  return s.replace(/&(#x?[0-9a-fA-F]+|[a-zA-Z]+);/g, (m, e) => {
    const k = e.toLowerCase();
    if (ENTITIES[k] !== undefined) return ENTITIES[k];
    if (k[0] === "#") return String.fromCodePoint(parseInt(k[1] === "x" ? k.slice(2) : k.slice(1), k[1] === "x" ? 16 : 10));
    return m;
  });
}

/* The spec's markup, a citation's prose and a citation's quoted title must all reduce to ONE spelling or a
 * comparison between them means nothing. Curly quotes, hyphens standing in for spaces (`determine-the-origin`
 * for `determine the origin`), possessive `'s`, and the spec's own <code>/<var> wrappers all differ from the
 * plain phrase while naming the same thing. */
function normTerm(s) {
  return decodeEntities(String(s).replace(/<[^>]*>/g, " "))
    .replace(/[‘’“”]/g, "'")
    .replace(/[‐-―]/g, "-")
    .toLowerCase()
    .replace(/[-_/]+/g, " ")
    .replace(/'s\b/g, "")
    .replace(/[^a-z0-9 ()[\]@.]+/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

/* ---- --regen: read the standard ------------------------------------------------------------------------- */

function curl(url) {
  return execFileSync("curl", ["-sSL", "--fail", "--max-time", "120", url], {
    encoding: "utf8", maxBuffer: 64 * 1024 * 1024,
  });
}

function attrHref(raw) {
  return raw.replace(/^["']|["']$/g, "");
}

function regen() {
  const toc = curl(SPEC_BASE);
  const updated = (/Last Updated\s*<span class=pubdate>([^<]*)</.exec(toc) || [])[1];
  if (!updated) throw new Error("the TOC's pubdate did not parse — the index would record no staleness fact");

  /* SECTIONS come from the table of contents, because the TOC is the one place the standard states the number,
   * the title and the page for every section in one pass. */
  const sections = new Map();
  const pages = new Set();
  const tocRe = /<a href=("[^"]*"|'[^']*'|[^ >]+)><span class=secno>([\d.]+)<\/span>([\s\S]*?)<\/a>/g;
  let parsed = 0;
  for (let m; (m = tocRe.exec(toc)); ) {
    parsed++;
    const href = attrHref(m[1]);
    const page = href.split("#")[0];
    const title = decodeEntities(m[3].replace(/<[^>]*>/g, "")).replace(/\s+/g, " ").trim();
    const had = sections.get(m[2]);
    /* Each top-level chapter is listed twice — once in the page's own nav, once in the tree — so a repeat is
     * expected. A repeat that DISAGREES about the title is a parse that has run off the end of an anchor. */
    if (had && had.title !== title) throw new Error(`§${m[2]} parsed as both "${had.title}" and "${title}" — the parse is wrong`);
    if (!had || (!had.page && page)) sections.set(m[2], { title, page, id: href.split("#")[1] || "" });
    if (page) pages.add(page);
  }
  /* THE PARSE IS CHECKED AGAINST THE PAGE, NOT AGAINST A FLOOR. A "did we get at least N" guard is a number
   * about nothing: the first version of this parse dropped every section whose title WRAPS ACROSS A LINE in
   * the TOC markup — §4.8.5 "The iframe element", §7.2, §7.4, §4.13.2 — and it sailed past a 500 floor while
   * the audit reported "HTML has no §4.8.5" for 65 correct citations. Every secno span in the TOC is one
   * section; if the count differs, the parse is wrong and the index must not be written. */
  const secnos = (toc.match(/<span class=secno>/g) || []).length;
  if (parsed !== secnos) {
    throw new Error(`the TOC has ${secnos} secno spans and the parse read ${parsed} of them — the parse is wrong`);
  }

  /* DFNS come from each page, filed under the nearest PRECEDING heading. A heading and its section number are
   * in the same element, so one linear walk over headings and <dfn>s in document order files every term. */
  const dfns = new Map();          /* normalized term -> [section number, …] */
  let dfnCount = 0;
  const sorted = [...pages].sort();
  for (const page of sorted) {
    const body = curl(SPEC_BASE + page);
    const marks = [];
    const headRe = /<h[2-6][^>]*>\s*<span class=secno>([\d.]+)<\/span>/g;
    for (let m; (m = headRe.exec(body)); ) marks.push({ at: m.index, no: m[1] });
    const dfnRe = /<dfn\b[^>]*>([\s\S]{0,400}?)<\/dfn>/g;
    for (let m; (m = dfnRe.exec(body)); ) {
      let lo = 0, hi = marks.length - 1, sec = null;
      while (lo <= hi) { const mid = (lo + hi) >> 1; if (marks[mid].at < m.index) { sec = marks[mid].no; lo = mid + 1; } else hi = mid - 1; }
      if (!sec) continue;
      /* AN IMPORTED TERM IS NOT A DEFINITION. §2.1.9 "Dependencies" re-exports several hundred terms that
       * other standards define — `computed value`, `containing block`, `converting colors` — each as a
       * <dfn> whose whole content is a link OUT to the standard that owns it. Indexing those files half of CSS
       * under one HTML section and made it the answer to every CSS citation in the tree (measured: 472
       * findings, nearly all "§2.1.9 Dependencies"). The marker is structural and holds wherever the spec
       * re-exports, not just in that section: content that is nothing but an absolute link elsewhere. */
      if (/^\s*<a [^>]*href=["']?https?:[\s\S]*<\/a>\s*$/.test(m[1])) continue;
      const term = normTerm(m[1]);
      /* A ONE-WORD term is not a citation check, it is a coincidence generator: `origin`, `document` and
       * `container` are defined by half the platform and appear in every other comment in this tree. The
       * attribution check needs a phrase the standard OWNS, so only multi-word dfns are indexed. */
      if (term.split(" ").length < 2 || term.length > 80) continue;
      dfnCount++;
      const list = dfns.get(term) || [];
      if (!list.includes(sec)) list.push(sec);
      dfns.set(term, list);
    }
    process.stderr.write(`  ${page}: ${marks.length} headings\n`);
  }

  const prev = existsSync(INDEX_FILE) ? JSON.parse(readFileSync(INDEX_FILE, "utf8")) : null;
  const out = {
    spec: "HTML",
    base: SPEC_BASE,
    specUpdated: (updated || "").trim(),
    fetched: new Date().toISOString().slice(0, 10),
    sections: Object.fromEntries([...sections].sort((a, b) => cmpNo(a[0], b[0]))),
    dfns: Object.fromEntries([...dfns].sort((a, b) => (a[0] < b[0] ? -1 : 1))),
  };
  mkdirSync(INDEX_DIR, { recursive: true });
  writeFileSync(INDEX_FILE, JSON.stringify(out, null, 1) + "\n");

  console.log(`\nindexed ${sections.size} sections and ${dfns.size} multi-word terms (${dfnCount} dfns) from ${sorted.length} pages`);
  if (prev) {
    /* THE RENUMBERING REPORT — the hazard §Browser half names, made visible. A section whose TITLE stayed and
     * whose NUMBER moved is exactly the citation that silently goes wrong, so it is named here rather than
     * discovered by a reader following it. */
    const byId = new Map();
    for (const [no, s] of Object.entries(prev.sections)) if (s.id) byId.set(s.page + "#" + s.id, no);
    let moved = 0;
    for (const [no, s] of Object.entries(out.sections)) {
      const was = s.id ? byId.get(s.page + "#" + s.id) : null;
      if (was && was !== no) { console.log(`  RENUMBERED  §${was} -> §${no}  "${s.title}"`); moved++; }
    }
    console.log(`  ${moved} section(s) renumbered since the committed index (fetched ${prev.fetched})`);
  }
}

function cmpNo(a, b) {
  const x = a.split("."), y = b.split(".");
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (+x[i] || 0) - (+y[i] || 0);
    if (d) return d;
  }
  return 0;
}

/* ---- the audit ------------------------------------------------------------------------------------------ */

/* A citation's SPEC. An explicit name before the § settles it; most citations in this tree have none,
 * because they continue a comment whose opening sentence already named the standard. Resolving those is the
 * whole difficulty, and the two obvious answers are both wrong: treating an unanchored §N as HTML checks
 * core/css against the HTML index and reports hundreds of findings nobody will read (measured: 944, almost all
 * of them `computed value` and `containing block`), while skipping them checks nothing at all — every one
 * of the misattributed determine-the-origin sites is unanchored.
 *
 * So the fallback is the FILE'S OWN DOMINANT ANCHOR, GATED. A file resolves its unanchored citations to the
 * standard it names most often, but only when that standard is a STRICT MAJORITY of the file's anchors and is
 * named at least three times — core/css/css_cascade.c writes `Cascade §7.3.5` four times and `HTML
 * §...` twice, and without the gate its 60 CSS citations are audited against the HTML index. A file that
 * clears neither bar is skipped whole, and how many citations that costs is printed rather than assumed.
 *
 * THE SPEC-NAME LIST IS THE ONE HAND-MAINTAINED THING HERE, SO IT AUDITS ITSELF. A standard this tree cites
 * under a name not on the list does not produce a wrong answer — it falls through to the file gate, which
 * costs COVERAGE, not correctness. But a silent coverage loss is the defect this whole file exists to end, so
 * the audit prints the capitalised tokens that sit in front of a § often enough to be a standard's name
 * and are not recognised: the list's gaps are a printed finding, exactly as idlgen.mjs prints a missing IDL
 * member rather than quietly not checking it. */
const OTHER_SPECS = [
  /* platform standards */
  "dom", "url", "fetch", "ecmascript", "ecma", "es", "csp", "xhr", "streams", "idl", "webidl", "xml",
  "namespaces", "encoding", "infra", "storage", "indexeddb", "database", "webcrypto", "svg", "mathml",
  "wasm", "uievents", "console", "performance", "workers", "websockets", "mimesniff", "rfc", "unicode",
  "utf", "tc39", "trusted", "permissions", "clipboard", "notifications", "geolocation", "geometry",
  "fullscreen", "pointerevents", "webaudio", "webrtc", "beacon", "referrer", "mixed", "cors", "cookies",
  /* CSS modules, as this tree spells them when it does not use the levelled shortname */
  "cssom", "css", "selectors", "cascade", "view", "values", "sizing", "fonts", "backgrounds", "text",
  "display", "position", "overflow", "images", "color", "transforms", "writing", "box", "inline",
  "contain", "align", "ui", "scroll", "logical", "variables", "syntax", "media", "mediaqueries",
  "highlight", "masking", "shapes", "multicol", "tables", "page", "flexbox", "grid", "counter", "lists",
  "break", "ruby", "pseudo", "speech", "transitions", "animations", "compositing", "filter", "srgb",
];
const HTML_NAMES = ["html", "htmls"];
/* A levelled CSS shortname (`css-sizing-3`, `css-values-4`, `selectors-4`) is how this tree spells a CSS
 * module most of the time, and it must classify as ANOTHER standard rather than as no anchor at all. */
const LEVELLED = /^[a-z]+(-[a-z0-9]+)*-[0-9]+$/;

function anchorToken(before) {
  /* Read backwards over the punctuation a citation can wear ("the HTML spec's §", `HTML §`,
   * `(HTML §`). */
  const tail = before.replace(/[\s'"’(\[]+$/, "");
  const m = /([A-Za-z][A-Za-z0-9+-]*)(?:\s+(?:Standard|standard|spec|Spec))?$/.exec(tail);
  return m ? m[1] : null;
}

function classifyAnchor(tok) {
  if (!tok) return null;
  const w = tok.toLowerCase();
  if (HTML_NAMES.includes(w)) return "html";
  if (OTHER_SPECS.includes(w) || LEVELLED.test(w)) return "other:" + w;
  return null;
}

function walk(dir, out = []) {
  /* THE CHECKOUT IS SHARED AND EDITED UNDER THIS WALK. An editor's temporary file appears between the readdir
   * and the stat and is gone before it, so a scan that trusts a name it just read crashes on another lane's
   * save. A vanished entry is not a finding and not an error — it is a file that was never in the tree
   * this run is measuring. */
  let names;
  try { names = readdirSync(dir); } catch { return out; }
  for (const e of names) {
    if (e === "node_modules" || e === ".git" || e === "lexbor" || e === "qjs" || e.includes(".tmp.")) continue;
    const p = join(dir, e);
    let st;
    try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) walk(p, out);
    else if (/\.(c|h)$/.test(e)) out.push(p);
  }
  return out;
}

function audit(argv) {
  if (!existsSync(INDEX_FILE)) {
    console.error(`no committed index at ${relative(ROOT, INDEX_FILE)} — run: node engine/citegen.mjs --regen`);
    process.exit(2);
  }
  const idx = JSON.parse(readFileSync(INDEX_FILE, "utf8"));
  const sections = idx.sections, dfns = idx.dfns;
  const titleToNo = new Map();
  for (const [no, s] of Object.entries(sections)) {
    const k = normTerm(s.title);
    if (!titleToNo.has(k)) titleToNo.set(k, []);
    titleToNo.get(k).push(no);
  }
  /* Longest-first matching needs to know how far a phrase can run before it stops being a term. */
  let maxWords = 2;
  for (const t of Object.keys(dfns)) maxWords = Math.max(maxWords, t.split(" ").length);

  const targets = argv.filter((a) => !a.startsWith("--"));
  const files = targets.length
    ? targets.map((t) => (statSync(t).isDirectory() ? walk(t) : [t])).flat()
    : walk(join(HERE, "host"));

  const findings = [];
  const stat = { total: 0, htmlAnchored: 0, htmlByFile: 0, other: 0, skipped: 0, confirmed: 0, unverified: 0 };
  const byOther = new Map();
  const unknownTok = new Map();     /* capitalised tokens in front of a § that OTHER_SPECS does not know */
  const CITE = /§([0-9]+(?:\.[0-9]+)*)/g;

  for (const file of files) {
    const src = readFileSync(file, "utf8");
    const lineOf = (off) => { let n = 1; for (let i = 0; i < off; i++) if (src.charCodeAt(i) === 10) n++; return n; };

    /* PASS 1 — what this file anchors, when it anchors at all. */
    const cites = [];
    const votes = new Map();
    CITE.lastIndex = 0;
    for (let m; (m = CITE.exec(src)); ) {
      const tok = anchorToken(src.slice(Math.max(0, m.index - 40), m.index));
      const a = classifyAnchor(tok);
      cites.push({ at: m.index, len: m[0].length, no: m[1], anchor: a });
      if (a) votes.set(a, (votes.get(a) || 0) + 1);
      else if (tok && /^[A-Z]/.test(tok) && tok.length > 2) unknownTok.set(tok, (unknownTok.get(tok) || 0) + 1);
    }
    let dominant = null, best = 0, second = 0;
    for (const [k, v] of votes) { if (v > best) { second = best; dominant = k; best = v; } else if (v > second) second = v; }
    /* THE GATE: named at least three times, and at least twice as often as the runner-up. A STRICT MAJORITY
     * over all anchors is the wrong test and was measured wrong — core/frame/navigable.c anchors HTML 33
     * times and nine other standards 43 times between them, so a majority rule skipped the whole file and 598
     * of its 674 citations went unaudited while it is plainly an HTML file. What decides a file's subject is
     * how far its first standard is ahead of its second, not whether it beats all others put together. */
    const fallback = best >= 3 && best >= 2 * second ? dominant : null;

    /* PASS 2 — resolve, then check. */
    for (const c of cites) {
      stat.total++;
      const spec = c.anchor || fallback;
      if (spec === "html") { if (c.anchor) stat.htmlAnchored++; else stat.htmlByFile++; }
      else if (spec) { stat.other++; byOther.set(spec.slice(6), (byOther.get(spec.slice(6)) || 0) + 1); continue; }
      else { stat.skipped++; continue; }

      const no = c.no;
      /* The trailing phrase, read across the comment's own line wrapping and leading `*` gutters, because a
       * citation and the term it names are routinely split by a newline in this tree. */
      const after = src.slice(c.at + c.len, c.at + c.len + 220).replace(/\n\s*\*?\s*/g, " ");
      const quoted = /^['"’“]?s?['"’“]?\s*["“]([^"”]{2,90})["”]/.exec(after);
      const words = normTerm(after.replace(/^'s\b/, " ")).split(" ").filter(Boolean);

      let verdict = null;

      /* (1) The number the standard does not have — ASKED ONLY OF AN EXPLICITLY ANCHORED CITATION, and
       * the asymmetry is the point rather than caution. This check fires on ABSENCE, which is exactly what a
       * mis-resolved citation produces: a file whose dominant anchor is HTML but whose §16.5 is CSS Color's
       * reports "HTML has no §16.5" and is right about the index and wrong about the world (measured: 688
       * such, 150 from one file). The term check below fires on a POSITIVE match against a phrase the HTML
       * standard defines, so it carries its own evidence that the comment is about HTML and can be asked of a
       * file-resolved citation. */
      if (!sections[no]) {
        if (c.anchor) verdict = { kind: "UNKNOWN-SECTION", msg: `HTML has no §${no}` };
      } else {
        const wantTitle = normTerm(sections[no].title);
        const where = (t) => dfns[t].map((n) => `§${n} "${sections[n] ? sections[n].title : "?"}"`).join(" / ");

        /* (2) The title the citation states, against the title the standard gives that number. */
        if (quoted) {
          const q = normTerm(quoted[1]);
          if (q === wantTitle) verdict = { kind: "OK-TITLED" };
          else if (dfns[q]) verdict = dfns[q].includes(no) ? { kind: "OK-TERM" }
            : { kind: "MISATTRIBUTED", msg: `"${quoted[1]}" is defined in ${where(q)}, not §${no} "${sections[no].title}"` };
          else if (titleToNo.has(q)) verdict = { kind: "TITLE-MISMATCH", msg: `"${quoted[1]}" titles §${titleToNo.get(q).join(", §")}; §${no} is "${sections[no].title}"` };
        }

        /* (3) TERM ATTRIBUTION — the check the misattributed sites need. Longest phrase first, so
         * `determine the origin` wins over any two-word prefix of it. */
        if (!verdict) {
          for (let n = Math.min(maxWords, words.length); n >= 2; n--) {
            const phrase = words.slice(0, n).join(" ");
            if (!dfns[phrase]) continue;
            verdict = dfns[phrase].includes(no) ? { kind: "OK-TERM" }
              : { kind: "MISATTRIBUTED", msg: `"${phrase}" is defined in ${where(phrase)}, not §${no} "${sections[no].title}"` };
            break;
          }
        }

        /* (4) The title stated unquoted, immediately after the number (`HTML §13.2.5.43 comment start
         * state`). */
        if (!verdict && wantTitle && words.slice(0, wantTitle.split(" ").length).join(" ") === wantTitle) {
          verdict = { kind: "OK-TITLED" };
        }
      }

      if (!verdict) { stat.unverified++; continue; }
      if (verdict.kind.startsWith("OK")) { stat.confirmed++; continue; }
      findings.push({ file: relative(ROOT, file), line: lineOf(c.at), no, anchored: !!c.anchor, ...verdict,
        text: src.slice(c.at, c.at + 100).split("\n")[0] });
    }
  }

  console.log(`spec-citation audit — HTML Living Standard, index fetched ${idx.fetched}, standard updated ${idx.specUpdated}`);
  console.log(`  ${Object.keys(sections).length} sections, ${Object.keys(dfns).length} multi-word terms indexed`);
  console.log(`  ${stat.total} citations in ${files.length} files`);
  console.log(`  HTML: ${stat.htmlAnchored} anchored + ${stat.htmlByFile} resolved by their file's dominant anchor = ${stat.htmlAnchored + stat.htmlByFile} audited`);
  console.log(`  ${stat.other} resolved to another standard (not audited — one spec, well); ${stat.skipped} unresolved (their file names no standard often enough to fall back on)`);
  console.log(`  ${stat.confirmed} confirmed against the standard, ${stat.unverified} carry no title and no term this index knows`);
  if (byOther.size) console.log(`  other standards seen: ${[...byOther].sort((a, b) => b[1] - a[1]).slice(0, 14).map(([k, v]) => `${k}=${v}`).join(" ")}`);
  const gaps = [...unknownTok].filter(([, v]) => v >= 8).sort((a, b) => b[1] - a[1]);
  if (gaps.length) console.log(`  capitalised tokens in front of a § that OTHER_SPECS does not know (a standard among these is coverage this audit is not getting): ${gaps.slice(0, 20).map(([k, v]) => `${k}=${v}`).join(" ")}`);

  const groups = new Map();
  for (const f of findings) { if (!groups.has(f.kind)) groups.set(f.kind, []); groups.get(f.kind).push(f); }
  const limit = argv.includes("--all") ? Infinity : 120;
  console.log("");
  for (const kind of ["UNKNOWN-SECTION", "MISATTRIBUTED", "TITLE-MISMATCH"]) {
    const g = groups.get(kind) || [];
    console.log(`${kind}: ${g.length}`);
    for (const f of g.slice(0, limit)) {
      console.log(`  ${f.file}:${f.line}  ${f.msg}`);
      console.log(`      ${f.text.trim()}`);
    }
    if (g.length > limit) console.log(`  … ${g.length - limit} more (--all)`);
  }
  console.log(`\n${findings.length} finding(s). This auditor REPORTS; it exits 0 by design — see the header.`);
}

const argv = process.argv.slice(2);
if (argv.includes("--regen")) regen();
else audit(argv);
