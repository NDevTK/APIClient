// GROUND TRUTH FOR nav-load.html, IN REAL CHROME, AND IT IS COMMITTED BECAUSE THE FIGURE IT PRODUCES IS
// QUOTED. CLAUDE.md §A-MEASUREMENT-CAN-OUTLIVE-ITS-INSTRUMENT rates an uncommitted instrument worse than a
// stale number: the figure is true, the method is sound, and a reader who re-derives it finds nothing — after
// which a measurement nobody made and one whose tool was never committed render identically. nav-load.html
// quotes this script's output in its own prose, so this script is tracked beside it.
//
// WHAT IT IS FOR, AND IT IS NOT A GATE. nav-load.html's whole oracle is a DIFFERENCE between three clients —
// real Chrome, the ambient seed, and the engine's own §7.4 navigation — and four of its addresses are claimed
// to be producible by the engine ALONE. A claim of that shape is armed only if somebody has shown that real
// Chrome does NOT produce them: without this run, a cell reading zero on both sides is the unarmed control
// CLAUDE.md §AND-THE-WAY-YOU-ESTABLISH-WHICH-HALF-A-FAILURE-LANDS-IN forbids. So this establishes the CHROME
// column of that table and nothing else.
//
// IT DRIVES NO ENGINE AND LOADS NO ARTIFACT, which is why a lane that may not build may still run it: no
// extension, no wasm, no `testing/harness.js`. It is a browser loading a page.
//
// THE REQUESTS ARE READ TWICE, FROM TWO CHANNELS, BECAUSE NEITHER ALONE CARRIES WHAT THE FIXTURE NEEDS.
// Chrome's own request event gives the complete LIST; `sec-fetch-dest` is added further down the network
// stack and is NOT in `request.headers()` here — it reads `-` for every row — so the `dest=` column that
// separates a browser load from an engine load is only visible in SERVE.MJS'S OWN LOG. This script prints the
// list and tells the reader to read the server's log for the dest values, rather than printing a `-` that
// would be mistaken for a measurement.
//
//   cd testing/corpus/control && PORT=18899 node serve.mjs > /somewhere/serve.log 2>&1 &
//   node nav-load-groundtruth.mjs                        # ORIGIN defaults to the PORT=18899 navload row
//   grep 'REQ control-navload ' /somewhere/serve.log      # ... and the dest= column
//
// THE ORIGIN IS A PARAMETER AND ITS DEFAULT IS A PRIVATE BASE, for serve.mjs's own reason: 8899 is shared and
// two lanes running the control at once lose a pass to EADDRINUSE, so this defaults to the 18899 base rather
// than teaching a reader to take the shared one.
import puppeteer from 'puppeteer';

/* nav-load.html IS THE 14th `DOCS` ROW, SO ITS PORT IS BASE + 13 — DERIVED HERE AND NOT WRITTEN DOWN. A
   literal would be a second copy of a number serve.mjs's table already decides, and that table's own rule is
   that a document APPENDED to it shifts nothing before it; a copy here would shift silently if one were ever
   inserted. The row is found by NAME. */
const BASE = Number(process.env.PORT || 18899);
const ORIGIN = process.env.ORIGIN || `http://127.0.0.1:${BASE + 13}/`;

/* WHAT EACH ADDRESS IS, AND WHICH WAY THIS RUN MUST ANSWER IT. The `want` is the CHROME column of
   nav-load.html's table and nothing else — an `ABSENT` here is never a claim about the engine, it is the
   statement that makes the engine's own PRESENT mean something. */
const WANT = [
  ['/nav/load-child.html',        'PRESENT', 'RUNG 1: the route, and the child navigable Chrome really loads'],
  ['/api/navload-child-framed',   'PRESENT', 'the ARMED CONTROL, and the frame discriminator answering FRAMED'],
  ['/api/navload-child-top',      'ABSENT',  'the discriminator must NOT answer TOP for a real frame'],
  ['/cfg.json',                   'PRESENT', 'RUNG 2: the reply the composed address needs'],
  ['/api/us-east-1/child-value',  'PRESENT', 'RUNG 2: the address composed out of that reply'],
  ['/api/navload-orphan-framed',  'ABSENT',  'ENGINE-ONLY: an orphan drive, which no browser performs'],
  ['/api/navload-orphan-top',     'ABSENT',  'ENGINE-ONLY: the same, by way of the ambient seed'],
  ['/api/navload-forced-reached', 'ABSENT',  'ENGINE-ONLY: the forced arm of an absent-state gate'],
  ['/nav/load-gated.html',        'ABSENT',  'ENGINE-ONLY path, and RUNG 3 says it is refused there too'],
  ['/api/navload-gated-ran',      'ABSENT',  "RUNG 3's positive falsifier — nothing may produce this"],
];

