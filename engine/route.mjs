/* THE CROSS-INSTANCE SEAM'S DRIVER —  node engine/route.mjs  (after node engine/build.mjs abi).
 *
 * IT IS IN THE REPOSITORY BECAUSE IT IS THE ONLY THING THAT PROVISIONS A SECOND INSTANCE. §SECURITY's rule is
 * that a host which cannot provision one has not tested the transport, and that every cross-instance mechanism
 * is then a design that has never run — which is exactly what the world registry was: written, reviewed,
 * self-tested, and reachable only from a fixture in /tmp that would vanish with the container. This is not a
 * regression test pinning a behaviour (those are deleted after use, because they prevent better designs); it
 * is a HARNESS, the two-WASM-instance sibling of engine/wpt.mjs and engine/test262.mjs, and what it reports is
 * the seam's own counters rather than an assertion about them.
 *
 * TWO INSTANCES, THREE WORLDS, ONE SEAM. `a` opens a cross-origin window and posts to it — first from the world
   of its own boot flow, then from each ARM of a concolic fork, so the three records that cross carry three
   different world vectors and two of them carry an ANCESTRY. The offscreen's job is played here: provision an
   instance for the document `a` named, and route each post to it with `a`'s origin stamped.

   WHAT THIS EXERCISES BEYOND DELIVERY. Record 1's world is `a`'s root world and has no ancestry, so the
   receiving instance materializes a segment for it from its own baseline. Records 2 and 3 are posted by the two
   arms of `if (__FLAGS.admin)`: the primary keeps the root world (its segment already exists), the sibling's
   world is a CHILD whose vector names the root as its nearest ancestor — so its segment is materialized by
   FORKING the segment record 1 created. That fork is world_segment's nearest-first materialization, and until
   this fixture drove it, nothing outside a self-test had ever called it.

   WHAT IT DOES NOT EXERCISE, said plainly: the forked segment is EMPTY, because no operation exists by which a
   foreign world writes in a peer instance. The production ABI's only cross-instance ops are `navigable.create`
   and `windowproxy.post`; a delivery runs under the RECEIVING flow's delta, so nothing accumulates in the
   sender's segment. The producer of a non-empty one is the peer answering a cross-document READ by running a
   program under the asking world — which this ABI has no entry for. */
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const factory = await import(join(ENGINE, '..', 'extension', 'lib', 'qjs', 'qjs.mjs'));
const boot = factory.default ?? factory;

const HTML_A = `<!doctype html><script>
  var w = window.open("https://b.test/child", "child");
  w.postMessage({hello:"root"}, "*");
  if (__FLAGS.admin) { w.postMessage({hello:"admin"}, "*"); } else { w.postMessage({hello:"public"}, "*"); }
</script>`;
/* `/hold` is NEVER answered, and that is the point: `b`'s boot flow stays live and owed, so the second and
   third arrivals have a timeline to arrive in. A document whose every flow has finished cannot receive, and
   the engine says so rather than delivering into nothing. */
const HTML_B = `<!doctype html><script>
  fetch("/hold");
  window.addEventListener("message", function (e) { fetch("/got?origin=" + e.origin + "&hello=" + e.data.hello); });
</script>`;

async function makeEngine(html, url, docId, csp) {
  const M = await boot();
  const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
  const str = (f, ...a) => String(M.ccall(f, 'string', a.map(() => 'number'), a.map(cs)) ?? '');
  M.ccall('qjs_init', 'number', ['number','number','number','number'], [cs(html), cs(url), cs(docId), cs(csp || '')]);
  M.ccall('qjs_begin', 'void', ['number'], [cs('')]);
  return { M, cs, str, docId, origin: new URL(url).origin, done: false };
}

const engines = [];
/* EXACT. A child document's NAME is prefixed by its creator's ("<creator>.<n>") but the creator is precisely
   the instance that does NOT hold it — that is why the notice exists. Prefix-matching routed the post straight
   back to the sender, which the engine caught twice over. */
const holderOf = (doc) => engines.find((e) => e.docId === doc) ?? null;

const posts = [];   /* routed records, in emission order, held until their target is free to receive one */
const got = [];     /* every /got the receiving page fetched — one per message its listener actually saw */

