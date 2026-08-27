/* THE CROSS-INSTANCE SEAM'S DRIVER — a STAGE of `node engine/build.mjs`, which runs it against the ABI program
 * it just linked and reports it beside the smoke. Standalone: `node engine/route.mjs`, after any build. (The
 * `abi` argument this line used to name is gone — a build produces both programs, so nothing selects one.)
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
   state IS its flows — and the asking flow must then FORK per answer (engine_host_answer records each, and
   flow_answer_fork builds the arm). THIS FIXTURE REACHES THAT, and a line here used to say it did not: `b` has
   one timeline only until the first record is routed to it, and by the time anything is asked of it it holds
   four. Every one of them answers, which is why the answers carry the WORLD of the flow that produced each —
   without it, N true answers and one answer relayed N times are the same bytes, this zone kept whichever
   arrived last, and `a` read `w.closed` twice in one expression and got `true` then `false` out of two
   contradictory timelines of one document.

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

/* `inheritedCsp` and `inheritedCspSelfOrigin` are HTML §7.1.7 "Policy containers"' clone of the CREATOR's,
   for an instance provisioned from a `navigable.create`. They are TWO because CSP §2.2 "Policies" makes a CSP
   list "a struct consisting of policies (a list of policies) and a self-origin", and the second cannot be
   recovered from the first — §2.2.2 states it from outside the bytes. `csp` USED TO BE PASSED IN THE
   `headers` SLOT, which is a raw policy value where the entry parses HTTP field lines: it carried no colon,
   so header_list_parse_field_lines had nothing to make of it and every child this driver provisioned ran
   under NO policy while the record that named one crossed intact. Both halves are the empty string for a root
   document, which has no creator.
   `inheritedCoep` AND ITS THREE COMPANIONS ARE THAT SAME CONTAINER'S §7.1.4 EMBEDDER POLICY ITEM. HTML §7.1.7's
   clone-a-policy-container moves EVERY item of a container, so an item that stopped at this seam would be an
   inheritance silently replaced by `unsafe-none` — and unlike the CSP list there is no empty spelling for it:
   §7.1.7 gives every container an embedder policy, so a document with no creator states §7.1.4's own initial
   value ("unsafe-none", "", "unsafe-none", "") rather than nothing. The two VALUES are §7.1.4's token strings;
   main.c is the only party that turns one into a value and it crashes on a token naming none of the three.
   `parentNavigable` IS HTML §7.3.1.3 "Child navigables"' PARENT, AND IT IS NOT AN ITEM OF THAT CONTAINER — a
   policy container is five policies and says nothing about a frame tree. §7.3.1.3 defines the term over the
   link ("a navigable 'is a child navigable', which means that its parent is non-null"), so a driver that says
   nothing is declaring a TOP-LEVEL TRAVERSABLE — which is exactly what an instance provisioned for a
   CROSS-ORIGIN CHILD is not. It crosses as the emitting engine's own navigable identity, relayed verbatim; `u`
   is that grammar's undefined and is what a root document with no embedder states, which is why it is the
   default here and never an empty string.
   `containerPolicy` IS §7.3.1.3's OTHER LINK — the CONTAINER of that same navigable — and it carries an
   ANSWER rather than an element, because an element belongs to a heap and crosses no instance boundary.
   Permissions Policy §9.5 is "given null or an element (container) and an origin (origin)", and both of those
   arguments belong to the creating instance: it holds the `<iframe>` and it computed the child's origin. So
   §9.5 runs ONCE there and this relays its result. `null` is that grammar's own word for "there is no
   container", which is what this driver's own root documents state — it invented them, nothing presents them —
   and it is the same fact `u` states one link along. Without it the engine takes §9.7 step 1, "if container is
   null, return `Enabled`", for a navigable that HAS one.
   `ancestorOrigins` IS HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST for the Document
   this instance will build — a THIRD statement about the same navigable, not a field of either above it.
   §3.1.3's steps read the PARENT DOCUMENT's own recorded list, that Document's ORIGIN RECORD and the CONTAINER
   ELEMENT: the parent field names a navigable and carries no ancestry, and the origin RECORD is exactly what a
   serialization drops — §7.1.1 decides an opaque origin by IDENTITY while every opaque origin is the three
   bytes `null`, so an instance re-running step 10 over relayed text would mask an ancestor that is not the
   parent's whenever the parent is opaque. The creating instance runs the steps once and this relays the
   result. `none` is that grammar's word for "there are no ancestors", which is what this driver's own root
   documents state — nothing embeds them — and is the same fact `u` and `null` state one link along. Without
   it the engine installs the EMPTY list, which is the positive claim that the Document is at the top of its
   own tree. */
