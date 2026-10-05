// WHICH REFUSALS THE CHOKEPOINT GRADES `decline` FOR A §7.4 NAVIGATION, ASKED OF `safeFetch` ITSELF.
//
// `extension/bridge.js`'s navigation loader aborts where a `decline` reaches a caller whose refusal arm is
// `none` (see DOC_REFUSAL_ARMS), so "can THIS caller be declined at all?" decides what is owed at each of its
// four call sites. `testing/egress_arm_probe.mjs` answers the FIRING question and cannot answer this one: the
// firing policy is ONE of six `decline` producers in `extension/lib/safe-fetch.js`, and the DESTRUCTIVE DENY
// LIST is not a signal arm — `safeFetchFiringRefusal` knows nothing about it. So the question has to be asked
// of the whole ladder, which means asking `safeFetch`.
//
// IT RESTATES NOTHING. The chokepoint is loaded VERBATIM into a realm of its own, exactly as
// `engine/trusted.mjs` and the arm probe load it, and every verdict below is that file's own answer. A second
// copy of the deny list or of the gate order here would be the copy that drifts.
//
// `fetch` THROWS A MARKED ERROR AND THAT IS THE POSITIVE CONTROL. CLAUDE.md §AND-THE-WAY-YOU-ESTABLISH-WHICH-
// HALF-A-FAILURE-LANDS-IN: a probe for a refusal must first be shown to let something THROUGH, or a DECLINE
// everywhere reads identically to a probe that never reached the ladder at all. A vector that reaches the wire
// is one no pre-request gate refused; nothing here opens a socket.
//
// THE VECTORS ARE TRANSCRIBED OFF THE CALL SITES, WITH THE GREP NAMED, which is the one thing here a reader
// must check rather than trust — a vector whose call site has moved is a row about nothing.
//
// WHAT IT ESTABLISHED WHEN IT WAS WRITTEN, recorded as the incident and not as a population, because the
// figure is a property of `_DESTRUCTIVE` and of the arm table and both move: a ROUTE SEED fires at an ordinary
// route and is DECLINED `blocked-destructive:<token>` at `/logout`, `/settings/reset`, `/account/delete` and
// `/subscription/cancel` — ordinary routes of any application with authentication — while the notice's own
// `_seedRefusal` pre-screen asks only `safeFetchFiringRefusal` and so lets every one of them be enqueued. The
// AMBIENT seed is exempt from both destructive gates by `provenance !== "observed"` and fires everywhere.
//
//   node testing/decline_grade_probe.mjs
"use strict";
import { readFileSync } from 'node:fs';
import { createContext, runInContext } from 'node:vm';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const EXT = join(dirname(fileURLToPath(import.meta.url)), '..', 'extension');
const REACHED = 'DECLINE_GRADE_PROBE_REACHED_THE_WIRE';

function zone() {
  const sandbox = { console, URL, TextDecoder, TextEncoder,
                    fetch: () => { throw new Error(REACHED); } };
  sandbox.self = sandbox; sandbox.globalThis = sandbox;
  createContext(sandbox);
  for (const f of ['check.js', 'lib/safe-fetch.js'])
    runInContext(readFileSync(join(EXT, f), 'utf8'), sandbox, { filename: join(EXT, f) });
  for (const n of ['safeFetch', 'safeFetchEgressStated'])
    if (typeof sandbox[n] !== 'function')
      throw new Error(`extension/lib/safe-fetch.js installed no \`${n}\` — this probe reads the chokepoint's ` +
                      'own answer rather than restating its rules, so a load that installs nothing leaves it ' +
                      'with nothing to ask and a clean-looking table of silence');
  /* THE DEFAULT TABLE, STATED ONCE AND EMPTY, WHICH IS WHAT AN UNCONFIGURED ORIGIN HAS — and the chokepoint
     ABORTS on a firing question asked before any host has spoken, so this is not setup: it is the one thing
     that makes every answer below be about the DEFAULT policy. */
  sandbox.safeFetchEgressStated({});
  return sandbox;
}

