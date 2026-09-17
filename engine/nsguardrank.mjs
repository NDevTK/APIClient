/* WHICH MEMBERS A NAMESPACE'S FIRST LANDING MUST CARRY — the §NO-STUBS ordering question, made mechanical.
 *
 * engine/absentrank.mjs answers WHICH ABSENT NAME TO BUILD and bands each by what its absence costs: THROWS
 * (every use is unguarded), `mixed`, `detect-only`, `shadowed`. Its own header says of the middle band that it
 * is "the honest middle — some use and some guard exist and only reading the site [decides]". This reads the
 * site. It asks a DIFFERENT question of a name that is a NAMESPACE — a global whose MEMBERS a bundle reads —
 * and that question decides a landing order rather than a queue position, so the two instruments partition
 * rather than overlap: absentrank says build X, this says which members X may not arrive without.
 *
 * WHY THE QUESTION EXISTS. CLAUDE.md §NO-STUBS: an absent name leaves `typeof NS` false and the bundle runs a
 * FALLBACK this engine CAN execute, so installing the namespace object before the behaviour behind it exists
 * flips the guard TRUE and the bundle "takes a branch nothing can complete AND abandons the branch that was
 * working". The unit of landing is therefore "the smallest diff that makes the guard's TRUE BRANCH
 * SURVIVABLE" — and nothing anywhere said which branch that is. It is not the most-used member and it is not
 * the simplest one: it is whichever member a guard that tested ONLY THE NAMESPACE goes on to CALL.
 *
 * THE DISCRIMINATOR IS A FACT ABOUT ECMAScript AND NOT A HEURISTIC, which is the whole reason this is worth
 * mechanising. With `NS` present and `NS.M` undefined:
 *   - `new NS.M(...)`, `NS.M(...)` and `NS.M.x` THROW a TypeError — the flow ends where a fallback used to run;
 *   - every other read of `NS.M` — `NS.M!==void 0`, `&&NS.M`, `!!NS.M`, `typeof NS.M`, `NS?.M&&`, `"M" in NS`,
 *     or a bare `NS.M;` whose value is discarded — yields `undefined` and cannot throw.
 * So a guard is SAFE exactly when the first thing it reaches is one of the second kind, and it CARRIES a
 * member exactly when the first thing it reaches is one of the first kind. Optional chaining does not change
 * this: `a?.b()` guards `a` being nullish and still throws when `b` is undefined, so the classification reads
 * what follows the MEMBER and never what precedes the namespace.
 *
 * WHAT IS NEUTRAL, AND SAYING SO IS HALF THE ANSWER. A BARE `new NS.M(...)` with no guard anywhere raises a
 * ReferenceError today and a TypeError after the namespace lands. Both end the flow, so an unguarded use is
 * neither a gain nor a loss from installing the namespace alone — it becomes a gain only when M ITSELF lands.
 * That is why this file ranks by CARRYING GUARDS and prints the member-read tally beside it rather than
 * ordering by it: the tally is the prize, and the carrying guards are the price of collecting any of it.
 *
 * IT DERIVES ITS NAMESPACES AND NEVER LISTS THEM. The candidates are the union of the three committed
 * vocabularies that own names on the global object — browser/platform_names.h (Web IDL), browser/
 * language_names.h (ECMAScript §19) and browser/i18n_names.h (ECMA-402 §8) — kept to those the corpus reads a
 * member of. A hand list is the second copy of a generated fact, and a probe that silently measures a SUBSET
 * reports a smaller cost and reads as permission; so a header whose GENERATED banner or array is gone is a
 * THROW rather than a shorter table.
 *
 * IT SAYS NOTHING ABOUT WHETHER A NAME IS ABSENT. That is absentrank's question, answered from idlgen and
 * idl_installed, and re-deriving it here would be the second copy this project has been wrong about before.
 * Every row prints, installed or not; what a row means for a landing is decided by the reader who already
 * knows which of them this engine reaches.
 *
 * EVERY CHANNEL IS ARMED IN BOTH DIRECTIONS BEFORE ANY COUNT IS READ, and the classifier is shown producing
 * each of its three verdicts on a site whose verdict is not in doubt. A classifier that cannot speak reports
 * the same zero as a corpus that does not do the thing, and the zero this one would report is the one that
 * says a namespace is free to land — the direction that costs something. The parts sum to the total and the
 * sum is asserted, so a guard this file fails to classify cannot vanish from the count.
 *
 * EXIT 0 FOR ANY CORPUS: an instrument, not a gate. Non-zero ONLY when an arming or a sum above fails, which
 * is a statement about THIS FILE having gone stale against its subjects and never about the engine.
 *
 * THE MEMBER TALLY RECONCILES AGAINST A GREP ANYONE CAN RUN, AND THE TWO DIFFER FOR REASONS WORTH KNOWING:
 * `grep -rhoE 'NS\\.[A-Za-z_$][A-Za-z0-9_$]*'` over this corpus cannot see `NS?.M` and this channel can, and
 * this channel additionally counts the `M` in `"x" in NS.M.prototype` that the `in` alternative used to
 * swallow. Measured on `Intl`: 259 by that grep, 260 here, the delta being one optional-chained read.
 *
 * NAMED RESIDUAL. WHAT IS NOT COVERED: a guard and the use it protects are associated by a forward WINDOW of
 * source text, so a guard whose true branch reaches its member further away than the window contributes to
 * the `no-member-in-window` band instead of naming the member it carries. WHAT THE NEXT DIFF BUILDS: a real
 * expression-scoped association — the guard's own boolean chain and the statement it governs — which needs
 * the code/not-code mask absentrank's own residual already names as written, measured and deliberately not
 * landed. HOW ITS ABSENCE WOULD SHOW: a namespace whose carrying count is zero while its `no-member-in-window`
 * count is not, with the printed sites reading as guards that plainly do call a member.
 * RETIREMENT: this residual goes when the association is expression-scoped rather than windowed. */
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve, relative } from "node:path";
import { corpusPrograms } from "./corpus_programs.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const argOf = (flag, dflt) => {
  const i = process.argv.indexOf(flag);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt;
};
const HOST = resolve(argOf("--host", join(HERE, "host")));
const CORPUS = resolve(argOf("--corpus", join(HERE, "..", "testing", "corpus", "mirror")));
/* How far past a namespace test this looks for the member that test lets through. Stated as an input rather
   than buried: it is the residual above, and a reader widening it must re-read the sites it adds. */