async function makeEngine(html, url, docId, headers, topLevelUrl, recipes, inheritedCsp, inheritedCspSelf,
                          inheritedCoep = 'unsafe-none', inheritedCoepEndpoint = '',
                          inheritedCoepReportOnly = 'unsafe-none', inheritedCoepReportOnlyEndpoint = '',
                          parentNavigable = 'u', containerPolicy = 'null', ancestorOrigins = 'none') {
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
    /* THE GUARD BYTE THE EXTRA ALLOCATION WAS ALWAYS FOR, and which nothing wrote. The engine asserts it on
       every pair it is handed: the length is what bounds the read, and the terminator is what makes the same
       buffer safe for the one consumer that still asks for a C string. Without it that assert reads whatever
       the allocator left there. */
    M.HEAPU8[p + u8.length] = 0;
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
  /* THE DOCUMENT CROSSES AS A PAIR, because a zero byte is a legal character in a document and `strlen` would
     end the parse at the first one. `bs` is the same helper the reply bodies use, for the same reason. */
  {
    const [hp, hn] = bs(html);
    M.ccall('qjs_init', 'number',
      ['number','number','number','number','number','number','number','number',
       'number','number','number','number','number','number','number'],
      [hp, hn, cs(url), cs(docId), cs(headers || ''), cs(topLevelUrl),
       cs(inheritedCsp || ''), cs(inheritedCspSelf || ''),
       cs(inheritedCoep), cs(inheritedCoepEndpoint),
       cs(inheritedCoepReportOnly), cs(inheritedCoepReportOnlyEndpoint),
       cs(parentNavigable), cs(containerPolicy), cs(ancestorOrigins)]);
    M._free(hp);
  }
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
/* THE DISTINCT `/got` REQUESTS THIS ZONE WAS SHOWN, WHICH IS NOT A COUNT OF LISTENER INVOCATIONS AND WAS READ
   AS ONE. `qjs_pending` is a SET keyed on the (method, URL) pair — engine_pending_fetches dedups it there,
   deliberately, because several flows park on one request and a second listing would make this zone provide
   twice — and every timeline of one document runs the same listener and issues the same bytes. What this
   witnesses is that the receiving page's own code RAN; how many times it ran is the engine's to state (the
   four ends of §9.3.3 step 8's task, read off `qjs_result` at the bottom of this file). */
const got = [];
/* EVERY CROSS-AGENT OPERATION THIS ZONE WAS ASKED TO PERFORM, keyed by the asking engine's request id, and
   whether it was ever answered. Keyed rather than counted because `qjs_host_requests` deliberately does NOT
   dedupe — an unanswered request is re-reported on every single step, so a count would be a step count, and
   the log line below would be one line per step for the rest of the run.
   THAT KEY IS ALSO THE RENDEZVOUS TOKEN the performing instance echoes on its answer. It has to be this zone's
   and not the asking flow's request id, because an id is unique only inside the instance that minted it and two
   askers may hold the same number — which is exactly the fact only the routing zone has. */
const reads = new Map();
/* Tokens the peer handed back rather than answering. Kept as a COUNT rather than a silence: a retraction and
   a lost operation look identical from here, and only the notice tells them apart. */
const retracted = [];
/* THE COMPLETIONS PERFORMING INSTANCES HAVE EMITTED, BY TOKEN — a LIST per token and not a slot, which is the
   defect this map used to be. A peer's document state IS its flows, so one question is answered once per live
   timeline and every one of those answers is true; `answers.set(token, …)` kept whichever arrived LAST and the
   relay below then handed the asker one arbitrary timeline's value and dropped the others. Measured: `b` held
   four timelines, request #4 was answered `.b1 .b1 .b0 .b0`, and `a`'s page — whose `"/closed?v=" +
   (typeof w.closed) + ":" + w.closed` reads that member TWICE — got `true` at one read and `false` at the next,
   out of two CONTRADICTORY timelines of one document. Every entry carries the WORLD that computed it, because
   that is the only thing that tells a second timeline from one answer relayed twice. */
const answers = new Map();
const answersFor = (token) => {
  if (!answers.has(token)) answers.set(token, []);
  return answers.get(token);
};
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
/* EVERY WORLD DEATH THAT CROSSED. A segment a peer materialized is a foreign flow's state living in its heap,
   and until this record existed nothing ever removed one — so an instance that had answered anything held every
   sender's timeline for the rest of its process and could not park at all (its own cold_park refuses it). This
   is what makes `held` fall below `made` in the counters printed at the bottom, which is the only way a release
   is observable from out here. */
const worldDeaths = [];
/* The vectors a park says its residue carries — see the `world.parked` arm. Recorded rather than relayed. */
const worldsParked = [];
/* Every record this zone could not route because no instance holds the document it names. Collected rather
   than logged: an unroutable operation parks its asker forever, so it is a failure and not a note. */
const routeFailures = [];
/* READS ARE DIFFERENT FROM POSTS HERE, AND CONFLATING THEM REPORTED A WORKING SEAM AS BROKEN. A post's cursor
   advances past a target that is not held yet, so that post is genuinely LOST and belongs in `routeFailures`
   at first sighting. A read is re-reported every step, so "no holder yet" is a retry — and `d1.2`'s navigable
   is created in the very step its read is asked. Held here and cleared the moment the read routes, so only a
   read still unheld when the drive ENDS is a failure. */
const unheldReads = new Map();
/* DECLARED HERE BECAUSE THE PHASES USE IT. A phase that cannot reach the state the next one measures has to
   stop there rather than carry a half-built precondition into an assertion about something else. */
const fail = (why) => { console.error('[route] FAILED: ' + why); process.exit(1); };

/* THE SCHEDULER'S THREE CODES (solver/engine.h), NAMED HERE BECAUSE THIS ZONE ACTS DIFFERENTLY ON EACH. The
   shipped ABI used to answer two of them — main.c folded STALLED into YIELD, "the bridge speaks two values" —
   and that fold is what hung this drive on every build. A yield asks to be OUTRANKED and costs nothing to
   ignore; a stall asks to be PAID, and stepping a stalled engine converts nothing into work. Measured here
   before the fold was deleted: 10,839,500 steps of the peer with `_switches:0`, `_flows:1`, no emission, and
   `_jobsRun` frozen at 10 — and DONE on the very next step once the reply it was owed was supplied. */
const STEP_DONE = 0, STEP_YIELD = 2, STEP_STALLED = 3;

/* ONE STEP of `e`, then everything the host owes it. Answers the engine's own code and how much this zone PAID
   into that engine in the same round, because the two together are what the pump's terminators are made of: a
   stall the payment MOVED is not a stall the driver may stop at (the frontier has work again — the same test
   engine_run makes with `r == ENGINE_STEP_STALLED && filled == 0`, and wpt_runner.c with `did`). */
async function service(e) {
  const r = e.M.ccall('qjs_step', 'number', [], []);
  /* THE CODE IS CHECKED FOR MEMBERSHIP AND NEVER DEFAULTED. A value outside the three is an ABI that moved
     under a driver still speaking the old one, which is exactly what this file was the casualty of. */
  if (r !== STEP_DONE && r !== STEP_YIELD && r !== STEP_STALLED)
    fail(`qjs_step answered ${r}, which is none of DONE(0)/YIELD(2)/STALLED(3) — the ABI carries three codes ` +
         'and this zone branches on all three, so a fourth is a contract that moved under this driver');
  /* WHAT THIS ZONE PUT INTO `e` THIS ROUND — replies delivered, operations answered, completions relayed back,
     records handed over. Counted rather than assumed, because "the engine is owed something" and "this zone
     supplied it" are two facts and the pump's stall terminator is precisely their disagreement. */
  let paid = 0;
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
    paid++;
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
      /* NO HOLDER **YET** IS A RETRY, NOT A VERDICT, and recording it as one made this drive report a working
         seam as broken. A read is re-reported every step (above), and `d1.2`'s navigable is CREATED in the same
         step that asks the read against it — so whichever notice the zone drains first decided the verdict. Both
         records this used to name arrived in the same run's census as ANSWERED, twenty rows above the FAILED
         line calling them "parked on a question nothing will ever answer": 13 asked, 2 withheld, 11 answered,
         and the accounting check below passed while this one failed. A permanent verdict on a transient
         condition — measured before the thing it is about has happened. Unheld at the END of the drive is the
         real failure, and it is derived there, from reads that were never routed. */
      const holder = holderOf(op.split('\t')[1]);
      if (!holder) { unheldReads.set(key, op); continue; }
      unheldReads.delete(key);
      reads.set(key, { asker: e.tag, op, world: op.split('\t')[2], answered: false, withheld: withholdReads,
                       /* WHICH PEER TIMELINES ANSWERED IT — the list this driver could not previously write,
                          because the answers were anonymous and it kept one. */
                       by: [] });
      console.log(`  [${e.tag}] cross-agent read asked: ${op}`);
      /* THE PEER IS ASKED, AND NOTHING IS ANSWERED INSIDE THAT CALL. It answers by RUNNING A PROGRAM — the IDL
         getter §7.2.5.1 defines the member as — as a flow on its own frontier, so the completion arrives on a
         later step of that instance and comes out through its notices like every other emission. Pumping it
         here is this zone's job precisely because the answer is not a return value. */
      holder.M.ccall('qjs_perform', 'void', ['number','number'], [holder.cs(key), holder.cs(op)]);
      /* A RECORD HANDED TO AN INSTANCE IS A PAYMENT TO THAT INSTANCE, and only where it IS this one: the peer
         is pumped on its own below, with its own count. A self-read (an agent asked about a document it holds)
         goes down this same branch, which is why the test is on identity rather than on the shape of the op. */
      if (holder === e) paid++;
      /* WITHHELD: the peer has been asked and its answer is left where it lands. The asking flow stays parked
         on the read — which is the correct behaviour for an unanswered request and is exactly the state a
         Level-1 eviction finds a flow in. */
      if (withholdReads) { console.log(`  [${e.tag}] WITHHELD — the asking flow stays suspended at the read`); continue; }
      /* THE PEER'S TURN RUNS TO ITS OWN END, AND THAT IS THE FIX RATHER THAN A LONGER WAIT. This waited for the
         FIRST answer (`() => answers.has(key)`) and then relayed and moved on — which stops the peer's turn in
         the middle of a question every one of its timelines is still answering, and leaves the rest to arrive
         on some later pump with no asker expecting them. A document's state IS its flows, so "the answer" is
         not a thing that exists: what exists is one answer per timeline, and the peer's own statement that it
         has said all of them is its frontier draining or STALLING on a bill this zone will not pay. Those are
         the same two terminators pumpUntil already documents; the predicate is `false` because there is no
         single emission to wait for. */
      const turn = await pumpUntil(holder, () => false);
      if (!answers.has(key)) { console.log(`  NOT ANSWERED (${turn}): ${key}`); continue; }
      /* EVERY ONE OF THEM, RELAYED VERBATIM AND IN ARRIVAL ORDER. The completion is in the engines' own grammar
         and this zone does not read it: a value that is an OBJECT is a NAME in the answering agent's namespace,
         which means nothing out here. The WORLD it crosses beside is in world_serialize's grammar and this zone
         does not read that either — it routes text; only an engine knows what a name means. The asking engine
         takes the first onto the register and records each further one as another timeline to fork an arm over
         (engine_host_answer), and it CRASHES on a repeat of one world, so relaying all of them is what the seam
         was built for rather than something this zone is getting away with. */
      const all = answers.get(key);
      for (const a of all) {
        e.M.ccall('qjs_host_answer_remote', 'void', ['number','number','number'],
                  [id, e.cs(a.world), e.cs(a.completion)]);
        paid++;
      }
      answers.delete(key);
      reads.get(key).answered = true;
      reads.get(key).by = all.map((a) => a.world);
      console.log(`  [${e.tag}] answered by ${all.length} peer timeline(s): ${all.map((a) => a.world).join(' ')}`);
      continue;
    }
    console.log(`  [${e.docId}] request: ${op.slice(0, 90)}`);
    if (!op.startsWith('document.fetch\t')) continue;
    /* THE TRAILING 0 IS THE NORMAL COMPLETION — an answer is a completion record, and this zone fetched bytes
       rather than relaying another instance's program, so it has nothing to have thrown in. */
    /* §7.4 step 14's answer: the RESPONSE'S HEADER LIST as JSON, the document as BYTES. A Document is parsed
       from a byte sequence, and this zone hands one over rather than a string it decoded first. The header
       list crosses as the HTTP FIELD LINES the response delivered — the empty string is a response that
       carried none, which is what this driver serves these bytes out of a literal with. It used to carry one
       extracted policy (`{csp: null}`), which is the shape that kept §7.1.3's opener policy out of every
       navigated Document. */
    e.answer(id, { headers: "" }, HTML_B);
    paid++;
  }
  await drainNotices(e);
  return { step: r, paid };
}

