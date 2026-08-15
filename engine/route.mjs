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

   AND IT ASKS THE READ, which is the half the ABI has no entry for and which nothing had ever asked. §7.2.5.1's
   cross-origin allowlist is a fixed twelve, and of them exactly ONE cannot be answered by the navigable's own
   record: `length` is the child-navigable count of the peer's ACTIVE DOCUMENT (window_proxy.c answers every
   other one in the asking turn). So `w.length` is the whole of this engine's synchronous cross-instance read
   surface, and until this fixture wrote it the request record `windowproxy.get` had never been emitted by the
   production entry at all — the sender's half was written, reviewed, and unexercised in exactly the way the
   world registry was.

   IT IS PLACED LAST AND ITS RESULT IS POSTED, so the read is a load-bearing part of the measurement rather
   than a statement whose value is discarded: the fourth and fifth records cannot be emitted until the read is
   ANSWERED, and the `/got` they produce carries `typeof e.data.n` — which is what distinguishes the number 0
   from the string "0", the sentence SECURITY.md states about this seam.

   AND THE PEER NOW ANSWERS IT, which is what this zone does with a `windowproxy.get` below: it hands the
   record to the instance that HOLDS the document (`qjs_perform`), pumps that instance until the program its
   answer is — an IDL getter, run as a flow on its own frontier — completes, and relays the COMPLETION back to
   the asking instance (`qjs_host_answer_remote`) in remote_object.c's grammar rather than as JSON, because a
   member whose value is an OBJECT crosses as a NAME and JSON cannot express one. This zone reads neither: it
   routes text, and only an engine knows what a name means.
   THE ANSWER IS PER TIMELINE, and the fourth and fifth records are what show it: both ARMS of `a`'s fork make
   the read, so one peer is asked the same question by two different worlds and answers each under the segment
   that world has here. A peer holding SEVERAL timelines answers each question that many times — its document's
   state IS its flows — and the asking flow must then FORK per answer; that half is named where the second
   answer arrives (engine_host_answer) and this fixture's `b` has one timeline, so it is not reached here. */
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const factory = await import(join(ENGINE, '..', 'extension', 'lib', 'qjs', 'qjs.mjs'));
const boot = factory.default ?? factory;

/* THE READ IS LAST AND BOTH ARMS MAKE IT, so two DIFFERENT worlds ask the same question of one peer — which is
   the case a single-timeline peer cannot answer with one number, and the reason the entry that performs it has
   to install the asking world's segment rather than read a property from C. */
const HTML_A = `<!doctype html><script>
  var w = window.open("https://b.test/child", "child");
  w.postMessage({hello:"root"}, "*");
  if (__FLAGS.admin) { w.postMessage({hello:"admin"}, "*"); } else { w.postMessage({hello:"public"}, "*"); }
  w.postMessage({hello:"length", n: w.length}, "*");
</script>`;
/* `/hold` is NEVER answered, and that is the point: `b`'s boot flow stays live and owed, so the second and
   third arrivals have a timeline to arrive in. A document whose every flow has finished cannot receive, and
   the engine says so rather than delivering into nothing. */
/* `typeof e.data.n` RIDES THE DELIVERY because `otherW.length === 0` distinguishes a number from the string
   "0", and an answer that arrived as text and stayed text would satisfy every loose check in this file while
   proving only that bytes moved. */
const HTML_B = `<!doctype html><script>
  fetch("/hold");
  window.addEventListener("message", function (e) {
    fetch("/got?origin=" + e.origin + "&hello=" + e.data.hello + "&n=" + (typeof e.data.n) + ":" + e.data.n);
  });
</script>`;

/* `topLevelUrl` is HTML §8.1.3.1's TOP-LEVEL CREATION URL — this zone's to state, because one instance is
   one document and only the zone that routed the create knows what embeds it. */
async function makeEngine(html, url, docId, csp, topLevelUrl) {
  const M = await boot();
  const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
  const str = (f, ...a) => String(M.ccall(f, 'string', a.map(() => 'number'), a.map(cs)) ?? '');
  M.ccall('qjs_init', 'number', ['number','number','number','number','number'],
    [cs(html), cs(url), cs(docId), cs(csp || ''), cs(topLevelUrl)]);
  M.ccall('qjs_begin', 'void', ['number'], [cs('')]);
  return { M, cs, str, docId, docUrl: url, origin: new URL(url).origin, done: false };
}

const engines = [];
/* EXACT. A child document's NAME is prefixed by its creator's ("<creator>.<n>") but the creator is precisely
   the instance that does NOT hold it — that is why the notice exists. Prefix-matching routed the post straight
   back to the sender, which the engine caught twice over. */
const holderOf = (doc) => engines.find((e) => e.docId === doc) ?? null;