const WINDOW = Number(argOf("--window", "400"));
const TOP = Number(argOf("--top", "12"));
const say = (s) => console.log(`[nsguardrank] ${s}`);
const die = (s) => { throw new Error(`[nsguardrank] CALIBRATION FAILED — ${s}`); };

/* ---- the vocabularies, from the artifacts that own them ------------------------------------------------ */
const vocab = (file, array) => {
  const t = readFileSync(join(HOST, "browser", file), "utf8");
  if (!/^\/\* GENERATED by /.test(t))
    die(`${file} has lost its GENERATED banner — it is no longer a derived artifact and this parse is reading ` +
        `whatever somebody typed. Re-read it before ranking anything against it.`);
  const m = new RegExp(`static const char \\*const ${array}\\[\\] = \\{([\\s\\S]*?)\\n\\};`).exec(t);
  if (!m) die(`${file} no longer declares ${array}[] in the shape this parses — a silently smaller vocabulary ` +
              `would report a smaller landing cost, which is the direction that reads as permission.`);
  const names = [...m[1].matchAll(/"([A-Za-z_$][\w$]*)"/g)].map((x) => x[1]);
  if (!names.length) die(`${file} parsed to an EMPTY ${array}[] — a vocabulary that is not there reads as a ` +
                         `vocabulary with no namespaces in it.`);
  return names;
};
const CANDIDATES = new Set([
  ...vocab("platform_names.h", "PLATFORM_NAMES"),
  ...vocab("language_names.h", "LANGUAGE_NAMES"),
  ...vocab("i18n_names.h", "I18N_NAMES"),
]);