/* PUMP UNTIL THE THING HAPPENS OR THE PEER SAYS IT IS DONE — and NOT for a count of steps. Four loops here
   carried `i < 400` (one `i < 4000`), which is a STEP CAP: §NO BOUNDS forbids one, and the exemption the
   harness backstop enjoys does not reach it. That exemption is for a harness refusing to wait forever for a
   PROCESS it launched, reported through a different signal; this is a driver deciding how much of a flow's
   own execution it is willing to watch, silently, and then reporting `NOT ANSWERED` — a truncation wearing a
   measurement's clothes. A peer whose answer legitimately needs one more step than the count was a FALSE
   NEGATIVE, and the number 400 was never derived from anything.
   Both real terminators were already here and neither needed a counter: the emitted output arriving, and the
   peer's own statement that its frontier drained (ENGINE_STEP_DONE).
   §@S's rule that only EMITTED OUTPUT proves a flow is done is the same rule.
   THE THIRD TERMINATOR IS THE PEER'S OTHER STATEMENT, AND ITS ABSENCE WAS THE HANG. A frontier can also be
   neither draining nor progressing: every member parked on something only the host can supply
   (ENGINE_STEP_STALLED). That is a positive statement — "I am owed X" — and it was invisible here because the
   ABI folded it into the yield, so a peer this zone deliberately never pays (`/hold`, which is what keeps `b`'s
   boot flow alive to receive) answered "call me again" for ever and this loop believed it. It is not a step
   cap and it truncates nothing: the flows keep their snapshots, the session stays live, and the very next
   payment resumes them — measured, DONE one step after the owed reply was supplied.
   THE STALL IS ONLY A TERMINATOR WHERE THE PAYMENT DID NOT MOVE IT, which is the same test engine_run makes
   (`r == ENGINE_STEP_STALLED && filled == 0`, engine.h: "0 at a stall ends the run, which is the honest answer
   to 'nobody can supply this'"). A stall this zone answered is a frontier with work again, and the loop
   carries on — so what stops the pump is never a round count, it is the two answers about one question
   agreeing that nothing more is coming.
   The three exits are the peer's own statements and this zone's, kept apart: DONE is the frontier drained,
   STALLED is a bill this zone will not pay, EMITTED is the thing the caller was waiting for. A peer that does
   none of the three is a HANG, and a hang is the harness's `RUN_BACKSTOP_MS` to report through its own signal —
   which is exactly where a driver's patience belongs, and the shape the browser-process gate's phase 4 chose
   independently ("termination is an emitted output, not a step cap ... deliberately no third exit"). */
const PUMP_EMITTED = 'emitted', PUMP_DRAINED = 'drained', PUMP_STALLED = 'stalled';
async function pumpUntil(target, done) {
  for (;;) {
    /* THE PREDICATE IS ASKED FIRST AND AGAIN AFTER EVERY ROUND, so a round that BOTH emitted the thing and
       ended the session answers EMITTED. The caller asked about the output, not about the frontier. */
    if (done()) return PUMP_EMITTED;
    const { step, paid } = await service(target);
    if (done()) return PUMP_EMITTED;
    if (step === STEP_DONE) return PUMP_DRAINED;
    if (step === STEP_STALLED && paid === 0) return PUMP_STALLED;
  }
}

/* THE ONE-WAY NOTICES, DRAINED — factored out of the step because the PARK takes its last one outside a step.
   A park announces every world this session ever put on the wire (solver/world.h) and then the instance is torn
   down, so a drain that only ever ran inside `service` would lose exactly the notices that say the dead
   session's segments can go — the leak this seam's counters exist to make visible, lost in the one place it is
   measurable. */