const posts = [];   /* routed records, in emission order, held until their target is free to receive one */
const got = [];     /* every /got the receiving page fetched — one per message its listener actually saw */
/* EVERY CROSS-AGENT OPERATION THIS ZONE WAS ASKED TO PERFORM, keyed by the asking engine's request id, and
   whether it was ever answered. Keyed rather than counted because `qjs_host_requests` deliberately does NOT
   dedupe — an unanswered request is re-reported on every single step, so a count would be a step count, and
   the log line below would be one line per step for the rest of the run.
   THAT KEY IS ALSO THE RENDEZVOUS TOKEN the performing instance echoes on its answer. It has to be this zone's
   and not the asking flow's request id, because an id is unique only inside the instance that minted it and two
   askers may hold the same number — which is exactly the fact only the routing zone has. */
const reads = new Map();
/* The completions performing instances have emitted, by token, until the asker is handed each one. */
const answers = new Map();

/* ONE STEP of `e`, then everything the host owes it. Returns false once the engine reports its frontier done. */
async function service(e) {
  const r = e.M.ccall('qjs_step', 'number', [], []);
  for (const u of e.str('qjs_pending').split('\n').filter(Boolean)) {
    if (u.includes('/hold')) continue;
    if (u.includes('/got')) { got.push(u); console.log(`  [${e.docId}] DELIVERED: ${u}`); }
    /* THE ONE REPLY RECORD every host of this engine delivers, crossing as JSON so it carries its type. This
       zone follows no redirect, so Fetch §4.1 gives the response a clone of the REQUEST's URL list — one item,
       RESOLVED against this document's address because a URL list holds URLs and `response.url` serializes the
       last of them. */
    const reply = { status: 200, statusText: 'OK', headers: [], body: '{}',
                    urlList: [new URL(u, e.docUrl).href] };
    e.M.ccall('qjs_provide', 'void', ['number','number'], [e.cs(u), e.cs(JSON.stringify(reply))]);
  }
  /* ONE OP IS ANSWERED, exactly as the offscreen answers exactly one: a `document.fetch` is a network fetch this
     zone can genuinely perform. Every other request is left UNANSWERED — the asking flow stays parked with its
     snapshot intact and its siblings keep running — because a host that answers what it cannot compute is how
     `navigable.create` came to be answered with "not created". An unanswered one keeps being reported, which is
     visible in a way a wrong answer is not. */
  for (const l of e.str('qjs_host_requests').split('\n').filter(Boolean)) {
    const id = +l.slice(0, l.indexOf('\t')), op = l.slice(l.indexOf('\t') + 1);
    /* A CROSS-AGENT OPERATION IS RECORDED THE FIRST TIME IT IS ASKED and never answered here, because there is
       nothing in the ABI to ask the peer WITH. It is not skipped silently: leaving it out of the accounting is
       how a seam whose read half has never run reports OK. */
    if (op.startsWith('windowproxy.get\t') || op.startsWith('object.')) {
      const key = `${e.docId}#${id}`;
      /* AN UNANSWERED REQUEST IS RE-REPORTED EVERY STEP, so the first sighting is the one that acts. */
      if (reads.has(key)) continue;
      /* EVERY CROSS-AGENT OPERATION NAMES ITS TARGET DOCUMENT as its first operand, which is the one fact only
         this zone can act on: it is what says which instance holds the object. */
      const holder = holderOf(op.split('\t')[1]);
      if (!holder) { console.log('  ROUTE FAILED: no instance holds', op.split('\t')[1]); continue; }
      reads.set(key, { asker: e.docId, op, answered: false });
      console.log(`  [${e.docId}] cross-agent read asked: ${op}`);
      /* THE PEER IS ASKED, AND NOTHING IS ANSWERED INSIDE THAT CALL. It answers by RUNNING A PROGRAM — the IDL
         getter §7.2.5.1 defines the member as — as a flow on its own frontier, so the completion arrives on a
         later step of that instance and comes out through its notices like every other emission. Pumping it
         here is this zone's job precisely because the answer is not a return value. */
      holder.M.ccall('qjs_perform', 'void', ['number','number'], [holder.cs(key), holder.cs(op)]);
      for (let i = 0; i < 400 && !answers.has(key); i++) if (!(await service(holder))) break;
      if (!answers.has(key)) { console.log(`  NOT ANSWERED: ${key}`); continue; }
      /* RELAYED VERBATIM. The completion is in the engines' own grammar and this zone does not read it: a value
         that is an OBJECT is a NAME in the answering agent's namespace, which means nothing out here. */
      e.M.ccall('qjs_host_answer_remote', 'void', ['number','number'], [id, e.cs(answers.get(key))]);
      answers.delete(key);
      reads.get(key).answered = true;
      continue;
    }
    console.log(`  [${e.docId}] request: ${op.slice(0, 90)}`);
    if (!op.startsWith('document.fetch\t')) continue;
    /* THE TRAILING 0 IS THE NORMAL COMPLETION — an answer is a completion record, and this zone fetched bytes
       rather than relaying another instance's program, so it has nothing to have thrown in. */
    e.M.ccall('qjs_host_answer', 'void', ['number','number','number'],
              [id, e.cs(JSON.stringify({ body: HTML_B, csp: null })), 0]);
  }
  for (const n of e.str('qjs_host_notices').split('\n').filter(Boolean)) {
    const f = n.split('\t');
    console.log(`  [${e.docId}] notice: ${f[0]} ${f.slice(1, 3).join(' ')}`);
    /* FIELD 5 IS THE CHILD'S TOP-LEVEL CREATION URL, decided by the creator's §7.4 and carried on the
       notice for the same reason field 6's policy container is: the new instance cannot derive it. The policy
       is LAST because it is the record's remainder — a raw CSP header may contain HTAB. */
    if (f[0] === 'navigable.create') engines.push(await makeEngine(HTML_B, f[3], f[1], f.slice(6).join('\t'), f[5]));
    else if (f[0] === 'windowproxy.post') posts.push({ doc: f[1], world: f[2], record: n, origin: e.origin });
    /* A COMPLETION THIS INSTANCE PRODUCED for an operation it was asked to perform, naming the token this zone
       minted. Held rather than delivered here: the asker is another instance and this loop is inside its step. */
    else if (f[0] === 'remoteop.answer') answers.set(f[1], f.slice(2).join('\t'));
    else throw new Error('a notice this host does not act on: ' + f[0]);
  }
  return r !== 0;
}