/* ---- the corpus, from the artifact that owns it ---------------------------------------------------------- */
/* WHICH FILES ARE PROGRAMS IS THE SERVER'S ANSWER AND NOT THIS FILE'S GUESS. This selected by FILENAME
   EXTENSION until it was measured, in the same words engine/absentrank.mjs used and with the same three
   files missing: testing/corpus/mirror.mjs folds a URL's query into the saved name as a `__q<sha256[0:8]>`
   suffix, so a bundle fetched with a query is saved as `all.js__q54b3907e` and no extension list reaches it.
   A namespace ranking that cannot see a 1.5 MB worker bundle under-reports what installing that namespace
   would cost, which is the direction that reads as permission. engine/corpus_programs.mjs takes the
   population from testing/corpus/provenance.json's recorded Content-Type, joined by CONTENT so it copies no
   part of the mirror's naming rule, and THROWS rather than going quietly short. ONE statement of that rule,
   two consumers — two right answers to one question is the shape that drifts, and this file and absentrank
   held the identical wrong one. */
const { files, onDisk, nProgram, nDocument, nExcluded } = corpusPrograms(CORPUS, "nsguardrank");

/* ---- the two channels, armed ---------------------------------------------------------------------------- */
const esc = (n) => n.replace(/[$]/g, "\\$");
/* A NAMESPACE TEST: the name in a position that reads its VALUE rather than taking a member off it. The
   trailing `(?![\s]*\??\s*\.)` is what keeps `window.Intl.DateTimeFormat()` out of this channel — that
   occurrence is a member read, and counting it as a test would score a USE as its own guard. */
const NOT_MEMBER_OR_WRITE = `(?!\\s*\\??\\s*\\.)(?!\\s*=(?![=>]))`;
const NSTEST = (n) => new RegExp(
  `\\btypeof\\s+${esc(n)}\\b${NOT_MEMBER_OR_WRITE}|` +
  `\\b(?:window|self|globalThis)\\s*\\.\\s*${esc(n)}\\b${NOT_MEMBER_OR_WRITE}`, "g");
/* A MEMBER READ, with what PRECEDES and FOLLOWS it, because those two decide whether an undefined member
   throws. `in` is its own alternative: `"M" in NS` is a member test that names no `NS.M` at all. */
const NSMEMBER = (n) => new RegExp(
  `(new\\s+)?(?:\\b(?:window|self|globalThis)\\s*\\.\\s*)?\\b${esc(n)}\\s*\\??\\s*\\.\\s*([A-Za-z_$][\\w$]*)\\s*(.?)|` +
  `["'\\x60]([A-Za-z_$][\\w$]*)["'\\x60]\\s*in\\s+(?=${esc(n)}\\b)`, "g");

for (const [what, re, pos, neg] of [
  ["namespace test", NSTEST("Zz"), ['typeof Zz<"u"', 'window.Zz)'], ['typeof Zz.member', 'window.Zz.member']],
  ["member read",    NSMEMBER("Zz"), ['Zz.member(', 'new Zz.member(', '"member" in Zz'], ['Zzz.member(', 'q.Zz2.member(']],
]) {
  for (const s of pos) if (![...s.matchAll(new RegExp(re.source, "g"))].length)
    die(`the ${what} channel did not match its own form in ${JSON.stringify(s)} — its 0 would mean nothing.`);
  for (const s of neg) if ([...s.matchAll(new RegExp(re.source, "g"))].length)
    die(`the ${what} channel matched its near miss ${JSON.stringify(s)} — it is counting something else.`);
}

/* THE GENERIC FORM OF THE MEMBER CHANNEL, whose capture groups this file reads by POSITION. It is the same
   pattern with the name replaced by a capture, and it is armed against the per-name form on the same input so
   the two cannot drift: a generic channel that stopped agreeing with the one the controls exercise would be
   reading a different population than the verdicts were calibrated on. */
const ANY_MEMBER = new RegExp(
  `(new\\s+)?(?:\\b(?:window|self|globalThis)\\s*\\.\\s*)?\\b([A-Za-z_$][\\w$]*)\\s*\\??\\s*\\.\\s*([A-Za-z_$][\\w$]*)\\s*(.?)|` +
  `["'\\x60]([A-Za-z_$][\\w$]*)["'\\x60]\\s*in\\s+(?=([A-Za-z_$][\\w$]*)\\b)`, "g");
