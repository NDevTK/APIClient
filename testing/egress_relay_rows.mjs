// The fired-send record across extension/bridge.js: what safeFetch states on a reply record's `egress` list is
// taken into the asking instance's own list (egressTake), which an analysis carries as `egressSends`. Each row
// asks bridge.js's own functions, loaded as engine/bridge_accept.mjs loads them; nothing is sent. The control is
// the zone's result-document acceptance, which every build of bridge.js declares.
//
//   node testing/egress_relay_rows.mjs
"use strict";
import { createContext } from 'node:vm';
import { loadZone, processSiteRpc } from '../engine/zone.mjs';

const Z = { console, fetch, URL, TextDecoder, TextEncoder, setTimeout, clearTimeout, MessageChannel,
            AbortController, AbortSignal, atob, swRpc: processSiteRpc, _globalStoreReady: new Promise(() => {}) };
Z.self = Z;
Z.globalThis = Z;
Z.module = { exports: {} };
createContext(Z);
loadZone(Z, ['bridge.js']);
if (typeof Z.resultDocumentRefusal !== 'function') {
  console.log('  CONTROL FAILED: extension/bridge.js did not load, so no row is a reading');
  process.exit(2);
}

const VECTOR = { destination: 'value', actor: 'page', provenance: 'derived' };
const PARTS = { address: 'concrete', headers: [], body: 'none' };
const entry = (over) => Object.assign({ url: 'https://a.test/api', vector: VECTOR, parts: PARTS, how: 'sent', refusal: null,
                                        admittedBy: { kind: 'default', arm: 2, why: 'the page\'s own run' } }, over);
/* What a function of the zone answers, or the word for its absence or its abort. */
const asked = (name, ...args) => {
  if (typeof Z[name] !== 'function') return 'no-' + name;
  try { return Z[name](...args); } catch (e) { return 'aborted'; }
};
const taken = (records) => {
  const log = typeof Z.egressLog === 'function' ? Z.egressLog() : null;
  if (log === null) return 'no-egressLog';
  for (const [method, r] of records) {
    if (typeof Z.egressTake !== 'function') return 'no-egressTake';
    try { Z.egressTake(log, method, r); } catch (e) { return 'aborted'; }
  }
  return log;
};

const ROWS = [
  { label: 'a reply record\'s entries are taken in order, each with its request\'s method', run: () => {
      const log = taken([['GET', { egress: [entry({ url: 'https://a.test/hop' }), entry({ url: 'https://a.test/landed' })] }]]);
      const s = log && log.sends;
      return [['entries', Array.isArray(s) ? s.length : String(log), 2],
              ['the first entry\'s method', Array.isArray(s) && s[0].method, 'GET'],
              ['the second entry\'s address', Array.isArray(s) && s[1].url, 'https://a.test/landed'],
              ['what admitted it', Array.isArray(s) && s[0].admittedBy.kind, 'default']]; } },
  { label: 'a refused request and a cache hit are entries too', run: () => {
      const log = taken([['GET', { egress: [entry({ how: 'not-sent', admittedBy: null, refusal: 'provenance=forced' })] }],
                         ['GET', { egress: [entry({ how: 'answered-locally', vector: null, parts: null, admittedBy: null })] }]]);
      const s = log && log.sends;
      return [['entries', Array.isArray(s) ? s.length : String(log), 2],
              ['the refusal', Array.isArray(s) && s[0].refusal, 'provenance=forced'],
              ['the hit\'s vector', Array.isArray(s) && s[1].vector, null]]; } },
  { label: 'a reply record that states no list is refused, never read as no sends', run: () =>
      [['no list', asked('egressRecordRefusal', {}) === null ? 'accepted' : String(taken([['GET', {}]])), 'aborted'],
       ['the refusal names the field', String(asked('egressRecordRefusal', {})).includes('`egress`'), true]] },
  { label: 'an entry outside the record\'s vocabulary is refused', run: () =>
      [['an unknown how', asked('egressRecordRefusal', { egress: [entry({ how: 'maybe' })] }) === null, false],
       ['an unknown admitting kind', asked('egressRecordRefusal', { egress: [entry({ admittedBy: { kind: 'other' } })] }) === null, false],
       ['a sent entry nothing admitted', asked('egressRecordRefusal', { egress: [entry({ admittedBy: null })] }) === null, false],
       ['a judged entry with no parts', asked('egressRecordRefusal', { egress: [entry({ parts: null })] }) === null, false],
       ['a well-formed record', asked('egressRecordRefusal', { egress: [entry({})] }), null]] },
  { label: 'an analysis states its instance\'s list, and one without the field is refused', run: () =>
      [['a list', asked('analysisEgressRefusal', { egressSends: [Object.assign({ method: 'GET' }, entry({}))] }), null],
       ['no instance ran', asked('analysisEgressRefusal', { egressSends: null }), null],
       ['the field absent', asked('analysisEgressRefusal', {}) === null, false]] },
];

let bad = 0;
for (const row of ROWS) {
  const notes = row.run().map(([what, have, want]) => ({ what, have, want }));
  const ok = notes.every((n) => n.have === n.want);
  if (!ok) bad++;
  console.log(`  ${ok ? 'ok  ' : 'BAD '} ${row.label}\n       ` +
              notes.map((n) => `${n.what}: ${n.have} (want ${n.want})`).join('; '));
}
console.log(bad === 0 ? '  every row agreed' : `  ${bad} ROW(S) DISAGREED`);
process.exit(bad === 0 ? 0 : 1);