async function drainNotices(e) {
  for (const n of e.str('qjs_host_notices').split('\n').filter(Boolean)) {
    const f = n.split('\t');
    console.log(`  [${e.docId}] notice: ${f[0]} ${f.slice(1, 3).join(' ')}`);
    /* FIELD 5 IS THE CHILD'S TOP-LEVEL CREATION URL, decided by the creator's §7.4 and carried on the
       notice for the same reason fields 6 and 7 — the two halves of §7.1.7's policy container clone — are:
       the new instance cannot derive any of them. The policy is LAST because it is the record's remainder — a
       raw CSP header may contain HTAB — and CSP §2.2's self-origin sits before it because an origin's
       serialization cannot. */
    /* A DOCUMENT THIS ZONE ALREADY HOLDS AN INSTANCE FOR IS NOT PROVISIONED TWICE, and that is not a guard
       against a duplicate notice — it is what a resumed document does. `a`'s replay re-runs its scripts, so it
       re-creates the child navigable and re-announces it under the same name, while the instance that holds
       that name has been running the whole time. Provisioning a second one would give one document two heaps
       and two object graphs, which is the state SECURITY.md's one-instance-per-cluster rule exists to prevent
       and which nothing downstream could tell from the routing. */
    if (f[0] === 'navigable.create') {
      if (holderOf(f[1])) console.log(`  [${e.tag}] create for ${f[1]}, already held — routing to the live instance`);
      /* FIELD 6 IS CSP §2.2's SELF-ORIGIN of the inherited list, FIELDS 7-10 ARE §7.1.4's EMBEDDER POLICY —
         its value, its reporting endpoint, its report-only value and its report-only endpoint — FIELD 11 IS
         HTML §7.3.1.3's PARENT NAVIGABLE, FIELD 12 IS THAT SECTION'S OTHER LINK (what the navigable's
         CONTAINER answered, which is Permissions Policy §9.5's result and which only the creating instance
         could compute, since §9.5's two arguments are that element and the child's origin), FIELD 13 IS HTML
         §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST for the child's Document (a THIRD
         answer over a THIRD algorithm, whose inputs include the parent Document's ORIGIN RECORD — which is
         what a serialization drops, since §7.1.1 decides an opaque origin by IDENTITY), and FIELD 14 IS
         THE LIST. The policy is the record's REMAINDER (a
         raw CSP header may contain HTAB), which is why everything that is not the policy sits before it: an
         origin's serialization cannot contain a tab, nor can §7.1.4's tokens, nor can remote_object.c's
         one-letter tag and '.'-terminated base64, RFC 8941 §3.3.3 "Strings" excludes one from a
         `report-to` endpoint, and §3.1.3's list is origin serializations joined by SPACE, which URL §3.2
         "Host miscellaneous" makes a forbidden host code point exactly as it does TAB.
         THE PARENT IS NOT PART OF THE CONTAINER BESIDE IT and is passed on its own: this fixture's `a` opens a
         CROSS-ORIGIN WINDOW, which §7.3.1.7 step 8 makes an AUXILIARY navigable — a full policy container, NO
         parent and NO container element — so the record carries `u` and `null` here, and a driver that folded
         either of them into the container beside them could not tell that from a frame.
         The child has no response headers of its own in this fixture, so that slot is empty. */
      else engines.push(await makeEngine(HTML_B, f[3], f[1], '', f[5], undefined, f.slice(14).join('\t'),
                                         f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]));
    }
    /* HTML §7.1.3.2's BROWSING CONTEXT GROUP SWAP — `navigable.swap <new doc> <url> <origin>`. The same act as
       a create and a different record: §7.3.2.3 makes the new browsing context "with null, null, and group", a
       NULL CREATOR, so there is no policy container to clone and no top-level creation URL only the creator
       could state (a top-level traversable's is its own address). The instance is NEW rather than joined
       because a swap changes the group, which is half of SECURITY.md's `(browsing context group, origin)` key.
       NOT REACHED BY THIS FIXTURE, and that is stated rather than left to be discovered: `a` opens a
       CROSS-ORIGIN window, which navigable.c hands to the create notice without ever fetching, so no load job
       of `a`'s obtains an opener policy at all. What would drive it is a SAME-ORIGIN `window.open` — the
       navigable is then built in `a`'s own heap and its load job runs here — answered by the `document.fetch`
       branch above with a `cross-origin-opener-policy: same-origin` field line, which `a`'s own `unsafe-none`
       does not match. That is one line in HTML_A and one URL-keyed header in the reply this driver serves. */
    else if (f[0] === 'navigable.swap') {
      if (f.length < 4 || !f[1] || !f[2]) fail(`a navigable.swap notice was short of its fields: ${n}`);
      if (holderOf(f[1])) fail(`§7.1.3.2's swap named a document an instance already holds: ${f[1]}`);
      /* NO CREATOR (§7.3.2.3 makes the new browsing context "with null, null, and group"), so no container to
         clone and no self-origin to state. */
      /* §7.3.1.3's PARENT DEFAULTS TO `u` HERE AND THAT IS THE SPEC: §7.1.3.2 step 2 returns before the
         predicate is evaluated for anything that is not a TOP-LEVEL browsing context, which the engine asserts
         where this record is written, so the navigable a swap provisions has no parent.
         AND §3.1.3's LIST DEFAULTS TO `none` BY THAT SAME SENTENCE READ ONE ALGORITHM ALONG: its steps 2-3
         return an empty output for a Document with no CONTAINER DOCUMENT, and a navigable with no parent has
         none. Both are defaults because both are what the section says, not because nothing was known. */
      engines.push(await makeEngine(HTML_B, f[2], f[1], '', f[2], undefined, '', ''));
    }
    else if (f[0] === 'windowproxy.post') posts.push({ doc: f[1], world: f[2], record: n, origin: e.origin });
    /* A COMPLETION THIS INSTANCE PRODUCED for an operation it was asked to perform, naming the token this zone
       minted. Held rather than delivered here: the asker is another instance and this loop is inside its step. */
    /* THE WORLD IS FIELD 3 AND THE COMPLETION IS THE REMAINDER, and the split is that way round because only
       one of them has a boundary: a world vector's own separators are ':' and ',' (world_serialize), while a
       completion is remote_object.c's grammar and may contain a tab. This zone reads neither. */
    else if (f[0] === 'remoteop.answer') {
      if (f.length < 4 || !f[1] || !f[2] || !f[3])
        fail(`a remoteop.answer notice was short of its fields — every answer names the rendezvous token, the ` +
             `TIMELINE of the flow that ran the program, and the completion: ${n}`);
      const list = answersFor(f[1]);
      /* ONE TIMELINE ANSWERS ONE QUESTION ONCE. Its flow spends the rendezvous token off its row as it answers
         (flow_answer_perform), so a repeat under one token from one world is this instance emitting twice —
         which is indistinguishable from a second timeline without the world, and which would fork the asker an
         arm into a timeline another arm already holds. The engine refuses it at its delivery entry too; this is
         the same statement made where the notices are read, so a duplicate is caught before it is relayed. */
      if (list.some((a) => a.world === f[2]))
        fail(`the peer answered one cross-agent operation TWICE from the SAME timeline — token ${f[1]}, world ` +
             `${f[2]}. A document answers a question once per live flow and spends the token off the row as it ` +
             `does, so a repeat is an emission this instance made twice and the arm forked over it would be a ` +
             `second flow exploring a peer timeline that is already an arm`);
      list.push({ world: f[2], completion: f.slice(3).join('\t') });
    }
    /* AN OPERATION HANDED BACK, because the instance holding it PARKED while still holding it. The record and
       the token do NOT have one lifetime and that is the whole reason this arm exists: the record is text
       whose names are global (documents, world vectors), while the TOKEN is a name in THIS zone's namespace
       and dies with this zone's session. So a parked engine cannot carry the token into its recipe and answer
       it later — a resumed engine would emit a completion under a name no zone ever minted. It hands the
       operation back instead, and the asking flow — still suspended, and still reporting its request every
       step because engine_host_requests deliberately does not dedupe — simply asks again.
       FORGETTING IS THE ACTION. Dropping the read from this map is what lets the next step re-route it; there
       is no store, no cross-session mapping, and nothing to persist. One notice per DISTINCT token, because
       one operation is attached to every live timeline and per-flow notices would repeat one hand-back across
       a frontier of thousands — the sender emits it when the LAST holder of that token leaves.
       AN ANSWER AND A RETRACTION FOR ONE TOKEN ARE NOT A CONTRADICTION, and an assert here used to say they
       were. A peer's document state IS its flows, so one operation is performed by every live timeline and
       `otherW.length` has N answers for N of them (solver/engine.c's engine_host_answer records the extras on
       purpose). A park that lands after one timeline has answered and before the others have is therefore an
       ordinary state: those answers are real, and the timelines that never ran are handed back. What would be
       a defect is a SECOND retraction of one token — that is a hand-back repeated per holder, which is the
       thing the last-holder rule exists to prevent — and that is what is checked instead. */
    else if (f[0] === 'remoteop.retracted') {
      if (f.length < 2 || !f[1]) fail(`a remoteop.retracted notice carried no token: ${n}`);
      const r = reads.get(f[1]);
      if (!r) fail(`a retracted token names no read this zone asked, or one already handed back: ${f[1]}`);
      retracted.push(f[1]);
      reads.delete(f[1]);
    }
    /* A WORLD OF THIS INSTANCE IS GONE — its flow left the frontier, or the whole session parked — so every
       peer holding a COW segment keyed on that name can drop it. BROADCAST, and that is the design rather than
       this driver being lazy: the sending engine deliberately does not record which peers a flow reached,
       because releasing a world with no segment is a no-op and tracking it would be state kept only to avoid
       one. Only the zone knows what the other instances are, which is this record's whole reason to exist. */
    else if (f[0] === 'world.gone') {
      if (f.length < 2 || !f[1]) fail(`a world.gone notice carried no world name: ${n}`);
      worldDeaths.push(f[1]);
      for (const peer of engines) {
        if (peer === e) continue;
        peer.M.ccall('qjs_world_gone', 'void', ['number'], [peer.cs(f[1])]);
      }
    }
    /* `world.parked` IS THE OPPOSITE DIRECTION AND IT IS NOT A DEATH — it says "a segment of yours is in the
       residue I am about to store", so the vector is one this driver must expect to see AGAIN when that
       document resumes. It is recorded and not relayed, and the reason it cannot simply be relayed is the leak
       it exists to make finite: `world.gone` is broadcast to LIVE instances, and a parked document is not one,
       so every death announced while it is cold is lost and the resumed instance would hold a segment for an
       ended world forever. Closing that needs a `world name -> parked document` index that outlives a browser
       restart (the residue does), which is the zone's to own and which `bridge.js` DFAILs on by name. This
       driver holds one process and no persistence, so it records the set and asserts on it rather than
       pretending to be that index. */
    else if (f[0] === 'world.parked') {
      if (f.length < 2 || !f[1]) fail(`a world.parked notice carried no vector: ${n}`);
      worldsParked.push(f[1]);
    }
    else throw new Error('a notice this host does not act on: ' + f[0]);
  }
}