for (const probe of ['new Zz.Coll(a)', 'window.Zz.Fmt();', 'Zz.Plural!==void 0', '"Seg" in Zz']) {
  const a = [...probe.matchAll(new RegExp(NSMEMBER("Zz").source, "g"))];
  const b = [...probe.matchAll(new RegExp(ANY_MEMBER.source, "g"))].filter((m) => m[2] === "Zz" || m[6] === "Zz");
  if (a.length !== b.length)
    die(`the generic member channel read ${JSON.stringify(probe)} ${b.length} time(s) and the per-name one ` +
        `${a.length} — the verdicts are calibrated on the per-name form and the table is read from the generic one.`);
}

/* Compiled once per name. `classify` runs at every namespace test, and re-deriving the pattern there was
   measured as the difference between seconds and minutes over this corpus. */
const memRe = new Map();
const memberRegex = (n) => {
  let src = memRe.get(n);
  if (!src) { src = NSMEMBER(n).source; memRe.set(n, src); }
  return new RegExp(src, "g");
};
/* THE VERDICT, AND IT IS THE ECMAScript FACT RATHER THAN A GUESS: with the member undefined, a CALL, a
   CONSTRUCT and a further PROPERTY throw; every other read yields undefined. */
const throwsIfMissing = (m) => !!m[1] || m[3] === "(" || m[3] === ".";
/* A NEW FUNCTION BODY IS A NEW SCOPE AND THE GUARD'S BRANCH DOES NOT GOVERN IT — the function may be called
   from anywhere. A windowed association cannot see a statement boundary, and this is the boundary that
   matters: crossing one is what turned a polyfill install and a `typeof NS?NS:{}` coalesce into a claim about
   a member used inside the NEXT function down. Such a hit is BANDED rather than dropped, because dropping it
   would report a namespace as cheaper to land than it is — the direction that reads as permission — and
   because the band is short enough to read entry by entry, which is how it must be read. */
const SCOPE_BREAK = /\bfunction\b|=>/;
const classify = (src, at, n) => {
  const win = src.slice(at, at + WINDOW);
  const re = memberRegex(n);
  for (const m of win.matchAll(re)) {
    if (m.index === 0) continue;                 /* the test itself, when the test WAS a `window.NS` form */
    if (m[4] !== undefined) return ["safe", m[4]];         /* `"M" in NS` — a member test naming no NS.M */
    if (!throwsIfMissing(m)) return ["safe", m[2]];
    return [SCOPE_BREAK.test(win.slice(0, m.index)) ? "carries-across-scope" : "carries", m[2]];
  }
  return ["no-member-in-window", null];
};
/* SHOWN PRODUCING EACH VERDICT BEFORE ANY OF THEM IS READ. A classifier silent on `carries` reports a
   namespace as free to land, which is the one answer that costs something. */