/* ONE STEP of `e`, then everything the host owes it. Returns false once the engine reports its frontier done. */
async function service(e) {
  const r = e.M.ccall('qjs_step', 'number', [], []);
  for (const u of e.str('qjs_pending').split('\n').filter(Boolean)) {
    if (u.includes('/hold')) continue;
    if (u.includes('/got')) { got.push(u); console.log(`  [${e.docId}] DELIVERED: ${u}`); }
    e.M.ccall('qjs_provide', 'void', ['number','number'], [e.cs(u), e.cs('{}')]);
  }
  /* ONE OP IS ANSWERED, exactly as the offscreen answers exactly one: a `document.fetch` is a network fetch this
     zone can genuinely perform. Every other request is left UNANSWERED — the asking flow stays parked with its
     snapshot intact and its siblings keep running — because a host that answers what it cannot compute is how
     `navigable.create` came to be answered with "not created". An unanswered one keeps being reported, which is
     visible in a way a wrong answer is not. */
  for (const l of e.str('qjs_host_requests').split('\n').filter(Boolean)) {
    const id = +l.slice(0, l.indexOf('\t')), op = l.slice(l.indexOf('\t') + 1);
    console.log(`  [${e.docId}] request: ${op.slice(0, 90)}`);
    if (!op.startsWith('document.fetch\t')) continue;
    e.M.ccall('qjs_host_answer', 'void', ['number','number'], [id, e.cs(JSON.stringify({ body: HTML_B, csp: null }))]);
  }
  for (const n of e.str('qjs_host_notices').split('\n').filter(Boolean)) {
    const f = n.split('\t');
    console.log(`  [${e.docId}] notice: ${f[0]} ${f.slice(1, 3).join(' ')}`);
    if (f[0] === 'navigable.create') engines.push(await makeEngine(HTML_B, f[3], f[1], f[5] || ''));
    else if (f[0] === 'windowproxy.post') posts.push({ doc: f[1], world: f[2], record: n, origin: e.origin });
    else throw new Error('a notice this host does not act on: ' + f[0]);
  }
  return r !== 0;
}

engines.push(await makeEngine(HTML_A, 'https://a.test/', 'd1', ''));

/* PHASE 1 — run `a` out, collecting its posts. Nothing is routed yet, so `b` holds no record and each one below
   can be delivered on its own; two records on one flow is a merge of possibly-contradictory senders, which the
   engine crashes on rather than performing. */
for (let i = 0; i < 2000; i++) if (!(await service(engines[0]))) break;
console.log(`a emitted ${posts.length} posts, worlds: ${posts.map((p) => p.world).join('  ')}`);

/* PHASE 2 — one at a time, each delivered before the next is routed. */
for (const p of posts) {
  const target = holderOf(p.doc);
  if (!target) { console.log('  ROUTE FAILED: no instance holds', p.doc); continue; }
  console.log(`routing world ${p.world} -> [${target.docId}] as origin ${p.origin}`);
  target.M.ccall('qjs_route', 'void', ['number','number'], [target.cs(p.record), target.cs(p.origin)]);
  const before = got.length;
  for (let i = 0; i < 400 && got.length === before; i++) if (!(await service(target))) break;
  if (got.length === before) console.log(`  NOT DELIVERED: ${p.world}`);
}

console.log(`\nposts routed: ${posts.length}   messages the receiving page saw: ${got.length}`);
for (const u of got) console.log('  ' + u);
/* THE SEAM'S OWN COUNT, from the receiving instance. `_worldSegments` is how many foreign worlds hold a segment
   here; `_worldSegmentsForked` is how many of those were built by FORKING an ancestor the ancestry named, which
   is the number this driver exists to move off zero. */
let forked = 0;
for (const e of engines) {
  const r = JSON.parse(e.str('qjs_result'));
  forked += r._worldSegmentsForked;
  console.log(`[${e.docId}] worldSegments=${r._worldSegments} forkedFromAncestor=${r._worldSegmentsForked} ` +
              `flows=${r._flows} switches=${r._switches}`);
}

/* WHAT MAKES THIS A SMOKE TEST RATHER THAN A PRINTOUT. Three things have to have happened, and each of them is
   a whole mechanism failing silently if it did not: a second instance was provisioned (the create notice was
   acted on), every routed record reached the receiving page's listener (the inbound half), and at least one
   segment was materialized by FORKING an ancestor (the world vector's ancestry was READ and used, which is the
   part that was written, reviewed and never run). */
const fail = (why) => { console.error('[route] FAILED: ' + why); process.exit(1); };
if (engines.length < 2) fail('no second instance was provisioned — the navigable.create notice went unanswered');
if (!posts.length) fail('the sender emitted no cross-instance post');
if (got.length !== posts.length)
  fail(`${posts.length} posts routed but the receiving page's listener saw ${got.length}`);
if (!forked) fail("no segment was materialized by forking an ancestor — the world vector's ancestry was carried " +
                  'and never used, which is the state this driver exists to detect');
console.log('[route] OK — two instances, every routed record delivered, ancestry-forked segments: ' + forked);