/* THE ROOT DOCUMENT IS ITS OWN TOP-LEVEL TRAVERSABLE, so its environment's top-level creation URL is its
   own address. */
engines.push(await makeEngine(HTML_A, 'https://a.test/', 'd1', '', 'https://a.test/', undefined, '', ''));

/* PHASE 1 — run `a` out, collecting its posts. Nothing is routed yet, so `b` holds no record and each one below
   can be delivered on its own; two records on one flow is a merge of possibly-contradictory senders, which the
   engine crashes on rather than performing.
   "RUN OUT" IS THE ENGINE'S OWN STATEMENT AND NOT A COUNT. This line was `for (let i = 0; i < 2000; i++)`,
   which is the step cap pumpUntil's own comment says §NO BOUNDS forbids, surviving in the phases because the
   pump had no third exit to offer them. It has one now: there is no output to wait for here, so the only
   terminators are `a`'s two — its frontier drained, or it stalled on the `/resume` this zone deliberately
   withholds until phase 3. */
const phase1 = await pumpUntil(engines[0], () => false);
if (phase1 === PUMP_DRAINED)
  fail("`a`'s frontier reported DONE in phase 1 while every one of its arms is parked on the `/resume` this " +
       'zone has not answered — a frontier with a flow suspended on an owed reply is STALLED (engine.h), so ' +
       'DONE here is those flows dropped with their continuations and phase 3 has nothing to resume');
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
    /* THE STALL IS WHAT ENDS THIS WAIT WHEN THE RECORD DOES NOT ARRIVE, and it names the difference from a
       drain: `b` is owed a `/hold` this zone never answers (that is what keeps its boot flow alive to receive
       at all), so a `b` with no runnable member says STALLED for ever and never DONE. Without the third exit
       this line was the hang — the whole drive stopped at the FIFTH post on every build. */
    const why = await pumpUntil(target, () => got.length !== before);
    if (why !== PUMP_EMITTED) console.log(`  NOT DELIVERED (${why}): ${p.world}`);
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
/* THE READ BEING ASKED IS THE EMITTED OUTPUT THIS PHASE WAITS FOR — the other two exits are `a` saying it
   cannot produce it, and each of them is a DIFFERENT defect, which is why they are reported apart. This loop
   also carried `i < 2000`, and the count was doing the third exit's job badly: it turned "the engine says it
   is blocked" into "I got bored", with no way to tell either from "the read was asked on step 1999". */
const phase3 = await pumpUntil(engines[0], () => reads.size !== preParkReads);
if (phase3 === PUMP_DRAINED)
  fail("`a`'s frontier reported DONE while one of its flows was suspended at a cross-instance read — a blocked " +
       'frontier is STALLED (engine.h), which is a bill, and DONE over a suspended flow is that flow dropped');
if (phase3 === PUMP_STALLED)
  fail("`a` STALLED before asking the withheld read, owed something this zone did not supply — the /resume " +
       'reply was answered above, so whatever the frontier is parked on now is a register entry this driver ' +
       `does not fill: ${engines[0].str('qjs_pending').split('\n').filter(Boolean).join(' ; ') || '(no fetch)'}` +
       ` / ${engines[0].str('qjs_host_requests').split('\n').filter(Boolean).join(' ; ') || '(no request)'}`);
if (reads.size === preParkReads)
  fail('the withheld read was never asked, so phase 4 has nothing to park a flow ON — either the /resume reply ' +
       'did not resume the flow that awaited it, or `w.closed` was answered without reaching the peer');

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
const peerTurn = await pumpUntil(engines[1], () => answers.size > 0);
if (peerTurn !== PUMP_EMITTED)
  fail(`the peer ${peerTurn === PUMP_DRAINED ? 'drained its frontier' : 'STALLED on work this zone will not ' +
       'supply'} without producing a completion for the withheld read — the record was handed to it ` +
       '(qjs_perform, above), so the program its answer is either never became a flow or never reached its ' +
       'end, and phase 4 would then park a flow whose orphan nothing produced');

const parked = engines[0];
const postsAtPark = posts.length;
parked.M.ccall('qjs_request_park', 'void', [], []);
const parkStep = parked.M.ccall('qjs_step', 'number', [], []);
const residue = JSON.parse(parked.str('qjs_result'))._park.join(';');
console.log(`\nphase 4: park step answered ${parkStep}, residue ${residue.length} bytes: ${residue}`);
/* THE PARK'S OWN NOTICES, TAKEN BEFORE THE INSTANCE GOES. Every world this session ever sent is unusable from
   the park onward — the resumed session mints in a disjoint generation — and `b`, which never left memory,
   holds a segment for each of them. This is the last moment anything can say so. */
const deathsAtPark = worldDeaths.length;
await drainNotices(parked);
console.log(`phase 4: the park announced ${worldDeaths.length - deathsAtPark} world death(s) to the peer`);
parked.M.ccall('qjs_teardown', 'void', [], []);
/* THE ORPHANS ARE COUNTED BEFORE THE RESUMED INSTANCE CAN ADD ANY. A completion held here names a token of the
   instance that has just gone: `b` computed it under the asking flow's world, and there is no longer a flow,
   a register or an instance for it to land in. */
const orphanCompletions = [...answers.keys()];
withholdReads = false;
engines[0] = await makeEngine(HTML_A, 'https://a.test/', 'd1', '', 'https://a.test/', residue, '', '');
console.log(`phase 4: resumed as [${engines[0].tag}] from the residue; [${engines[1].tag}] never left memory`);
/* THE RESUMED SESSION, PUMPED WITH ITS OWN ROUTING INTERLEAVED — which is why this is not a `pumpUntil`: the
   posts the replayed flows emit have to be routed to `b` between rounds, and pumpUntil steps one engine. The
   three exits are the same three, spelled out because each is a different thing to say about the resume. */