for (const [s, ntests, want, member] of [
  ['typeof Zz>"u"?fallback:new Zz.Coll(a).compare', 1, "carries", "Coll"],
  ['if(!window.Zz)return S;const d=window.Zz.Fmt();', 1, "carries", "Fmt"],
  ['typeof Zz<"u"&&Zz.Plural!==void 0?p:q', 1, "safe", "Plural"],
  ['"undefined"!=typeof Zz&&"function"==typeof Zz.Fmt', 1, "safe", "Fmt"],
  ['typeof Zz<"u"&&"Seg" in Zz&&(k=new Zz.Seg())', 1, "safe", "Seg"],
  /* THE MINIFIER IN THIS CORPUS EMITS TEMPLATE LITERALS FOR STRINGS, so a `"M" in NS` channel spelled with
     only `"` and `'` cannot see the form the corpus actually writes. It could not: the first run of this
     table used double quotes, passed, and the corpus site it was written FOR — `` `Segmenter`in Intl `` —
     was reported as CARRYING a member it plainly tests. A control is written in the subject's spelling. */
  ['typeof Zz<`u`&&`Seg`in Zz&&(k=new Zz.Seg())', 1, "safe", "Seg"],
  /* `"M" in NS.Other.prototype` IS BOTH a member test and a member READ of `Other`, so the `in` alternative
     must not CONSUME the name: it did, and three reads of DateTimeFormat went missing from one file's tally
     while the verdict stayed right. A lookahead keeps both, and the verdict is still the `in` because it
     matches at the lower index. */
  ['typeof Zz<`u`&&"fmt"in Zz.Fmt.prototype', 1, "safe", "fmt"],
  /* `window.NS = ...` IS A WRITE AND NOT A TEST, and the polyfill idiom that installs one
     (`window.NS=Object.assign(typeof NS<"u"?NS:{},poly)`) is exactly where this matters: scored as a test it
     scans forward into unrelated code and names whatever member it finds there. Asserted at 1 rather than 2
     because the `typeof NS` in the same expression IS a test. */
  ['window.Zz=Object.assign(typeof Zz<`u`?Zz:{},p);q(new Zz.Fmt())', 1, "carries", "Fmt"],
  /* AND THE SAME IDIOM WITH THE USE IN THE NEXT FUNCTION DOWN, which is the shape the corpus actually holds:
     the guard's branch does not govern that body, so the hit is banded rather than claimed. */
  ['window.Zz=Object.assign(typeof Zz<`u`?Zz:{},p);function C(e){q(new Zz.Fmt())}', 1, "carries-across-scope", "Fmt"],
  ['Object(d.determine)("undefined"!=typeof Zz)', 1, "no-member-in-window", null],
  /* `window.NS?.M` IS NOT A NAMESPACE TEST AND MUST NOT BE COUNTED AS ONE. It reads the member in the same
     expression that tests the namespace, so it is safe BY CONSTRUCTION rather than by a forward scan — and
     scoring it as a test would make the classifier's verdict depend on what follows it. Asserted at 0 here
     because the first time this table was run it asserted 1 and the channel was right. */
  ['window.Zz?.Fmt&&(e=1)', 0, null, null],
]) {
  const t = [...s.matchAll(NSTEST("Zz"))];
  if (t.length !== ntests) die(`the control ${JSON.stringify(s)} produced ${t.length} namespace tests, not ${ntests}.`);
  if (!ntests) continue;
  const [got, gm] = classify(s, t[0].index + t[0][0].length, "Zz");
  if (got !== want || gm !== member)
    die(`the classifier read ${JSON.stringify(s)} as ${got}/${gm} and it is ${want}/${member} — a verdict it ` +
        `cannot produce is a verdict its absence from the table does not mean.`);
}

/* ---- read ------------------------------------------------------------------------------------------------ */
/* ONE PASS PER FILE AND NOT ONE PER CANDIDATE. The generic channels capture the RECEIVER and the Set decides;
   a per-name regex over every file is the same answer and was measured at minutes rather than seconds, which
   is the difference between an instrument a reader re-runs and one they quote from memory. */
const ANY_NSTEST = new RegExp(
  `\\btypeof\\s+([A-Za-z_$][\\w$]*)\\b${NOT_MEMBER_OR_WRITE}|` +
  `\\b(?:window|self|globalThis)\\s*\\.\\s*([A-Za-z_$][\\w$]*)\\b${NOT_MEMBER_OR_WRITE}`, "g");
const row = () => ({ tests: 0, safe: 0, carries: 0, across: 0, nowin: 0, needs: new Map(),
                     reads: new Map(), sites: [], farSites: [] });
const rows = new Map();
const at = (n) => { if (!rows.has(n)) rows.set(n, row()); return rows.get(n); };
for (const f of files) {
  const src = readFileSync(f, "utf8");
  /* member reads, generic */
  for (const m of src.matchAll(ANY_MEMBER)) {
    const n = m[2];
    if (n === undefined || !CANDIDATES.has(n)) continue;
    const r = at(n);
    r.reads.set(m[3], (r.reads.get(m[3]) || 0) + 1);
  }
  /* namespace tests, generic, each classified by a forward scan for ITS OWN name */
  for (const t of src.matchAll(ANY_NSTEST)) {
    const n = t[1] !== undefined ? t[1] : t[2];
    if (!CANDIDATES.has(n)) continue;
    const r = at(n);
    r.tests++;
    const [verdict, member] = classify(src, t.index + t[0].length, n);
    const cite = () => {
      const from = Math.max(0, t.index - 30);
      return `${relative(CORPUS, f)}  \u2026${src.slice(from, t.index + 110).replace(/\s+/g, " ")}\u2026`;
    };
    if (verdict === "carries") {
      r.carries++;
      r.needs.set(member, (r.needs.get(member) || 0) + 1);
      r.sites.push(cite());
    } else if (verdict === "carries-across-scope") { r.across++; r.farSites.push(`${member} — ${cite()}`); }
    else if (verdict === "safe") r.safe++;
    else r.nowin++;
  }
}
/* THE PARTS SUM TO THE TOTAL. A guard this file failed to classify would otherwise leave the table rather
   than the band, and a namespace would read as cheaper to land than it is. */
