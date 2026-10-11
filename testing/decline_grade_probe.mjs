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
// THE PLATFORM EDGE THROWS A MARKED ERROR AND THAT IS THE POSITIVE CONTROL: a refusal probe must first let
// something through. The edge is `fetch` for a network URL and `swRpc`, the relay a credentialed send's identity
// reads its jar through; a vector reaching either passed every pre-request gate, and no socket opens. A `data:`
// URL is answered by the host's own fetch, Fetch §4.3's "data" arm without a network.
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
  const sandbox = { console, URL, TextDecoder, TextEncoder, Headers,
                    fetch: (u, init) => {
                      if (String(u).startsWith('data:')) return fetch(u, init);
                      throw new Error(REACHED);
                    },
                    swRpc: () => { throw new Error(REACHED); } };
  sandbox.self = sandbox; sandbox.globalThis = sandbox;
  createContext(sandbox);
  /* The ast-worker.html order for the files a credentialed navigation's identity is built and spoken by. */
  for (const f of ['check.js', 'lib/public-suffix-table.js', 'lib/site.js', 'lib/referrer-policy.js',
                   'lib/request-identity.js', 'speak-as-rule.js', 'lib/speak-as.js', 'lib/navigable-notice.js',
                   'lib/document-record.js', 'lib/safe-fetch.js'])
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
   git grep -n 'const r = await self.safeFetch(abs, { pageOrigin: principalOrigin' extension/bridge.js
   The route seed states the declaration's words (derived, page, its drive and witness marks, here an
   undriven, unpinned declaration) and the ambient seed the tool's (observed, tool, unstated, unstated):
   git grep -n 'navigationLoad(seed.url' extension/bridge.js
   git grep -n 'navigationLoad(msg.seedUrl' extension/bridge.js
   Both are credentialed for a same-origin document, so both speak as `navigationIdentity`'s identity, which
   is identityFor over the speaker. A declared route's speaker is engineIdentity of the declaring document,
   with no referrer and no policy; the ambient seed's is the browser frame's (seedSpeaker), which this realm
   has no browser to ask, so it states the same engine identity. No gate before the edge reads the identity's
   source. */
function navIdentity(url) {
  return z.identityFor(z.engineIdentity(new URL(PRINCIPAL).origin, PRINCIPAL),
                       { url, requestInit: { credentials: 'include', mode: 'navigate', referrer: '',
                                             referrerPolicy: null } },
                       'strict-origin-when-cross-origin');
}
function navOpts(url, over) {
  return Object.assign({ pageOrigin: new URL(PRINCIPAL).origin,
                         destination: 'document', mode: 'navigate', provenance: 'derived',
                         doc: z.docRecord('observed', 'undriven', 'unpinned', 'none',
                                          z.safeFetchDocumentAddressSpace(PRINCIPAL, null)),
                         actor: 'page', driven: 'undriven', pinned: 'unpinned', credentials: 'include',
                         credentialed: true, identity: navIdentity(url) }, over || {});
}
const AMBIENT = { provenance: 'observed', actor: 'tool', driven: 'unstated', pinned: 'unstated' };

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
    url: 'https://app.example.com/logout', want: 'WIRE', over: AMBIENT },
  { label: 'AMBIENT SEED, /account/delete (the same)',
    url: 'https://app.example.com/account/delete', want: 'WIRE', over: AMBIENT },
  /* navigationLoad's data: arm is Fetch §4.3's "data" arm and never asks safeFetch, so it cannot decline: a
     well-formed URL is a response and a malformed one (no comma, Fetch §6 "data: URLs") a network error,
     which the loader reports as unavailable. safeFetch itself refuses the scheme. */
  { label: 'DATA ARM, a well-formed data: URL (answered, never asked of safeFetch)',
    url: 'data:text/html,<p>x', want: 'ANSWERED', scheme: true },
  { label: 'DATA ARM, a data: URL with no comma (a network error, not a decline)',
    url: 'data:text/html', want: 'REFUSE:network', scheme: true },
  { label: 'safeFetch asked for a data: URL (the scheme is refused, not declined)',
    url: 'data:text/html,<p>x', want: 'REFUSE:network' },
];

const z = zone();
let bad = 0, wire = 0, decl = 0;
for (const row of ROWS) {
  let got, detail;
  try {
    const r = row.scheme ? await z.safeFetchSchemeData(row.url)
                         : await z.safeFetch(row.url, navOpts(row.url, row.over));
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
