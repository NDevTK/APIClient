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

   AND IT ASKS THE READ, which is the half the ABI has no entry for and which nothing had ever asked. HTML
   §7.2.1.3.1 CrossOriginProperties ( O )'s cross-origin allowlist is a fixed THIRTEEN, and of them TWO cannot
   be answered out of the asking agent's own record: `length` is the child-navigable count of the peer's ACTIVE
   DOCUMENT, and `closed` is a fact only the agent that ran §7.2.2.1 opening and closing windows' close() holds
   — a local `true` is terminal and answered here, but a local `false` says nothing about the peer, which is
   why that one crosses too. (window_proxy.c answers the other eleven in the asking turn.) Those two ARE this
   engine's synchronous cross-instance read surface, and until this fixture wrote them the request record
   `windowproxy.get` had never been emitted by the production entry at all — the sender's half was written,
   reviewed, and unexercised in exactly the way the world registry was.

   IT IS PLACED LAST AND ITS RESULT IS POSTED, so the read is a load-bearing part of the measurement rather
   than a statement whose value is discarded: the fourth and fifth records cannot be emitted until the read is
   ANSWERED, and the `/got` they produce carries `typeof e.data.n` — which is what distinguishes the number 0
   from the string "0", the sentence SECURITY.md states about this seam.

   AND THE PEER NOW ANSWERS IT, which is what this zone does with a `windowproxy.get` below: it hands the
   record to the instance that HOLDS the document (`qjs_perform`), which answers by running a program — an IDL
   getter, as a flow on its own frontier — and relays each COMPLETION, as it is emitted, back to
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
   ordinary case rather than the exotic one. Phase 3 relays one read its first answer and holds the rest, so a
   standby waits at it; phase 4 parks `a`, tears it down, and resumes it from its own residue while `b` stays
   exactly where it was. What that measures is stated as
   assertions at the bottom, and each of them is about a thing nothing else in the tree can observe:
     - the completion the peer computed for the PRE-PARK read has nowhere to land (the asking instance is gone
       and the resumed one never asked that question), so it is an ORPHAN this zone must still be holding;
     - the resumed flow RE-ISSUES the read — which is the cold tier's own claim about the replies a host owes
       (solver/cold.h), and the only thing that makes a park at a cross-instance read lossless;
     - a resumed standby still refuses the peer timelines its family took before the park;
     - and the world name it re-issues under must be one the peer has never seen. A WorldId's serial counts
       from 1 in every session under a document name that is stable BY REQUIREMENT, so the name carries a third
       coordinate: the session's GENERATION, minted into every world and carried across the tier by the residue
       itself (solver/world.h, solver/cold.h's 'g' record). The resumed session therefore mints in a namespace
       disjoint from the one that parked, and `b` materializes new segments instead of answering out of a dead
       flow's. This driver is what makes that decidable — it is the only place both sessions are visible. */
/* THE INTERFACE AND THE ARTIFACT, FROM THE ONE MODULE THAT ANSWERS FOR BOTH. This driver used to load
   `check.js`/`mojo.js`/`mojom.js` into its own isolated context and walk `Init`'s parameters here; the OTHER
   raw-ccall driver (engine/solvergate.mjs) did neither, kept a hand-aligned array of values in declaration
   order, and went short when HTML §3.1.3's ancestor origins statement was added — forty runs of forty aborting
   in `navigable_root_ancestor_origins`. Two drivers deriving the same list two ways is one derivation too
   many, so the load, the walk and the built glue's own declared arity live in engine/renderer_abi.mjs and both
   drivers read them from there. */
import { GLUE_PATH, abiOperands } from './renderer_abi.mjs';
import { NAVIGABLE_NOTICE, navigableNoticeField } from './noticegen.mjs';

const factory = await import(GLUE_PATH);
const boot = factory.default ?? factory;

/* THE READ IS LAST AND BOTH ARMS MAKE IT, so two DIFFERENT worlds ask the same question of one peer — which is
   the case a single-timeline peer cannot answer with one number, and the reason the entry that performs it has
   to install the asking world's segment rather than read a property from C.
   AND ONE READ COMES FIRST, BEFORE THE FORK, WHICH IS WHAT MAKES THE ANCESTRY FORK REACHABLE AT ALL. A read
   and a post in the ROOT world, taken before the branch, put that world's segment at the peer before either
   child world arrives; the two arms then name it as their nearest ancestor and world_segment forks it. The
   fixture has to CREATE the precondition its own `forkedFromAncestor` check is about. */
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
   bytes `null`, so an instance re-running step 12.1 over relayed text would mask an ancestor that is not the
   parent's whenever the parent is opaque. The creating instance runs the steps once and this relays the
   result. `none` is that grammar's word for "there are no ancestors", which is what this driver's own root
   documents state — nothing embeds them — and is the same fact `u` and `null` state one link along. Without
   it the engine installs the EMPTY list, which is the positive claim that the Document is at the top of its
   own tree. */
async function makeEngine(html, url, docId, headers, topLevelUrl, recipes, arrival, inheritedCsp,
                          inheritedCspReportOnly, inheritedCspSelf,
                          inheritedCoep = 'unsafe-none', inheritedCoepEndpoint = '',
                          inheritedCoepReportOnly = 'unsafe-none', inheritedCoepReportOnlyEndpoint = '',
                          inheritedReferrerPolicy = 'strict-origin-when-cross-origin',
                          parentNavigable = 'u', openerNavigable = 'N', containerPolicy = 'null',
                          ancestorOrigins = 'none',
                          creationSandboxFlags = 'none') {
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
  /* KEYED ON THE REQUEST: `qjs_pending` answers `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>DRIVEN<TAB>
     CREDENTIALS<TAB>MODE<TAB>REFERRER_POLICY<TAB>REFERRER<TAB>ASKERS<TAB>URL` lines and the delivery matches the
     five-field identity, so a GET and a POST to one address are two questions with two replies. */
  const provide = (method, mode, referrerPolicy, referrer, u, reply, body) => {
    const [p, n] = bs(body);
    try { M.ccall('qjs_provide', 'void', ['number','number','number','number','number','number','number','number'],
                  [cs(method), cs(mode), cs(referrerPolicy), cs(referrer), cs(u), cs(JSON.stringify(reply)),
                   p, n]); }
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
    /* ONE VALUE PER DECLARED PARAMETER, BY THE MOJOM'S OWN NAME. There is no operand LIST here at all any
       more: `renderer_abi.mjs` walks `content.mojom.Renderer.Init`'s own parameters, refuses a declared
       parameter this record has no value for (a driver OLDER than the interface) and a key the interface
       declares no parameter of (one NEWER than it, or a misspelling), and compares the count it walked off
       against what the BUILT GLUE declares the entry accepts. That third comparison is the one neither list
       can make alone, and it is why this driver's failure used to be emscripten's own "native function
       `qjs_init` called with 15 args but expects 14" — accurate, and naming a number rather than a parameter.
       WHAT IS STILL STATED HERE IS THE ONE FACT A MOJOM DOES NOT CARRY: which C entry the method is. An
       interface declares methods, not symbols in a wasm module, so `Init` → `qjs_init` is written at the call
       exactly as `renderer.html`'s `ABI` table writes it.
       AND `cs` IS THIS DRIVER'S MINT, into the module it just booted — the `bytes` pair `bs` built above is
       passed as the pair, because only this side knows what it wrote into linear memory. */
    const operands = abiOperands('Init', 'qjs_init', {
      document: [hp, hn], url, docId, headers: headers || '', topLevelUrl,
      inheritedCsp: inheritedCsp || '', inheritedCspReportOnly, inheritedCspSelfOrigin: inheritedCspSelf || '',
      inheritedCoep, inheritedCoepEndpoint,
      inheritedCoepReportOnly, inheritedCoepReportOnlyEndpoint, inheritedReferrerPolicy,
      parentNavigable, openerNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags,
    }, cs);

    M.ccall('qjs_init', 'number', operands.map(() => 'number'), operands);
    /* AND THE DOCUMENT'S BLOCK IS NOT FREED, because the declaration says the entry KEEPS it: `mojom.js` marks
       that one parameter `retained`, and what it is retained FOR is the load that fills the parser's input
       byte stream, which reads through this pointer rather than copying it. This driver used to free it here —
       the same use-after-free the extension's own placement is shaped to prevent, written where nobody was
       checking, because this is the caller that bypasses mojo. It is the frame's lifetime either way: the
       module is torn down whole. */
  }
  /* THE RESIDUE SEEDS THE FRONTIER INSTEAD OF THE BOOT FLOW (solver/cold.h). It is ';'-joined records, which
     is the language cold_park_recipes both writes and reads; the extension joins the stored ARRAY the same
     way, so this zone stores what that one stores. */
  /* HOW THIS DOCUMENT WAS REACHED (solver/engine.h's engine_set_arrival): the notice's provenance for a peer,
     `observed` for a root this driver names. Stated before the seed, since every timeline's grade starts there. */
  if (!/^(observed|derived|forced),(driven|undriven),(pinned|unpinned)$/.test(arrival))
    fail(`an instance for ${url} was made with the arrival \`${arrival}\`, which is not engine.h's arrival word`);
  M.ccall('qjs_set_arrival', null, ['number'], [cs(arrival)]);
  M.ccall('qjs_begin', 'void', ['number'], [cs(recipes || '')]);
  return { M, cs, str, provide, answer, docId, tag: `${docId}/s${++instanceSerial}`,
           docUrl: url, origin: new URL(url).origin, done: false };
}

const engines = [];
/* EXACT. A child document's NAME is prefixed by its creator's ("<creator>.<k>") but the creator is precisely
   the instance that does NOT hold it — that is why the notice exists. Prefix-matching routed the post straight
   back to the sender, which the engine caught twice over. */
const holderOf = (doc) => engines.find((e) => e.docId === doc) ?? null;

const posts = [];   /* routed records, in emission order, held until their target is free to receive one */
/* HOW MANY RECORDS EACH TARGET DOCUMENT WAS HANDED, because the aggregate cannot answer the question the
   delivery check asks. A record is admitted or lost by ONE receiving document, so a receiver that admits a
   record in several of its timelines contributes several deliveries to a sum in which another receiver's
   losses then disappear. Keyed on the document name and not on an instance, because the instance holding a
   document is replaced across the park while the name is what a record is routed on. Filled explicitly rather
   than with a `??  0`: a histogram this file builds is a producer this file owns, and the one number that must
   not be silently invented here is a count of records nobody routed. */
const routedTo = new Map();
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
/* The tokens whose answering instance said no timeline will answer again (`remoteop.exhausted`), relayed to the
   asker after that token's answers so its waiting standby retires. */
const exhaustedTokens = new Set();
const answersFor = (token) => {
  if (!answers.has(token)) answers.set(token, []);
  return answers.get(token);
};
/* Every `/closed` the ASKING page fetched — `a`'s own report of what `w.closed` answered, collected apart from
   `got` because it is a different measurement: `got` counts what crossed INTO `b`, this counts what came BACK.
   EACH ENTRY CARRIES THE SESSION THAT FETCHED IT, AND THAT IS NOT BOOKKEEPING — it is the whole of a defect
   this list had. Three readers below say "the RESUMED instance" about it and one of them is a LOOP EXIT, so a
   list that spans sessions is a stale read at the point the resumed session has not been stepped once.
   THE ARGUMENT THAT MADE THE TAG UNNECESSARY IS RETIRED AND IS WRITTEN DOWN RATHER THAN DELETED, because it
   re-derives from the fixture in one reading and the next person to re-derive it would take the tag back out.
   It was: `a`'s only `/closed` sits in the `.then` of `/resume`, `/resume` is answered in phase 3, and reads
   were withheld from that point on — so no `/closed` could exist before the park and every entry was the
   resumed session's by construction. Its middle premise is what went: a drive of a function the bundle
   shipped and never called reaches that `.then` body with the reply still owed (solver/engine.c's
   engine_orphan_seed, §What-the-tool-produces' headline surface), so the callback runs in phase 1, reads
   `w.closed` off the peer for real, and puts two entries in this list before phase 3 is reached. Nothing about
   those entries is wrong; they are simply not the session the readers below are about. */
const closedReports = [];
/* …AND THE ONE QUESTION THOSE READERS ASK OF IT. Scoped by the ASKING SESSION's tag and never by `docId`: a
   resumed document is the SAME document and a DIFFERENT session, which is the distinction `tag` exists for
   (see `instanceSerial`) and is exactly the one being drawn here. */
const closedBy = (tag) => closedReports.filter((r) => r.asker === tag);
/* `/resume` IS DEFERRED, WHICH IS HOW THIS DRIVER ORDERS TWO INSTANCES WITHOUT A CLOCK: `a` parks on this
   fetch, `b` is routed its messages and closes, and phase 3 answers it. A `setTimeout` would order nothing —
   both engines advance only when this loop steps them.
   WHAT IT ORDERS IS THE PHASE AND NOT THE CALLBACK, and the stronger claim that stood here is retired. It said
   the read "has to happen AFTER `b` has closed itself, and the only thing that orders one instance's flow
   against another's here is an owed reply" — which read the deferral as a guarantee that `a`'s `.then` body
   cannot run early. It cannot be one: a flow that has run out of the work the page ARRANGED for it drives a
   function the bundle shipped and never called, and this `.then` callback is one until the reply lands, so the
   body runs in phase 1 with `/resume` still owed. That is the mechanism working, and this fixture's ordering
   survives it because what phase 3 actually needs is a read taken while `holdAfterFirst` is set — which the
   answered `/resume` still produces, on the flow that was genuinely awaiting it. An early drive adds a real
   read of a real peer; it does not remove the one phase 4 parks on. */
let resumeOwed = true;
/* …and whether `a` has been seen owing it, the output phase 1 ends on. */
let resumeSeen = false;
/* WHILE THIS IS SET, A CROSS-AGENT READ ASKED IS RELAYED ITS FIRST ANSWER AND NO OTHER, and not its exhaustion.
   The asking flow takes that one timeline's answer and leaves a standby at the read holding the timelines its
   family took (solver/engine.c's flow_answer_standby), which is the state phase 4 parks; the held answers are
   the peer's in-flight turn, orphaned when the asker pages out. */
let holdAfterFirst = false;
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

/* ONE STEP of `e`, then everything the host owes it. Answers the engine's own code. */
async function service(e) {
  const r = e.M.ccall('qjs_step', 'number', [], []);
  /* THE CODE IS CHECKED FOR MEMBERSHIP AND NEVER DEFAULTED. A value outside the three is an ABI that moved
     under a driver still speaking the old one, which is exactly what this file was the casualty of. */
  if (r !== STEP_DONE && r !== STEP_YIELD && r !== STEP_STALLED)
    fail(`qjs_step answered ${r}, which is none of DONE(0)/YIELD(2)/STALLED(3) — the ABI carries three codes ` +
         'and this zone branches on all three, so a fourth is a contract that moved under this driver');
  /* THE NOTICES ARE DRAINED FIRST, BECAUSE ONE OF THEM PROVISIONS THE INSTANCE A REQUEST IN THE SAME ROUND
     NAMES. The ABI orders the three registers one `qjs_step` fills against none of the others, and
     `navigable.create` is what puts a document into `engines`: read after the request register, a
     `windowproxy.get` naming that document would find no holder for a round. */
  await drainNotices(e);
  for (const line of e.str('qjs_pending').split('\n').filter(Boolean)) {
    const t = line.split('\t');
    if (t.length !== 12 || t.some((x, i) => i !== 1 && i !== 8 && x === ''))
      fail('a pending line is not `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>' +
           `DRIVEN<TAB>CREDENTIALS<TAB>MODE<TAB>REFERRER_POLICY<TAB>REFERRER<TAB>ASKERS<TAB>URL\`: ${line} — ` +
           'the empty DESTINATION is Fetch §2.2.5\'s own default and the empty REFERRER POLICY is Referrer ' +
           'Policy §3\'s, and they are the fields that may be empty');
    /* THE DESTINATION IS NOT NAMED HERE, AND THAT IS A STATEMENT. It is the CORB class — whether a reply may
       be ingested as code — and this driver ingests nothing: it mints its own reply out of a literal, so
       there are no fetched bytes for a class to be about. Its vocabulary is asserted where it is WRITTEN
       (the join's `destination_is_type`) and where bytes are actually classified by it; a driver that
       re-checked it here would be a third speller of Fetch §2.2.5's enumeration. The CREDENTIALS MODE beside
       it is skipped for the identical reason and one step further: it says whose session pays, this driver
       has no session, and its vocabulary is asserted at the door that decides from it. THE PINNED MARK is
       skipped on the same ground and it is the sharpest of the three: it exists so a FIRING policy can tell a
       data request whose address rests on a witness this engine chose from one the app's own text spells, and
       this driver fires nothing — it answers every park out of a literal. Its vocabulary is checked below all
       the same, for the reason the other two vocabularies are. */
    /* THE ASKERS are skipped for the pinned mark's reason: they name the documents a firing policy judges the
       request by, and this driver fires nothing. */
    const [method, , initiator, provenance, pinned, driven, , mode, referrerPolicy, referrer, , u] = t;
    /* THIS DRIVER ANSWERS EVERY PARK WHATEVER IT SAYS ABOUT ITSELF — its fixtures' fetches are named `/hold`,
       `/resume`, `/got` and `/closed` and the reply is `{}` served out of a literal, so there is no network
       here for a firing policy to be about. The vocabularies are checked because a driver that read a field it
       never verified is the one that would carry a producer's drift into every assertion below it. */
    if (initiator !== 'parser' && initiator !== 'script')
      fail(`a pending line states the initiator \`${initiator}\`, which is neither token solver/engine.h declares`);
    if (provenance !== 'observed' && provenance !== 'derived' && provenance !== 'forced')
      fail(`a pending line states the provenance \`${provenance}\`, which is none of the three tokens ` +
           'solver/engine.h declares');
    if (pinned !== 'pinned' && pinned !== 'unpinned')
      fail(`a pending line states the pinned mark \`${pinned}\`, which is neither token solver/engine.h ` +
           'declares — a third value would be answered by whichever arm a firing policy happens to have as ' +
           'its else, which is the permissive one');
    if (driven !== 'driven' && driven !== 'undriven')
      fail(`a pending line states the drive mark \`${driven}\`, which is neither token solver/engine.h declares`);
    if (u.includes('/hold')) continue;
    if (u.includes('/resume') && resumeOwed) { if (e === engines[0]) resumeSeen = true; continue; }
    if (u.includes('/got')) { got.push(u); console.log(`  [${e.docId}] DELIVERED: ${u}`); }
    if (u.includes('/closed')) { closedReports.push({ asker: e.tag, url: u });
                                 console.log(`  [${e.tag}] READ BACK: ${u}`); }
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
                    urlList: [new URL(u, e.docUrl).href], computedType: 'application/json', type: 'basic' };
    /* THE BODY TRAVELS AS BYTES, beside that JSON and never inside it: §2.2.5 makes a response's body a BYTE
       SEQUENCE, and the only ways to put one in JSON are to encode it or to DECODE it — and a decode run by
       the zone that FETCHED is exactly what left HTML §8.1.4.2's classic-script decode nothing to decode. */
    e.provide(method, mode, referrerPolicy, referrer, u, reply, '{}');
  }
  /* ONE OP IS ANSWERED, exactly as the offscreen answers exactly one: a `document.fetch` is a network fetch this
     zone can genuinely perform. Every other request is left UNANSWERED — the asking flow stays parked with its
     snapshot intact and its siblings keep running — because a host that answers what it cannot compute is how
     `navigable.create` came to be answered with "not created". An unanswered one keeps being reported, which is
     visible in a way a wrong answer is not. */
  for (const l of e.str('qjs_host_requests').split('\n').filter(Boolean)) {
    /* `id<TAB>asker<TAB>op`; the asker is a firing policy's input and this driver fires nothing. */
    const t1 = l.indexOf('\t'), t2 = t1 > 0 ? l.indexOf('\t', t1 + 1) : -1;
    if (t2 <= t1 + 1)
      fail(`a host request line is not \`id<TAB>asker<TAB>op\`: ${l} — engine_host_requests names the asking ` +
           'document on every record');
    const id = +l.slice(0, t1), op = l.slice(t2 + 1);
    /* A CROSS-AGENT OPERATION IS RECORDED THE FIRST TIME IT IS ASKED and never answered here, because there is
       nothing in the ABI to ask the peer WITH. It is not skipped silently: leaving it out of the accounting is
       how a seam whose read half has never run reports OK. */
    /* By namespace, as the offscreen routes it (extension/bridge.js): `windowproxy.close` reaches the peer
       through this same arm, and this driver's documents close no peer, so every record it sees here is a read. */
    if (op.startsWith('windowproxy.') || op.startsWith('object.')) {
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
      /* AND IT IS SAID OUT LOUD. "Asked, and not routable YET" and "never asked" are two facts and this branch
         reported neither — a silent `continue`, which is the absent-versus-zero defect built into the
         instrument. The phase that ends on such a round prints what the ENGINE emitted, so a reader has no way
         to tell an engine that did nothing from a zone holding an operation it could not place; that is
         precisely how `a emitted 0 posts` was read as a collapse in the engine. One line per DISTINCT key,
         because an unanswered request is re-reported on every step and a line per step would be a step count. */
      if (!holder) {
        if (!unheldReads.has(key))
          console.log(`  [${e.tag}] cross-agent read HELD — no instance holds \`${op.split('\t')[1]}\` yet: ${op}`);
        unheldReads.set(key, op);
        continue;
      }
      unheldReads.delete(key);
      reads.set(key, { asker: e.tag, engine: e, id, op, world: op.split('\t')[2], holdAfterFirst,
                       /* WHICH PEER TIMELINES WERE RELAYED AND WHETHER A FLOW TOOK EACH (relayAnswer). A held
                          answer stays only on `answers`. */
                       by: [], taken: [], exhaustedRelayed: false });
      console.log(`  [${e.tag}] cross-agent read asked: ${op}`);
      /* THE PEER IS ASKED, AND NOTHING IS ANSWERED INSIDE THAT CALL. It answers by RUNNING A PROGRAM — the IDL
         getter HTML §7.2.1.3.4 CrossOriginGetOwnPropertyHelper ( O, P ) reaches the member through, as a flow
         on its own frontier — once per timeline, on later rounds, through its notices (relayAnswer). */
      holder.M.ccall('qjs_perform', 'void', ['number','number'], [holder.cs(key), holder.cs(op)]);
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
    /* AND FETCH §2.2.6 "Responses"' RESPONSE URL, which HTML §7.4.5 "Populating a session history entry"
       determines the loaded Document's ORIGIN over — "set responseOrigin to the result of determining the
       origin given RESPONSE'S URL". This driver serves one literal document to every address and follows no
       redirect, so the response's URL is the ADDRESS THAT WAS ASKED FOR; stating it is a fact about this
       driver's own network rather than a field copied to satisfy a reader, and a host that DOES follow a
       redirect reports a different string in the same slot.
       `document.fetch<TAB><provenance><TAB><driven><TAB><pinned><TAB><destination><TAB><referrer policy><TAB>
       <initiator><TAB><url>` — ADDRESS LAST, past the seventh tab after the verb. This driver reads the fields before it nowhere:
       it serves one literal document out of memory and spends no network. What it must not do is take the
       line's tail for a URL. */
    const fetchArgs = op.slice('document.fetch\t'.length);
    const fetchTab = fetchArgs.indexOf('\t');
    const drivenTab = fetchTab > 0 ? fetchArgs.indexOf('\t', fetchTab + 1) : -1;
    const pinnedTab = drivenTab > 0 ? fetchArgs.indexOf('\t', drivenTab + 1) : -1;
    const destTab = pinnedTab > 0 ? fetchArgs.indexOf('\t', pinnedTab + 1) : -1;
    const policyTab = destTab > 0 ? fetchArgs.indexOf('\t', destTab + 1) : -1;
    const initTab = policyTab > 0 ? fetchArgs.indexOf('\t', policyTab + 1) : -1;
    if (fetchTab <= 0 || drivenTab <= fetchTab + 1 || pinnedTab <= drivenTab + 1 || destTab <= pinnedTab + 1 ||
        policyTab <= destTab || initTab <= policyTab + 1 || initTab >= fetchArgs.length - 1)
      fail('a document.fetch request is not ' +
           `\`document.fetch<TAB><provenance><TAB><driven><TAB><pinned><TAB><destination><TAB><referrer policy>` +
           `<TAB><initiator><TAB><url>\`: ${op} — core/frame/navigable.c writes all eight fields on every ` +
           'path, the policy possibly empty');
    e.answer(id, { url: fetchArgs.slice(initTab + 1), headers: "" }, HTML_B);
  }
  return { step: r };
}

/* THE DRIVE ADVANCES IN ROUNDS, AND EACH PHASE ENDS ON THE OUTPUT IT IS WAITING FOR. A round steps every live
   instance once, routes every post emitted so far, and yields to the event loop. Only emitted output proves a
   phase done: no step count, and no wait for an instance to drain or stall, which is a bound in disguise here —
   `b` owes a `/hold` this zone never answers and forks on what it is sent, so it reaches neither. A phase whose
   output never comes is a hang, for the harness backstop to report through its own signal. */
async function round() {
  for (const e of engines) {
    if (e.gone || e.done) continue;
    const { step } = await service(e);
    if (step === STEP_DONE) e.done = true;
  }
  routePending();
  await new Promise((r) => setImmediate(r));
}
async function runUntil(done) { while (!done()) await round(); }

/* A PEER'S COMPLETION, RELAYED VERBATIM TO THE ASKING INSTANCE IN THE ROUND IT IS EMITTED. The asking engine takes
   the first onto the register and forks an arm per further timeline (engine_host_answer), and `taken` is its own
   statement of whether a flow took each one. An answer to a read asked under `holdAfterFirst` past its first,
   and any answer whose asker was parked, is held: the second is an orphan. */
function relayAnswer(token, a) {
  const r = reads.get(token);
  if (!r || r.engine.gone || (r.holdAfterFirst && r.by.length)) return;
  r.taken.push(r.engine.M.ccall('qjs_host_answer_remote', 'number', ['number','number','number','number'],
                                [r.id, r.engine.cs(a.world), r.engine.cs(a.marks), r.engine.cs(a.completion)]));
  r.by.push(a.world);
  a.relayed = true;
}
/* The peer's word that no timeline will answer again, relayed after the answers it follows and held with them. */
function relayExhausted(token) {
  const r = reads.get(token);
  if (!r || r.engine.gone || r.holdAfterFirst || r.exhaustedRelayed) return;
  r.engine.M.ccall('qjs_host_answers_exhausted', 'void', ['number'], [r.id]);
  r.exhaustedRelayed = true;
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
       the new instance cannot derive any of them. The policy texts are BASE64 fields because a raw CSP header
       may contain HTAB; CSP §2.2's self-origin is a TOKEN because an origin's serialization cannot. */
    /* A DOCUMENT THIS ZONE ALREADY HOLDS AN INSTANCE FOR IS NOT PROVISIONED TWICE, and that is not a guard
       against a duplicate notice — it is what a resumed document does. `a`'s replay re-runs its scripts, so it
       re-creates the child navigable and re-announces it under the same name, while the instance that holds
       that name has been running the whole time. Provisioning a second one would give one document two heaps
       and two object graphs, which is the state SECURITY.md's one-instance-per-cluster rule exists to prevent
       and which nothing downstream could tell from the routing. */
    if (f[0] === 'navigable.create') {
      const C = NAVIGABLE_NOTICE.create;
      /* Counted before any field is read: a record one field short would otherwise reach makeEngine with
         `undefined` where a policy goes, which is indistinguishable from a creator that sent no policy. The count
         and every index are navigable_notice.h's, through noticegen.mjs. */
      if (f.length !== C.parts)
        fail(`a navigable.create notice was short of its fields (${f.length} of ${C.parts}): ${n} — ` +
             'navigable.c composes it from navigable_notice.h, so this driver and that engine disagree');
      /* A held name is the same document created by another timeline's history (solver/world.c's
         world_mint_doc); at another address it would be two documents claiming one name, which the zones refuse
         (`blocked-asker:child-readdressed`) and this fixture, whose every open names one address, never asks. */
      if (holderOf(f[C.child])) {
        if (holderOf(f[C.child]).docUrl !== f[C.address])
          fail(`a navigable.create named ${f[C.child]}, held at ${holderOf(f[C.child]).docUrl}, at another ` +
               `address ${f[C.address]}`);
        console.log(`  [${e.tag}] create for ${f[C.child]}, already held — routing to the live instance`);
      }
      /* The provenance is counted and read nowhere else here, for the `document.fetch` branch's reason: this
         driver serves a literal and spends no network.
         The parent is not part of the container beside it and is passed on its own: this fixture's `a` opens a
         cross-origin window, which §7.3.1.7 step 8 makes an auxiliary navigable (a full policy container, no
         parent and no container element), so the record carries `u` and `null` there, and a driver that folded
         either into the container could not tell that from a frame.
         The child has no response headers of its own in this fixture, so that slot is empty. */
      else engines.push(await makeEngine(HTML_B, f[C.address], f[C.child], '', f[C.topLevelUrl], undefined,
                                         `${f[C.provenance]},${f[C.driven]},${f[C.pinned]}`,
                                         navigableNoticeField(C, f, 'policy'),
                                         navigableNoticeField(C, f, 'policyReportOnly'),
                                         f[C.cspSelfOrigin], f[C.coep], f[C.coepEndpoint], f[C.coepReportOnly],
                                         f[C.coepReportOnlyEndpoint], f[C.referrerPolicy], f[C.parent],
                                         f[C.opener], f[C.containerPolicy], f[C.ancestorOrigins],
                                         f[C.sandboxFlags]));
    }
    /* HTML §7.1.3.2's browsing context group swap — `navigable.swap`, navigable_notice.h's provision record.
       The same act as a create and a different record: §7.3.2.3 makes the new browsing context "with null,
       null, and group", a null creator, so there is no policy container to clone and no top-level creation URL
       only the creator could state (a top-level traversable's is its own address). The instance is NEW rather
       than joined because a swap changes the group, which is half of SECURITY.md's `(browsing context group,
       origin)` key.
       NOT REACHED BY THIS FIXTURE, and that is stated rather than left to be discovered: `a` opens a
       CROSS-ORIGIN window, which navigable.c hands to the create notice without ever fetching, so no load job
       of `a`'s obtains an opener policy at all. What would drive it is a SAME-ORIGIN `window.open` — the
       navigable is then built in `a`'s own heap and its load job runs here — answered by the `document.fetch`
       branch above with a `cross-origin-opener-policy: same-origin` field line, which `a`'s own `unsafe-none`
       does not match. That is one line in HTML_A and one URL-keyed header in the reply this driver serves. */
    else if (f[0] === 'navigable.swap') {
      const P = NAVIGABLE_NOTICE.provision;
      /* The provenance and the two marks of the navigation that caused the swap are the new instance's
         arrival. */
      if (f.length !== P.parts || !f[P.document] || !f[P.address] || !f[P.provenance] || !f[P.driven] ||
          !f[P.pinned] || !f[P.issuer])
        fail(`a navigable.swap notice was short of its fields: ${n}`);
      if (holderOf(f[P.document]))
        fail(`§7.1.3.2's swap named a document an instance already holds: ${f[P.document]}`);
      /* NO CREATOR (§7.3.2.3 makes the new browsing context "with null, null, and group"), so no container to
         clone and no self-origin to state. */
      /* §7.3.1.3's PARENT DEFAULTS TO `u` HERE AND THAT IS THE SPEC: §7.1.3.2 step 2 returns before the
         predicate is evaluated for anything that is not a TOP-LEVEL browsing context, which the engine asserts
         where this record is written, so the navigable a swap provisions has no parent.
         AND §3.1.3's LIST DEFAULTS TO `none` BY THAT SAME SENTENCE READ ONE ALGORITHM ALONG: its steps 2-3
         return an empty output for a Document with no CONTAINER DOCUMENT, and a navigable with no parent has
         none. Both are defaults because both are what the section says, not because nothing was known. */
      engines.push(await makeEngine(HTML_B, f[P.address], f[P.document], '', f[P.address], undefined,
                                    `${f[P.provenance]},${f[P.driven]},${f[P.pinned]}`, '', '', ''));
    }
    else if (f[0] === 'windowproxy.post') posts.push({ doc: f[1], world: f[2], record: n, origin: e.origin });
    /* A COMPLETION THIS INSTANCE PRODUCED for an operation it was asked to perform, naming the token this zone
       minted. Held rather than delivered here: the asker is another instance and this loop is inside its step. */
    /* THE WORLD IS FIELD 3, THE ANSWERING TIMELINE'S MARKS FIELD 4, AND THE COMPLETION IS THE REMAINDER, and the
       split is that way round because only the completion lacks a boundary: a world vector's own separators are
       ':' and ',' (world_serialize), while a completion is remote_object.c's grammar and may contain a tab. This
       zone reads none of them. */
    else if (f[0] === 'remoteop.answer') {
      if (f.length < 5 || !f[1] || !f[2] || !f[3] || !f[4])
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
      const entry = { world: f[2], marks: f[3], completion: f.slice(4).join('\t'), relayed: false };
      list.push(entry);
      relayAnswer(f[1], entry);
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
    else if (f[0] === 'remoteop.exhausted') {
      if (f.length < 2 || !f[1]) fail(`a remoteop.exhausted notice carried no token: ${n}`);
      if (!reads.get(f[1])) fail(`an exhausted token names no read this zone asked: ${f[1]}`);
      if (exhaustedTokens.has(f[1])) fail(`one token was exhausted twice: ${f[1]}`);
      exhaustedTokens.add(f[1]);
      relayExhausted(f[1]);
    }
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
    /* `document.seed <address> <provenance> <driven> <pinned> <issuer>` — an address a document declared is a page of its own application
       (engine/host/solver/route_seed.h), reached whenever a fixture here calls `history.pushState` with a URL.
       THIS DRIVER REFUSES IT, AND THE REFUSAL IS ABOUT WHAT THIS DRIVER IS. It provisions the peer instances a
       CROSS-INSTANCE fixture needs — a create, a swap, a routed post — from records that name the document
       and everything only the creating engine could compute. A declared route names none of that: it is a work
       item on the ONE frontier, ranked against every other by what an admission of that address has been
       worth, and there is no frontier here for it to be on and no value order to rank it in. `extension/
       bridge.js` is the zone that holds both; a fixture that wants a route seeded belongs there.
       THE GRAMMAR IS STILL CHECKED, because a malformed record is a WRITER that changed and that is a defect
       whichever host reads it. */
    else if (f[0] === 'document.seed') {
      if (f.length !== 6 || !f[1] || !f[2] || (f[3] !== 'driven' && f[3] !== 'undriven') ||
          (f[4] !== 'pinned' && f[4] !== 'unpinned') || !f[5])
        fail(`a document.seed notice was short of its fields — every declaration names the ADDRESS, the ` +
             `PROVENANCE, the DRIVE MARK, the WITNESS MARK and the declaring document the trusted zone ` +
             `decides whether to load it from: ${n}`);
      if (f[2] !== 'derived' && f[2] !== 'forced')
        fail(`a document.seed notice carries the provenance \`${f[2]}\`, which is neither \`derived\` nor ` +
             `\`forced\` — a declaration is made by RUNNING the page's code and no load produces one, so ` +
             `\`observed\` is unreachable for it and any other word is a field read at the wrong tab`);
      fail(`a document at \`${f[1]}\` was declared a page of its own application, and this driver holds no ` +
           `frontier to make it a work item on. It provisions PEERS for a cross-instance fixture (a create, ` +
           `a swap, a routed post) out of records that carry what only the creating engine could compute; a ` +
           `route declaration carries an address and a provenance and is ranked by value against every other ` +
           `work item, which is extension/bridge.js's Level-1 order and nothing this process has`);
    }
    /* `navigable.request`, navigable_notice.h's request table — a child navigable's request facts, which only
       extension/bridge.js's seed load reads; this driver checks the grammar. */
    else if (f[0] === 'navigable.request') {
      const R = NAVIGABLE_NOTICE.request;
      if (f.length !== R.parts || !f[R.creator] || !f[R.destination] || !f[R.address])
        fail(`a navigable.request notice does not match navigable_notice.h's request table: ${n}`);
    }
    else throw new Error('a notice this host does not act on: ' + f[0]);
  }
}

/* THE ROOT DOCUMENT IS ITS OWN TOP-LEVEL TRAVERSABLE, so its environment's top-level creation URL is its
   own address. */
engines.push(await makeEngine(HTML_A, 'https://a.test/', 'd1', '', 'https://a.test/', undefined, 'observed,undriven,unpinned', '', '', ''));

/* EVERY POST IS ROUTED THE ROUND IT IS EMITTED, as the offscreen routes one: the receiving engine attaches it
   to every live timeline (engine_route) and each timeline delivers or refuses it at its own step, so nothing
   here waits for a listener. */
let routedUpTo = 0;
function routePending() {
  for (; routedUpTo < posts.length; routedUpTo++) {
    const p = posts[routedUpTo];
    const target = holderOf(p.doc);
    if (!target) { routeFailures.push(`post ${p.world} -> ${p.doc}`); continue; }
    if (!routedTo.has(p.doc)) routedTo.set(p.doc, 0);
    routedTo.set(p.doc, routedTo.get(p.doc) + 1);
    target.M.ccall('qjs_route', 'void', ['number','number'], [target.cs(p.record), target.cs(p.origin)]);
  }
}

/* PHASE 1 — `a` runs, with `b` beside it, until it is owed the `/resume` this zone withholds: the output phase 3
   starts from. */
await runUntil(() => resumeSeen);
console.log(`phase 1: \`a\` owes /resume; it emitted ${posts.length} posts, worlds: ` +
            posts.map((p) => p.world).join('  '));

/* PHASE 3 — `/resume` is answered, so `a`'s parked flow reads `w.closed` (twice: `typeof w.closed`, then
   `w.closed`), each read suspending on the peer. Under `holdAfterFirst` each read is relayed one timeline's
   answer and holds the rest, so the flow takes that one, leaves a standby beside it, and goes on. The phase ends
   on the output that says both happened: the flow's `/closed` report, and a read with an answer taken and
   another held. */
resumeOwed = false;
holdAfterFirst = true;
const preParkWorlds = new Set([...posts.map((p) => p.world), ...[...reads.values()].map((r) => r.world)]
                              .map((v) => v.split(',')[0]));
const closedBeforeHold = closedBy(engines[0].tag).length;
const heldOf = (token) => (answers.get(token) || []).filter((a) => !a.relayed).length;
const heldRead = () => [...reads.entries()].find(([k, r]) => r.holdAfterFirst && r.taken[0] === 1 && heldOf(k));
console.log('\nphase 3: /resume answered; each read is relayed its first answer and holds the rest');
await runUntil(() => closedBy(engines[0].tag).length > closedBeforeHold && heldRead() !== undefined);
console.log(`phase 3: \`a\` took ${heldRead()[0]}'s first answer and ${heldOf(heldRead()[0])} answer(s) are held`);

/* PHASE 4 — THE PARK. `a` is paged out with its standbys at those reads, torn down, and resumed from its own
   residue into a NEW instance, while `b` — which holds the answers, the export table and a segment for every
   world `a` has sent it — is not touched. That asymmetry is the point: Level-1 eviction gives up ONE document's
   engine. The residue is a replay recipe (solver/cold.h), so a read is re-issued, not carried; what a family took
   crosses as its foreclosed commitment rows. */
const parked = engines[0];
const postsAtPark = posts.length;
parked.M.ccall('qjs_request_park', 'void', [], []);
const parkStep = parked.M.ccall('qjs_step', 'number', [], []);
const residue = JSON.parse(parked.str('qjs_result'))._park.join(';');
console.log(`\nphase 4: park step answered ${parkStep}, residue ${residue.length} bytes: ${residue}`);
/* THE PARK'S OWN NOTICES, TAKEN BEFORE THE INSTANCE GOES: every world this session sent is dead from here on,
   and `b` holds a segment for each. */
const deathsAtPark = worldDeaths.length;
await drainNotices(parked);
console.log(`phase 4: the park announced ${worldDeaths.length - deathsAtPark} world death(s) to the peer`);
parked.M.ccall('qjs_teardown', 'void', [], []);
parked.gone = true;
/* THE ORPHANS: completions held for a read the parked instance asked. They name a token of an instance that is
   gone, so there is no flow, register or instance for them to land in. */
const orphanCompletions = [...reads.entries()].filter(([k, r]) => r.engine === parked && heldOf(k)).map(([k]) => k);
/* Still set for the resumed session. A foreclosed row written in this session (a standby left at a first take,
   or an arm's own world) can only refuse a later answer to the same request, so with every later answer held a
   foreclosed refusal phase 5 counts is one against a row the residue carried. */
engines[0] = await makeEngine(HTML_A, 'https://a.test/', 'd1', '', 'https://a.test/', residue, 'observed,undriven,unpinned', '', '', '');
const resumedTag = engines[0].tag;
console.log(`phase 4: resumed as [${resumedTag}] from the residue; [${engines[1].tag}] never left memory`);

/* WHAT THE PARKED FAMILIES HAD TAKEN, AND WHETHER A RESUMED STANDBY STILL REFUSES IT. A foreclosed row
   (`r<hex vector>,0,3`, flow.h's FLOW_COMMIT_ARM_ANSWER_TAKEN) names a peer timeline a family member took before
   the park, and `b` never left its session, so its timelines still relate to the row. Each parked flow carries
   its own rows, and the union over flows covers every timeline `b` has (each side of its first branch is some
   family's take), so which answers a resumed request took cannot say which rows it met. The resumed instance
   says it: `hostAnswersForeclosed` counts holders that refused an answer under their own foreclosed row. */
const foreclosedRows = residue.split(';').filter((r) => /^r[0-9a-f]+,0,3$/.test(r)).length;
if (!foreclosedRows)
  fail('the park wrote no foreclosed ANSWER_TAKEN row while `a` held a standby at a read whose first answer it ' +
       'took — flow_answer_standby writes that row on the standby, so the park did not carry it');
const foreclosedRefusals = () => {
  const n = JSON.parse(engines[0].str('qjs_result'))._cold.hostAnswersForeclosed;
  if (typeof n !== 'number')
    fail(`[${resumedTag}] the result document's \`_cold\` carries no \`hostAnswersForeclosed\` — ` +
         'solver/metrics/cold.def declares it, so an absent one is a producer that moved under this reader');
  return n;
};
const resumedTook = () => [...reads.values()].filter((r) => r.asker === resumedTag && r.taken.includes(1));

/* PHASE 5 — THE RESUMED SESSION, until it has emitted the outputs this drive is about: a resumed holder that
   refused an answer under its family's foreclosed row, a resumed request that took one, and a `w.closed` read
   back true. The census is read last, because composing the result document is the costly conjunct. */
await runUntil(() => closedBy(resumedTag).some((r) => r.url.includes('v=boolean:true')) &&
                     resumedTook().length > 0 && foreclosedRefusals() > 0);
console.log(`\nphase 5: the residue carried ${foreclosedRows} foreclosed row(s); ${foreclosedRefusals()} resumed ` +
            `holder(s) refused an answer under one, and ${resumedTook().length} resumed request(s) took an answer`);

const resumedReads = [...reads.values()].filter((r) => r.asker === engines[0].tag);
/* EVERY WORLD NAME THE RESUMED SESSION PUT ON THE WIRE, against every name the session that ended had already
   put there. A name in both sets is one `b` keyed a segment on for a flow that no longer exists. */
const reusedWorlds = [...new Set([...resumedReads.map((r) => r.world),
                                  ...posts.slice(postsAtPark).map((p) => p.world)]
                                 .map((v) => v.split(',')[0]))].filter((h) => preParkWorlds.has(h));

console.log(`\nposts routed: ${posts.length}   distinct /got requests the receiving page issued: ${got.length}`);
for (const u of got) console.log('  ' + u);
console.log(`cross-agent reads asked: ${reads.size}`);
for (const r of reads.values())
  console.log(`  [${r.asker}] ${r.by.length ? `relayed ${r.by.length} answer(s), ` +
              `${r.taken.filter((t) => t === 0).length} refused` : 'no answer relayed'}` +
              `${r.holdAfterFirst ? ', held after its first' : ''} ${r.op}`);
/* THE SEAM'S OWN COUNTS, from each instance, none defaulted: a `|| 0` would print the number a healthy run
   prints for a producer that stopped emitting the field. `held` is the live segment table, `made` how many were
   ever materialized, `forked` how many were built by forking an ancestor the ancestry named. The four ends are
   §9.3.3 step 8's task's (solver/engine.h). */
const ENDS = ['_routedTasksFired', '_routedTasksTargetOrigin', '_routedTasksTargetGone', '_routedTasksThrew'];
const ends = { _routedTasksFired: 0, _routedTasksTargetOrigin: 0, _routedTasksTargetGone: 0, _routedTasksThrew: 0 };
let forked = 0, delivered = 0, refused = 0;
for (const e of engines) {
  const r = JSON.parse(e.str('qjs_result'));
  for (const k of ['_worldSegmentsForked', '_routedDelivered', '_routedRefused', '_routedZeroDelivery',
                   '_orphansDriven', '_orphansAsked', ...ENDS])
    if (typeof r[k] !== 'number')
      fail(`[${e.tag}] the result document carries no \`${k}\` — solver/result.c emits it unconditionally, so ` +
           'an absent one is a producer that moved under this reader');
  forked += r._worldSegmentsForked;
  delivered += r._routedDelivered;
  refused += r._routedRefused;
  for (const k of ENDS) ends[k] += r[k];
  console.log(`[${e.tag}] worldSegments held=${r._worldSegmentsHeld} made=${r._worldSegmentsMade} ` +
              `forkedFromAncestor=${r._worldSegmentsForked} flows=${r._flows} switches=${r._switches} ` +
              `routedDelivered=${r._routedDelivered} routedRefused=${r._routedRefused} ` +
              `routedZeroDelivery=${r._routedZeroDelivery} (a gauge, not a loss reading here) ` +
              `orphans driven=${r._orphansDriven} asked=${r._orphansAsked}`);
  console.log(`[${e.tag}] §9.3.3 step 8 task ends: fired=${r._routedTasksFired} ` +
              `declinedByTargetOrigin=${r._routedTasksTargetOrigin} targetDestroyed=${r._routedTasksTargetGone} ` +
              `threw=${r._routedTasksThrew}`);
}
/* Named residual. Not covered: whether every routed record reached some timeline of its document.
   `_routedZeroDelivery` is a gauge that reads a record still attached to an unstepped timeline like a lost one,
   and a receiver that forks on what it is sent and owes a `/hold` never consumes every attachment, so no moment
   of this drive makes it a loss reading. Next diff builds: the per-attachment ticket solver/engine.c's RoutedRec
   residual names, so a record's last attachment consumed is an emission this drive can wait for. Absence shows
   as: a peer's message admitted by no timeline passing this drive. */

/* WHAT MAKES THIS A SMOKE TEST RATHER THAN A PRINTOUT: a second instance was provisioned, a routed record became
   a task that fired at the receiving Window and ran the page's own listener, a segment was materialized by
   forking an ancestor, and the synchronous cross-instance read was asked of the instance holding the document.
   Each is a whole mechanism failing silently if it did not happen. */
if (engines.length < 2) fail('no second instance was provisioned — the navigable.create notice went unanswered');
if (!posts.length) fail('the sender emitted no cross-instance post');
if (!ends._routedTasksFired)
  fail(`${posts.length} record(s) were routed, ${delivered} became a task, and NOT ONE of those tasks fired an ` +
       'event at the receiving Window — §9.3.3 step 8.7 is the only end at which a page learns anything. The ends ' +
       `they did reach: declined by targetOrigin ${ends._routedTasksTargetOrigin}, target destroyed ` +
       `${ends._routedTasksTargetGone}, threw ${ends._routedTasksThrew}`);
/* No `/got` has two readings. The listener never ran, or it ran and composed an address the engine could not
   ground, which it declines before any host sees it (pending_park_request's unaddressed refusal). The peer's
   call-site table separates them: a `/got` site rooted in a routed delivery's `message.` sources is the second. */
if (!got.length) {
  const peer = JSON.parse(engines[1].str('qjs_result'));
  if (!Array.isArray(peer.fetchCallSites) || typeof peer._cold.replyUnaddressed !== 'number')
    fail(`[${engines[1].tag}] the result document carries no \`fetchCallSites\` list or no ` +
         '`_cold.replyUnaddressed` — solver/result.c emits both, so an absent one is a producer that moved');
  const sites = peer.fetchCallSites.filter((s) => s.url.includes('/got') && s.addressRoot !== null &&
                                                  s.addressRoot.includes('message.'));
  const classes = [...new Set(sites.map((s) => s.addressClass))].join('/');
  if (sites.length)
    fail(`the receiving page's \`message\` listener ran and composed its /got request at ${sites.length} ` +
         `call site(s) rooted in a delivery's sources, address classes ${classes}, and none reached a host: ` +
         `the peer declined ${peer._cold.replyUnaddressed} request(s) unaddressed. First: ${sites[0].url}`);
  fail('the receiving page never issued the request its `message` listener is written to issue, so the event ' +
       'fired at a Window whose listener is not the one the document registered');
}
if (!forked) fail("no segment was materialized by forking an ancestor — the world vector's ancestry was carried " +
                  'and never used');
/* A zero here would mean `w.length` resolved without reaching the peer: HTML §7.2.2.2 Indexed access on the
   Window object answers it from the PEER's active document's child navigables. */
if (!reads.size)
  fail('`w.length` on a cross-origin WindowProxy asked the peer nothing — HTML §7.2.2.2 Indexed access on the ' +
       "Window object answers it from the document-tree child navigables of the PEER's active document");
if (routeFailures.length)
  fail(`${routeFailures.length} record(s) named a document no instance holds, so their askers are parked on a ` +
       `question nothing will ever answer: ${routeFailures.join(' ; ')}`);
if (unheldReads.size)
  fail(`${unheldReads.size} cross-agent read(s) named a document no instance held when the drive ended: ` +
       [...unheldReads.values()].join(' ; '));
/* THE TYPE OF EVERY `w.closed` READ BACK, every session's: §7.2.2 The Window object declares `closed` a boolean,
   and an answer that arrived as text satisfies `v ? …` and is not one. Phase 5 already waited for a resumed
   `true`, which is the check a wrong answer passes: `b` closed itself, and only `b`'s record says so. */
const notBool = closedReports.filter((r) => !r.url.includes('v=boolean:'));
if (notBool.length)
  fail(`${notBool.length} of ${closedReports.length} reads of \`w.closed\` came back under a type §7.2.2 The ` +
       `Window object does not declare for it — it is a boolean: ${notBool.map((r) => r.url).join(' ; ')}`);

/* ── THE PARK: one of the two instances LEAVING MEMORY while the other keeps its state, which is what Level-1
   eviction does. */
if (parkStep !== 0)
  fail(`the park step answered ${parkStep} rather than DONE — engine_sched_step takes the park before its first ` +
       'pick and closes the session, so anything else means the frontier was not written out');
if (!residue)
  fail('the parked frontier wrote an EMPTY residue while a standby waited at a cross-instance read — an empty ' +
       'park document is how a fully-explored document DELETES its cold entry (solver/cold.h)');
/* THE PEER'S IN-FLIGHT TURN OUTLIVES THE INSTANCE THAT ASKED: the held completions name a token whose asking
   instance no longer exists, and this zone is the only thing left holding them. */
if (!orphanCompletions.length)
  fail('no completion was held for a read the parked instance asked — phase 3 ended on one, so the park lost it');
console.log(`orphaned peer completions after the park: ${orphanCompletions.length} — ${orphanCompletions.join(' ')}`);
/* THE RESUMED FLOW RE-ISSUES ITS READS: a recipe is a replay (solver/cold.h), so the request is re-issued by the
   code that issued it the first time and answered with today's value. */
if (!resumedReads.length)
  fail('the resumed instance asked the peer nothing — the flow parked at a cross-instance read did not replay ' +
       'to it, so the park lost it');
/* A WorldId is (document, GENERATION, serial): the serial counts from 1 in every session and the document name
   is stable, so without the generation the resumed session would hand the peer names a dead flow's timeline
   still holds a segment under. Nothing inside either engine sees both sessions; this driver does. */
if (reusedWorlds.length)
  fail(`the resumed session put ${reusedWorlds.length} world name(s) on the wire that the session that ended ` +
       `had already sent: ${reusedWorlds.join(' ')}`);
if (!worldDeaths.length)
  fail('no world death crossed this seam: `a` parked its whole frontier, so every world it sent was due a ' +
       'death, and `b` holds each segment for the rest of its process without one');
console.log(`world deaths announced: ${worldDeaths.length} — ${worldDeaths.join(' ')}`);
console.log(`[route] OK — two instances, ${posts.length} record(s) routed into ${delivered} delivery(ies) ` +
            `(${refused} declined by a receiving timeline), ancestry-forked segments: ${forked}, cross-agent ` +
            `reads asked: ${reads.size}, ${foreclosedRefusals()} resumed holder(s) refused an answer under a ` +
            `foreclosed row, w.closed read back true after the park, parked and resumed across ` +
            `${residue.length} bytes of residue`);

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
