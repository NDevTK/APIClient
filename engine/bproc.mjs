/* THE BROWSER PROCESS'S DRIVER —  node engine/bproc.mjs  (after node engine/build.mjs).
 *
 * WHY IT IS IN THE REPOSITORY. `engine/build.mjs` links `extension/lib/bproc/bproc.mjs` and nothing else in
 * this tree would ever load it: its shipped caller is a Worker inside a Chrome extension, so without this the
 * program would be BUILT and never RUN, which §Testing calls the excluded test one layer down. It is the same
 * role `engine/route.mjs` plays for the renderer ABI — a harness, not a pinned behaviour.
 *
 * WHAT IT DRIVES NOW. It drove `bp_corb_check` over nine MISLABELLED responses, and those entries are gone:
 * CLAUDE.md §Architecture puts type sniffing back in `extension/lib/safe-fetch.js`, and the C that had been
 * transliterated out of it — `network/{mime_sniff,corb,json_sniff,nosniff,resource_kind}.c` — is deleted with
 * them. What is left in the program is the RENDERER REGISTRY, which is what makes its name a description, and
 * that is what this drives. (The rows it used to carry had also stopped agreeing with the ABI: they passed
 * `noSniff` as a NUMBER long after both entries were changed to take the HEADER VALUE, so this file had been
 * calling a signature the program no longer had — an unrun harness rotting exactly as an unbuilt entry does.)
 *
 * ITS ORACLE IS SECURITY.md, WHICH IS WHY EACH ROW CARRIES THE SENTENCE THAT DECIDES IT. "One WASM instance
 * per ORIGIN-KEYED AGENT CLUSTER — `(browsing-context group, origin)`." Every row below is a TRANSITION of
 * that table whose outcome the rule states, and the one transition the rule FORBIDS — a second renderer for a
 * live cluster — is not a row at all: registry.c makes it a `CHECK`, fatal in every build, so a driver that
 * "tested" it would be testing that this process aborts, which is not a value anything may inspect.
 *
 * AND THE KEY IS BYTES WITH AN INTERIOR NUL, which is the one marshalling this boundary can get silently
 * wrong. `clusterKeyOf` joins the browsing-context group and the origin with a NUL, so a key passed through
 * `ccall`'s `"string"` would arrive truncated at the separator and every origin in one tab would answer to one
 * key — the registry whose whole job is to refuse a merged agent cluster would perform one. The last two rows
 * are two keys that are equal up to that separator and must be two clusters.
 */
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const factory = await import(join(ENGINE, '..', 'extension', 'lib', 'bproc', 'bproc.mjs'));
const M = await (factory.default ?? factory)();

const KEY_ENC = new TextEncoder();
/* The placement `extension/browser-process.js` performs, verbatim in shape: bytes and a LENGTH, never a C
   string. One extra byte so a zero-length key still has an address of its own. */
function withKey(key, fn) {
  const b = KEY_ENC.encode(key);
  const p = M._malloc(b.length + 1);
  if (!p) throw new Error('OOM placing an agent cluster key');
  M.HEAPU8.set(b, p);
  try { return fn(p, b.length); } finally { M._free(p); }
}
const create = (key) =>
  withKey(key, (p, n) => M.ccall('bp_renderer_create', 'number', ['number', 'number'], [p, n]));
const launched = (id) => M.ccall('bp_renderer_launched', null, ['number'], [id]);
const launchFailed = (id) => M.ccall('bp_renderer_launch_failed', null, ['number'], [id]);
const terminated = (id) => M.ccall('bp_renderer_terminated', null, ['number'], [id]);
const snapshot = () => {
  const r = JSON.parse(M.ccall('bp_registry_snapshot', 'string', [], []));
  for (const f of ['clusters', 'routingIds']) {
    if (typeof r[f] !== 'string') throw new Error('the registry snapshot carries no `' + f + '`');
  }
  for (const f of ['live', 'launched', 'terminated', 'failed', 'nextRoutingId']) {
    if (typeof r[f] !== 'number') throw new Error('the registry snapshot carries no `' + f + '`');
  }
  return r;
};

/* A cluster key as bridge.js builds one: the browsing-context group, a NUL, the origin. */
const K = (group, origin) => group + '\0' + origin;

const ROWS = [
  { name: 'a cluster with no renderer gets one, and the id is positive',
    why: 'a routing id is the only name a renderer has and it is minted HERE and nowhere else, which is what ' +
         'makes it evidence of which process decided the instance should exist',
    run: () => { const id = create(K('7', 'https://a.example')); launched(id);
                 return { positive: id > 0, live: snapshot().live, launched: snapshot().launched }; },
    want: { positive: true, live: 1, launched: 1 } },

  { name: 'a launch that FAILED frees its agent cluster',
    why: 'the zygote has already removed the frame, so leaving the registration would refuse that cluster a ' +
         'renderer forever with nothing anywhere to say why — registry.h states it as the reason there are ' +
         'two entries and not one taking a boolean',
    run: () => { const id = create(K('7', 'https://b.example')); launchFailed(id);
                 const s = snapshot(); return { live: s.live, failed: s.failed }; },
    want: { live: 1, failed: 1 } },

  { name: 'the freed cluster may have a renderer again, with a NEW id',
    why: 'the refusal is of a second LIVE instance and not of a cluster that has ever asked; a routing id is ' +
         'never reissued, because a stale id coming back must be recognisable as one',
    run: () => { const before = snapshot().nextRoutingId;
                 const id = create(K('7', 'https://b.example')); launched(id);
                 return { fresh: id >= before, live: snapshot().live }; },
    want: { fresh: true, live: 2 } },

  { name: 'a renderer DEATH releases the slot',
    why: 'a real renderer can exit on its own and the browser learns of it; here the offscreen owns the frame ' +
         'and so is what notices, and this table is where the cluster is freed',
    run: () => { const id = create(K('7', 'https://c.example')); launched(id); terminated(id);
                 const s = snapshot(); return { live: s.live, terminated: s.terminated }; },
    want: { live: 2, terminated: 1 } },

  { name: 'the SAME origin in another browsing-context group is another cluster',
    why: 'SECURITY.md keys the instance on `(browsing-context group, origin)`, so neither half alone decides',
    run: () => { const id = create(K('9', 'https://a.example')); launched(id); return { live: snapshot().live }; },
    want: { live: 3 } },

  { name: 'two keys equal up to the NUL separator are TWO clusters',
    why: 'the row this whole byte-and-length marshalling exists for: `ccall`\'s "string" would truncate both ' +
         'keys at the separator, they would collide, and the second create would abort as a duplicate — so a ' +
         'marshalling regression shows up here as a CRASH rather than as a wrong number',
    run: () => { const id = create('7\0https://d.example'); launched(id);
                 const id2 = create('7\0https://d.example.evil'); launched(id2);
                 const s = snapshot(); return { live: s.live, distinct: id !== id2 }; },
    want: { live: 5, distinct: true } },
];

let bad = 0;
for (const r of ROWS) {
  const got = r.run();
  const ok = Object.keys(r.want).every((k) => got[k] === r.want[k]);
  if (!ok) bad++;
  console.log((ok ? '  ok   ' : '  FAIL ') + r.name.padEnd(56) + JSON.stringify(got) +
              (ok ? '' : '   want: ' + JSON.stringify(r.want) + '\n         ' + r.why));
}
console.log('[bproc] final registry: ' + JSON.stringify(snapshot()));
console.log('[bproc] ' + (ROWS.length - bad) + '/' + ROWS.length + ' renderer-registry transitions agree with ' +
            'SECURITY.md');
process.exit(bad ? 1 : 0);