const reqs = [];
const errs = [];
const b = await puppeteer.launch({
  executablePath: process.env.CHROME || '/opt/chrome-for-testing/chrome',
  headless: true,
  /* `--no-proxy-server` BECAUSE THIS BOX HAS AN EGRESS PROXY AND THE SUBJECT IS 127.0.0.1. A proxied loopback
     request is a different request, and one that fails reads as the fixture not making it. */
  args: ['--no-sandbox', '--disable-setuid-sandbox', '--no-proxy-server'],
});
const pg = await b.newPage();
pg.on('request', r => reqs.push([r.method(), new URL(r.url()).pathname, r.resourceType()]));
pg.on('pageerror', e => errs.push(String(e).slice(0, 200)));
pg.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text().slice(0, 200)); });
/* THE LOAD ITSELF IS ITS OWN OUTCOME, AND IT EXITS 2 RATHER THAN BEING LEFT TO THROW. An unhandled rejection
   here exits 1, which is the SAME code a disagreeing row uses — so a reader meeting a 1 could not tell "Chrome
   answered a row the wrong way", which is a finding about the fixture, from "there was no server", which is a
   finding about the invocation. CLAUDE.md's three-states-behind-one-answer shape, in an exit code. Three
   outcomes, three codes: 0 every row as expected, 1 a row disagreed, 2 this run is not a reading at all. */
try {
  await pg.goto(ORIGIN, { waitUntil: 'networkidle0', timeout: 20000 });
} catch (e) {
  console.log(`NOT A READING: could not load ${ORIGIN} — ${String(e).split('\n')[0]}`);
  console.log('Start the server first:  cd testing/corpus/control && PORT=18899 node serve.mjs &');
  await b.close();
  process.exit(2);
}
/* AND A SETTLING WAIT, BECAUSE RUNG 2 IS A REPLY CHAIN AND `networkidle0` CAN BE REACHED BEFORE THE `.then`
   THAT COMPOSES ITS ADDRESS HAS RUN. A run that omitted this would report RUNG 2's own floor ABSENT from
   Chrome, which would arm a cell that must not be armed. */
await new Promise(r => setTimeout(r, 1500));
await b.close();

console.log(`GROUND TRUTH for ${ORIGIN} — real Chrome, headless, NO extension\n`);
console.log('EVERY REQUEST IT MADE (method path resourceType; `sec-fetch-dest` is NOT visible here — read');
console.log("serve.mjs's own `REQ control-navload … dest=` lines for that column):");
for (const r of reqs) console.log('  ' + r.join(' '));
console.log('\nPAGE ERRORS: ' + (errs.length ? '\n  ' + errs.join('\n  ') : 'none'));
console.log('\nPER ADDRESS — the CHROME column of nav-load.html\'s oracle table:');
const paths = reqs.map(r => r[1]);
let bad = 0;
for (const [p, want, why] of WANT) {
  const n = paths.filter(x => x === p).length;
    const got = n > 0 ? 'PRESENT' : 'ABSENT';
  const ok = got === want;
  if (!ok) bad++;
  console.log(`  ${ok ? ' ok ' : 'FAIL'} ${String(n).padStart(2)}  ${p.padEnd(30)} ${want.padEnd(8)} ${why}`);
}
/* THE CONTROL THAT MAKES THE `ABSENT` ROWS A READING RATHER THAN A SILENCE: this run must have reached the
   fixture at all. Every `ABSENT` row above is satisfied identically by a browser that loaded nothing, so
   without a PRESENT row having spoken, a clean-looking table of absences is the unarmed probe CLAUDE.md
   forbids — and the exit code says so rather than leaving it to be noticed. */
const spoke = WANT.filter(([p, w]) => w === 'PRESENT' && paths.indexOf(p) >= 0).length;
console.log(`\n${WANT.length - bad}/${WANT.length} addresses as expected; ` +
            `${spoke}/${WANT.filter(([, w]) => w === 'PRESENT').length} PRESENT rows actually spoke`);
if (spoke === 0) {
  console.log('UNARMED: no PRESENT row spoke, so nothing above is a reading — is the server up on ' + ORIGIN + '?');
  process.exit(2);
}
process.exit(bad ? 1 : 0);
