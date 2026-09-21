// ONE LIVE SITE, ONE VIRGIN BROWSER.
//
// Drives the already-built extension in real Chrome against a REAL URL over the internet, then reads the
// RESULT DOCUMENT out of the offscreen brain. It does NOT scrape the console for `@H`: no shipped path
// prints one. Endpoints arrive as `fetchCallSites` inside the engine's ONE `@RESULT` line, which bridge.js
// parses into the analysis object the brain stores at `doc._astResults`; `self._engineLog` carries the
// per-run counters (flows/switches/endpoints/sinks/park) or `{crashed:true}` for a run that produced no
// result document at all. Those two are the instrument.
//
// The console is read for one thing only: the ABORT LINE. renderer-host.js tees the renderer's STDERR to
// this document's console.debug, and a `@WHY` there carries the assert's cond + file:line — the crash
// signature the census ranks by.
//
// usage: node site.mjs <id> <url> [pass]   (caller restarts Chrome around each invocation)
//
// THE PASS IS PART OF THE TRANSCRIPT'S NAME, AND THE ROW SAYS WHICH NAME IT WROTE. `logs/<id>.log` is one
// path per site, so the second pass of a census OVERWRITES the first pass's transcript and every row of every
// pass is then read against the LAST pass's console. That is cross-attribution of a crash, not a missing
// file: a site that RAN in pass 1 and aborted in pass 3 has pass 3's @WHY sitting in the only log pass 1's
// row can find, so the clean pass is published as an abort. report.mjs went looking for a pass-qualified
// name first -- and NOTHING in this tree wrote one, so that lookup could only ever fall through, which is the
// read-with-no-writer defect with a filename for a field. The name is now WRITTEN here and CARRIED on the
// row (`logFile`), so the reader is told rather than left to guess between two spellings.
import puppeteer from 'puppeteer';
import { writeFileSync, readFileSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { loadavg, cpus } from 'node:os';

const id = process.argv[2], url = process.argv[3], pass = process.argv[4] || '';
const DWELL = Number(process.env.DWELL || 40000);
const CDP = Number(process.env.CDP || 9451);
const OUT = new URL('./logs/', import.meta.url);
const LOG_NAME = (pass ? pass + '-' : '') + id + '.log';

const b = await puppeteer.connect({ browserURL: `http://127.0.0.1:${CDP}` });

const sink = [];
/* THE ID CHROME MINTED, WHICH IS A FACT ABOUT THE PATH IT LOADED. An unpacked extension's id is derived from
   its absolute directory, so two lanes' copies get different ids and a row can be CHECKED against the
   directory it claims rather than trusting that claim. This is the second half of the EXT default fix below:
   getting the default right stops the row describing a neighbour's tree, and recording this stops a WRONG
   `HARNESS_EXT_DIR` or a refused `restart` from doing the same thing silently. */
let loadedExtId = null;
const hook = (label, e) => { if (e.__w) return; e.__w = 1; e.on('console', m => sink.push(label + ' ' + m.text())); };
const attach = async (t) => {
  if (!t.url().startsWith('chrome-extension://')) return;
  if (!loadedExtId) loadedExtId = t.url().slice('chrome-extension://'.length).split('/')[0];
  try {
    if (t.type() === 'service_worker') { const w = await t.worker(); if (w) hook('[sw]', w); return; }
    const p = await t.page().catch(() => null);
    if (!p) return;
    hook('[' + t.url().split('/').pop().split('?')[0] + ']', p);
    for (const w of p.workers()) hook('[w]', w);
    p.on('workercreated', w => hook('[w]', w));
  } catch { }
};
for (const t of b.targets()) await attach(t);
b.on('targetcreated', attach);

let off = null;
for (let i = 0; i < 160 && !off; i++) {
  const t = b.targets().find(t => t.url().includes('ast-worker.html'));
  off = t ? await t.page().catch(() => null) : null;
  if (!off) await new Promise(r => setTimeout(r, 250));
}
if (!off) { console.log('ROW ' + JSON.stringify({ id, url, fatal: 'no offscreen document' })); process.exit(0); }
hook('[offscreen]', off);

/* All depths in ONE evaluate so they describe the same instant. `state.docs` is the brain's per-document
   model; `_astResults` is the analysis bridge.js built out of the engine's @RESULT. Nothing here defaults a
   producer's field with `|| x` where the producer is the engine — the arrays below are the ones bridge.js
   asserts field-for-field before it stores them; the `||` guards are for a doc the brain minted but the
   engine has not answered for yet, which is a state this probe must be able to SEE rather than crash on.
   `answered` READS `_astRun`, NOT `_astResults`: the incremental merge writes a snapshot into `_astResults`
   while the engine is still exploring, so that slot no longer answers "has this run returned" — and `run`
   beside it is what the run WAS, so a page whose findings came out of a crashed engine can never be counted
   as a completed analysis of the same page. */
const PROBE = `(() => ({
  runs: (self._engineLog || []).slice(),
  crashes: self._engineCrashOccurred || 0,
  docs: [...state.docs.values()].map(d => ({
    url: d.url || '',
    answered: !!(d._astRun || d._astError),
    run: d._astRun === undefined ? null : d._astRun,
    astError: d._astError ? String(d._astError).slice(0,300) : null,
    sites: (d._astResults || []).flatMap(a => (a.fetchCallSites || []).map(x => (x.method||'?') + ' ' + (x.url||''))),
    sinks: (d._astResults || []).reduce((n,a) => n + ((a.securitySinks||[]).length), 0),
    errs:  (d._astResults || []).flatMap(a => (a.resolverErrors || []).map(e => e.context + ': ' + e.message)),
  })),
  global: [...globalStore.endpoints.keys()],
  /* THE DOMAIN COLUMNS, AND WHY THEY ARE NOT READ OFF \`endpoints\`. That map is endpointKey → the record
     lib/merge.js builds, which is {method, service, key, headers, firstSeen} and carries NO parameters at
     all — so a probe pointed there reports "no parameter carries a domain" for every run of every site,
     forever, and the reading is a property of the container rather than of the engine. The exclusions and
     bounds a branch narrowed live one map over, under the learned discovery doc, per parameter.
     ABSENT AND ZERO ARE KEPT APART, which is the whole point of the column: \`null\` means the probe could
     not reach a \`parameters\` object at all, and an object whose every \`with*\` column reads 0 means it
     reached them and none carried a domain. THE COLUMNS ARE NOT RESTATED HERE: this sentence used to list
     them and had already gone stale twice over, naming neither \`astParams\` nor \`withLeq\`. Collapsing those two is the defect this column exists to detect,
     performed inside the instrument built to detect it. Every level of the walk is optional in the producer,
     so each is tested rather than chained — a naive \`?.\` chain yields null for a shape that is
     present-but-empty and cannot tell the two readings apart either.
     AND \`params\` IS THE WRONG DENOMINATOR FOR THEM, WHICH IS WHY \`astParams\` SITS BESIDE IT. Only a param
     lib/learn.js minted from a FORCED-EXECUTION call site can ever carry a domain: that arm alone runs
     _mergeExcludes/_mergeBounds/_mergePredicates, and it alone writes \`_astInferred\`. The other producers in
     that file mint a param from LIVE TRAFFIC — a query name off an observed URL, a path segment observed to
     vary, a concrete segment aligned with a hole — and none of them passes through a domain merge at all, so
     they are rows that CANNOT contribute to the numerator however well the engine narrows. A ratio over
     \`params\` therefore mixes two populations that take opposite readings, and its zero is consistent with
     the engine never having learned a single forced-execution param — which is a finding about the LEARNED
     SURFACE one component upstream, not about the domain machinery this column is pointed at.
     SO THE ENTAILMENT IS PUBLISHED RATHER THAN LEFT TO BE RE-DERIVED. \`astParams:0\` makes every domain
     column an entailed zero carrying no information about a domain, and \`astParams>0\` with them all at 0 is
     the reading that is closest to being about this machinery.
     AND \`astParams\` IS STILL ONE POPULATION TOO WIDE, WHICH IS THE SAME DEFECT THIS PARAGRAPH ALREADY FIXED
     ONCE, RECURRING AT THE NEXT LEVEL DOWN. solver/endpoint.c's \`kv_add\` gates EVERY domain read on
     \`if (hole)\`, and the hole comes from \`concolic_hole_key\`, whose first line returns NULL for a shape
     with no brace in it. A QUERY or BODY param is minted for every pair the address holds, so \`?limit=20\`
     is an \`astParams\` row whose hole is NULL and which therefore CANNOT carry a domain however well the
     engine narrows — the identical \"rows that cannot contribute to the numerator\" argument the paragraph
     above makes against \`params\`, one level further in. Its zero is consistent with a page whose forced
     execution learned only concrete parameters, which is a finding about the LEARNED SURFACE and not about
     this machinery.
     \`astPathParams\` IS THE DENOMINATOR THAT IS SOUND BY CONSTRUCTION, and it needs nothing from the engine
     because the record already states it. solver/endpoint.c's path scan mints a param ONLY for a segment
     that holds a brace and passes the brace-stripped segment name AS the hole key, so a param whose
     \`location\` is \"path\" has a non-NULL hole at the read, always, and the record carries \`location\` on
     every param. A zero over THIS denominator is the one reading that is about the domain machinery: a
     parameter the engine could look a domain up for, that carried none.
     \`astPathParams\` DID NOT COVER THE QUERY HALF AND NOW \`astHoleParams\` DOES, WHICH IS THE ENGINE DIFF
     THIS COLUMN'S RESIDUAL ASKED FOR AND NOT A SECOND DENOMINATOR BESIDE IT. The record used to carry \`name\`,
     \`location\`, \`validValues\` and the four optional domains and no statement of whether the value named a
     hole, so a query param reading 0 was two facts — concrete, nothing owed, or a hole whose every gate was
     lost — rendered identically. solver/endpoint.c now writes \`valueClass\` on EVERY param, off the same
     \`hole\` the four domain reads are gated on, so the two are separable: \`astHoleParams\` counts the rows a
     domain could have been looked up for, whatever their location.
     \`astPathParams\` STAYS AND IS NOT A SUPERSEDED FALLBACK, WHICH IS THE ONE THING A LATER READER WILL GET
     WRONG ABOUT IT. It is sound BY CONSTRUCTION and needs nothing from the engine, so it is the denominator
     that still answers for a record learned by a build predating \`valueClass\` — the case
     \`astUnstatedParams\` exists to make visible. On an engine that states the key the two are a CROSS-CHECK
     rather than a duplicate: a path param is always "unknown" and endpoint.c's \`kv_add\` asserts it, so
     \`astHoleParams >= astPathParams\` holds, and the pair disagreeing is a finding about the engine that no
     single column could report.
     WHAT IS STILL NOT COVERED IS THE BODY HALF, AND THE ENGINE IS NO LONGER THE REASON. endpoint.c states a
     body field's \`valueClass\` exactly as it states a query param's; what stops it reaching here is that
     lib/learn.js routes a param whose \`location\` is "body" into \`_bodyParams\` and onto
     \`doc.schemas[<Method>Request].properties\` rather than \`m.parameters\`, and this walk visits
     \`m.parameters\` only. That file does NOT merge \`valueClass\` onto those properties, and says at the site
     why not: the four domains leave them by one road — lib/discovery.js's \`_buildDiscoveryFieldShell\` lifts
     each by name onto a FieldDef — and a name \`FIELD_DEF_ABSENT\` does not declare cannot travel it, so
     writing it there today would be a field stored on every body row and read by nothing.
     WHAT THE NEXT DIFF BUILDS: \`_astValueClass\` declared in extension/lib/field-def.js, lifted by
     \`_buildDiscoveryFieldShell\` beside the four, merged in lib/learn.js's body block, and walked here as
     its OWN columns rather than folded into these — one landing, because each part alone is a write nothing
     reads, and its own columns because a body field and a URL parameter are two different things to send and
     one denominator over two populations is the defect the paragraphs above fix twice.
     HOW ITS ABSENCE WOULD SHOW: a page whose forced execution learns its parameters in POST bodies reports
     every column here at 0 against a live engine and a populated surface, and that zero reads as the
     narrowing machinery producing nothing rather than as this walk never having been shown the rows.
     THIS COLUMN SET IS ALSO THE REACHABILITY WITNESS FOR THE WHOLE
     chain: solver/decide.c records a gate under a hole key, solver/endpoint.c reads it back at kv_add and
     emits \`excludes\`/\`bounds\`/\`predicates\`, and lib/learn.js merges those onto exactly the objects walked
     here — every hop present with a live caller, and nothing at any level asserting that one arrives.
     THERE IS ONE COLUMN PER WAY A GATE NARROWS A DOMAIN, AND THE KINDS ARE NAMED RATHER THAN COUNTED. An
     equality's false arm determines a value and fills \`withExcl\`; an ordering determines an interval and
     fills \`withBnd\`; a METHOD CALL determines neither and fills \`withPred\` — CLAUDE.md §@H's own headline
     shape (\`{startsWith:/api}\`); and a LOOSE equality's HOLDING arm determines a SET rather than a value,
     which is why solver/decide.c files it instead of pinning it, and it fills \`withLeq\`. Summing them would
     hide exactly the case each column exists to find, which is a page gated only by one of them.
     THIS PARAGRAPH BEGAN \"THERE ARE THREE DOMAIN COLUMNS\" WHILE THE FOURTH WAS EMITTED, MERGED AND
     UNCOUNTED. solver/endpoint.c writes \`looselyEquals\`, lib/learn.js's \`_mergeLooselyEquals\` merges it
     onto these very objects, and lib/merge.js intersects it across documents — and nothing here read it, so a
     page whose gates are \`==\` read as a page with no gates. That is the exact failure the sentence was
     written to prevent, committed by the sentence, and it is the SECOND instance of one shape rather than an
     accident: lib/learn.js's own banner records lib/merge.js restating this same record as \"{name, location,
     validValues[], excludes[], bounds{}}\" and never revisiting it when two kinds were added. So the rule
     that file states is the cure and is adopted here — THE KINDS ARE A SET AND NEVER A COUNT, because a set
     is checkable by grepping this file for each name and a number is checkable by nothing. It was the number
     that made the omission invisible, and this file restates the record in two more comments below.
     RETIREMENT: this record goes when these columns are derived from the field set lib/learn.js declares
     rather than listed here, so a kind added there cannot go missing from this walk. */
  domains: (() => {
    let params = 0, astParams = 0, astPathParams = 0, astHoleParams = 0, astUnstatedParams = 0,
        withExcl = 0, withBnd = 0, withPred = 0, withLeq = 0, reached = false;
    for (const svc of globalStore.discoveryDocs.values()) {
      const methods = svc && svc.doc && svc.doc.resources && svc.doc.resources.learned
                   && svc.doc.resources.learned.methods;
      if (!methods) continue;
      for (const m of Object.values(methods)) {
        if (!m || !m.parameters) continue;
        reached = true;
        for (const p of Object.values(m.parameters)) {
          params++;
          if (p && p._astInferred) astParams++;
          /* THE SOUND DENOMINATOR — see the banner. A path param exists only where the segment held a brace,
             and its hole key IS that segment's brace-stripped name, so its domain read is never gated out by
             a NULL hole the way a concrete query pair's is. */
          if (p && p._astInferred && p.location === "path") astPathParams++;
          /* THE DENOMINATOR THE ENGINE NOW STATES, WHICH COVERS THE QUERY HALF THE ONE ABOVE CANNOT — see the
             banner. solver/endpoint.c writes \`valueClass\` on EVERY param from the same \`hole\` the four
             domain reads are gated on, so this counts exactly the rows a domain could have been looked up
             for. It is a SUPERSET of \`astPathParams\` and not a replacement: a path param is always
             "unknown" and endpoint.c asserts it, so \`astHoleParams >= astPathParams\` holds on any engine
             that states the key, and the two disagreeing is a finding about the engine. */
          if (p && p._astInferred && p._astValueClass === "unknown") astHoleParams++;
          /* AND THE THIRD STATE, PUBLISHED RATHER THAN LEFT TO BE RE-DERIVED, for the reason this banner
             already publishes \`astParams\`' entailment. This zone is deployed on WRITE and the engine is
             live only after a build, and the store outlives both — so a row learned by an engine that
             predates \`valueClass\` states NOTHING, which is neither "unknown" nor "concrete". Without this
             column such a run reads \`astHoleParams: 0\`, which is byte-identical to a page whose every
             forced-execution param was a concrete query pair. A nonzero here says the sound denominator for
             THIS run is \`astPathParams\` and not \`astHoleParams\`; a zero is what licenses the latter. */
          if (p && p._astInferred && p._astValueClass === undefined) astUnstatedParams++;
          if (p && Array.isArray(p._excludedValues) && p._excludedValues.length) withExcl++;
          if (p && p._bounds && Object.keys(p._bounds).length) withBnd++;
          if (p && Array.isArray(p._predicates) && p._predicates.length) withPred++;
          /* PRESENCE-AND-LENGTH, WHICH IS \`_predicates\`' TEST AND NOT A SECOND ONE — lib/learn.js's
             \`intersectLooselyEquals\` spells \"this run proved nothing\" as the EMPTY array exactly as
             \`intersectPredicates\` does, so an empty one is a param an engine run reached and narrowed
             nothing on, which is not a param carrying a domain. */
          if (p && Array.isArray(p._looselyEquals) && p._looselyEquals.length) withLeq++;
        }
      }
    }
    return reached ? { params, astParams, astPathParams, astHoleParams, astUnstatedParams,
                       withExcl, withBnd, withPred, withLeq } : null;
  })(),
}))()`;

const pg = await b.newPage();
let nav = 'ok', finalUrl = url, status = 0;
try {
  const r = await pg.goto(url, { waitUntil: 'domcontentloaded', timeout: 45000 });
  status = r ? r.status() : 0;
  finalUrl = pg.url();
  if (!r) nav = 'NO-RESPONSE';
} catch (e) { nav = 'goto:' + String(e && e.message || e).slice(0, 120); }

/* THE DWELL IS WALL-CLOCK, SO THE LOAD IS PART OF THE MEASUREMENT AND THE ROW CARRIES IT.
   A busy machine gives the engine less CPU inside the same 40 seconds, so a row's counters fall without
   anything about the engine changing -- which is how a run at load 92 on four cores was published as a
   regression. That is the elapsed-time defect CLAUDE.md names, and the honest fix here is not to switch
   this probe to a CPU clock (the engine is in another process, and the number wanted is "what does a user
   get in 40 seconds") but to REPORT the conditions beside the number, sampled at both ends of the dwell so
   a build that started mid-row is visible rather than averaged away. */
const loadBefore = loadavg();
await new Promise(r => setTimeout(r, DWELL));
const loadAfter = loadavg();

const cur = await off.evaluate(new Function('return (' + PROBE + ');'))
  .catch(e => ({ probeError: String(e && e.message || e), runs: [], docs: [], global: [] }));

const j = sink.join('\n');
const origin = (() => { try { return new URL(finalUrl).origin; } catch { return ''; } })();
const mine = (cur.docs || []).filter(d => d.url.startsWith(origin));
const runs = cur.runs || [];
const myRuns = runs.filter(r => (r.url || '').startsWith(origin));

/* THE REVISION THE NUMBER BELONGS TO, carried by the row itself. The shared checkout's artifact was rebuilt
   in the middle of the first pass of this census, so two rows that look comparable were measurements of two
   different programs. A row that names its own wasm hash and build head cannot be quoted against another
   build by accident. */
/* THE DIRECTORY THIS BLOCK DESCRIBES MUST BE THE ONE CHROME LOADED, AND THE DEFAULT WAS THE OTHER ONE.
   `run.sh`'s whole mechanism is that the LANE holds a private copy of `extension/` — harness.js derives its
   EXT_DIR from harness.js's own location, so Chrome loads `$LANE/extension`. Defaulting here to the shared
   checkout therefore described a directory the browser may never have opened, and the failure is silent and
   confident: a run whose `restart` was REFUSED (the harness declines a port another lane's browser holds)
   keeps driving the previous browser, while this block reports the shared tree's freshly staged hash and
   head. Measured: three rows published `builtFromHeadClaim` two builds ahead of the artifact that produced
   them, and the crash they carried resolved to a `file:line` the claimed head does not contain. So LANE
   decides, exactly as it decides for harness.js, and the `wasmSha256` below is then a hash OF the bytes
   Chrome loaded rather than of a neighbour's. */
const EXT = process.env.HARNESS_EXT_DIR || (process.env.LANE ? process.env.LANE + '/extension'
                                                             : '/home/user/APIClient/extension');
let artifact = { extDir: EXT, extDirFrom: process.env.HARNESS_EXT_DIR ? 'HARNESS_EXT_DIR'
                                        : process.env.LANE ? 'LANE' : 'shared-checkout-default' };
try { artifact.wasmSha256 = createHash('sha256').update(readFileSync(EXT + '/lib/qjs/qjs.wasm')).digest('hex'); } catch (e) { artifact.wasmErr = String(e.message); }
/* THE HASH IS THE ONLY FIELD THAT IS TRUE. `head` (and, on a pre-subtree artifact, `qjsHead`) is what the
   SOURCE TREE was called at build
   time, and the tree is edited continuously by other lanes -- so a build of a dirty tree records a revision
   whose sources do not contain the program that ran. That is not hypothetical: rows have carried a head whose
   quickjs.c lacks the very DFAIL text those same rows printed. They are kept because a wrong name is still a
   hint, and renamed so no reader can mistake them for the revision the number belongs to. */
try {
  const b = JSON.parse(readFileSync(EXT + '/lib/qjs/qjs.mjs.build.json', 'utf8'));
  /* `builtFromQjsHeadClaim` RECORDS AN ERA NOW, NOT AN ENGINE. It was the `engine/qjs` submodule's commit;
     the subtree merge made that path tracked content, so no build writes the field and `head` above names the
     whole program. A row that HAS it was produced by a pre-merge artifact — kept because that is real
     evidence about which artifact answered, and written as `null` rather than left `undefined` so an absent
     field and an unasked question do not read alike. */
  artifact.builtFromHeadClaim = b.head; artifact.builtAt = b.at;
  artifact.builtFromQjsHeadClaim = typeof b.qjsHead === 'string' ? b.qjsHead : null;
  /* THE STAMP ANSWERS A THREE-STATE QUESTION AND THIS ROW RECORDS ALL THREE. `dirty` alone used to carry both
     "asked git, nothing differs" and "could not ask git at all" -- and the second is the state in which the
     stamp knows NOTHING, so folding it into an empty `dirty` published the STRONGEST claim available (built
     from a clean revision) out of the weakest evidence. `unasked` is the paths whose state could not be read.
     ITS ABSENCE IS AGE, NOT A NEGATIVE ANSWER: an artifact stamped before that field existed has no opinion,
     which is a third thing again and is recorded as such rather than defaulted to `[]`. */
  if (b.dirty !== undefined) artifact.builtFromDirtyTree = b.dirty;
  if (b.unasked !== undefined) artifact.builtFromUnaskedPaths = b.unasked;
  /* A NON-EMPTY `dirty` IS A POSITIVE FACT and is reported as one whatever else the stamp does or does not
     carry -- paths that DIFFER were, necessarily, successfully asked about. Only an EMPTY list is ambiguous,
     and only then does `unasked` decide between "clean" and "nobody could tell". */
  artifact.cleanTreeClaimIs = b.dirty === undefined ? 'no-dirty-field(stamp predates it)'
    : b.dirty.length ? 'dirty(' + b.dirty.length + ' path(s) differ)'
      : b.unasked === undefined ? 'unprovable(stamp predates the unasked field — an empty dirty list here is NOT proof of a clean tree)'
        : b.unasked.length ? 'unprovable(' + b.unasked.length + ' path(s) git could not be asked about)'
          : 'clean(asked, nothing differs)';
} catch (e) { artifact.buildJsonErr = String(e.message); }
/* THE ID THE BROWSER ACTUALLY LOADED, carried beside the directory this block read. They are two answers to
   one question and a reader can now see both; an id that does not change between two rows whose `extDir`
   does is a `restart` that was refused, which is the state that produced this session's worst measurement. */
artifact.loadedExtId = loadedExtId;

/* THE TRUSTED ZONE CHROME LOADED, HASHED FOR THE SAME REASON `wasmSha256` IS. That hash is defended above as
   "a hash OF the bytes Chrome loaded rather than of a neighbour's", and the argument was made for one half of
   a two-half program. The other half is this zone's JavaScript, which CLAUDE.md §A-CROSS-BOUNDARY-DIFF calls
   INTERPRETED FROM THE TREE and therefore DEPLOYED ON WRITE: it needs no build, so NOTHING in
   `qjs.mjs.build.json` moves when it changes and every engine-side field on this row reads identically across
   two runs of two different programs.
   MEASURED, AND IT COST A LANE ITS PREMISE: two full 18-site censuses carried byte-identical `artifact`
   blocks — same `wasmSha256`, same `builtFromHeadClaim`, same `cleanTreeClaimIs: clean(asked, nothing
   differs)` — and one ran a trusted zone five fixes older than the other's tree, because the lane's pinned
   copy predated those fixes and a pin is not re-read. A later reader took the newer census for an AFTER of
   those fixes on exactly that evidence; what refuted it was hashing the pin's own safe-fetch.js by hand
   against the commit that fixed it. `cleanTreeClaimIs` is the trap rather than the cure — it is a claim about
   the ENGINE CONE at BUILD time and reads like one about the whole program.
   THE POPULATION IS DERIVED, NOT LISTED, AND THAT IS THE LOAD-BEARING HALF. A hand-kept list of the files
   this zone loads is the second copy CLAUDE.md forbids, and it would have been WRONG for the very change that
   motivated this field: one of those five fixes ADDS A FILE, so a list written the day before would have
   hashed unchanged files and reported no movement across the commit that added the primitive. Everything
   under the extension directory is hashed except the engine artifact already hashed above, and the FILE COUNT
   is published beside the digest so a population that changed size is visible in the row rather than
   recoverable only by re-deriving it.
   IT IS A DIGEST AND NOT A REVISION ON PURPOSE. A pin may hold uncommitted edits, a peer may have written one
   between the copy and the run, and a revision name cannot express either; the bytes can. */
artifact.trustedZone = (() => {
  const skip = EXT + '/lib/qjs';
  const files = [];
  (function walk(d) {
    let ents; try { ents = readdirSync(d, { withFileTypes: true }); } catch { return; }
    for (const e of ents) {
      const p = d + '/' + e.name;
      if (e.isDirectory()) { if (p !== skip) walk(p); } else if (e.isFile()) files.push(p);
    }
  })(EXT);
  if (!files.length) return null;
  files.sort();
  const h = createHash('sha256');
  for (const p of files) {
    h.update(p.slice(EXT.length + 1)); h.update('\0');
    h.update(createHash('sha256').update(readFileSync(p)).digest()); h.update('\0');
  }
  return { files: files.length, sha256: h.digest('hex') };
})();

/* A RUN SAYS WHICH OF FOUR STATES IT IS IN, AND IT SAYS IT IN `run`. This file asked `r.crashed`, a boolean
   bridge.js DELETED — its own comment says why, at the site: "IT IS `run` AND NOT `crashed:true`, AND THE
   BOOLEAN IS DELETED RATHER THAN KEPT BESIDE IT", because a flag that can say one of three states makes the
   other two the same value. The name survived on THIS side, so every read of it was `undefined`, and the
   census published `crashedRuns: 0` for pages whose every run aborted at engine creation — a false clean bill
   on the row a reader trusts most, and the mirror of the eight `|| 0` defaults CLAUDE.md records: a name read
   here and written nowhere, with nothing to make it crash. `crashWithoutReason`, the flag that exists to catch
   exactly this class, read `r.crashed` too and so could never raise.
   IT IS NOT A RENAME. Swapping the spelling would leave the next deletion just as silent, so the row's own
   producer contract is ASSERTED first: every `_engineLog` entry carries `run` (bridge.js's engineLogWrite
   writes it unconditionally and DCHECKs the word against RUN_OUTCOMES), so an entry without one is that seam
   having changed under this probe and must stop the row rather than be counted as a run of some kind.
   AND "NOT CRASHED" IS NOT WHAT THE COUNTER READS WANT. They want the runs that CARRY counters, which bridge
   states positively: a crashed run reports none at all, deliberately, "because seven zeroes read as a run that
   explored nothing". So the filter names the two outcomes that have them rather than negating one that does
   not. */
const RUN_WITH_COUNTERS = new Set(['complete', 'partial']);
for (const r of runs) {
  if (typeof r.run !== 'string') {
    console.log('ROW ' + JSON.stringify({ id, url, fatal: 'an _engineLog entry carries no `run` word — the ' +
      'offscreen’s run-outcome contract changed under this probe, so no count below it means anything', sample: r }));
    process.exit(0);
  }
}
const counted = myRuns.filter(r => RUN_WITH_COUNTERS.has(r.run));

/* THE ORDER THE FRONTIER WAS IN, WHICH IS THE ROW EVERY JOB COUNT ABOVE HAS TO BE READ THROUGH AND WHICH THIS
   FILE HAS NEVER CARRIED. `jobsRun: 0` beside a large `jobsQueued` reads as the scheduler failing to serve
   its own queue — §Every-runtime-job-is-a-scheduler-flow makes every reaction, microtask, timer and delivery a
   first-class member, so a queue that never moves reads as an ORDERING result. solver/flow.h's split is what
   refuses that: a queued job waits on the HOST (`jobsOwed`), on its member finishing its own program
   (`jobsFramed` — HTML §8.1.4.4 "Calling scripts", clean up after running script step 3), or on RANK
   (`jobsReady`), and ONLY THE LAST IS THE WFQ'S TO MOVE. A run whose backlog is all `jobsFramed` has
   nothing rank-eligible for the order to have got wrong, and its zero is a statement about members not
   finishing their programs — one component away from the row it invites blaming.
   IT WAS ON THE RUN RECORD THE WHOLE TIME. solver/result.c composes `_wfq` onto every document it builds,
   partials included; extension/bridge.js relays it WHOLE onto every engine-log row; the PROBE above takes
   `_engineLog` entries whole. This file, the one that ranks the corpus, was again the only reader missing —
   the FOURTH time this row has been the consumer that never asked for the field written to answer its own
   ambiguity, after `orphansAsked`, `unitsDone` and the @S arrival census.
   IT IS A GAUGE AND EVERY COUNTER ABOVE IS A LIFETIME COUNTER, which is why it is selected separately and why
   both indices are published. `jobsQueued`/`jobsRun`/`unitsDone` are monotone totals over the run; this is
   a WALK OF THE FRONTIER AT ONE INSTANT and can fall. Differencing it across samples is arithmetic on nothing,
   and pairing it with a counter from a DIFFERENT entry is the two-moments defect CLAUDE.md records as having
   been written three times and wrong twice. So `wfqFrom` and `countersFrom` are emitted: equal indices mean
   the split and the counters are ONE SAMPLE, and unequal indices mean they are not and may not be reconciled.
   AND THE LAST COUNTED ENTRY IS THE WRONG ONE TO ASK, which is why this walks backwards. bridge.js states the
   contract: no `_wfq` is a BROKEN CONTRACT, `{members: 0}` is an EMPTY FRONTIER carrying NO term rows at
   all, and a full object is a READING. A finalize document is composed after the frontier drained or parked,
   so the last counted entry is routinely `{members: 0}` — taking the split from it would report `null` for
   every run that finished, which is the reading of that instant and not of the run. The last entry with a LIVE
   frontier is the one that observed an order. Where no entry has one, these are `null` and never 0: a census
   that never saw a standing frontier and one that saw a frontier with no backlog are different findings. */
const wfqLive = (() => {
  for (let i = counted.length - 1; i >= 0; i--) {
    const w = counted[i].wfq;
    if (w && typeof w === 'object' && !Array.isArray(w) && w.members > 0) return { w, i };
  }
  return null;
})();
/* ABSENT STAYS ABSENT. An artifact older than a given row omits it, and `|| 0` would turn "this build does
   not publish that row" into "the engine measured zero" — the defaulted-field defect, in the instrument. */
/* WHAT THIS ROW ASKED THE CENSUS FOR, RECORDED BY THE ASK ITSELF AND NEVER BY A LIST BESIDE IT. Every pick
   below adds its key here, so the set of census rows this file consumes IS the act of consuming them and
   there is nothing to keep in step; `unaskedRelatives` at the foot reads it against the keys the engine
   actually published on the objects this row was taken from.
   IT IS A CENSUS OF THE QUESTION AND NOT OF THE ANSWER, which is why `wfqRow` records before it tests. A row
   this file asks for that an older artifact does not carry is still ASKED — that is a fact about this file —
   and recording at the answer would make the set shrink against exactly the artifacts whose gaps it exists
   to describe (CLAUDE.md's rule that an invariant over a gated operation censuses the ASK, never the
   outcome, or it re-implements the gate's own legitimate refusals as absences). */
const taken = new Set();
const wfqRow = (k) => (taken.add(k), wfqLive && typeof wfqLive.w[k] === 'number' ? wfqLive.w[k] : null);
/* THE POPULATION `flow_step`'s LADDER CANNOT REACH AT ALL, WHICH THIS FILE HAS NEVER CARRIED AND WHICH NO
   ROW ABOVE CAN BE DERIVED INTO. Running a queued task, host-blocking, the lifecycle events, resuming a
   parked orphan drive, seeding an orphan, a rendering opportunity, a due timer, an idle period, a host-owed
   reply, a close request and FINISHED all sit in the ELSE of solver/engine.c's `if (f->script_i < f->dyn_n)`
   — so a member whose cursor still names a program row can reach NONE of them, and `live - outOfPrograms` is
   exactly how many members stand in that state. NO COUNT OF THOSE ARMS IS STATED HERE: the list moves and the
   CONDITION is the durable fact, so grep that test inside `flow_step` for the set this engine has today.
   It was an INFERENCE FROM ARM HISTOGRAMS until this row, and the two halves of the subtraction are ONE
   WALK AT ONE INSTANT: solver/cold.c raises `out->flows++` at the top of the loop and
   `out->out_of_programs++` inside the same body, and solver/result.c splices both into ONE `composef` whose
   format opens `{"live":%ld` and closes `"programCursors":%s}`. That is the whole reason they are carried
   together in one object rather than flat beside the lifetime counters above: flat, a reader would subtract
   `wfqMembers` — a DIFFERENT walk's count, taken at whichever entry `wfqFrom` names — and that subtraction is
   the two-moments defect this file already had to publish two indices to refuse.
   IT IS A GAUGE, SO IT TAKES `wfqLive`'s BACKWARD WALK AND NOT `counted[last]`. solver/cold.h states the
   hazard by name: the walk visits the frontier's STANDING members, so "a census taken when `live` is 0
   reports 0 whatever every member did before it left", and it measured both readings in one session — a real
   bundle holding this row at ~1 live member in 10 from the first census through the 129th while orphan asks
   stayed 0, and a DRAINING fixture whose terminal census read this row 0 with `programCursors` `{0: 0}` and
   `live` 0 after asking 1536 orphans on the way. The same 0, and the opposite fact. `steps` and
   `stepUnitRuns` above take the LAST counted entry because result.c calls them lifetime counts and they
   cannot fall; this one can, so the last entry that observed a STANDING frontier is the only one that
   observed anything at all, and `from` says which entry that was so a reader can align it with `wfqFrom` and
   `countersFrom` instead of assuming.
   THE PARTITION IS DERIVED AND NEVER HAND-TYPED. The rows beneath the total are whatever numeric keys the
   composer spells with that prefix, so a fourth arm added to cold.c's if/else chain is carried by this row
   the day it lands — which is the SEVENTH time this file would otherwise have been the consumer that never
   asked for the field written to answer its own ambiguity, after `orphansAsked`, `unitsDone`, the @S arrival
   census, the WFQ split, the arrivals/departures pair and the step histogram. `outOfProgramsAtTheLadderUnits`
   is spliced with `%s` and is an OBJECT, so the numeric test excludes it — the same split engine/build.mjs's
   `censusRowSet` makes by reading the CONVERSION in the format string rather than an exclusion list beside it.
   AND THE SUM IS THE CONTRACT, WHICH IS WHY IT STOPS THE ROW. cold.c DCHECKs
   `unrun + framed + atTheLadder == outOfPrograms` where the whole population is in hand and one member was
   not — and a DCHECK is compiled out of the release build this census drives, so this is the only place that
   identity is checked on the artifact a corpus run actually measures. A disagreement means an arm was added
   without a row and the breakdown is "a SELECTION being read as a partition" in cold.c's own words, which
   makes the total and every part under it a guess — the same reason a missing `run` word stops the row above
   rather than being counted as a run of some kind.
   ABSENT STAYS ABSENT, AND THE TWO SILENCES ARE DIFFERENT FACTS. No counted entry ever holding a standing
   frontier is `null`, exactly as `wfqMembers` is. An artifact whose build predates these rows yields the
   object with `live` stated and the total absent — so "this census never saw a frontier" and "this build does
   not publish the row" are never the same value, and neither is ever `0`. */
const coldLive = (() => {
  for (let i = counted.length - 1; i >= 0; i--) {
    const c = counted[i].cold;
    if (c && typeof c === 'object' && !Array.isArray(c) && c.live > 0) return { c, i };
  }
  return null;
})();
const frontierPrograms = (() => {
  if (!coldLive) return null;
  const c = coldLive.c;
  const out = { live: c.live };
  if (typeof c.outOfPrograms === 'number') {
    const parts = Object.keys(c).filter(k => k !== 'outOfPrograms' && k.startsWith('outOfPrograms')
                                             && typeof c[k] === 'number');
    /* THE PARTS ARE ONLY CHECKED AGAINST THE TOTAL WHERE THERE ARE PARTS. A build publishing the total and
       no breakdown is an older artifact, not a broken partition, and summing nothing to a non-zero total
       would stop the row for having measured an engine that never claimed a partition at all. */
    if (parts.length) {
      const sum = parts.reduce((a, k) => a + c[k], 0);
      if (sum !== c.outOfPrograms) {
        console.log('ROW ' + JSON.stringify({ id, url, fatal: 'the frontier census breaks `outOfPrograms` ' +
          c.outOfPrograms + ' into rows summing to ' + sum + ' — solver/cold.c raises them on one if/else ' +
          'chain over one walk and DCHECKs the identity, so a disagreement on this artifact is an arm added ' +
          'without a row and the breakdown is a SELECTION being published as a partition', cold: c }));
        process.exit(0);
      }
      for (const k of parts) out[k] = c[k];
    }
    out.outOfPrograms = c.outOfPrograms;
  }
  const pc = c.programCursors;
  if (pc && typeof pc === 'object' && !Array.isArray(pc)) out.programCursors = pc;
  /* THE KEYS THIS OBJECT ENDED UP WITH ARE WHAT IT TOOK, so the derived partition above registers itself
     without being named twice. `from` is this file's own index and not a census row, so it is added after. */
  for (const k of Object.keys(out)) taken.add(k);
  out.from = coldLive.i;
  return out;
})();
const row = {
  id, url, finalUrl, status, nav, artifact, measuredAt: new Date().toISOString(),
  dwellMs: DWELL, cores: cpus().length, loadBefore, loadAfter,
  runsTotal: runs.length,
  runsMine: myRuns.length,
  crashedRuns: runs.filter(r => r.run === 'crashed').length,
  crashedMine: myRuns.filter(r => r.run === 'crashed').length,
  /* WHAT THE RUNS OF THIS ORIGIN ACTUALLY SAID, spelled out rather than summarised into one boolean — the
     shape that produced this defect. A site whose engine never started and a site that finished with nothing
     are different findings and this is where they stop looking alike. */
  runOutcomesMine: myRuns.reduce((m, r) => (m[r.run] = (m[r.run] || 0) + 1, m), {}),
  /* EVERY ENTRY CARRIES THE RUN'S CUMULATIVE TOTAL, so a SUM counts one endpoint once per snapshot and a MAX
     over a log that is never cleared between page loads is non-decreasing by construction. This file reported
     37 for a page that had learned ONE, and reported three repetitions as a rising spread when the instrument
     could not have produced a falling one. `_engineLog` is declared at load and trimmed to 200 entries; it is
     not a per-run buffer. So: read the LAST entry, which is that run's own total, and count DISTINCT addresses
     for the headline. `endpointSnapshots` keeps the old quantity under a name that says what it is, because it
     is still the right thing to watch a live run advance by. */
  endpoints: counted.length ? counted[counted.length - 1].endpoints : null,
  endpointSnapshots: counted.length,
  sinks: counted.length ? counted[counted.length - 1].sinks : null,
  /* THE RUNG THE @S SEARCH DIED AT, WHICH `sinks` ALONE CANNOT NAME. Emission is working-PoC-only and
     fire-verified, so `sinks: 0` is the reading for BOTH "no tainted value ever reached a sink" and "sinks
     were reached, candidates were constructed, none of them fired" — opposite findings needing opposite fixes,
     and indistinguishable from outside. `_candidates` is solve_candidate_count(), the searches' own tally, and
     bridge.js has been relaying it onto every run record all along; only this row never asked for it. Measured
     across eight live app pages, every one read `sinks: 0`, and without this there was no way to say whether
     that was a page with no sinks or a search that never got to one. */
  candidates: counted.length ? counted[counted.length - 1].candidates : null,
  /* AND THE FOUR RUNGS BELOW `candidates`, BECAUSE `candidates: 0` HAS THE SAME TWO READINGS ONE STEP DOWN.
     The comment above bought one rung and stopped: measured over three passes of twelve live app pages,
     EVERY measurable site read `sinks: 0` AND `candidates: 0`, and the row could not say whether the search
     had nothing to search (no attacker source was ever read) or never got there (sources read, no sink
     reached) or was declined (taint reached a sink and the check was unforgeable, so §Attacker-sources
     SUPPRESSES it — correctly, and it must never look like a page with no sinks). Those are three opposite
     findings behind one zero, which is exactly what §@S forbids: "a candidate killed by a gate must be
     distinguishable from one a filter ate and from one that was never scheduled".
     `bridge.js` has been forwarding all four onto every run record with a comment naming them "the only
     thing that distinguishes an analysed page with nothing to find from a page nobody got to"; this row —
     the one that ranks the corpus — was the consumer that never asked. A written field with no reader is
     the same broken contract as a read field with no writer, and it is harder to see because the value is
     real and asserted and consumed by nothing. */
  sourceReads: counted.length ? counted[counted.length - 1].sourceReads : null,
  sinkReached: counted.length ? counted[counted.length - 1].sinkReached : null,
  sinkTainted: counted.length ? counted[counted.length - 1].sinkTainted : null,
  sinkSuppressed: counted.length ? counted[counted.length - 1].sinkSuppressed : null,
  /* THE ABSENT-GLOBAL CENSUS, WHICH IS THE ONLY THING THAT SEPARATES "THIS PAGE HAD NOTHING BEHIND THAT
     GUARD" FROM "WE COULD NOT LOOK" — the same defect as the five fields above, one rung further out.
     §NO-STUBS: a page writes `if (window.X)`, this engine does not have `X`, the read is CORRECTLY decided
     false, the fallback branch runs, and every endpoint and sink behind the true branch is unreachable with
     nothing anywhere saying so. That is the one absence this project's forcing function cannot surface,
     because nothing throws — so it needs an instrument rather than a crash. `solver/absent.c` has been
     counting it and `bridge.js` has been relaying it onto every run record; measured before this diff, ZERO
     of the 85 committed censuses carried it, so the row that RANKS the corpus was once again the consumer
     that never asked.
     BOTH NUMBERS OR NEITHER, because the FRACTION is the whole point and a numerator alone reproduces the
     ambiguity this row exists to remove: `absentOwed: 0` against `absentAsked > 0` is the positive statement
     that this engine answered every name the standards were asked for, and `absentOwed: 0` against
     `absentAsked: 0` is a census that was never reached. Opposite findings, one digit.
     A MISSING KEY IS FATAL RATHER THAN NULL. `null` means the question was not asked — no counted run, or an
     artifact too old to publish the census. A census that IS present and does not carry these rows is a
     RENAMED KEY in absent.c, and defaulting that to 0 would report a clean engine for as long as the drift
     stood. The keys are matched by a distinctive SUBSTRING and the match is asserted to be exactly one, so a
     rename is loud and a punctuation edit is survivable; a hand-copied full key would go quietly to null.
     RETIREMENT: this record goes when this file derives its census key names from absent_json()'s own
     composer rather than matching copies of them. */
  ...(() => {
    const pick = (a, needle) => {
      const hits = Object.keys(a).filter((k) => k.includes(needle));
      if (hits.length !== 1) return { err: hits.length + ' keys of the `absent` census contain ' +
        JSON.stringify(needle) + ' — solver/absent.c renamed or duplicated a composer row and this file ' +
        'matches a copy of it; re-derive from absent_json() rather than defaulting to 0' };
      if (typeof a[hits[0]] !== 'number') return { err: 'the `absent` census states ' +
        JSON.stringify(hits[0]) + ' as a non-number' };
      return { v: a[hits[0]] };
    };
    if (!counted.length) return { absentAsked: null, absentOwed: null };
    const a = counted[counted.length - 1].absent;
    if (a === undefined || a === null) return { absentAsked: null, absentOwed: null };
    const asked = pick(a, 'reads of the global object');
    const owed = pick(a, 'a standard owns it');
    if (asked.err || owed.err) return { absentFatal: asked.err || owed.err };
    return { absentAsked: asked.v, absentOwed: owed.v };
  })(),
  /* THE ORPHAN SURFACE, WHICH IS THE HEADLINE ONE AND HAD NO COLUMN. §What-the-tool-produces is "what the
     bundle CAN do but didn't", and until the engine's own pair crossed the result document, whether a session
     ever drove a function the page never called could only be read off a stdout the renderer does not tee.
     BOTH OR NEITHER: `orphansDriven: 0` alone is three findings — the bundle ships no uncalled code, the walk
     ran and the heap had none, or no flow ever reached the end of its own work — and only `orphansAsked`
     picks out the middle one, which is the only one of the three that is a scheduling result to act on. */
  orphansDriven: counted.length ? counted[counted.length - 1].orphansDriven : null,
  orphansAsked: counted.length ? counted[counted.length - 1].orphansAsked : null,
  flows: counted.length ? counted[counted.length - 1].flows : null,
  switches: counted.length ? counted[counted.length - 1].switches : null,
  /* QUEUED BESIDE RUN, for the same reason held is emitted beside made: `jobsRun: 0` alone cannot say whether
     the pump never ran or nothing was ever enqueued for it, and those are a scheduler bug and a page that got
     nowhere. */
  jobsQueued: counted.length ? counted[counted.length - 1].jobsQueued : null,
  jobsRun: counted.length ? counted[counted.length - 1].jobsRun : null,
  /* …AND THE PRECONDITION FOR RUNNING ONE, WHICH IS WHAT MAKES `jobsRun` READABLE AT ALL — the third time this
     row has been the consumer that never asked for the field written to answer its own ambiguity. Every job arm
     is under HTML §8.1.4.4 "Calling scripts"' clean-up-after-running-script step 3 boundary (the JavaScript
     execution context stack is empty), so a low `jobsRun` beside a huge `jobsQueued` has TWO opposite readings
     and this row could state neither: flows are reaching that boundary and finding nothing to do, or NO FLOW
     EVER REACHES IT — no program in the document finishes, so the reaction pump is never eligible. Those need
     opposite fixes. `solver/engine.c`'s g_units_done exists for precisely that distinction and says so at its
     own site; `bridge.js` has been relaying it onto every run record under a comment naming the same two
     readings; `popup.js` renders it. This file, the one that ranks the corpus, was the only reader missing, and
     a written field with no reader is the same broken contract as a read field with no writer.
     WHAT IT COST, MEASURED: openlibrary.org over three passes read `jobsQueued` 63946/74944/78738 against
     `jobsRun` 67/67/67 — an identical integer across runs whose flow counts differ by 30%, which is a lock-out
     and not a throughput limit — and the row could not say which of the two it was. Read off the live
     offscreen, `unitsDone` was 2 at 116093 jobs queued and 1012 flows: the second reading, and the reason
     `sinkReached`, `orphansAsked` and the endpoint surface above it were all zero on a page that ships nine
     `innerHTML` sites. The @S rungs beside it are uninterpretable without this one. */
  unitsDone: counted.length ? counted[counted.length - 1].unitsDone : null,
  parked: counted.length ? counted[counted.length - 1].park : null,
  /* WHETHER A RESUMED PROGRAM EVER ENDS, WHICH IS THE ONE QUESTION `jobsFramed` POSES AND CANNOT ANSWER.
     `jobsFramed` says a backlog waits on members finishing their own programs; it does not say whether any
     member ever does. solver/step_unit.h splits a resume's FOUR outcomes and states the contract in its own
     words: "`resume-program` therefore means, and only means, A RESUME THAT LEFT THE FRAME LIVE." Two arms
     leave the member framed (`resume-program`, `report-an-exception`) and two do not
     (`resume-ended-its-frame`, `program-detached-its-base`), so reading the first arm alone as "the thread
     resumes programs that never finish" is the DID-NOT-END half quoted as the whole — the misreading that
     header's own banner records having happened once already, where 2582 of 2630 steps in that row was
     "consistent with a frontier ending a program on nearly every step AND with one that has ended nothing
     since its first — two opposite diagnoses, and the row was the whole of the evidence for both".
     IT WAS ON THE RUN RECORD THE WHOLE TIME — the SIXTH time this row has been the consumer that never asked
     for the field written to answer its own ambiguity, after `orphansAsked`, `unitsDone`, the @S arrival
     census, the WFQ split and the arrivals/departures pair. solver/result.c composes the histogram into every
     document it builds; bridge.js relays `_cold` WHOLE onto every engine-log row and ASSERTS it (a histogram
     shape, never defaulted); this file was the only reader missing.
     LIFETIME, SO IT TAKES THE LAST COUNTED ENTRY and not `wfqLive`'s backward walk. `stepUnitRuns` and
     `steps` are LIFETIME COUNTS by result.c's own line; `_cold.stepUnits` beside them is a GAUGE over the
     standing frontier and is deliberately NOT carried here, because pairing a gauge with these would be the
     two-moments defect one field over. `countersFrom` already says which entry all of these came from.
     ABSENT STAYS ABSENT: an artifact predating the histogram omits it, and a `{}` would read as an engine
     that ran no steps. */
  stepUnitRuns: (() => {
    taken.add('stepUnitRuns');
    if (!counted.length) return null;
    const c = counted[counted.length - 1].cold;
    if (!c || typeof c !== 'object' || Array.isArray(c)) return null;
    const h = c.stepUnitRuns;
    return (h && typeof h === 'object' && !Array.isArray(h)) ? h : null;
  })(),
  steps: (() => {
    taken.add('steps');
    if (!counted.length) return null;
    const c = counted[counted.length - 1].cold;
    if (!c || typeof c !== 'object' || Array.isArray(c)) return null;
    return typeof c.steps === 'number' ? c.steps : null;
  })(),
  /* …AND HOW MANY OF THOSE STANDING MEMBERS COULD REACH `flow_step`'s LADDER AT ALL — `live` and the
     out-of-programs partition it is taken over, from ONE walk, so `live - outOfPrograms` is the size of the
     population every arm below that cursor test excludes. Composed above `row` with its own backward walk;
     see that block for why it is not `counted[last]` and why the three rows are derived rather than named. */
  frontierPrograms,
  /* …AND WHAT THE JOB BACKLOG ABOVE IS ACTUALLY WAITING ON — see `wfqLive`. Read `jobsReady` with
     `jobWGap` and never alone (a gap of 0 is both "no ready holder" and "the top of the queue holds a
     runnable job"), and read a `jobsReady: 0` with `memUnframed`, which separates its two silences: with
     `memUnframed: 0` the resume seam is not ending frames and the reader goes to flow_step, and with
     `memUnframed > 0` the jobs sit on framed members while the unframed hold none and the reader goes to
     where jobs are queued. `wfqMembers` is the population all five are taken over. */
  jobsReady: wfqRow('jobsReady'),
  jobsFramed: wfqRow('jobsFramed'),
  jobsOwed: wfqRow('jobsOwed'),
  jobWGap: wfqRow('jobWGap'),
  memUnframed: wfqRow('memUnframed'),
  wfqMembers: wfqLive ? wfqLive.w.members : null,
  /* AND WHETHER ANYTHING HAS EVER LEFT, WHICH THIS ROW DERIVED BY SUBTRACTION WHEN THE ENGINE STATES IT.
     report.mjs composed `gone` as `flows - wfqMembers` under a guard that the two halves came from ONE
     census entry, and that subtraction is ARITHMETICALLY SOUND -- `_flows` is flow_created_count(), whose
     `g_flows_created++` sits in flow_new beside `g_arrivals++`, the two being the same one increment in the
     one constructor and neither ever decremented, so `_flows - members` IS `g_departures` for all time, and
     flow.c asserts that identity twice (`g_arrivals - g_departures == g_flows_n`, and again over the census
     it is about to publish). It was never wrong. It was DERIVED, and a derived half cannot be checked: the
     subtraction is a number for every pair of inputs, including the pair where one of them stopped being
     written, which is the argument solver/result.c already makes for emitting `finished` and `sold` as rows
     rather than leaving either to a consumer. The producer emits both halves here too -- result.c composes
     `"arrivals":%lld,"departures":%lld` into `_wfq` and states their identity with `members` in the comment
     directly above it -- and bridge.js relays `_wfq` WHOLE, so this reader needed nothing built and no field
     plumbed. It is the FIFTH time this row has been the consumer that never asked for the field written to
     answer its own ambiguity, after `orphansAsked`, `unitsDone`, the @S arrival census and the WFQ split.
     AND THE GUARD GOES WITH THE SUBTRACTION RATHER THAN SURVIVING IT. `wfqFrom === countersFrom` exists
     because the two halves were selected by DIFFERENT rules: `flows` off `counted[counted.length - 1]` and
     `members` off the last entry with a live frontier, which are the same entry only when the run's last
     counted document still held one. These two are read through `wfqRow` off the SAME object as `members`,
     so they are one sample BY CONSTRUCTION and there is no reconciliation left to be silent about. The pair
     below stays: it is still what says which entry the fifteen counters above came from.
     ABSENT STAYS ABSENT, and here that is a statement about the READER and not the writer: existing
     `census-cc-*.jsonl` rows carry no `wfqDepartures` because this file never asked for it, while the
     artifact that produced them emits it -- so those rows read `-` rather than 0, and a re-run is what fills
     them rather than a default. THAT COSTS NOTHING TO CARRY, because those rows are not tracked at all:
     `.gitignore` ignores `testing/corpus/census-*.jsonl` deliberately, so there is no historical corpus for
     this reader to be unable to speak about -- every census that outlives its own session is one this file
     writes from here on. */
  wfqArrivals: wfqRow('arrivals'),
  wfqDepartures: wfqRow('departures'),
  /* WHICH ENTRY EACH HALF CAME FROM, so the gauge and the counters can never be silently reconciled. */
  wfqFrom: wfqLive ? wfqLive.i : null,
  countersFrom: counted.length ? counted.length - 1 : null,
  /* AND WHICH *SCOPE* THEY ARE, WHICH IS A SECOND AXIS AND THE ONE A READER ACTUALLY GETS WRONG. The pair
     above separates two MOMENTS; this separates two POPULATIONS, and the row mixes them by construction:
     fifteen fields above are read off `counted[counted.length - 1]`, which is ONE RUN's cumulative
     totals -- the counters behind them are per-instance statics with no reset anywhere, so a fresh run is
     a fresh set -- while `docsAnswered`, `docsSeenMine`, `siteEndpoints` and `distinctEndpoints` below are
     UNIONS over every document of every run at this origin. Nothing in the key vocabulary said so, and a
     number's SCOPE is exactly as unquotable-without as its KIND (CLAUDE.md §A-GAUGE-AND-A-LIFETIME-COUNTER,
     one axis over).
     WHAT IT COST, MEASURED: a coordinator read `jobsQueued 176` and `jobsRun 1` off a gitpod row, put them
     beside `distinctEndpoints 90` in a table, and reported a scheduler lock-out. That row's `runsTotal` is
     SEVEN. The counters were one run's and the endpoint count was seven runs'; a lane then established the
     backlog was `jobsFramed` throughout with `jobsOwed` 0 in 229 of 229 censuses, so there was nothing
     rank-eligible for an ordering to have got wrong and the reported defect did not exist.
     AND THE COINCIDENCE IS WHY NOBODY CAUGHT IT: on those same rows `endpoints` (one run) and
     `distinctEndpoints` (the union) are BOTH 90, because one run happened to carry every address. Two
     differently-scoped fields agreeing is the spot-check that validates a number by the absence of the
     case it cannot see, and it sat two lines apart in this file's own output.
     IT IS A STRING AND NOT A COUNT, deliberately: a count can be summed, differenced or compared against a
     neighbour, which is the whole failure being fixed, and this must only ever be READ. It also does not
     match a `\w*` sweep over any existing counter name, so every query already written against archived
     censuses keeps measuring what it measured (CLAUDE.md's rule about a new key matching an old pattern).
     IT RETIRES when the per-run block states its own scope in its keys -- which is a rename, so it waits
     for a pass that can re-derive every archived comparison rather than being smuggled in beside a fix. */
  countersScope: counted.length
    ? 'ONE RUN (' + counted.length + ' counted of ' + myRuns.length + ' at this origin) -- NOT comparable '
      + 'with docsAnswered/docsSeenMine/siteEndpoints/distinctEndpoints, which union every run'
    : null,
  docsAnswered: mine.filter(d => d.answered).length,
  docsSeenMine: mine.length,
  docsAllOrigins: [...new Set((cur.docs || []).map(d => { try { return new URL(d.url).origin; } catch { return d.url; } }))],
  /* THE HEADLINE NUMBER: distinct addresses learned, which is what "endpoints" has to mean in a report.
     IT IS A REACH FIGURE AND NOT AN API-SURFACE FIGURE, AND IT HAS ALREADY BEEN QUOTED AS THE SECOND.
     CLAUDE.md is explicit that "Static assets are NEVER endpoints (magic-byte + content-type, not URL
     suffix) but still drive the code path" -- so a learned address counted here is a place the engine got
     to, and whether it is part of the app's API is a question ANOTHER component answers from the RESPONSE.
     WHAT IT COST, MEASURED: a coordinator read 90 off a gitpod row and reported to the user, twice, that a
     real production SPA now yields 90 endpoints -- as evidence the engine works on real apps. Counting the
     addresses in that row's own `siteEndpoints`: 89 end `.js` and 1 ends `.svg`, and nothing else is in it.
     They are the app's own module graph -- the rolldown runtime, the vendor chunk, the generated `*_pb`
     descriptor modules, an icon. Zero derived API addresses. The count was exactly right and the claim made
     from it was not, which is why this sits at the field and not in a report.
     THE SUFFIXES ABOVE ARE A DESCRIPTION AND NOT A CLASSIFIER, deliberately: deciding what a thing IS from
     its URL suffix is the banned name-matching, and the real classifier reads response magic-bytes.
     THE SENTENCE THAT STOOD HERE SAID THE CLASSIFIER "COULD NOT RUN, BECAUSE EVERY ONE OF THOSE ADDRESSES
     WAS REFUSED AT THE CHOKEPOINT", and unified the two silences: "the refusal that stopped the descriptor
     decode is the same refusal that leaves every chunk unclassified". It is REWRITTEN RATHER THAN DELETED
     because the unification is what a reader re-derives, and because it was a claim about THIS TREE rather
     than about a standard, so it rotted exactly the way such a claim rots. The chokepoint's default
     permissions are DATA in lib/safe-fetch.js, and two arms name `destination` `program` and `destination`
     `subresource` under no further condition -- a script, a module import, a lazy chunk and a stylesheet
     all fire at an unconfigured origin. A drive of THIS page at engine 3a0929e8 read the engine-side
     decline counter at zero in three passes. The chunks are FETCHED. Two silences, not one.
     AND THE RETIREMENT LEAVES THIS ROW WORSE OFF RATHER THAN BETTER, which is why it is not a tidy-up.
     While everything was refused, "no API surface" and "every API request refused" were one story with one
     cure. They are now two, they take OPPOSITE work -- improve the driving, or widen the origin -- and
     NOTHING THIS FILE EMITS SEPARATES THEM. Derive that rather than trust it:
     `grep -oE '^  [a-zA-Z_]+:' testing/corpus/site.mjs` lists every field this object publishes and no
     member of that list names a refusal. A page's own `fetch()` carries the EMPTY destination, which is
     neither permitted word, so it fires only on the arms additionally requiring an unpinned address or an
     observed pair -- and an address a forced equality PINNED is precisely the gated API surface this
     product exists to reach. That population is declined by default, correctly and configurably, and is
     invisible in every column here.
     NAMED RESIDUAL, AND IT IS NOW TWO. NOT COVERED: (a) this row cannot state its own composition, because
     the fact that would split it (is this address an asset) is not in the record it reads -- the
     classification lives on a decoded response in the offscreen store and never reaches the census; (b)
     this row cannot state what the chokepoint REFUSED, though the chokepoint already names every refusal
     in a vocabulary of its own. NEXT DIFF: (a) the record carrying the classifier's own answer, so
     learned-addresses and classified-as-asset are two columns neither quotable as the other; (b) a refusal
     count split by the SIGNAL that refused, so a row with no API surface says which reading it is.
     HOW EITHER ABSENCE SHOWS: (a) an endpoint count read as an API surface by a reader who would have to
     open the address list to find out otherwise; (b) a reader concluding the driving is weak from a low
     endpoint count, on a run where the driving reached every address and the policy declined them.
     STILL UNESTABLISHED, said because a retirement that overclaims is worse than what it replaces: whether
     a refusal of a PARSER-INSERTED subresource reaches that decline counter at all. The zero above is
     evidence about requests the engine ASKED for and must not be read as evidence about the parser's.
     RETIREMENT: both halves go when this object publishes a classified-as-asset count and a refusal count,
     because the two readings are then separated by the row rather than by this paragraph. */
  siteEndpoints: [...new Set(mine.flatMap(d => d.sites))],
  distinctEndpoints: new Set(mine.flatMap(d => d.sites)).size,
  pageErrors: [...new Set(mine.flatMap(d => d.errs))].slice(0, 40),
  globalEndpoints: (cur.global || []).length,
  /* `null` = the probe reached no `parameters` object; an object = it did. NOT defaulted to an empty object:
     absence and zero are different facts here and the column exists to keep them apart. THE COLUMNS ARE NOT
     RESTATED HERE either — this sentence listed four of the six and drifted with the one above it, which is
     why the probe's own banner now carries the set and these two carry none. */
  domains: cur.domains === undefined ? null : cur.domains,
  /* THE ENGINE'S OWN RECORD FIRST, THE CONSOLE ONLY AS A SUPPLEMENT. A console scrape is the wrong surface by
     construction -- the renderer does not tee its stdout -- so a run whose abort reached the result document
     and not the console read `why: []`, and this harness reported a site that ABORTED as one that ran clean
     and learned nothing. Those are opposite findings, and the empty array was a property of the instrument.
     `errs` is what the engine itself recorded; the console is unioned in because a crash before the document
     exists has nowhere else to go. */
  why: [...new Set([...mine.flatMap(d => d.errs || []), ...(j.match(/@WHY[^\n]*/g) || [])]
                   .filter(x => typeof x === 'string' && x.includes('@WHY')))],
  atE: [...new Set(j.match(/@E [^\n]*/g) || [])].slice(0, 10),
  /* THE TWO HALVES OF ONE FACT, READ TOGETHER. A crashed run with no reason, or a reason with no crashed run,
     means one of the two producers is not being read -- which is the defect this row already suffered once.
     `@E` COUNTS AS A REASON. It was left out, so a run killed by a CHECK -- always-fatal in dev AND release,
     the loudest thing the engine can say -- was reported as a crash nobody could explain, while the row's own
     `atE` field held the cond and the file:line. Every abort on this corpus that fires a CHECK rather than a
     DCHECK read that way, which is a flag that raises itself precisely where it is least true. */
  crashWithoutReason: runs.filter(r => r.run === 'crashed').length > 0 &&
                      [...mine.flatMap(d => d.errs || []), ...(j.match(/@WHY[^\n]*/g) || []),
                       ...(j.match(/@E [^\n]*/g) || [])].length === 0,
  crashesFlag: cur.crashes,
  consoleLines: sink.length,
  probeError: cur.probeError,
  /* THE TRANSCRIPT THIS ROW BELONGS TO, BY NAME. A reader that reconstructs the name from `id` has to know
     which passes ran and in what order; a reader handed the name reads exactly the console the row's own
     counters came out of. `pass` is echoed beside it so a census file alone says how its rows were grouped. */
  pass, logFile: LOG_NAME,
};
/* WHAT THE ENGINE PUBLISHED BESIDE A ROW THIS FILE TOOK, AND THIS FILE DID NOT TAKE. Seven times now this
   file has been the consumer that never asked for the field written to answer its own ambiguity, and every
   one of those was repaired AT ITS OWN SITE -- seven corrected predicates and nothing anywhere that makes
   the eighth loud. CLAUDE.md names the diff to prefer when a fix keeps recurring: the MISSING INVARIANT
   rather than the rewritten predicate, because one check covers every future spelling of the question.
   THE POPULATION IS NOT "EVERY ROW THIS FILE DROPS", WHICH WAS MEASURED AND REFUSED. The four censuses
   result.c composes carry 211 rows and this row takes 17 of them; accusing the other 194 is a finding
   against 92% of the population, and a verdict red on every run is furniture that buries the next real
   thing under it. What every one of the
   seven had instead is a SHAPE: the engine published a row whose name EXTENDS one this file already carried
   -- a total whose parts, a gauge whose split, a count whose partition -- so the ambiguity and its answer sat
   on one relayed object under two names and only one was read.
   BOTH SIDES ARE DERIVED AND NEITHER IS TYPED HERE. The available set is `Object.keys` of the census objects
   this row was actually taken from -- bridge.js relays `_cold` and `_wfq` WHOLE, so at this line every key
   the engine published is in hand -- and the taken set is the picks' own record. A hand-kept list of expected
   names would be the eighth copy of the fact that has now drifted seven times.
   AND IT IS DECIDED AT RUNTIME BECAUSE NO STATIC SCAN OF THIS FILE CAN DECIDE IT, MEASURED IN BOTH
   DIRECTIONS. A name-grep over this file reports `finished` and `sold` as read when all three occurrences are
   PROSE, and reports `outOfProgramsUnrun` as unread when the block above derives it by prefix and spells it
   nowhere. This tree's own two conventions guarantee both errors -- decisions are recorded in prose at the
   site, and consumers derive their row set rather than hand-listing it -- so the false positive and the false
   negative are not bad luck, they are what the conventions produce. engine/fieldgate.mjs is blind here for
   the second reason and not the first: its WRITE-NO-READER question is existential over consumers, and
   engine/build.mjs's `censusRowSet` derives all 84 `_cold` rows for the smoke, so every one of these names
   HAS a reader and always did. The defect was never reachability; it is one named consumer's completeness,
   which is a universal question about this file and not an existential one about the corpus.
   IT IS A LIST AND NOT A COLOUR, AND IT DOES NOT STOP THE ROW. A census run that died because a lane added a
   row to the engine would stop every lane for a documentation-shaped reason. `[]` is an observed empty
   answer and `null` is no census to ask, which are different facts and neither is the other.
   ARMED, AND THE CONTROL IS HISTORICAL RATHER THAN INVENTED: run against the carried set as it stood before
   the `_wfq` split landed (`jobs` taken, the split not) it names `jobsReady jobsFramed jobsOwed`, and against
   the set as it stood before the out-of-programs partition landed it names all four `outOfPrograms*` rows --
   the exact rows of two of the seven, one of them CROSS-CENSUS, which is why the available set unions the
   objects rather than asking each alone. It reads `[]` today, and that zero is worth having only because
   those two runs made it speak.
   RETIREMENT: this goes when a row cannot be taken except through a helper that registers it AND the engine
   states its own total/part relations, at which point the extension test stops being a proxy for them. */
row.unaskedRelatives = (() => {
  const seen = [coldLive && coldLive.c, wfqLive && wfqLive.w]
    .filter(o => o && typeof o === 'object' && !Array.isArray(o));
  if (!seen.length) return null;
  const available = new Set();
  for (const o of seen) for (const k of Object.keys(o)) available.add(k);
  const out = [];
  for (const k of available) {
    if (taken.has(k)) continue;
    for (const t of taken)
      if (k !== t && k.startsWith(t) && /^[A-Z]/.test(k.slice(t.length))) { out.push(k + ' extends ' + t); break; }
  }
  return out.sort();
})();
try { writeFileSync(new URL(LOG_NAME, OUT), j); } catch (e) { row.logWriteErr = String(e.message); }
console.log('ROW ' + JSON.stringify(row));
await b.disconnect();