for (;;) {
  if (closedReports.length) break;
  const { step, paid } = await service(engines[0]);
  if (closedReports.length) break;
  if (step === STEP_DONE)
    fail('the RESUMED instance drained its frontier without the replayed flow ever reading `w.closed` back — ' +
         'a recipe is a replay (solver/cold.h), so a session that finishes without re-issuing the read did ' +
         'not replay to the point the parked flow was suspended at');
  if (step === STEP_STALLED && paid === 0)
    fail('the RESUMED instance STALLED owed something this zone did not supply, before `w.closed` came back: ' +
         `${engines[0].str('qjs_pending').split('\n').filter(Boolean).join(' ; ') || '(no fetch)'} / ` +
         `${engines[0].str('qjs_host_requests').split('\n').filter(Boolean).join(' ; ') || '(no request)'} — ` +
         'the read is answered by relaying the peer\'s completion, so a stall here is that relay not happening');
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
  console.log(`  [${r.asker}] ${r.withheld ? 'WITHHELD-AND-PARKED' : r.answered ? 'ANSWERED' : 'UNANSWERED'}` +
              `${r.by.length ? ` by ${r.by.length} timeline(s)` : ''} ${r.op}`);
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
/* AND WHAT THE RECEIVING HALF DID WITH THE RECORDS THIS ZONE ROUTED — the two numbers the check below is
   about, read off the instances rather than counted here, because only an engine knows how many of its
   TIMELINES a record was offered to and how many of them it belonged to. */
let delivered = 0, refused = 0;
/* …AND WHAT BECAME OF THE TASKS THOSE DELIVERIES QUEUED, which is the half this file used to INFER and could
   not. §9.3.3 step 8's task has four ends and only one of them runs a listener; solver/engine.h declares them,
   core/frame/window_message.c reports the end a task RUNS to, and solver/flow.c reports the one §7.5.10 step
   7's removal walk reaches before it can run. */
const ENDS = ['_routedTasksFired', '_routedTasksTargetOrigin', '_routedTasksTargetGone', '_routedTasksThrew'];
const ends = { _routedTasksFired: 0, _routedTasksTargetOrigin: 0, _routedTasksTargetGone: 0, _routedTasksThrew: 0 };
for (const e of engines) {
  const r = JSON.parse(e.str('qjs_result'));
  forked += r._worldSegmentsForked;
  /* NEITHER IS DEFAULTED. A `|| 0` here would turn "this build stopped emitting the pair" into the number a
     healthy run also produces, and the check below would then be satisfied by an engine that said nothing. */
  if (typeof r._routedDelivered !== 'number' || typeof r._routedRefused !== 'number')
    fail(`[${e.tag}] the result document carries no routed-delivery census — solver/result.c emits the pair ` +
         'and solver/engine.c counts it at the two lines that ARE the two outcomes, so an absent field is a ' +
         'producer that moved under this reader and every delivery assertion below would be reading a hole');
  /* AND NOR IS ANY OF THE FOUR, for the same reason and one worse: a missing end reads as ZERO, and the check
     below is that the four SUM to at least the deliveries — so a producer that stopped emitting one of them
     would fail this driver with a message about a lost message. */
  for (const k of ENDS) {
    if (typeof r[k] !== 'number')
      fail(`[${e.tag}] the result document carries no \`${k}\` — solver/result.c emits all four ends of ` +
           '§9.3.3 step 8\'s task together (solver/engine.h says why one number could not say which), so an ' +
           'absent one is a producer that moved under this reader');
    ends[k] += r[k];
  }
  delivered += r._routedDelivered;
  refused += r._routedRefused;
  console.log(`[${e.tag}] worldSegments held=${r._worldSegmentsHeld} made=${r._worldSegmentsMade} ` +
              `forkedFromAncestor=${r._worldSegmentsForked} flows=${r._flows} switches=${r._switches} ` +
              `routedDelivered=${r._routedDelivered} routedRefused=${r._routedRefused}`);
  console.log(`[${e.tag}] §9.3.3 step 8 task ends: fired=${r._routedTasksFired} ` +
              `declinedByTargetOrigin=${r._routedTasksTargetOrigin} targetDestroyed=${r._routedTasksTargetGone} ` +
              `threw=${r._routedTasksThrew}`);
}

/* WHAT MAKES THIS A SMOKE TEST RATHER THAN A PRINTOUT. Four things have to have happened, and each of them is
   a whole mechanism failing silently if it did not: a second instance was provisioned (the create notice was
   acted on), every routed record was admitted by at least one TIMELINE of the receiving document and every
   task those deliveries became reached one of §9.3.3 step 8's four ends (the inbound half — see the checks
   below for why that is three statements, why the equality it replaced was a claim about the SCHEDULER, and
   why the count it was made from could never have been a count of handler invocations), at least one
   segment was materialized by FORKING an ancestor (the world vector's ancestry was READ and used), and the one
   synchronous cross-instance READ this engine has was asked AND answered by the instance that holds the
   document — which is the half that has never run. */
if (engines.length < 2) fail('no second instance was provisioned — the navigable.create notice went unanswered');
if (!posts.length) fail('the sender emitted no cross-instance post');
/* A ROUTED RECORD IS NOT A HANDLER INVOCATION, AND THIS LINE ASSERTED THAT IT WAS.
 *
 * `got.length !== posts.length` compared the number of records THIS ZONE routed against the number of times the
 * receiving page's `message` handler ran, and the engine has never promised those are equal. solver/engine.c's
 * engine_route attaches every arriving record to EVERY live flow of the receiving document and says why in the
 * same breath — "THE MESSAGE ARRIVES IN EVERY TIMELINE OF THE RECEIVING DOCUMENT, and that is what makes it
 * arrive at all", because the page's listener was registered by a script and therefore lives in the COW delta
 * of the flow that ran it; a delivery seeded from the baseline arrives at a document where it was never
 * registered. So N timelines that admit a record produce N invocations, and the equality above is the claim
 * that the receiver has exactly ONE timeline.
 *
 * IT PASSED ONLY WHILE THE RECEIVER'S OTHER TIMELINES WERE STARVED, and that is measurable rather than
 * inferred: across the two builds where this went from OK to FAILED, the ONLY other difference in this whole
 * drive was the RECEIVING instance's switch count (97 against 108) — the sending instance's line was identical
 * to the digit, every read was asked and answered by the same timelines in the same order, and the two extra
 * invocations were two more of the deliveries the engine had already attached finally getting the thread. An
 * assertion whose truth is a function of how many turns the scheduler happened to give one instance is the
 * schedule-dependent answer §Testing's differential exists to catch, and solver/engine.c already names this
 * exact defect one layer down, about the superseded pairwise queue check: "a router that hands over one record
 * and pumps before handing over the next" got one answer and a router that handed over both first got another.
 * This file re-introduced it in its own assertion.
 *
 * WHAT IS ACTUALLY INVARIANT is stated instead, and it is two separate facts that fail for different reasons.
 *   - NO RECORD IS LOST: every routed record is admitted by at least one timeline, so the engine's own count of
 *     deliveries is at least the number of records this zone handed over. A zero-delivery record is a peer's
 *     message no timeline of this document ever receives, which the page cannot distinguish from one that was
 *     never sent.
 *   - EVERY DELIVERY REACHED AN END: the four ends of §9.3.3 step 8's task sum to at least the deliveries. A
 *     sum BELOW the deliveries is the one outcome that is a defect — a task that was queued and never ran, a
 *     work item the ONE frontier dropped — and it is the only one of the four the page cannot tell from a
 *     message that was never sent.
 * `refused` is printed with them because a low delivery count has two readings — the other timelines DECLINED
 * the record (they are on the other side of a sender branch) or they were never offered it — and those take
 * opposite actions.
 *
 * AND THE SECOND HALF USED TO BE MEASURED WITH `got.length`, WHICH CANNOT COUNT WHAT IT WAS COUNTING.
 *
 * `got.length !== delivered` read "the receiving page's listener ran N times" off the number of DISTINCT
 * `/got` lines this zone was shown in `qjs_pending`, and that register is a SET OF REQUESTS: solver/engine.c's
 * engine_pending_fetches dedups over the (method, URL) pair and says why in its own comment — several flows
 * park on one request and `engine_provide` fills every entry naming it, so listing it twice would make this
 * zone provide twice and the second call would find nothing left. N timelines of one document run the SAME
 * listener and therefore issue BYTE-IDENTICAL requests (here they are identical to the character, because the
 * data of a cross-origin message is concolic and every field renders as its shape), so the count collapses by
 * construction. It is the §Testing defect of measuring what a harness prints rather than what the shipped path
 * writes, and it was reported as four §9.3.3 tasks the scheduler had lost: measured at d13ab52b, 9 deliveries
 * against 5 `/got` lines, with `_jobsQueued` and `_jobsRun` both 14 and the receiving instance answering
 * STALLED with an empty frontier — every task had run. It also moved between runs of ONE build (5, 5, 7) with
 * `delivered` fixed at 9, because how many identical requests are outstanding SIMULTANEOUSLY is a property of
 * the schedule, which is the same schedule-dependence this file's own comment above says it had removed.
 *
 * The statement it was reaching for — one handler invocation per delivery, no more — lives where it can
 * actually be made: solver/engine.c asserts §9.3.3 step 8's ONE global task at the enqueue, and
 * core/frame/window_message.c records the task's end once per task. `got` is kept for what it does witness,
 * which is not a count at all: that the receiving page's own code RAN and issued the request its listener was
 * written to issue. */
if (delivered < posts.length)
  fail(`${posts.length} record(s) were routed and the receiving instances delivered ${delivered} — a record ` +
       'that no timeline of the target document admits is a peer\'s message the page never receives and ' +
       `cannot know it did not (refused as not-this-timeline: ${refused})`);
const endsTotal = ENDS.reduce((n, k) => n + ends[k], 0);
/* `<` AND NOT `!==`, AND THE SLACK IS A MECHANISM RATHER THAN TOLERANCE: a fork gives the arm its own Array
   naming the parent's job RECORDS (solver/flow.c's flow_job_fork), so a timeline that branches between the
   enqueue and the run delivers the message once in EACH arm — two timelines, two ends, one queued task. Only
   the deficit is a defect, and solver/engine.c asserts the same inequality at the frontier's DONE; this is that
   assertion read from the outside, where a build with DCHECKs compiled out is the only place it can be seen. */
if (endsTotal < delivered)
  fail(`${delivered} delivery(ies) became a §9.3.3 step 8 task and only ${endsTotal} of those tasks ever ` +
       `reached an end (fired ${ends._routedTasksFired}, declined by targetOrigin ` +
       `${ends._routedTasksTargetOrigin}, target destroyed ${ends._routedTasksTargetGone}, threw ` +
       `${ends._routedTasksThrew}) — the missing ${delivered - endsTotal} were queued on a receiving ` +
       'timeline and never run, which is a peer\'s message dropped by the scheduler rather than by any of the ' +
       'spec\'s own ways of delivering nothing, and is the one of the four the page cannot tell from a ' +
       `message that was never sent (records routed: ${posts.length}, refused as not-this-timeline: ${refused})`);
/* AND AT LEAST ONE OF THEM REACHED A PAGE. This is deliberately NOT `fired >= posts.length`, and the reason is
   a real ordering rather than caution: `b`'s own listener calls `window.close()`, §7.2.2.1 step 6.2 queues the
   definitely-close, and a LATER record delivered into a timeline that has already run it is removed by §7.5.10
   step 7 before its task can fire — a correct end, and one whose count is a function of which of `b`'s
   timelines got the thread first. An assertion over it would be the schedule-dependent claim this file already
   made once. Zero, on the other hand, is not schedule-dependent at all: it is the whole inbound half never
   having happened. */
if (!ends._routedTasksFired)
  fail(`${posts.length} record(s) were routed, ${delivered} became a task, and NOT ONE of those tasks fired an ` +
       'event at the receiving Window — §9.3.3 step 8.7 is the only end at which a page learns anything, so ' +
       'this is the inbound half of the seam not existing. The ends they did reach: declined by targetOrigin ' +
       `${ends._routedTasksTargetOrigin}, target destroyed ${ends._routedTasksTargetGone}, threw ` +
       `${ends._routedTasksThrew}`);
if (!got.length)
  fail('the receiving page never issued the request its `message` listener is written to issue — every ' +
       'delivery reached an end and none of them ran the page\'s own code, so the event fired at a Window ' +
       'whose listener is not the one the document registered');
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
if (unheldReads.size)
  fail(`${unheldReads.size} cross-agent read(s) named a document no instance held for the WHOLE drive, so ` +
       `their askers are parked on a question nothing will ever answer: ${[...unheldReads.values()].join(' ; ')}`);
/* AND THE LAST ONE IS THE ONE A WRONG ANSWER PASSES. Every check above fails by a NUMBER staying zero — a
   record that did not cross, a segment that was not forked, a completion that never came — and none of them
   would have moved if `w.closed` had been answered out of `a`'s own byte, because a local answer is instant and
   plausible. So this check is on the VALUE: `b` closed itself, so §7.2.2.1's `closed` is true about that
   traversable, and the only record in existence that says so is `b`'s. `typeof` rides along for the reason it
   rides on the delivery — an answer that arrived as text and stayed text satisfies `v ? ...` and is not a
   boolean. */
/* HOW MANY OF THEM THERE ARE IS NOT A NUMBER THIS DRIVER MAY PIN, and pinning it was a confident false red.
   This line demanded EXACTLY ONE, on the reasoning that "this driver arranged exactly one … a two is a flow
   that came back beside one that had never left" — and there is no flow that never left (the parked session was
   torn down, `qjs_teardown`), while there are at least two that came back: HTML_A branches on
   `__FLAGS.admin`, which is server-injected absent state, so BOTH arms are real timelines of `a` and both run
   the `.then` that reads the member. The expression reads it TWICE more (`typeof w.closed` and `w.closed` are
   two [[Get]]s), and each read is answered once per timeline the PEER holds, over each of which the asking flow
   forks an arm — so the count is a property of how much both documents forked, which is exactly the kind of
   number §Testing says a gate may not store an expected copy of. What is checkable is the VALUE and the
   PRESENCE, and both of those are what a wrong answer gets wrong. */
if (!closedReports.length)
  fail('the asking page never read `w.closed` back — every report here is produced by the RESUMED instance, ' +
       'since the answer was withheld from the session that parked, so a zero is a flow that did not come back ' +
       'from the cold tier');
/* THE TYPE, ON EVERY ONE OF THEM. `typeof` rides the report for the reason it rides the delivery — an answer
   that arrived as text and stayed text satisfies `v ? …` and is not a boolean, and §7.2.2 The Window object
   declares `closed` as one. */
const notBool = closedReports.filter((u) => !u.includes('v=boolean:'));
if (notBool.length)
  fail(`${notBool.length} of ${closedReports.length} reads of \`w.closed\` came back under a type §7.2.2 The ` +
       `Window object does not declare for it — it is a boolean: ${notBool.join(' ; ')}`);
/* AND THE ONE A WRONG ANSWER PASSES. Every check above fails by a NUMBER staying zero — a record that did not
   cross, a segment that was not forked, a completion that never came — and none of them would have moved if
   `w.closed` had been answered out of `a`'s own byte, because a local answer is instant and plausible. So this
   check is on the VALUE: `b` ran `window.close()` in the timeline that received the third message, and the only
   record in existence that says so is `b`'s.
   AT LEAST ONE AND NOT ALL, because both answers are true of the document they were computed in. HTML §7.2.2.1
   "Opening and closing windows" gives the getter as "return true if this's browsing context is null or its is
   closing is true; otherwise false", and the same section's close() steps set "thisTraversable's is closing to
   true" — in the agent that RUNS close(), which is `b`, and in the timelines of `b` that reached that line.
   `b`'s other timelines never received the message that runs it and answer false as truly. A run in which
   NONE of them says true is the defect this fixture exists for: `a` holds a WindowProxy for the same traversable
   and its own copy of that record is never written, so `w.closed` was false for ever about a window that had
   closed itself — a plausible value instead of a missing one, which is the only kind of cross-instance defect
   that no counter can see. */
if (!closedReports.some((u) => u.includes('v=boolean:true')))
  fail(`no read of \`w.closed\` came back true, across ${closedReports.length}: ${closedReports.join(' ; ')}. ` +
       'The OTHER instance ran window.close(), and HTML §7.2.2.1 "Opening and closing windows" makes the ' +
       "getter's answer the OR of a null browsing context and the top-level traversable's `is closing`, which " +
       "close() sets in the agent that RUNS it — so an answer read out of this agent's own copy of that record " +
       'is false about a window that has closed itself');

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
   wrongly resolvable. The LEAK that used to remain owed beside it is the record above: the park announces every
   world this session sent, `b` releases the segment it held for each, and the counters below are where that is
   visible — `held` falling below `made` is world_release running, and it could never happen before. */
if (reusedWorlds.length)
  fail(`the resumed session put ${reusedWorlds.length} world name(s) on the wire that the session that ended ` +
       `had already sent: ${reusedWorlds.join(' ')}. A WorldId's serial counts from 1 in every session while ` +
       'the document name is stable by requirement, so the peer — which never left memory — already holds a ' +
       'segment keyed on each of them, and answers the resumed flow under the timeline of a flow that no ' +
       'longer exists. Two timelines wearing one name, across sessions. Build the session component of a world ' +
       'name — the residue is what crosses the tier and can carry it');
/* A SEAM THAT MATERIALIZED AND NEVER RELEASED IS THE STATE THIS RECORD WAS BUILT FOR, so a run in which no
   world ever died is a run in which the reclamation did not happen — which reads identically to the tree before
   it existed, and is exactly the shape "a mechanism nobody can see run is one that has never run" names. */
if (!worldDeaths.length)
  fail('no world death crossed this seam. `a` ran its flows out and then PARKED its whole frontier, so both ' +
       'announcements were due (a flow leaving the frontier, and a session whose generation is now dead) — ' +
       'and with neither, every segment `b` materialized is a foreign flow\'s state it holds for the rest of ' +
       'its process, and `b` can never park while it does (solver/cold.c refuses it)');
console.log(`world deaths announced: ${worldDeaths.length} — ${worldDeaths.join(' ')}`);
console.log(`[route] OK — two instances, ${posts.length} record(s) routed into ${delivered} delivery(ies) ` +
            `across the receiver's timelines (${refused} refused as not-this-timeline) with one listener run ` +
            'each, ancestry-forked segments: ' + forked +
            `, cross-agent reads answered: ${readsAnswered}, w.closed read back: ${closedReports[0]}` +
            `, parked and resumed across ${residue.length} bytes of residue`);

/* ── VARIANT A: PARK THE ANSWERING INSTANCE, AS A PROBE AND NOT A GATE ──────────────────────────────────────
 * `node engine/route.mjs park-probe`. Opt-in, and deliberately NOT part of the drive above, for the reason the
 * capability it probes is missing: it is EXPECTED to abort today, and a gate that requires an abort
 * institutionalises the gap and breaks on the day someone closes it. It also cannot share a process with the
 * drive — a DCHECK is a `abort()`, so an in-line probe would take the green run down with it.
 *
 * WHAT IT PARKS, AND WHY THAT IS THE WHOLE POINT. Every park this drive performs is of the ASKER (`a`, phase 4),
 * and `flow_owes_answer` is false there. It is two disjuncts and they are DIFFERENT unbuilt capabilities:
 *   (1) `perform_q` non-empty  — a record arrived and the flow has not stepped; parking drops pure TEXT.
 *   (2) `dyn_token[i]` non-NULL — the operation STARTED; parking drops a record PLUS a half-run program and its
 *       continuation.
 * This is (1): route a record to `b` and park `b` WITHOUT pumping it, so nothing has converted the record into a
 * program row. (2) needs one `qjs_step` between the two and is a separate phase, because they fire one assert and
 * name different work — one passing says nothing about the other.
 *
 * (2) ALSO HAS A PREREQUISITE THAT IS NOT A CODING PROBLEM, recorded here so the next reader does not build the
 * recipe half first and produce an answer nobody can route. `cold.c` says "the record and the token are text and
 * cross as text"; the record half is true and the TOKEN half is not. The token is the trusted zone's
 * (`engine.h`: not the asking flow's request id, which is unique only within the asking instance), and
 * `g_host_answers_late` exists because the zone REFUSES an answer arriving after a session closed. So: does a
 * token outlive the answering instance's park? If YES, `b` resumes, re-queues the program and answers that same
 * token, and the zone must hold the mapping across `b`'s park. If NO, `a`'s flow must ALSO be paged and re-issue
 * under a NEW token next session — which is what the reply door already does. Different systems; answer it
 * before building either.
 *
 * THE PERMANENT SHAPE, once the capability exists: park `b` mid-operation, resume it, and assert `a`'s flow comes
 * back with the VALUE — on the value like `w.closed` above and for the same reason, because a counter passes when
 * the answer was fabricated locally.
 *
 * MEASURED, AND IT DOES NOT REACH `flow_owes_answer` AT ALL — there is a PRIOR gap and this probe is how it was
 * found. Run against a green drive, variant A aborts at `solver/cold.c:574` on `held == 0`:
 *     "the frontier was parked while this instance holds a segment of a FOREIGN world … held=2
 *      materialized-ever=5 … Build the cross-instance park: a foreign segment travels with the WORLD's name
 *      (solver/world.h), not with this document's flows"
 * `b` cannot park while it holds `a`'s world segments, and it holds two by the time anything has been asked of
 * it — so the answering instance is unparkable for a reason that has nothing to do with owing an answer. That
 * orders the work: the cross-instance park (foreign segments travelling by world name, re-routed by the
 * offscreen) comes BEFORE either disjunct of `flow_owes_answer` is reachable, and before the token-lifetime
 * question below can be asked of a real run. A probe that reported only "it aborted" would have sent the next
 * reader to build the recipe half of a capability nothing can reach yet.
 *
 * THAT PRIOR GAP IS NOW BUILT, so the paragraph above is history rather than the current state: a foreign
 * segment crosses the tier as the VECTOR it was materialized from (`cold.h`'s `w` record), the `held == 0`
 * refusal is deleted, and the park emits `world.parked` per vector — which is why `drainNotices` above has an
 * arm for it. This probe should therefore now reach what it was written for: `cold_park_flow`'s
 * `DCHECK(!flow_owes_answer(f))`, disjunct (1), the record sitting unstepped in `perform_q`. If it still stops
 * at `cold.c`, the cross-instance park did not take and THAT is the finding — do not read a moved abort as a
 * fixed one without reading which line it names. */
if (process.argv[2] === 'park-probe') {
  const peer = engines[1];
  const sample = [...reads.values()].find((r) => holderOf(r.op.split('\t')[1]) === peer);
  if (!sample) fail('park-probe: no read in this drive was held by the peer, so there is no record to park it on');
  console.log(`\npark-probe (variant A): routing a record to [${peer.tag}] and parking it WITHOUT a step`);
  console.log(`  record: ${sample.op}`);
  peer.M.ccall('qjs_perform', 'void', ['number','number'], [peer.cs('park-probe'), peer.cs(sample.op)]);
  /* NOT PUMPED. The record is in `perform_q` and no flow has stepped it, which is disjunct (1) exactly. */
  peer.M.ccall('qjs_request_park', 'void', [], []);
  const st = peer.M.ccall('qjs_step', 'number', [], []);
  const res = JSON.parse(peer.str('qjs_result'))._park.join(';');
  console.log(`park-probe: the peer parked holding an unstepped record — step ${st}, residue ${res.length} bytes`);
  console.log('park-probe: NO ABORT. cold.c now carries the record across a park, or the DCHECK it was ' +
              'written for is no longer reached by this shape — read solver/cold.c before believing the first.');
}
