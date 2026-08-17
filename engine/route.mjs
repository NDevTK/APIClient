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
   answer arrives (engine_host_answer) and this fixture's `b` has one timeline, so it is not reached here.

   AND THE PARK, WHICH IS THE HALF OF THIS SEAM THAT HAD NEVER RUN AT ALL. Every mechanism above was exercised
   with both instances resident for the whole run, so "a flow suspends at a cross-instance read" was tested and
   "…and is then PAGED OUT while the read is outstanding" was not — which is not a corner: Level-1 eviction
   gives up ONE document's engine for a document worth more, so the peer OUTLIVING the asker's park is the
   ordinary case rather than the exotic one. Phase 4 withholds one read, parks `a` on it, tears `a` down, and
   resumes it from its own residue while `b` stays exactly where it was. What that measures is stated as
   assertions at the bottom, and each of them is about a thing nothing else in the tree can observe:
     - the completion the peer computed for the PRE-PARK read has nowhere to land (the asking instance is gone
       and the resumed one never asked that question), so it is an ORPHAN this zone must still be holding;
     - the resumed flow RE-ISSUES the read — which is the cold tier's own claim about the replies a host owes
       (solver/cold.h), and the only thing that makes a park at a cross-instance read lossless;
     - and the world name it re-issues under must be one the peer has never seen. A WorldId's serial counts
       from 1 in every session under a document name that is stable BY REQUIREMENT, so the name carries a third
       coordinate: the session's GENERATION, minted into every world and carried across the tier by the residue
       itself (solver/world.h, solver/cold.h's 'g' record). The resumed session therefore mints in a namespace
       disjoint from the one that parked, and `b` materializes new segments instead of answering out of a dead
       flow's. This driver is what makes that decidable — it is the only place both sessions are visible. */
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const factory = await import(join(ENGINE, '..', 'extension', 'lib', 'qjs', 'qjs.mjs'));
const boot = factory.default ?? factory;

/* THE READ IS LAST AND BOTH ARMS MAKE IT, so two DIFFERENT worlds ask the same question of one peer — which is
   the case a single-timeline peer cannot answer with one number, and the reason the entry that performs it has
   to install the asking world's segment rather than read a property from C.
   AND ONE READ COMES FIRST, BEFORE THE FORK, WHICH IS WHAT MAKES THE ANCESTRY FORK REACHABLE AT ALL. The
   comment below used to say the root world's segment is the one "record 1 created" — but record 1 is a POST,
   and this driver holds every post until phase 2 while it performs every READ inline in phase 1. So the peer
   met the two CHILD worlds first and materialized both from its baseline, correctly and with no ancestor to
   fork, and `forkedFromAncestor` was 0 — the exact number the check at the bottom exists to catch, sitting at
   zero underneath a driver that aborted before it got there. A read in the ROOT world, taken before the branch,
   is what puts that segment at the peer first; the two arms then name it as their nearest ancestor and
   world_segment forks it. The fixture has to CREATE the precondition its own assertion is about. */
const HTML_A = `<!doctype html><script>
  var w = window.open("https://b.test/child", "child");
  w.postMessage({hello:"root", n: w.length}, "*");
  if (__FLAGS.admin) { w.postMessage({hello:"admin"}, "*"); } else { w.postMessage({hello:"public"}, "*"); }
  w.postMessage({hello:"length", n: w.length}, "*");
  fetch("/resume").then(function () { fetch("/closed?v=" + (typeof w.closed) + ":" + w.closed); });
</script>`;
/* `/hold` is NEVER answered, and that is the point: `b`'s boot flow stays live and owed, so the second and
   third arrivals have a timeline to arrive in. A document whose every flow has finished cannot receive, and
   the engine says so rather than delivering into nothing. */
/* `typeof e.data.n` RIDES THE DELIVERY because `otherW.length === 0` distinguishes a number from the string
   "0", and an answer that arrived as text and stayed text would satisfy every loose check in this file while
   proving only that bytes moved. */
/* AND `b` CLOSES ITSELF, which is the one state change no read of `a`'s own records could ever discover.
   §7.2.2.1 opening and closing windows makes `closed` the OR of a null browsing context and the top-level
   traversable's is closing, and close() sets is closing IN THE AGENT THAT RUNS IT — here, `b`. `a` holds a
   WindowProxy for the same traversable and its copy is never written, so `w.closed` was `false` forever about
   a window that had closed itself. It is the reverse direction of `w.length`: `length` is a fact `a` cannot
   COMPUTE, `closed` is a fact `a` is confidently WRONG about, and only the second one fails silently. */
const HTML_B = `<!doctype html><script>
  fetch("/hold");
  window.addEventListener("message", function (e) {
    fetch("/got?origin=" + e.origin + "&hello=" + e.data.hello + "&n=" + (typeof e.data.n) + ":" + e.data.n);
    if (e.data.hello === "length") window.close();
  });
</script>`;

/* `topLevelUrl` is HTML §8.1.3.1's TOP-LEVEL CREATION URL — this zone's to state, because one instance is
   one document and only the zone that routed the create knows what embeds it. */
/* THE INSTANCE COUNTER IS THIS ZONE'S, AND IT IS THE THING THE ENGINE'S WORLD NAME IS MISSING. A resumed
   document is the SAME document — same name, same address, same routing — and a DIFFERENT session, whose
   request ids and export ids both count from 1 again. So every token this zone mints carries `tag` and not
   `docId`: without it the resumed instance's request 1 lands on the rendezvous of the parked instance's
   request 1 and the driver answers a question nobody asked. */
let instanceSerial = 0;

async function makeEngine(html, url, docId, csp, topLevelUrl, recipes) {
  const M = await boot();
  const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
  const str = (f, ...a) => String(M.ccall(f, 'string', a.map(() => 'number'), a.map(cs)) ?? '');
  /* §2.2.5's BODY, INTO THIS INSTANCE'S LINEAR MEMORY. It crosses beside the record's JSON rather than inside
     it, because JSON cannot say a byte sequence and every way of making it able to is an algorithm run by the
     zone that fetched — which is what the extension's `resp.text()` was, and what left the classic-script
     decode with nothing of the response's charset to honour. `bytes` may be a string here, and that is an
     ENCODE (this file's fixtures are written as source text), never a decode. */
  const bs = (b) => {
    const u8 = typeof b === 'string' ? new TextEncoder().encode(b) : b;
    const p = M._malloc(u8.length + 1);
    M.HEAPU8.set(u8, p);
    return [p, u8.length];
  };
  /* KEYED ON THE REQUEST, WHICH IS THE PAIR: `qjs_pending` answers `METHOD<TAB>URL` lines and the delivery
     matches both halves, so a GET and a POST to one address are two questions with two replies. */
  const provide = (method, u, reply, body) => {
    const [p, n] = bs(body);
    try { M.ccall('qjs_provide', 'void', ['number','number','number','number','number'],
                  [cs(method), cs(u), cs(JSON.stringify(reply)), p, n]); }
    finally { M._free(p); }
  };
  const answer = (id, meta, body) => {
    const [p, n] = bs(body);
    try { M.ccall('qjs_host_answer', 'void', ['number','number','number','number','number'],
                  [id, cs(JSON.stringify(meta)), 0, p, n]); }
    finally { M._free(p); }
  };
  M.ccall('qjs_init', 'number', ['number','number','number','number','number'],
    [cs(html), cs(url), cs(docId), cs(csp || ''), cs(topLevelUrl)]);
  /* THE RESIDUE SEEDS THE FRONTIER INSTEAD OF THE BOOT FLOW (solver/cold.h). It is ';'-joined records, which
     is the language cold_park_recipes both writes and reads; the extension joins the stored ARRAY the same
     way, so this zone stores what that one stores. */
  M.ccall('qjs_begin', 'void', ['number'], [cs(recipes || '')]);
  return { M, cs, str, provide, answer, docId, tag: `${docId}/s${++instanceSerial}`,
           docUrl: url, origin: new URL(url).origin, done: false };
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
/* Every `/closed` the ASKING page fetched — `a`'s own report of what `w.closed` answered, collected apart from
   `got` because it is a different measurement: `got` counts what crossed INTO `b`, this counts what came BACK. */
const closedReports = [];
/* `/resume` IS DEFERRED, WHICH IS HOW THIS DRIVER ORDERS TWO INSTANCES WITHOUT A CLOCK. `a`'s read of
   `w.closed` has to happen AFTER `b` has closed itself, and the only thing that orders one instance's flow
   against another's here is an owed reply: `a` parks on this fetch, `b` is routed its messages and closes, and
   phase 3 answers it. A `setTimeout` would order nothing — both engines advance only when this loop steps
   them. */
let resumeOwed = true;
/* WHILE THIS IS SET, A CROSS-AGENT READ IS ASKED OF THE PEER AND ITS COMPLETION IS NOT RELAYED — the asking
   flow stays suspended at the read with its snapshot intact, which is the state phase 4 parks it in. The peer
   is still ASKED, deliberately: the question is what becomes of a peer's IN-FLIGHT turn when the instance that
   asked pages out, and a read that was never put to the peer would not have one. */
let withholdReads = false;
/* Every record this zone could not route because no instance holds the document it names. Collected rather
   than logged: an unroutable operation parks its asker forever, so it is a failure and not a note. */
const routeFailures = [];
/* DECLARED HERE BECAUSE THE PHASES USE IT. A phase that cannot reach the state the next one measures has to
   stop there rather than carry a half-built precondition into an assertion about something else. */
const fail = (why) => { console.error('[route] FAILED: ' + why); process.exit(1); };

/* ONE STEP of `e`, then everything the host owes it. Returns false once the engine reports its frontier done. */
async function service(e) {
  const r = e.M.ccall('qjs_step', 'number', [], []);
  for (const line of e.str('qjs_pending').split('\n').filter(Boolean)) {
    const tab = line.indexOf('\t');
    if (tab <= 0) fail(`a pending line carries no METHOD: ${line}`);
    const method = line.slice(0, tab), u = line.slice(tab + 1);
    if (u.includes('/hold')) continue;
    if (u.includes('/resume') && resumeOwed) continue;
    if (u.includes('/got')) { got.push(u); console.log(`  [${e.docId}] DELIVERED: ${u}`); }
    if (u.includes('/closed')) { closedReports.push(u); console.log(`  [${e.docId}] READ BACK: ${u}`); }
    /* THE ONE REPLY RECORD every host of this engine delivers, crossing as JSON so it carries its type. This
       zone follows no redirect, so Fetch §4.1 gives the response a clone of the REQUEST's URL list — one item,
       RESOLVED against this document's address because a URL list holds URLs and `response.url` serializes the
       last of them.
       `computedType` IS THIS ZONE'S DECISION AND IT IS WHY THIS DRIVER STOPPED RUNNING. The sniff belongs to
       whoever READ the bytes, so fetch_reply_computed_type asserts the field rather than defaulting it — and
       this record was written before that field existed, so every host that grew one left this one behind and
       the seam driver aborted on its FIRST reply. It is a host that has to state it like any other: the bytes
       below are the two characters `{}`, this zone minted them, and `application/json` is what it computed them
       to be. */
    const reply = { status: 200, statusText: 'OK', headers: [],
                    urlList: [new URL(u, e.docUrl).href], computedType: 'application/json' };
    /* THE BODY TRAVELS AS BYTES, beside that JSON and never inside it: §2.2.5 makes a response's body a BYTE
       SEQUENCE, and the only ways to put one in JSON are to encode it or to DECODE it — and a decode run by
       the zone that FETCHED is exactly what left HTML §8.1.4.2's classic-script decode nothing to decode. */
    e.provide(method, u, reply, '{}');
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
      const key = `${e.tag}#${id}`;
      /* AN UNANSWERED REQUEST IS RE-REPORTED EVERY STEP, so the first sighting is the one that acts. */
      if (reads.has(key)) continue;
      /* EVERY CROSS-AGENT OPERATION NAMES ITS TARGET DOCUMENT as its first operand, which is the one fact only
         this zone can act on: it is what says which instance holds the object. */
      const holder = holderOf(op.split('\t')[1]);
      if (!holder) { routeFailures.push(`read ${op}`); continue; }
      reads.set(key, { asker: e.tag, op, world: op.split('\t')[2], answered: false, withheld: withholdReads });
      console.log(`  [${e.tag}] cross-agent read asked: ${op}`);
      /* THE PEER IS ASKED, AND NOTHING IS ANSWERED INSIDE THAT CALL. It answers by RUNNING A PROGRAM — the IDL
         getter §7.2.5.1 defines the member as — as a flow on its own frontier, so the completion arrives on a
         later step of that instance and comes out through its notices like every other emission. Pumping it
         here is this zone's job precisely because the answer is not a return value. */
      holder.M.ccall('qjs_perform', 'void', ['number','number'], [holder.cs(key), holder.cs(op)]);
      /* WITHHELD: the peer has been asked and its answer is left where it lands. The asking flow stays parked
         on the read — which is the correct behaviour for an unanswered request and is exactly the state a
         Level-1 eviction finds a flow in. */
      if (withholdReads) { console.log(`  [${e.tag}] WITHHELD — the asking flow stays suspended at the read`); continue; }
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
    /* §7.4 step 14's answer: the POLICY as JSON, the document as BYTES. A Document is parsed from a byte
       sequence, and this zone hands one over rather than a string it decoded first. */
    e.answer(id, { csp: null }, HTML_B);
  }
  for (const n of e.str('qjs_host_notices').split('\n').filter(Boolean)) {
    const f = n.split('\t');
    console.log(`  [${e.docId}] notice: ${f[0]} ${f.slice(1, 3).join(' ')}`);
    /* FIELD 5 IS THE CHILD'S TOP-LEVEL CREATION URL, decided by the creator's §7.4 and carried on the
       notice for the same reason field 6's policy container is: the new instance cannot derive it. The policy
       is LAST because it is the record's remainder — a raw CSP header may contain HTAB. */
    /* A DOCUMENT THIS ZONE ALREADY HOLDS AN INSTANCE FOR IS NOT PROVISIONED TWICE, and that is not a guard
       against a duplicate notice — it is what a resumed document does. `a`'s replay re-runs its scripts, so it
       re-creates the child navigable and re-announces it under the same name, while the instance that holds
       that name has been running the whole time. Provisioning a second one would give one document two heaps
       and two object graphs, which is the state SECURITY.md's one-instance-per-cluster rule exists to prevent
       and which nothing downstream could tell from the routing. */
    if (f[0] === 'navigable.create') {
      if (holderOf(f[1])) console.log(`  [${e.tag}] create for ${f[1]}, already held — routing to the live instance`);
      else engines.push(await makeEngine(HTML_B, f[3], f[1], f.slice(6).join('\t'), f[5]));
    }
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

/* PHASE 2 — one at a time, each delivered before the next is routed. Written as a routine because phase 4
   routes the posts a RESUMED sender emits through the identical path: one record, then pump the target until
   its listener has seen it. */
let routedUpTo = 0;
async function routePending() {
  for (; routedUpTo < posts.length; routedUpTo++) {
    const p = posts[routedUpTo];
    const target = holderOf(p.doc);
    if (!target) { routeFailures.push(`post ${p.world} -> ${p.doc}`); continue; }
    console.log(`routing world ${p.world} -> [${target.tag}] as origin ${p.origin}`);
    target.M.ccall('qjs_route', 'void', ['number','number'], [target.cs(p.record), target.cs(p.origin)]);
    const before = got.length;
    for (let i = 0; i < 400 && got.length === before; i++) if (!(await service(target))) break;
    if (got.length === before) console.log(`  NOT DELIVERED: ${p.world}`);
  }
}
await routePending();

/* PHASE 3 — `b` HAS NOW CLOSED ITSELF, so `a`'s parked read is released and asks the one question whose answer
   lives entirely in the other instance's record. `a` is stepped rather than told anything: the reply to
   `/resume` resumes the flow it parked, that flow reads `w.closed`, and the read suspends again on the peer
   exactly as `w.length` did — the whole point being that a driver never states the answer, it only routes.
   AND THE ANSWER IS WITHHELD, which is what leaves `a` in the one state phase 4 is about: a flow suspended AT
   a cross-instance read, its snapshot intact, with the peer's turn already running. */
resumeOwed = false;
withholdReads = true;
const preParkWorlds = new Set([...posts.map((p) => p.world), ...[...reads.values()].map((r) => r.world)]
                              .map((v) => v.split(',')[0]));
const preParkReads = reads.size;
console.log(`\nphase 3: /resume answered — a's parked flow reads w.closed, and the answer is WITHHELD`);
let aLive = true;
for (let i = 0; i < 2000 && reads.size === preParkReads; i++) { aLive = await service(engines[0]); if (!aLive) break; }
if (reads.size === preParkReads)
  fail('the withheld read was never asked, so phase 4 has nothing to park a flow ON — either the /resume reply ' +
       'did not resume the flow that awaited it, or `w.closed` was answered without reaching the peer');
if (!aLive)
  fail("`a`'s frontier reported DONE while one of its flows was suspended at a cross-instance read — a blocked " +
       'frontier is STALLED (engine.h), which is "call me again", and DONE over a suspended flow is that flow ' +
       'dropped');

/* PHASE 4 — THE PARK. `a` is paged out ON that suspended read, torn down, and resumed from its own residue
   into a NEW instance, while `b` — which holds the answer, the export table and a segment for every world `a`
   has sent it — is not touched. That asymmetry is the point: Level-1 eviction gives up ONE document's engine.
   THE RESIDUE IS A REPLAY RECIPE (solver/cold.h), not a serialized continuation: it names the ARMS each
   suspended flow took, and the flow's delta, its frames and the replies it was owed are regenerated by
   re-running the document under them. So the read this flow is suspended at is not carried across — it is
   RE-ISSUED, and everything below is about whether the question it re-asks is the same question. */
/* THE PEER'S TURN RUNS TO ITS END FIRST, with the asker still suspended on it. That is what makes the
   completion an ORPHAN rather than a question nobody started: `b` installs the asking world's segment, runs
   the IDL getter as a flow on its own frontier and emits `remoteop.answer` — and by the time it does, the
   instance that asked has been parked and torn down. */
for (let i = 0; i < 400 && !answers.size; i++) if (!(await service(engines[1]))) break;

const parked = engines[0];
const postsAtPark = posts.length;
parked.M.ccall('qjs_request_park', 'void', [], []);
const parkStep = parked.M.ccall('qjs_step', 'number', [], []);
const residue = JSON.parse(parked.str('qjs_result'))._park.join(';');
console.log(`\nphase 4: park step answered ${parkStep}, residue ${residue.length} bytes: ${residue}`);
parked.M.ccall('qjs_teardown', 'void', [], []);
/* THE ORPHANS ARE COUNTED BEFORE THE RESUMED INSTANCE CAN ADD ANY. A completion held here names a token of the
   instance that has just gone: `b` computed it under the asking flow's world, and there is no longer a flow,
   a register or an instance for it to land in. */
const orphanCompletions = [...answers.keys()];
withholdReads = false;
engines[0] = await makeEngine(HTML_A, 'https://a.test/', 'd1', '', 'https://a.test/', residue);
console.log(`phase 4: resumed as [${engines[0].tag}] from the residue; [${engines[1].tag}] never left memory`);
for (let i = 0; i < 4000 && !closedReports.length; i++) {
  if (!(await service(engines[0]))) break;
  await routePending();
}
const resumedReads = [...reads.values()].filter((r) => r.asker === engines[0].tag);
/* EVERY WORLD NAME THE RESUMED SESSION PUT ON THE WIRE, against every name the session that ended had already
   put there. A name in both sets is one `b` keyed a segment on for a flow that no longer exists. */
const reusedWorlds = [...new Set([...resumedReads.map((r) => r.world),
                                  ...posts.slice(postsAtPark).map((p) => p.world)]
                                 .map((v) => v.split(',')[0]))].filter((h) => preParkWorlds.has(h));

console.log(`\nposts routed: ${posts.length}   messages the receiving page saw: ${got.length}`);
for (const u of got) console.log('  ' + u);
const readsAnswered = [...reads.values()].filter((r) => r.answered).length;
console.log(`cross-agent reads asked: ${reads.size}   answered: ${readsAnswered}`);
for (const r of reads.values())
  console.log(`  [${r.asker}] ${r.withheld ? 'WITHHELD-AND-PARKED' : r.answered ? 'ANSWERED' : 'UNANSWERED'} ${r.op}`);
/* THE SEAM'S OWN COUNTS, from the receiving instance, and HELD IS NOT MADE. This line printed one number under
   a sentence describing the other: `_worldSegments` carried world.c's CUMULATIVE materialized count while the
   words beside it said "how many foreign worlds hold a segment here", which is the live table. The two agree
   exactly until world_release runs — the one event a segment count exists to make visible — so the print was
   at its most wrong precisely where this driver is looking. Both cross now: `held` is the live table,
   `made` is how many were ever materialized, and `forked` is how many of those were built by FORKING an
   ancestor the ancestry named, which is the number this driver exists to move off zero. held far below made is
   a seam that materialized and released; held == made is a live peer; held above made is impossible and
   world.c DCHECKs it. */
let forked = 0;
for (const e of engines) {
  const r = JSON.parse(e.str('qjs_result'));
  forked += r._worldSegmentsForked;
  console.log(`[${e.tag}] worldSegments held=${r._worldSegmentsHeld} made=${r._worldSegmentsMade} ` +
              `forkedFromAncestor=${r._worldSegmentsForked} flows=${r._flows} switches=${r._switches}`);
}

/* WHAT MAKES THIS A SMOKE TEST RATHER THAN A PRINTOUT. Four things have to have happened, and each of them is
   a whole mechanism failing silently if it did not: a second instance was provisioned (the create notice was
   acted on), every routed record reached the receiving page's listener (the inbound half), at least one
   segment was materialized by FORKING an ancestor (the world vector's ancestry was READ and used), and the one
   synchronous cross-instance READ this engine has was asked AND answered by the instance that holds the
   document — which is the half that has never run. */
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
const withheldReads = [...reads.values()].filter((r) => r.withheld);
if (readsAnswered !== reads.size - withheldReads.length)
  fail(`${reads.size} cross-agent read(s) asked, ${withheldReads.length} withheld on purpose and ` +
       `${readsAnswered} answered — the instance holding the ` +
       'document did not produce a completion for one of them. Either no instance holds the document the ' +
       'operation names (a `navigable.create` notice this zone dropped), or the performing instance never ' +
       'reached the end of the program its answer is — which is a flow on its frontier and can be parked ' +
       'behind anything else that frontier is doing');
if (routeFailures.length)
  fail(`${routeFailures.length} record(s) named a document no instance holds, so their askers are parked on a ` +
       `question nothing will ever answer: ${routeFailures.join(' ; ')}`);
/* AND THE LAST ONE IS THE ONE A WRONG ANSWER PASSES. Every check above fails by a NUMBER staying zero — a
   record that did not cross, a segment that was not forked, a completion that never came — and none of them
   would have moved if `w.closed` had been answered out of `a`'s own byte, because a local answer is instant and
   plausible. So this check is on the VALUE: `b` closed itself, so §7.2.2.1's `closed` is true about that
   traversable, and the only record in existence that says so is `b`'s. `typeof` rides along for the reason it
   rides on the delivery — an answer that arrived as text and stayed text satisfies `v ? ...` and is not a
   boolean. */
if (closedReports.length !== 1)
  fail(`the asking page reported ${closedReports.length} reads of \`w.closed\` and this driver arranged exactly ` +
       'one — it is produced by the RESUMED instance, since the answer was withheld from the session that ' +
       'parked, so a zero here is a flow that did not come back from the cold tier and a two is a flow that ' +
       'came back beside one that had never left');
if (!closedReports[0].includes('v=boolean:true'))
  fail(`\`w.closed\` answered \`${closedReports[0].split('v=')[1]}\` about a top-level traversable that had run ` +
       'window.close() in the OTHER instance. §7.2.2.1 opening and closing windows makes `closed` the OR of a ' +
       'null browsing context and the top-level traversable\'s is closing, and close() writes is closing in the ' +
       'agent that RUNS it — so an answer read out of this agent\'s own copy of that record is false about a ' +
       'window that has closed itself, which is the one cross-instance defect that produces a plausible value ' +
       'instead of a missing one');

/* ── THE PARK, WHICH IS THE PART OF THIS SEAM NOTHING HAD EVER EXERCISED ──────────────────────────────────
   The checks above are all about two instances that were both resident for the whole run. These four are about
   one of them LEAVING MEMORY while the other keeps its state, which is what Level-1 eviction does. */
if (parkStep !== 0)
  fail(`the park step answered ${parkStep} rather than DONE — engine_sched_step takes the park before its first ` +
       'pick and closes the session, so anything else means the frontier was not written out and the residue ' +
       'below is not the residue');
if (!residue)
  fail('the parked frontier wrote an EMPTY residue while a flow was suspended at a cross-instance read — an ' +
       'empty park document is how a fully-explored document DELETES its cold entry (solver/cold.h), so a ' +
       'suspended flow that produces one is a flow dropped under the name of a positive answer');
/* THE PEER'S IN-FLIGHT TURN OUTLIVES THE INSTANCE THAT ASKED, and nothing on either side knows. `b` was asked
   the withheld read, ran the program its answer is, and emitted a completion under a token whose asking
   instance no longer exists — no flow, no register, no engine. This zone is holding it because this zone is
   the only thing left that can; the engine's own delivery entry would take it (engine_host_answer walks the
   registers of an instance whose session is closed and finds the entry still there, writes the value onto it,
   and nothing will ever read it) or, once the instance is torn down, there is nowhere to deliver it at all. */
if (!orphanCompletions.length)
  fail('the peer produced no completion for the read that was parked on — either the withheld read never ' +
       'reached the peer, or the peer did not run the program its answer is, and in both cases phase 4 parked ' +
       'a flow that was not actually suspended at a cross-instance read');
console.log(`orphaned peer completions after the park: ${orphanCompletions.length} — ${orphanCompletions.join(' ')}`);
/* AND THE RESUMED FLOW RE-ISSUES THE READ, which is the cold tier's whole claim about the replies a host owes:
   the residue names the ARMS the flow took, so re-running the document under them re-issues the request and it
   is answered with TODAY's value (§Time-travel-resume). A resumed session that asked nothing would mean the
   suspended flow's path did not replay to the point it was suspended at, which is a dropped flow wearing the
   name of a successful park. */
if (!resumedReads.length)
  fail('the resumed instance asked the peer nothing — the flow that was parked AT a cross-instance read did ' +
       'not replay to that read, so the park lost it. A recipe is a replay (solver/cold.h): the request is ' +
       'not carried across, it is re-issued by the code that issued it the first time');
/* THE ONE A WRONG NAME PASSES, AND IT LIVES OUT HERE BECAUSE NOTHING INSIDE EITHER ENGINE CAN SEE BOTH
   SESSIONS. A WorldId is (document, GENERATION, serial): the document name is stable across a park by
   requirement — the routing above depends on it — and the serial counts from 1 in every session, so without
   the generation the resumed session would hand the peer the exact names the session that ended handed it and
   the peer would answer each resumed flow out of a dead flow's timeline. The ancestry would match too, so no
   assert in either engine could catch it. The generation rides the residue (solver/cold.h's 'g' record) and
   world_session_resume mints above it, so a name from the ended session is now unresolvable rather than
   wrongly resolvable. What remains owed is the LEAK, not the collision: nothing tells a peer a world is gone,
   so world_release still has no caller and the dead session's segments stay. */
if (reusedWorlds.length)
  fail(`the resumed session put ${reusedWorlds.length} world name(s) on the wire that the session that ended ` +
       `had already sent: ${reusedWorlds.join(' ')}. A WorldId's serial counts from 1 in every session while ` +
       'the document name is stable by requirement, so the peer — which never left memory — already holds a ' +
       'segment keyed on each of them, and answers the resumed flow under the timeline of a flow that no ' +
       'longer exists. Two timelines wearing one name, across sessions. Build the session component of a world ' +
       'name (the residue is what crosses the tier and can carry it), or the park-time notice that gives ' +
       'world_release its missing caller');
console.log('[route] OK — two instances, every routed record delivered, ancestry-forked segments: ' + forked +
            `, cross-agent reads answered: ${readsAnswered}, w.closed read back: ${closedReports[0]}` +
            `, parked and resumed across ${residue.length} bytes of residue`);