/* THE ROOT DOCUMENT IS ITS OWN TOP-LEVEL TRAVERSABLE, so its environment's top-level creation URL is its
   own address. */
engines.push(await makeEngine(HTML_A, 'https://a.test/', 'd1', '', 'https://a.test/'));

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
const readsAnswered = [...reads.values()].filter((r) => r.answered).length;
console.log(`cross-agent reads asked: ${reads.size}   answered: ${readsAnswered}`);
for (const r of reads.values()) console.log(`  [${r.asker}] ${r.answered ? 'ANSWERED' : 'UNANSWERED'} ${r.op}`);
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

/* WHAT MAKES THIS A SMOKE TEST RATHER THAN A PRINTOUT. Four things have to have happened, and each of them is
   a whole mechanism failing silently if it did not: a second instance was provisioned (the create notice was
   acted on), every routed record reached the receiving page's listener (the inbound half), at least one
   segment was materialized by FORKING an ancestor (the world vector's ancestry was READ and used), and the one
   synchronous cross-instance READ this engine has was asked AND answered by the instance that holds the
   document — which is the half that has never run. */
const fail = (why) => { console.error('[route] FAILED: ' + why); process.exit(1); };
if (engines.length < 2) fail('no second instance was provisioned — the navigable.create notice went unanswered');
if (!posts.length) fail('the sender emitted no cross-instance post');
if (got.length !== posts.length)
  fail(`${posts.length} posts routed but the receiving page's listener saw ${got.length}`);
if (!forked) fail("no segment was materialized by forking an ancestor — the world vector's ancestry was carried " +
                  'and never used, which is the state this driver exists to detect');
/* ASKED IS THE FIRST HALF AND IT IS NOW TRUE. A zero here would mean `w.length` resolved WITHOUT reaching the
   peer, which is a §7.2.5.1 fidelity bug and not a transport gap: `length` is the child-navigable count of the
   OTHER document, so an answer produced in the asking instance counted this document's frames and called them
   the other's. */
if (!reads.size)
  fail('`w.length` on a cross-origin WindowProxy asked the peer nothing — §7.2.5.1 answers it from the child-' +
       "navigable count of the PEER's active document, so an answer that never left this instance counted the " +
       "asking document's own frames");
/* AND ANSWERED IS THE SECOND, AND IT IS THE HALF THAT CARRIES A VALUE BACK. Everything above this line is the
   ASKING half — the world vector, its ancestry, the segment the peer materializes from it, the origin stamp. A
   read that is asked and never answered leaves the asking flow parked with its snapshot intact, which is the
   correct behaviour and an unfinished seam: the peer has to install the asking world's segment and answer BY
   RUNNING A PROGRAM, on its own frontier, and hand back a COMPLETION rather than a value. */
if (readsAnswered !== reads.size)
  fail(`${reads.size} cross-agent read(s) asked and ${readsAnswered} answered — the instance holding the ` +
       'document did not produce a completion for one of them. Either no instance holds the document the ' +
       'operation names (a `navigable.create` notice this zone dropped), or the performing instance never ' +
       'reached the end of the program its answer is — which is a flow on its frontier and can be parked ' +
       'behind anything else that frontier is doing');
console.log('[route] OK — two instances, every routed record delivered, ancestry-forked segments: ' + forked +
            `, cross-agent reads answered: ${readsAnswered}`);