for (const [n, r] of rows)
  if (r.safe + r.carries + r.across + r.nowin !== r.tests)
    die(`${n}: ${r.safe}+${r.carries}+${r.across}+${r.nowin} classified guards against ${r.tests} counted — a guard left ` +
        `the count without entering a band.`);

/* ---- say -------------------------------------------------------------------------------------------------- */
/* THE GLOBAL OBJECT'S OWN NAMES ARE NOT A NAMESPACE, and this is the SAME SET the channels above already use
   as a global prefix rather than a second list. `window.addEventListener` is a read of a GLOBAL, not of a
   member of a namespace called `window` — so those rows are absentrank's population and not this one's, and
   a `typeof window` SSR check is a test of the environment rather than of a platform surface this engine may
   or may not install. They are dropped from the RANKING and their counts stay in `rows`, so the sum assertion
   above still covers every guard the channels saw. */
const GLOBAL_SELF = new Set(["window", "self", "globalThis"]);
const ranked = [...rows].filter(([n, r]) => r.reads.size && !GLOBAL_SELF.has(n))
  .sort((a, b) => b[1].carries - a[1].carries ||
        [...b[1].reads.values()].reduce((x, y) => x + y, 0) - [...a[1].reads.values()].reduce((x, y) => x + y, 0));
say(`${files.length} corpus file(s) under ${CORPUS} (${nProgram} program + ${nDocument} document, ${nExcluded} other, ${onDisk} on disk — typed by the server's own Content-Type in provenance.json, never by extension); ${CANDIDATES.size} candidate global name(s) from the three ` +
    `committed vocabularies; ${ranked.length} of them have a member read here.`);
say(`ORDERED BY WHAT INSTALLING THE NAMESPACE ALONE WOULD COST: a CARRYING guard tests only the namespace and ` +
    `then CALLS, CONSTRUCTS or DEREFERENCES a member, so it runs a working fallback today and raises a ` +
    `TypeError the moment the namespace exists without that member. A SAFE guard also tests the member and ` +
    `answers the same either way. An UNGUARDED use already throws and is neither — it becomes a gain only ` +
    `when its own member lands, which is what the member tally beside each row is.`);
for (const [n, r] of ranked.slice(0, TOP)) {
  const total = [...r.reads.values()].reduce((x, y) => x + y, 0);
  const sorted = [...r.reads].sort((a, b) => b[1] - a[1]);
  const mem = sorted.slice(0, TOP).map(([m, c]) => `${m} ${c}`).join(", ") +
              (sorted.length > TOP ? `, … ${sorted.length - TOP} more` : "");
  say(``);
  say(`${n}: ${r.carries} carrying / ${r.across} carrying-across-scope / ${r.safe} safe / ${r.nowin} ` +
      `no-member-in-window  (${r.tests} namespace ` +
      `test(s)); ${total} member read(s) over ${r.reads.size} member(s)`);
  if (r.needs.size)
    say(`   A FIRST LANDING OF ${n} MUST CARRY: ${[...r.needs].sort((a, b) => b[1] - a[1])
        .map(([m, c]) => `${m} (${c} site${c > 1 ? "s" : ""})`).join(", ")}`);
  say(`   members read: ${mem}`);
  for (const s of r.sites.slice(0, TOP)) say(`   carrying site: ${s}`);
  if (r.sites.length > TOP) say(`   … ${r.sites.length - TOP} further carrying site(s) — raise --top to read them.`);
  for (const s of r.farSites.slice(0, TOP)) say(`   across-scope site (read it): ${s}`);
  if (r.farSites.length > TOP) say(`   … ${r.farSites.length - TOP} further across-scope site(s).`);
}
say(``);
say(`A row is printed whether or not this engine installs the name; which rows are ABSENT is engine/` +
    `absentrank.mjs's question and is deliberately not re-derived here.`);