const PRINCIPAL = 'https://app.example.com/page';
/* bridge.js's `navigationLoad` opts, transcribed:
   git grep -n 'destination: "document", provenance: provenance' extension/bridge.js
   The two seeds differ only in the two words their own call sites state:
   git grep -n 'refusalArm' extension/bridge.js                       (the ROUTE seed: derived/page)
   git grep -n 'PROVENANCE_OBSERVED, PROVENANCE_OBSERVED' extension/bridge.js  (the AMBIENT seed: observed/tool) */
function navOpts(over) {
  return Object.assign({ pageUrl: PRINCIPAL, pageOrigin: new URL(PRINCIPAL).origin,
                         destination: 'document', provenance: 'derived', docReach: 'observed',
                         actor: 'page', pinned: 'unstated', credentials: 'include',
                         credentialed: true }, over || {});
}

const ROWS = [
  { label: 'ROUTE SEED, an ordinary route (CONTROL: must reach the wire)',
    url: 'https://app.example.com/dashboard/overview', want: 'WIRE' },
  { label: 'ROUTE SEED, /logout',           url: 'https://app.example.com/logout',              want: 'DECLINE' },
  { label: 'ROUTE SEED, /settings/reset',   url: 'https://app.example.com/settings/reset',      want: 'DECLINE' },
  { label: 'ROUTE SEED, /account/delete',   url: 'https://app.example.com/account/delete',      want: 'DECLINE' },
  { label: 'ROUTE SEED, /subscription/cancel', url: 'https://app.example.com/subscription/cancel', want: 'DECLINE' },
  /* AND THE SAME ADDRESSES AT THE AMBIENT SEED'S TWO WORDS. Both destructive gates are scoped
     `credentialed && provenance !== "observed"`, so this is the one navigation caller they do not reach — the
     loosening is argued at the gate itself and this is where it is exercised rather than assumed. */
  { label: 'AMBIENT SEED, /logout (observed: the deny list does not apply)',
    url: 'https://app.example.com/logout', want: 'WIRE', over: { provenance: 'observed', actor: 'tool' } },
  { label: 'AMBIENT SEED, /account/delete (the same)',
    url: 'https://app.example.com/account/delete', want: 'WIRE', over: { provenance: 'observed', actor: 'tool' } },
];

const z = zone();
let bad = 0, wire = 0, decl = 0;
for (const row of ROWS) {
  let got, detail;
  try {
    const r = await z.safeFetch(row.url, navOpts(row.over));
    if (r && r.refusal) { got = r.refusal.kind === 'decline' ? 'DECLINE' : 'REFUSE:' + r.refusal.kind;
                          detail = r.refusal.reason; }
    else { got = 'ANSWERED'; detail = 'status ' + (r && r.status); }
  } catch (e) {
    if (String(e && e.message).indexOf(REACHED) >= 0) { got = 'WIRE'; detail = 'no pre-request gate refused'; }
    else { got = 'THREW'; detail = String(e && e.message); }
  }
  if (got === 'WIRE') wire++;
  if (got === 'DECLINE') decl++;
  const ok = got === row.want;
  if (!ok) bad++;
  console.log(`  ${ok ? 'ok  ' : 'BAD '} ${row.label}\n       ${got}  ${detail}`);
}
console.log('');
/* THE CONTROL IS TWO-SIDED AND THE CLEAN BILL IS WITHHELD UNLESS BOTH SPOKE. A run in which nothing reached
   the wire is a run whose zone refused everything for some reason of its own; a run in which nothing was
   declined is a run that never exercised a deny arm. Either way nothing below is a reading. */
if (wire === 0 || decl === 0) {
  console.log(`  CONTROL DID NOT SEPARATE (${wire} reached the wire, ${decl} were declined) — nothing above ` +
              'is a reading about the deny list.');
  process.exit(2);
}
console.log(`  CONTROLS SEPARATED: ${wire} reached the wire and ${decl} were declined.`);
console.log(bad === 0 ? '  every row answered as its label expects'
                      : `  ${bad} ROW(S) DISAGREED — read the reason beside each`);
process.exit(bad === 0 ? 0 : 1);
