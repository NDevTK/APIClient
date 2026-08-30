/* THE NATIVE HOST'S TRUSTED ZONE — the party that answers a parked request, for the host that is a PROCESS
 * rather than a browser extension.
 *
 * `node engine/trusted.mjs <url> [path-to-binary]`, after `node engine/build.mjs native`.
 *
 * WHY THIS IS JAVASCRIPT AND NOT C, WHICH IS THE ONLY QUESTION THIS FILE'S EXISTENCE ANSWERS. SECURITY.md gives
 * the extension exactly one network chokepoint — `extension/lib/safe-fetch.js` — and CLAUDE.md's §Architecture
 * has no carve-out for a host being native. So the native zone had two possible shapes, and three facts chose
 * this one:
 *   · THERE IS NO TLS IN THIS TREE AND NONE MAY BE GROWN. §Bind-before-build ends "hand-roll last (never
 *     crypto/parsers from scratch)". The only native network anywhere here is `engine/host/wpt_runner.c`'s
 *     plaintext HTTP/1.1 over an AF_INET socket to a loopback wptserve — a corpus transport, not a web client.
 *     A C chokepoint could therefore not reach an `https:` origin at all, which is the scheme of essentially
 *     the whole surface this tool exists to learn. The POLICY is the easy half of `safe-fetch.js`; the CLIENT
 *     is the hard one, and Node already has it.
 *   · THE ZONE MUST BE THE PARENT. A trusted process forked BY the untrusted one has its argv, its
 *     environment, its file descriptors and its lifetime chosen by the process it is supposed to police, and
 *     at one uid it can be ptraced by it. SECURITY.md's shape is the reverse — the trusted zone PROVISIONS
 *     the untrusted one — and it is also the shape the ABI arm already had: `document<TAB>…` on its stdin was
 *     always a record somebody else writes.
 *   · A SECOND COPY OF THE RULES WOULD DRIFT. `safe-fetch.js` is 600 lines of stated reasoning that moved
 *     twice in the last two commits alone (every post-fetch gate now judges `resp.url`; nosniff is Fetch's
 *     split-and-match rather than a substring). A C twin's drift would be a hole that exists on one host only,
 *     which is the worst shape a security difference can have. This file LOADS THAT FILE VERBATIM.
 *
 * AND WHAT THE BOUNDARY HERE IS NOT, STATED RATHER THAN IMPLIED. In the extension the engine runs in another
 * PROCESS, in a sandboxed frame at a unique opaque origin, reachable only over a validated mojom interface.
 * Here it is LINKED INTO the child (`main.c`'s ABI and `test_forced.c`'s `--abi` arm are one program), so the
 * child is untrusted AS A WHOLE and what separates it from this zone is a pipe and a process. That is weaker
 * than the extension's boundary and it is the honest description of it: what this zone still owns absolutely
 * is the NETWORK — the child opens no socket — and that is the invariant `safe-fetch.js` exists for.
 *
 * THERE IS NO COOKIE JAR HERE, WHICH IS AN ANSWER AND NOT AN OMISSION. `opts.credentialed` attaches the
 * PERSON'S cookies, and it does so because the extension's `fetch` runs in a browser profile that has some.
 * Node's `fetch` has no cookie store at all, so `credentials:"include"` would attach nothing: the mode would
 * spend no session, would gate the reply against a CORS grant nothing needs, and would name a person who is
 * not present. SECURITY.md's "a session-less tab models nothing" is about a host where a session EXISTS to be
 * spent; this host is a CLEAN CLIENT, which is a coherent thing to be and is precisely the surface CLAUDE.md
 * §What-the-tool-produces aims at ("learn the LOGGED-IN API surface WHILE LOGGED OUT"). So `credentialed` is
 * passed FALSE as a positive statement with its reason, never left off — and the destructive-path deny list,
 * which `safe-fetch.js` scopes to exactly the credentialed case, is correctly inert here for the same reason
 * it is armed there: the harm it prevents needs the session.
 *
 * WHAT THIS ZONE WILL FIRE, AND WHAT IT REFUSES — the whole of the policy, and it is deliberately narrow.
 * CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE: every outbound request states whether it is OBSERVED, DERIVED
 * or FORCED, and a reply to a FORCED request is evidence about what a server says to a request no client
 * makes. Two requests are OBSERVED and this zone performs them:
 *   · THE SEED. This zone originated it, from an address a person typed. Nothing derived it.
 *   · A PARSER-INSERTED `<script src>` OF THE SEEDED DOCUMENT — the pending line's `parser` initiator, which
 *     is HTML §4.12.1 "The script element"'s parser-inserted flag (solver/engine.h). It is named by the BYTES
 *     THIS ZONE ITSELF FETCHED, so a real load of that document makes exactly that request.
 * EVERYTHING ELSE IS REFUSED, WITH THE REASON, AND THE REASON IS NO LONGER THAT THE TWO LOOK ALIKE. A park
 * made by RUNNING CODE — a page `fetch()`, an injected `<script src>`, a dynamic `import()` — is DERIVED or
 * FORCED, and the pending line now SAYS which: the engine composes it at the park from the parser-inserted
 * flag and from whether the parking flow's path had taken an arm its own concrete example contradicts
 * (solver/flow.h's `path_forced`). What is missing is the AUTHORISATION for a DERIVED fetch — the per-origin
 * widening that lets a person say "at this host, fire what the bundle derives", which exists here for
 * NAVIGATIONS and not yet for fetches — and, for a FORCED one, nothing is missing at all: it is refused by
 * the rule. See PROVENANCE_DECLINE, which states the two separately. Neither refusal is a default: the reason
 * travels to the child, which prints it at the stall. That this zone is stricter than `bridge.js` — which fires every pending line
 * uncredentialed — is not a second policy: it is a hole SECURITY.md §Network already names as its own standing
 * open item ("that vocabulary … is the next subproblem"), and a zone written after the rule does not get to
 * inherit one that predates it.
 *
 * THE CHANNEL IS HALF-DUPLEX, WHICH IS WHAT KEEPS IT FROM DEADLOCKING. The child announces its bill whenever
 * the bill CHANGES and never reads back; it reads only after writing `stalled`, which is the one moment every
 * flow is parked and blocking denies nobody the thread. This zone therefore QUEUES its answers and writes them
 * only in reply to `stalled`. Two processes each filling the other's pipe while neither drains is a hang with
 * no symptom, and that one rule makes it impossible rather than unlikely.
 *
 * AND IT ROUTES, WHICH IS THE ZONE'S ONE OTHER FACT: which instance holds which document. An instance is an
 * ORIGIN-KEYED AGENT CLUSTER (SECURITY.md), so a CROSS-ORIGIN child is a second PROCESS of the same binary and
 * everything that crosses between them crosses this zone — a posted message with the sender's origin STAMPED
 * here (the untrusted engine may not name its own sender), a synchronous cross-origin read handed to the
 * instance that HOLDS the document and answered by it BY RUNNING A PROGRAM, and the completion relayed back to
 * the flow that suspended at the read. This zone reads none of those payloads: it routes TEXT, and only an
 * engine knows what a world vector or an object name means. `engine/route.mjs` is the same protocol against
 * wasm instances and is where its reasoning is written at length.
 *
 * WHAT IT STILL DOES NOT DO, AND WHY THE ROUTING ABOVE IS NOT THEREFORE EXERCISED BY DEFAULT. Provisioning a
 * peer means LOADING a document at an address the PAGE'S OWN CODE chose, and this zone cannot yet establish
 * that address's provenance (see UNSTATED_PROVENANCE). CLAUDE.md's §Attacker-sources is explicit that a
 * navigation whose provenance is not established CRASHES at the decision rather than proceeding, so by default
 * it does — and the refusal travels to the child, which prints it at the stall. What lifts it is the second of
 * the two things that bullet names, and it is built here because it is the zone's to own: the PER-ORIGIN
 * WIDENING (`--explore <origin>`), which is a person saying "at this host, navigate what the bundle names".
 * See NAVIGATION_WIDENING for why that is a whole answer here and not half of one. */
import { spawn } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { existsSync, readFileSync } from 'node:fs';
import { createContext, runInContext } from 'node:vm';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const EXT_DIR = join(ENGINE, '..', 'extension');

/* THE CHOKEPOINT, LOADED VERBATIM INTO A REALM OF ITS OWN. The isolated context is `engine/route.mjs`'s
   reasoning applied one file over: evaluating `check.js` over THIS realm would install `DCHECK`/`CHECK` into a
   driver whose own code does not expect them, and a zone silently gaining assert machinery is a difference
   between what it runs and what production runs. The load order is `ast-worker.html`'s, because safe-fetch
   asserts through check.js.
   ONLY THE HOST OBJECTS ARE INJECTED. A fresh vm context has the ECMAScript intrinsics and nothing else, so
   `fetch`, `URL`, `TextDecoder` and `TextEncoder` come from this realm — which is also why nothing below may
   `instanceof` a value that came back out of it: a Uint8Array minted in that realm is not this realm's
   Uint8Array, and `ArrayBuffer.isView` is the cross-realm question. */
const ZONE = (() => {
  const sandbox = { console, fetch, URL, TextDecoder, TextEncoder };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  createContext(sandbox);
  for (const f of ['check.js', 'lib/safe-fetch.js'])
    runInContext(readFileSync(join(EXT_DIR, f), 'utf8'), sandbox, { filename: join(EXT_DIR, f) });
  if (typeof sandbox.safeFetch !== 'function')
    throw new Error('extension/lib/safe-fetch.js did not install `safeFetch` — this zone loads that file ' +
                    'verbatim rather than restating its rules, so a load that installs nothing leaves this ' +
                    'process with no chokepoint and therefore no permission to reach the network at all');
  return sandbox;
})();

/* EVERY FIELD OF EVERY REPLY, ASSERTED AND NEVER DEFAULTED — the same list `bridge.js` asserts at the same
   seam, because it is the same producer answering the same question and a second reader that checked less
   would be the one a drift slips past. `status === 0` is the one status no HTTP response has and is exactly
   what the chokepoint answers when the request never went on the wire (bad URL, blocked scheme, blocked
   private target, CORB, a refused credentialed read); the engine's delivery turns the JSON `null` into Fetch
   §5.6's TypeError, which is what a request that failed IS — a status-0 record would instead resolve the
   page's promise with a status no server returns. */
function replyRecord(r, where) {
  if (!r || typeof r !== 'object' || typeof r.status !== 'number' || !ArrayBuffer.isView(r.body))
    throw new Error(`safeFetch answered ${where} with something other than its reply record — Fetch §2.2.4 ` +
                    "\"Bodies\" makes a body's source a BYTE SEQUENCE and every path of that function returns " +
                    'one, so a string or a missing field here is the chokepoint itself having changed shape');
  if (!Array.isArray(r.urlList) || r.urlList.length < 1)
    throw new Error(`safeFetch answered ${where} with no URL list — Fetch §2.2.6 "Responses"' response.url ` +
                    'and response.redirected are read off nothing else, so the engine would report every ' +
                    'redirect as none. The chokepoint reports [requested] even for a blocked read, because ' +
                    'the request URL is a fact even when the reply is not');
  if (!r.headers || typeof r.headers !== 'object')
    throw new Error(`safeFetch answered ${where} with no header map`);
  if (typeof r.computedType !== 'string')
    throw new Error(`safeFetch answered ${where} with no computed content type — solver/reply_decode.c reads ` +
                    'this field instead of re-deriving a type, so an absent stamp is a producer that failed ' +
                    'and never a resource whose type is unknown');
  if (typeof r.statusText !== 'string')
    throw new Error(`safeFetch answered ${where} with no statusText — it is written on every path (the ` +
                    'blocked arms carry their REFUSAL REASON in it) and the empty string is what any HTTP/2 ' +
                    'response says, so the two cannot be collapsed by a default');
  if (r.status === 0) return null;
  return { meta: { status: r.status, statusText: r.statusText, headers: Object.entries(r.headers),
                   urlList: r.urlList, computedType: r.computedType },
           bytes: Buffer.from(r.body.buffer, r.body.byteOffset, r.body.byteLength) };
}

/* THE RESPONSE'S HTTP FIELD LINES, WHICH IS THE ONE FORM A HEADER LIST CROSSES THIS ABI IN
   (core/fetch/headers.h). The empty string is the positive statement "this response carried no headers" —
   `header_list_parse_field_lines` reads it as exactly that — so there is nothing here to default.
   THE MAP HAS ALREADY JOINED REPEATED FIELDS with ", ", which is Fetch §2.2.2 "Headers"' `get` and is what
   `safe-fetch.js` builds from `resp.headers.forEach`. That is the value CSP §2.2's policy-list serialization
   is defined over and what `policy_container.c` splits apart again, so the join is the form the engine wants
   rather than a loss this line is papering over. */
function fieldLines(entries) {
  return entries.map(([n, v]) => `${n}: ${v}`).join('\n');
}

/* THE ONE FIELD ENCODING. Bytes and any text that is not a bare token cross base64: the channel is
   line-oriented and tab-separated, a document may legally contain a 0x00 and a newline, and a JSON serializer
   that escapes control characters today is a property of the ENCODER rather than of the channel — a grammar
   that leans on it is a coupling nothing asserts and the next producer would not know it had.
   `-` IS ABSENCE, WHICH IS A DIFFERENT FACT FROM EMPTINESS, and main.c holds the two apart at both entries
   this feeds: a NETWORK ERROR has no body at all, a 204 has one that is zero bytes long, an answer that is a
   number has none while an answer that is a document has bytes. */
const b64 = (x) => Buffer.from(x === undefined ? '' : x).toString('base64');
const ABSENT = '-';

/* WHY THIS ZONE WOULD NOT PERFORM A REQUEST, IN ITS OWN WORDS. It crosses to the child because the party that
   refused is the party that knows why: a stall whose reason is guessed by the process that did not make the
   decision is the stale-`DFAIL` failure with a process boundary in the middle. */
function decline(reason) { return `decline\t${b64(reason)}`; }

const UNSTATED_PROVENANCE =
  'a park made by RUNNING CODE, whose provenance this zone cannot establish. CLAUDE.md ' +
  '§A-REQUEST-CARRIES-THE-PROVENANCE splits an outbound request into OBSERVED / DERIVED / FORCED and makes a ' +
  'reply to a FORCED one evidence about what a server says to a request no client makes — plausible, ' +
  'unattributable, and it PROPAGATES, because one invented field is the example that shapes the next ' +
  'endpoint. The pending register STATES which of the three this park is (solver/engine.h\'s ' +
  'PENDING_PROVENANCE_*, composed at the park from HTML §4.12.1\'s parser-inserted flag and whether the ' +
  'parking flow\'s path had taken an arm its own concrete example contradicts), so a page fetch() and a ' +
  'forced arm\'s fetch() are no longer one class — and this zone still fires neither, for the reasons below ' +
  'rather than because it cannot tell them apart. TWO THINGS WOULD LIFT IT, in order: a flow that RECORDS ' +
  'whether its path took a forced arm, which is what separates DERIVED from FORCED; and the per-origin ' +
  'configuration CLAUDE.md requires before a forced request may be fired at all. The FIRST now exists ' +
  '(solver/flow.h\'s `path_forced`). The SECOND now exists and covers NAVIGATIONS only ' +
  '(`--explore <origin>`, see NAVIGATION_WIDENING). What a FETCH is still waiting for is that same widening ' +
  'said about fetches: the pending line now carries the CORB class too (Fetch §2.2.5\'s destination), so the ' +
  'reason a running-code park is refused here is no longer that this zone could not classify it — it is that ' +
  'nobody has said "at this host, fire what the bundle derives", and CLAUDE.md makes that a person\'s ' +
  'sentence rather than an inference';

/* WHY A PARK THIS ZONE CAN CLASSIFY IS STILL NOT FIRED, IN ITS OWN WORDS AND PER CLASS. Three states behind
 * one answer is the defect CLAUDE.md names at §@S ("a rung whose ABSENCE and whose ZERO read alike"), and a
 * refusal that reads the same for a DERIVED park and a FORCED one is that defect performed on the one seam
 * whose whole subject is telling them apart. The two are refused for genuinely different reasons: a DERIVED
 * request is one a person may authorise per origin and has not; a FORCED one is refused by the rule itself,
 * and would be under any widening short of a sentence about forcing specifically. */
const PROVENANCE_DECLINE = {
  derived:
    'a DERIVED park — the page\'s own code computed this address from real inputs, so it is a fact about the ' +
    'app and firing it is the ACTIVE DISCOVERY CLAUDE.md §Attacker-sources calls REQUIRED. What is missing is ' +
    'the authorisation: §Attacker-sources makes firing configurable and PER ORIGIN, "default conservative, ' +
    'widened deliberately per origin, never inferred from a site looking like a test", and this zone\'s ' +
    '`--explore <origin>` says that about NAVIGATIONS only. Saying it about fetches is the next diff here',
  forced:
    'a FORCED park — a value in this request exists only because a gate was forced, so a reply to it is ' +
    'evidence about what a server says to a request no client makes. CLAUDE.md §@H forbids such a reply ever ' +
    'being carried as OBSERVED, and the danger is that it is PLAUSIBLE: a 401 body parses as JSON and yields ' +
    'fields that exist nowhere, and one invented field is the example that shapes the next endpoint. The ' +
    'request is DERIVED IN FULL and REPORTED, which §Attacker-sources says is not a gap in the report but IS ' +
    'the report — that surface is what forced execution finds and a sniffer cannot',
};

/* THE PER-ORIGIN WIDENING, WHICH IS A PERSON'S SENTENCE AND NOT AN INFERENCE — `--explore <origin>`, repeated.
 * CLAUDE.md §Attacker-sources: "It is CONFIGURABLE AND PER-ORIGIN, BECAUSE EXPERIMENTATION IS NOT ALWAYS WRONG
 * AND A SINGLE SWITCH CANNOT SAY SO … Default conservative, widened deliberately per origin, never inferred
 * from a site looking like a test." Firing what a bundle NAMES at an app you own is the point of the tool;
 * doing it at a stranger's production account is not; and no property of the address distinguishes them, which
 * is exactly why the answer is a person's and is stated per origin rather than derived.
 *
 * IT COVERS NAVIGATIONS AND NOTHING ELSE, and that narrowness is the design rather than a first instalment.
 * §Attacker-sources' real-navigable bullet puts the WHOLE of a navigation's safety in the choice of ADDRESS:
 * a top-level navigation is a GET, which RFC 9110 §9.2.1 "Safe Methods" contains, so the METHOD half is
 * already answered and the entire remaining question is PROVENANCE — which is what this flag answers. A
 * running-code FETCH has a second question this flag cannot answer (which CORB class the body is), so it is
 * refused on its own ground and not swept in here.
 *
 * AND THE ONE COMBINATION THAT IS NEVER A SETTING IS UNREACHABLE HERE BY CONSTRUCTION, which is why this is a
 * whole answer and not a hole with a flag over it. That combination is credentialed AND state-mutating AND
 * forced. This process has NO COOKIE JAR (see this file's header: Node's `fetch` has no cookie store, so
 * `credentials:"include"` would attach nothing and name a person who is not present), and `safe-fetch.js` is
 * GET-only BY ABSENCE — it hardcodes `method:"GET"` and reads neither `opts.method` nor `opts.body`. So both
 * of the other two conjuncts are false at every setting of this flag, including its widest. */
const NAVIGATION_WIDENING = new Set();

/* WHY A NAVIGATION TO A *FORCED* ADDRESS IS REFUSED AT AN UNWIDENED ORIGIN — a different sentence from
   UNWIDENED_NAVIGATION because it is a different fact: there the provenance could not be established at all,
   here it is established and it is the one the widening exists to gate. */
const FORCED_NAVIGATION =
  'a DOCUMENT LOAD at an address that exists only because a GATE WAS FORCED. CLAUDE.md §Attacker-sources ' +
  'makes exactly this the per-origin widening — "default conservative, widened deliberately per origin, never ' +
  'inferred from a site looking like a test" — so the refusal is this policy\'s ANSWER for an origin nobody ' +
  'has widened rather than a capability that is missing. Pass `--explore <origin>` to widen it, and note what ' +
  'that then obliges: §@H makes the reply to a forced request evidence about what a server says to a request ' +
  'no client makes, so its values are carried as FORCED and never merged into the observed pool. Until then ' +
  'the address is DERIVED IN FULL and REPORTED, which §Attacker-sources says is not a gap in the report ' +
  'but IS the report'

/* WHY A NAVIGATION AT AN UNWIDENED ORIGIN IS REFUSED, in the zone's own words — the same shape as
   UNSTATED_PROVENANCE and a different sentence, because it is a different decision over a different record. */
const UNWIDENED_NAVIGATION =
  'a DOCUMENT LOAD at an address the page\'s own code chose. CLAUDE.md §Attacker-sources makes a navigation\'s ' +
  'whole safety the choice of address and its whole remaining question PROVENANCE: an OBSERVED or DERIVED ' +
  'address is navigated freely, a FORCED one is the deliberate per-origin widening, and one whose provenance ' +
  'is NOT ESTABLISHED crashes at the decision rather than proceeding. This zone cannot establish it — the ' +
  'create notice carries the child\'s name, address, origin, §8.1.3.1 top-level creation URL, §7.1.7 policy ' +
  'container, §7.3.1.3 links and §3.1.3 ancestors, and says nothing about WHO NAMED THE ADDRESS, which is the ' +
  'one fact the pending register already carries for a fetch (solver/engine.h\'s INITIATOR). Two things lift ' +
  'it and either is enough: carry that initiator on the create notice so a navigable the PARSER of the ' +
  'seeded document created is OBSERVED by the same argument its parser-inserted <script src> is; or say ' +
  '`--explore <origin>` for this host, which is a person authorizing exactly this';

async function main() {
  /* THE FLAGS ARE TAKEN OUT BEFORE THE POSITIONALS ARE COUNTED, so `--explore` may appear anywhere and the
     two addresses this command line carries cannot be confused for each other. An `--explore` with no value
     is REFUSED rather than ignored: the flag is a person authorizing a navigation, and one that authorized
     nothing while reading as though it had is the shape of permission this zone must never grant by accident.
     THE VALUE IS NORMALIZED TO AN ORIGIN through the URL parser, because that is what the comparison is
     against — a person who types `https://b.test/some/path` means the host, and a string compare against a
     serialized origin would silently authorize nothing. */
  const positional = [];
  for (let i = 2; i < process.argv.length; i++) {
    if (process.argv[i] !== '--explore') { positional.push(process.argv[i]); continue; }
    const v = process.argv[++i];
    if (v === undefined) {
      console.error('[trusted] `--explore` was given no origin. The flag is a person authorizing navigation ' +
                    'at a host; one that names none authorizes nothing, and accepting it would leave this ' +
                    'zone reading as though a permission had been granted.');
      process.exit(2);
    }
    NAVIGATION_WIDENING.add(new URL(v).origin);
  }
  const target = positional[0];
  const bin = positional[1] || join(ENGINE, 'host', 'out', 'qjs-native-none');

  if (!target) {
    console.error('usage: node engine/trusted.mjs <url> [path-to-native-binary] [--explore <origin>]...');
    process.exit(2);
  }
  if (!existsSync(bin)) {
    console.error(`[trusted] no native host at ${bin} — build it with \`node engine/build.mjs native\`. This ` +
                  'zone drives the SHIPPED ABI (main.c) through test_forced.c\'s `--abi` arm; there is no ' +
                  'second engine here to fall back to.');
    process.exit(2);
  }

  /* THE SEED, WHICH IS THE ONE REQUEST THIS ZONE ORIGINATES. It is OBSERVED by construction: a person named
     the address and nothing in any bundle derived it. `pageUrl` is that same address because a top-level
     document's SSRF principal is itself — which is what lets `http://localhost/…` reach the person's own
     loopback (normal web rules) while a public page may not, and it is `safe-fetch.js`'s rule rather than a
     decision taken here.
     UNCREDENTIALED, STATED — see this file's header: there is no cookie jar in this process, so the mode
     would name a person who is not present. */
  const seed = await ZONE.safeFetch(target, { pageUrl: target, destination: 'document',
                                              credentialed: false });
  const seeded = replyRecord(seed, 'the seed document');
  if (!seeded)
    throw new Error(`the chokepoint refused the seed: ${seed.statusText} — the document a session is rooted ` +
                    'at is the whole of this process\'s input, and there is no tree to build from nothing. ' +
                    'The refusal names its own ground (`blocked-scheme:`, `blocked-private-from-public`, ' +
                    '`blocked-corb:<rule>:<type>`), which is the answer rather than a symptom of one');

  /* HTML §7.4.5 "Populating a session history entry" determines the loaded Document's ORIGIN over the
     RESPONSE'S URL — "set responseOrigin to the result of determining the origin given response's URL" — and
     Fetch §2.2.5 "Requests" makes that the LAST item of the URL list, not the address that was asked for. So
     the document is seated where its bytes came from, and every relative reference the page builds, every
     later SSRF principal and CORB comparison, is taken from that and never from `target`. Following the
     redirect chain happened inside the chokepoint, which is the only zone that can see it. */
  const docUrl = String(seeded.meta.urlList[seeded.meta.urlList.length - 1]);
  const docId = 'd1';

  /* ── THE ROUTING TABLE ────────────────────────────────────────────────────────────────────────────────────
     "Which instance holds which document" is the one fact only this zone has, and every line below is that
     fact being asked. It is a LIST scanned for an EXACT name and never a prefix match: a child document's name
     is prefixed by its creator's (`<creator>.<n>`) and the creator is precisely the instance that does NOT
     hold it — that is why the notice exists at all — so a prefix match routes a post straight back to its
     sender, which the engine catches at its arrival entry rather than silently performing. */
  const instances = [];
  const holderOf = (doc) => instances.find((i) => i.live && i.docId === doc) ?? null;
  const loops = [];
  let serial = 0;
  /* THE ADDRESSES THIS SESSION HAS ALREADY PROVISIONED AN INSTANCE FOR OUT OF A ROUTE DECLARATION, and the
     serial that names them. Every flow that reaches a `pushState` declares, so one route arrives many times;
     a second PROCESS for one address would run one page twice under two heaps, which is the state
     SECURITY.md's one-instance-per-cluster rule exists to prevent. It is not §NO BOUNDS' visited-set: this
     driver is ONE PASS over one document, with no frontier, no value order and no re-visit — the extension's
     zone is where an address is ranked and legitimately fetched again, and it holds no set at all. */
  const seededRoutes = new Set();
  let seedSerial = 0;

  /* A FAILED PIECE OF WORK IS RAISED WHERE IT CAN BE READ AND NOT SWALLOWED WHERE IT HAPPENS. The work is
     started when the bill is announced and awaited a round or more later, so a rejection would otherwise sit
     unhandled for exactly as long as the engine keeps running — which is the window in which Node decides an
     unhandled rejection is a process-level event. */
  let fatal = null;
  const raise = (e) => { fatal = fatal || e; };
  /* HOW MUCH WORK THIS ZONE HAS STARTED AND NOT FINISHED. It is the difference between "nobody can be paid"
     and "nobody has been paid YET", and those take opposite actions: the first ends the session with the
     zone's reason, the second waits. Without it a fetch in flight for one instance would be read as a refusal
     by another that happened to stall while it was outstanding. */
  let busy = 0;

  /* WORK THIS ZONE COULD NOT ROUTE YET — a post or an operation naming a document no instance holds. HELD and
     retried rather than failed at first sighting, and the reason is a property of the CHANNEL rather than
     caution: the child announces its bill only when the bill CHANGES (test_forced.c's abi_announce), so a
     record this zone drops is never offered again, and a `windowproxy.post` legitimately outruns the
     `navigable.create` that names its target — the create is a notice and the load it needs is a network
     round trip. `route.mjs` learned the same thing the other way round and recorded it: a permanent verdict on
     a transient condition, measured before the thing it is about has happened. What IS a failure is a record
     still held when every instance has ended, and that is reported at the bottom where it is decidable. */
  const held = [];
  const retryHeld = () => { for (let i = held.length - 1; i >= 0; i--) if (held[i].go()) held.splice(i, 1); };

  /* EVERY CROSS-AGENT OPERATION IN FLIGHT, BY THE TOKEN THIS ZONE MINTED. The token is the ZONE's rendezvous
     and never the asking flow's own request id, because an id is unique only inside the instance that minted
     it and two peers may hold the same number — which is exactly the fact only the routing zone has. */
  const reads = new Map();

  /* HTML §7.5.1 "Shared document creation infrastructure"'s ten remaining facts, for a document that is its
     own TOP-LEVEL TRAVERSABLE. They are written HERE and not assumed by the child, because the party that
     knows a document is at the top of its tree is the party that seated it — and every one of them is a
     POSITIVE statement in its own grammar rather than a blank:
       · §8.1.3.1 "Environments"' TOP-LEVEL CREATION URL is this document's own address.
       · §7.1.7 "Policy containers"' inherited container is EMPTY IN BOTH HALVES — §7.1.7 clones a CREATOR's
         and a top-level traversable has none. Two halves because CSP §2.2 "Policies" makes a CSP list "a
         struct consisting of policies (a list of policies) and a self-origin" and §2.2.2 states the second
         from outside the policy bytes.
       · §7.1.4 "Cross-origin embedder policies"' item of that container has NO empty spelling, so a container
         with no creator states that section's own initial value.
       · §7.3.1.3 "Child navigables" defines "is a child navigable" as "its parent is non-null", so `u` —
         core/frame/remote_object.h's undefined — says this navigable has none.
       · Permissions Policy §9.5 "Create a Permissions Policy for a navigable" is given "null or an element
         (container) and an origin"; `null` is that grammar's word for the first, and nothing embeds this.
       · HTML §3.1.3 "Ancestor origins"' list is `none` by the same sentence read one algorithm along. */
  const topLevelFacts = (url) =>
    [url, b64(''), '', 'unsafe-none', '', 'unsafe-none', '', 'u', 'null', 'none'];

  /* …AND THE SAME TEN FOR A PEER, TAKEN OFF THE CREATE NOTICE VERBATIM. Not one of them is derivable in the
     instance that will host the child — they are items of the CREATOR's §7.1.7 container plus three separate
     statements about the navigable's place in the creator's frame tree — which is the whole reason the notice
     carries them. The POLICY is the record's REMAINDER because a raw CSP header field value may contain HTAB
     (RFC 9110 §5.5 "Field Values"), and it crosses this channel base64'd for exactly that reason; every other
     field is an origin serialization, a §7.1.4 token, remote_object.c's tag-and-base64 identity, §4.1's
     feature tokens or a SPACE-joined origin list, none of which can contain a tab and each of which the
     engine asserts as much at the writer. */
  const createFacts = (f) =>
    [f[5], b64(f.slice(14).join('\t')), f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]];

  /* PROVISION AN INSTANCE — one PROCESS per ORIGIN-KEYED AGENT CLUSTER, which is SECURITY.md's key and not
     "per document": a SAME-ORIGIN child is a second REALM in the creator's own heap and never reaches this
     zone at all (navigable.c's `child_in_this_agent` emits no notice for one), so everything that arrives
     here is a cluster this process does not host.
     THE SHAPE IS `wpt_runner.c`'s, WHICH IS WHY THERE IS NO SECOND ONE: that host already provisions a child
     with the child's NAME, ADDRESS, ORIGIN and POLICY over a pipe and stamps the child's origin from the
     parent. What differs is only which process is trusted — there the runner owns the network, here the
     parent does — and the record on the wire is this channel's `document` line rather than an argv. */
  async function provision({ docId, url, origin, headers, bytes, facts }) {
    const child = spawn(bin, ['--abi'], { stdio: ['pipe', 'pipe', 'inherit'] });
    const e = {
      docId, docUrl: url, origin, child,
      /* THE INSTANCE'S OWN NAME IN THIS ZONE'S NAMESPACE. A document NAME is stable by requirement (the
         routing depends on it) while an instance is one session of it, and a rendezvous token has to name the
         second: without the serial a resumed instance's request 1 would land on the rendezvous of the ended
         instance's request 1 and this zone would answer a question nobody asked. */
      tag: `${docId}/s${++serial}`,
      say: (rec) => child.stdin.write(rec + '\n'),
      ready: [], answered: new Map(), stalled: false, live: true, result: null,
      /* BOTH HALVES OF HOW A CHILD ENDED, BECAUSE ONE OF THEM IS THE ONLY WAY AN ABORT IS VISIBLE FROM HERE.
         Node's `close` carries `(code, signal)` and a process killed by a signal has code `null`; taking the
         first argument alone therefore reported a `SIGABRT` — which is what every DCHECK in the engine ends
         as — as the string "null", indistinguishable from an ordinary exit this zone could not read. The
         engine's whole verification story is that its asserts fire loudly, and the party relaying that was
         erasing the one field that says one fired. */
      closed: new Promise((res) => child.on('close', (code, signal) => res({ code, signal }))),
    };
    child.stdout.setEncoding('utf8');   /* a StringDecoder, so a multi-byte character split across two chunk
                                           boundaries is not two replacement characters — the @RESULT line is
                                           the engine's own JSON and carries whatever a page put in it */
    /* A CHILD THAT ABORTS CLOSES THIS PIPE UNDER A ROUND THAT IS ALREADY BEING WRITTEN, and an unhandled
       `error` on a Node stream is a PROCESS-LEVEL throw — so the zone died of `write EPIPE` with a Node stack
       standing exactly where the child's own `@WHY` had just explained itself. That is the worst possible
       ordering: the engine names the invariant it broke, and the party whose job is to report it replaces the
       naming with an IO symptom of the naming. The race is not orderable away either — `live` goes false when
       the child's STDOUT ends, and a round queued before that arrives after it — so it is handled rather than
       avoided. The instance is marked dead and the reason RECORDED, never swallowed: the per-instance report
       at the bottom is what says an instance produced no `@RESULT`, and this is why. */
    child.stdin.on('error', (err) => {
      e.live = false;
      e.channelError = err;
      console.error(`[trusted] the channel to instance [${e.tag}] at ${e.docUrl} failed while this zone was ` +
                    `writing a round to it: ${err.code || err.message}. The instance's own words are above ` +
                    'this line — its stderr is this process\'s — and they are the diagnosis; this is the ' +
                    'pipe noticing afterwards.');
    });
    e.say(['document', url, docId, b64(fieldLines(headers)), b64(bytes), ...facts].join('\t'));
    instances.push(e);
    loops.push(drive(e));
    retryHeld();
    flush();
    return e;
  }

  /* ONE ANSWER PER REQUEST, EVER — keyed on what the request IS, and per INSTANCE because two instances
     legitimately park on the same address. A pending entry stays on the register until it is filled and the
     child re-states the whole bill whenever it changes, so without this a re-announced request would be
     fetched again and `qjs_provide` would be called twice for one park, which is engine_provide's
     answered-twice abort. It is not a bound: nothing is dropped, and a request this zone never answers keeps
     being reported until the frontier stalls on it. */
  const track = (e, key, work) => {
    busy++;
    e.answered.set(key, work.then((v) => v, raise).then(() => { busy--; retryHeld(); flush(); }));
  };

  /* A DOCUMENT LOAD, WHICH IS A DIFFERENT DECISION FROM A FETCH AND IS MADE HERE FOR BOTH OF ITS CALLERS —
     §7.4.5's load for a navigable this instance holds, and the load that roots a PEER. Both are HTML §7.4.5
     "Populating a session history entry"'s create-navigation-params-by-fetching, both are GETs, and the whole
     of what separates them is which instance the bytes end up in.
     THE REFUSAL IS THE DEFAULT AND IT NAMES ITSELF. See UNWIDENED_NAVIGATION: the create notice says nothing
     about who named the address, so provenance is UNESTABLISHED and §Attacker-sources makes that a crash at
     the decision rather than a load.
     A LOAD THAT DID NOT LOAD IS A DOCUMENT TOO. `replyRecord` answers null for the chokepoint's status 0 — a
     blocked scheme, a private target, a refused read — and HTML still gives that navigable a Document: the
     empty byte sequence is the `about:blank`-shaped one the engine's own child_document builds. The ADDRESS
     is then the one that was asked for, because a refusal has no response URL to have been redirected to, and
     saying so is a fact about this zone's own network rather than a field filled to satisfy a reader. */
  async function navigate(url, fromDocUrl, what, provenance) {
    const abs = new URL(url, fromDocUrl).href;

    /* THE PROVENANCE IS THE WHOLE DECISION AND IT IS STATED BY THE CALLER, never derived here. §Attacker-
       sources: "an OBSERVED or DERIVED address is navigated freely, a FORCED one is the deliberate per-origin
       widening", and one whose provenance is NOT ESTABLISHED crashes at the decision rather than proceeding.
       `null` IS "THE RECORD DOES NOT STATE IT" AND IS A POSITIVE ANSWER, not a caller that forgot: a
       `navigable.create` carries the child's name, address, origin, top-level creation URL, policy container,
       §7.3.1.3 links and §3.1.3 ancestors and says nothing about WHO NAMED THE ADDRESS, while a
       `document.seed` states it as its second field. Two records, two answers, and the difference decides
       whether the widening is what stands between this zone and a fetch.
       AN UNKNOWN WORD STOPS IT rather than falling to whichever arm is written as the else — the same reader
       contract `workFetch` keeps over the same vocabulary, applied to the decision that spends the network. */
    if (provenance !== null && provenance !== 'observed' && provenance !== 'derived' && provenance !== 'forced')
      throw new Error(`a document load was asked for with the provenance \`${provenance}\`, which is neither ` +
                      'null (the record does not state it) nor one of the three tokens solver/engine.h ' +
                      'declares — this zone decides whether to spend the network on that field');
    const established = provenance === 'observed' || provenance === 'derived';
    if (!established && !NAVIGATION_WIDENING.has(new URL(abs).origin))
      return { declined: `${what} ${abs} — ${provenance === 'forced' ? FORCED_NAVIGATION : UNWIDENED_NAVIGATION}` };
    /* Fetch §2.2.5's `document` DESTINATION — this is HTML's navigate algorithm's own fetch, which is that
       section's own `document` row. Not script-like, so no CORB: an HTML parser is what reads these bytes. */
    const rec = replyRecord(await ZONE.safeFetch(abs, { pageUrl: fromDocUrl, destination: 'document',
                                                        credentialed: false }),
                            `${what} ${abs}`);
    /* HTML §7.4.5 determines the loaded Document's ORIGIN over the RESPONSE'S URL — "set responseOrigin to the
       result of determining the origin given response's URL" — and Fetch §2.2.5 "Requests" makes that the LAST
       item of the URL list. Only this zone followed the redirect chain, so only this zone can say where the
       bytes came from, and the principal every cross-origin check in that document is written against is
       taken from it and never from the address the creating engine asked for. */
    if (!rec) return { url: abs, headers: [], bytes: Buffer.alloc(0) };
    return { url: String(rec.meta.urlList[rec.meta.urlList.length - 1]), headers: rec.meta.headers,
             bytes: rec.bytes };
  }

  const workFetch = async (e, method, destination, initiator, provenance, url) => {
    const abs = new URL(url, e.docUrl).href;
    /* THE PRODUCER'S VOCABULARY, CHECKED BEFORE IT IS ACTED ON. solver/engine.h declares exactly two initiator
       tokens and exactly three provenance tokens, and an unknown one would be routed by whichever arm of the
       tests below happened to be written as the else — which is the defaulted-field defect landing on a
       firing decision. */
    if (initiator !== 'parser' && initiator !== 'script')
      throw new Error(`the pending line states the initiator \`${initiator}\`, which is neither token ` +
                      'solver/engine.h declares — this zone reads that field, so an unknown value must stop ' +
                      'it rather than fall to a default');
    if (provenance !== 'observed' && provenance !== 'derived' && provenance !== 'forced')
      throw new Error(`the pending line states the provenance \`${provenance}\`, which is none of the three ` +
                      'tokens solver/engine.h declares — this zone\'s firing decision reads that field, so ' +
                      'an unknown value must stop it rather than fall to a default');
    /* THE DECISION IS READ OFF THE FIELD NOW, AND THE FIELD IS THE ENGINE'S FACT AND NOT THIS ZONE'S GUESS.
       `observed` is "a real load of this document makes exactly this request", composed at the park from the
       parser-inserted flag and the parking flow's path; it is what this zone performs. The other two are
       declined for reasons that are no longer "this zone cannot tell them apart" — see PROVENANCE_DECLINE,
       which states each of the two separately because they are refused by different things. */
    if (provenance !== 'observed') {
      e.ready.push(decline(`${method} ${abs} — ${PROVENANCE_DECLINE[provenance]}`));
      return;
    }
    /* AND `observed` IMPLIES PARSER-INSERTED, because that flag is one of the two facts it is composed from.
       Asserted rather than assumed: the two are stated independently on the line, so a provenance that
       reached `observed` by some other route is the composition having drifted from the flag it is made of. */
    if (initiator !== 'parser')
      throw new Error(`${method} ${abs} is OBSERVED and was not parser-inserted — the two are composed from ` +
                      'one flag (solver/pending.h), so this zone is being told two things about one park ' +
                      'that cannot both be true');
    if (method !== 'GET') {
      /* §Attacker-sources: a state-mutating request is NEVER fired to learn. `safe-fetch.js` enforces this by
         ABSENCE (it hardcodes `method:"GET"` and reads neither `opts.method` nor `opts.body`), so issuing one
         here would fetch a POST's address as a GET and hand the bytes back under the POST's key — the reply
         would match the request it names and still be a response the server never gave for it. */
      e.ready.push(decline(`${method} ${abs} — a non-GET park. The chokepoint is GET-only by ABSENCE ` +
                           '(SECURITY.md §Network), so this address can only be DERIVED and reported, never ' +
                           'issued; answering it with a GET\'s body would be a wrong answer rather than a ' +
                           'missing one'));
      return;
    }
    /* THE CORB CLASS IS THE REQUEST'S OWN DESTINATION, PASSED THROUGH — never a keyword decided here. Fetch
       §2.2.5 "Requests" gives every request a destination and the engine states it at each park; the
       chokepoint asks §2.2.5's SCRIPT-LIKE predicate of it, so a body that becomes executable code must be
       JS-typed or same-origin and everything else is data. This line used to hardcode `as: 'script'` on the
       strength of the INITIATOR — sound only because this zone fires nothing but parser-inserted parks, and
       wrong the moment it fires anything else, which is exactly what the provenance work above is heading
       toward. Reading it off the field costs nothing and cannot go stale. */
    const rec = replyRecord(await ZONE.safeFetch(abs, { pageUrl: e.docUrl, destination, credentialed: false }),
                            `the parser-inserted script ${abs}`);
    e.ready.push(['provide', method, url, b64(rec ? JSON.stringify(rec.meta) : 'null'),
                  rec ? b64(rec.bytes) : ABSENT].join('\t'));
  };

  /* ── WRITING, WHICH IS THE ONE THING THIS ZONE DOES ON ITS OWN CLOCK ──────────────────────────────────────
     THE CHANNEL IS HALF-DUPLEX PER INSTANCE: a child reads only after writing `stalled`, which is the one
     moment every flow of that instance is parked and blocking denies nobody the thread. So a round is written
     to an instance ONLY while it is stalled, and never otherwise.
     A ROUND THAT PAYS NOTHING IS A REFUSAL AND THE CHILD IS ENTITLED TO READ IT AS ONE, which is why a stalled
     instance with an empty queue is left UNWRITTEN rather than sent a bare `go`: what it is waiting for may be
     a program another instance is still running, and telling it "nothing" at that moment would end a live
     session over a peer's turn that had not finished. It waits, its snapshots intact, and its siblings keep
     running — which is what a cross-instance read being a SUSPEND POINT looks like from out here.
     AND THE SESSION ENDS WHEN NOBODY CAN BE PAID, WHICH IS A GLOBAL ANSWER AND NOT A PER-INSTANCE ONE. Every
     live instance stalled at once, every queue empty, and nothing in flight: that is this zone stating it
     cannot supply what the frontier is waiting for, and it is the same test `engine_run` makes one level down
     (`r == ENGINE_STEP_STALLED && filled == 0`). It is not a bound and it truncates nothing — every flow keeps
     its snapshot, and the reason travels as the zone's own words rather than as a guess made by the child. */
  const flush = () => {
    let moved = false;

    for (const i of instances) {
      if (!i.live || !i.stalled || !i.ready.length) continue;
      for (const rec of i.ready.splice(0, i.ready.length)) i.say(rec);
      i.say('go');
      i.stalled = false;
      moved = true;
    }
    if (moved || busy) return;
    const live = instances.filter((i) => i.live);
    if (!live.length || !live.every((i) => i.stalled)) return;
    for (const i of live) {
      i.say(decline(
        'every instance of this session is stalled at once, every queue is empty and this zone has no work ' +
        'outstanding — so what each frontier is parked on is something this zone will not supply, and the ' +
        'refusals above this line are why. The other instances and what each of them is owed are on their ' +
        'own channels; this one is being told only that nothing more is coming for it'));
      i.say('go');
      i.stalled = false;
    }
  };

  /* THE VECTORS A PARK SAYS ITS RESIDUE CARRIES — recorded, and the reason they cannot simply be relayed is
     the leak they exist to make finite: `world.gone` is broadcast to LIVE instances, and a parked document is
     not one, so every death announced while it is cold is lost and the resumed instance would hold a segment
     for an ended world for ever. Closing it needs a `world name -> parked document` index that outlives a
     session (the residue is what crosses the tier and can carry it); this zone holds no store at all, so it
     records the set and reports it rather than pretending to be that index. */
  const parkedWorlds = [];

  /* ASK THE INSTANCE THAT HOLDS THE DOCUMENT — the whole of the read half, and the only part of it that is
     this zone's own is WHICH instance. Every cross-agent operation names its target document as its first
     operand, which is the one fact only the routing table can act on.
     NOTHING IS ANSWERED INSIDE THIS CALL and there is nothing to read when it returns: a peer answers BY
     RUNNING A PROGRAM — the IDL getter HTML §7.2.1 "Security infrastructure for Window, WindowProxy, and
     Location objects" defines the member as — so the operation becomes a flow on that instance's own frontier
     and each completion arrives later, through its notices, once per live timeline. */
  function askOperation(e, id, op) {
    const target = op.split('\t')[1];
    const token = `${e.tag}#${id}`;
    const go = () => {
      const h = holderOf(target);
      if (!h) return false;
      reads.set(token, { asker: e, id, op, by: [] });
      h.ready.push(['perform', token, b64(op)].join('\t'));
      flush();
      return true;
    };

    if (!go()) held.push({ doc: target, what: `operation ${token} \`${op.slice(0, 90)}\` -> ${target}`, go });
  }

  /* THE ONE-WAY NOTICES, ROUTED. Every one is a CROSS-INSTANCE fact and the routing is this zone's alone; the
     records themselves are the emitting engine's grammar and cross VERBATIM, because a zone that took one
     apart and wrote it back out would be restating a grammar it does not own where nothing can check it
     against the writer. `e` is the instance that emitted it, which is what a delivery is STAMPED with —
     SECURITY.md keys authorization on what the trusted zone knows for exactly this reason, and an origin the
     untrusted engine supplied for a foreign message would defeat every `event.origin` check in every bundle. */
  async function onNotice(e, record) {
    const f = record.split('\t');

    if (f[0] === 'navigable.create') {
      /* FIFTEEN FIELDS, and the count MOVES when the record grows: the field added last is exactly the one an
         unmoved count would let arrive as `undefined`, and every one of them below is read. */
      if (f.length < 15)
        throw new Error(`a navigable.create notice was short of its fields: ${record} — navigable.c writes the ` +
                        'child, the creator, the address, the origin, §8.1.3.1\'s top-level creation URL, CSP ' +
                        '§2.2\'s self-origin, the four items of §7.1.4\'s embedder policy, §7.3.1.3\'s parent ' +
                        'navigable and its container\'s Permissions Policy §9.5 answer, HTML §3.1.3\'s ' +
                        'ancestor origins, and the policy');
      /* A DOCUMENT THIS ZONE ALREADY HOLDS AN INSTANCE FOR IS NOT PROVISIONED TWICE, and that is not a guard
         against a duplicate notice — it is what a REPLAYED document does, re-creating its child navigable and
         re-announcing it under the same name. A second instance would give one document two heaps and two
         object graphs, which is the state SECURITY.md's one-instance-per-cluster rule exists to prevent and
         which nothing downstream could tell from the routing. */
      if (holderOf(f[1])) return;
      const loaded = await navigate(f[3], e.docUrl, `navigable.create ${f[1]}`, null);
      if (loaded.declined) { e.ready.push(decline(loaded.declined)); return; }
      /* THE CHILD'S PRINCIPAL IS THE ORIGIN OF THE URL THIS ZONE FETCHED, derived here and never read off the
         notice even though the notice carries one: SECURITY.md draws the line at this exact record — a NAME
         may be minted by the untrusted side because it is only a name, while the ORIGIN is what every
         bundle's cross-origin check is written against. */
      await provision({ docId: f[1], url: loaded.url, origin: new URL(loaded.url).origin,
                        headers: loaded.headers, bytes: loaded.bytes, facts: createFacts(f) });
      return;
    }
    /* `navigable.swap <new document> <url> <origin>` — HTML §7.1.3.2 "Browsing context group switches due to
       opener policy": a navigation whose response's opener policy does not match its navigable's active
       document's builds that Document in a NEW top-level browsing context in a NEW browsing context group. It
       is the same provisioning act and a different record because §7.3.2.3 creates that context "with null,
       null, and group" — a NULL CREATOR, so there is no policy container to clone, no parent, no container
       element and no ancestor list, and the navigable's own address is its environments' top-level creation
       URL. A new GROUP is a new instance for the same reason a cross-origin child is: SECURITY.md keys one on
       `(browsing context group, origin)` and a swap changes the first half. */
    if (f[0] === 'navigable.swap') {
      if (f.length < 4 || !f[1] || !f[2])
        throw new Error(`a navigable.swap notice was short of its fields: ${record}`);
      if (holderOf(f[1])) return;
      const loaded = await navigate(f[2], e.docUrl, `navigable.swap ${f[1]}`, null);
      if (loaded.declined) { e.ready.push(decline(loaded.declined)); return; }
      await provision({ docId: f[1], url: loaded.url, origin: new URL(loaded.url).origin,
                        headers: loaded.headers, bytes: loaded.bytes, facts: topLevelFacts(loaded.url) });
      return;
    }
    /* `document.seed <address> <provenance>` — AN ADDRESS THE APPLICATION DECLARED IS A PAGE OF ITSELF, from
       HTML §7.4.4 "Non-fragment synchronous \"navigations\""' URL and history update steps (which is where
       every client-side router's `history.pushState` ends up). It is the one document this zone provisions
       that nothing NAVIGATED to: the bundle merely NAMED the route, which is precisely the surface
       §What-the-tool-produces exists for and the one forced execution alone could never reach, because the
       code walks nowhere.
       IT IS THE ONE RECORD THAT STATES ITS OWN PROVENANCE, and that is what lets it be navigated without the
       per-origin widening a `navigable.create` needs: the create says nothing about who named the address, and
       this says `derived` or `forced` outright (solver/route_seed.h — `observed` is unreachable for it,
       because no load of anything produces a declaration).
       THE DOCUMENT IS NAMED BY THIS ZONE, unlike a child navigable's. The engine mints a name for a navigable
       it CREATED because a page already holds a WindowProxy for it and a delivery has to route there; nothing
       holds a proxy for a declared route — it is a top-level traversable in a browsing-context group of its
       own — so the name is this zone's to mint, which is also SECURITY.md's rule for anything that is not
       merely a name the untrusted side already handed the page.
       ONE PER ADDRESS, WHICH IS NOT A VISITED-SET. Every flow that reaches the statement declares, and a
       router in a loop declares many times; provisioning a second PROCESS for an address this session is
       already exploring would run one page twice under two heaps. Nothing is refused — the address keeps its
       instance, and this driver's session is one pass over one document rather than a frontier that re-visits. */
    if (f[0] === 'document.seed') {
      if (f.length < 3 || !f[1] || !f[2])
        throw new Error(`a document.seed notice was short of its fields: ${record} — solver/route_seed.c ` +
                        'writes the ADDRESS and the PROVENANCE with one snprintf, and the second is the ' +
                        'whole of what this zone decides whether to load it from');
      if (f[2] !== 'derived' && f[2] !== 'forced')
        throw new Error(`a document.seed notice states the provenance \`${f[2]}\`, which is neither ` +
                        '`derived` nor `forced` — a declaration is made by RUNNING the page\'s code and no ' +
                        'load of anything produces one, so `observed` is unreachable on this record and any ' +
                        'other word is a field read at the wrong tab');
      const seedAbs = new URL(f[1], e.docUrl).href;
      if (seededRoutes.has(seedAbs)) return;
      seededRoutes.add(seedAbs);
      const loaded = await navigate(seedAbs, e.docUrl, `document.seed ${seedAbs}`, f[2]);
      if (loaded.declined) { e.ready.push(decline(loaded.declined)); return; }
      /* A TOP-LEVEL TRAVERSABLE WITH NO CREATOR — nothing embedded this document and nothing opened it, so
         §7.1.7 has no container to clone, §7.3.1.3 gives it no parent and no container element, and §3.1.3's
         steps 2-3 return the empty list. `topLevelFacts` is those ten statements, and the address it is given
         is the RESPONSE's, because §7.5.1's creationURL is where the bytes came from. */
      await provision({ docId: `seed${++seedSerial}`, url: loaded.url, origin: new URL(loaded.url).origin,
                        headers: loaded.headers, bytes: loaded.bytes, facts: topLevelFacts(loaded.url) });
      return;
    }
    /* `windowproxy.post <target doc> <world> <target origin> <base64>` — §9.4.4 across instances, relayed
       WHOLE with the sender's origin BESIDE the record rather than spliced into it. */
    if (f[0] === 'windowproxy.post') {
      if (f.length < 5) throw new Error(`a windowproxy.post notice was short of its fields: ${record}`);
      const go = () => {
        const t = holderOf(f[1]);
        if (!t) return false;
        t.ready.push(['route', e.origin, b64(record)].join('\t'));
        flush();
        return true;
      };
      if (!go()) held.push({ doc: f[1], what: `post ${f[2]} -> ${f[1]}`, go });
      return;
    }
    /* `remoteop.answer <token> <world> <completion>` — a COMPLETION the answering instance produced for an
       operation it was asked to perform. THE WORLD IS FIELD 3 AND THE COMPLETION IS THE REMAINDER, and the
       split is that way round because only one of them has a boundary: a world vector's own separators are
       ':' and ',' (world_serialize) while a completion is remote_object.c's grammar and may contain a tab.
       This zone reads neither — it routes text, and only an engine knows what a name means.
       EVERY ONE OF THEM IS RELAYED, IN ARRIVAL ORDER. A peer's document state IS its flows, so one question is
       answered once per live timeline and every one of those answers is TRUE; a zone that kept one would hand
       the asker an arbitrary timeline's value and drop the rest, and a page reading the member twice in one
       expression would get two contradictory answers with nothing able to name the disagreement. */
    if (f[0] === 'remoteop.answer') {
      if (f.length < 4 || !f[1] || !f[2] || !f[3])
        throw new Error('a remoteop.answer notice was short of its fields — every answer names the rendezvous ' +
                        `token, the TIMELINE of the flow that ran the program, and the completion: ${record}`);
      const r = reads.get(f[1]);
      if (!r)
        throw new Error(`a peer answered under a rendezvous token this zone never minted, or one already ` +
                        `handed back: ${f[1]}. The token is the zone's own name for (instance, request), so ` +
                        'an unknown one is a relay that invented a rendezvous rather than echoing one');
      /* ONE TIMELINE ANSWERS ONE QUESTION ONCE — the answering flow spends the token off its row as it
         answers, so a repeat under one token from one world is that instance emitting twice, which is
         indistinguishable from a second timeline without the world and would fork the asker an arm into a
         timeline another arm already holds. */
      if (r.by.includes(f[2]))
        throw new Error('the peer answered one cross-agent operation TWICE from the SAME timeline — token ' +
                        `${f[1]}, world ${f[2]}`);
      r.by.push(f[2]);
      r.asker.ready.push(['remote', String(r.id), f[2], b64(f.slice(3).join('\t'))].join('\t'));
      flush();
      return;
    }
    /* `remoteop.retracted <token>` — an operation handed BACK, because the instance holding it parked while
       still holding it. The record and the token do not have one lifetime and that is the whole reason this
       arm exists: the record is text whose names are global, while the TOKEN is a name in THIS zone's
       namespace and dies with this zone's session.
       IT IS RE-ISSUED HERE RATHER THAN WAITED FOR, and that is where this channel differs from `route.mjs`.
       There the asking instance's whole request register is re-read every step, so a forgotten read is
       re-offered by the engine itself; here the child announces its bill only when the bill CHANGES, and a
       retraction changes the bill of the PERFORMING instance rather than the asking one. So a zone that merely
       forgot the token would leave the asking flow parked on a question nobody holds any more, with nothing
       anywhere able to say so. */
    if (f[0] === 'remoteop.retracted') {
      if (f.length < 2 || !f[1]) throw new Error(`a remoteop.retracted notice carried no token: ${record}`);
      const r = reads.get(f[1]);
      if (!r) throw new Error(`a retracted token names no operation this zone asked: ${f[1]}`);
      reads.delete(f[1]);
      askOperation(r.asker, r.id, r.op);
      return;
    }
    /* `world.gone <name>` — a world of the sending instance is finished with, so every peer holding a COW
       segment keyed on that name can drop it. BROADCAST, and that is the design rather than this zone being
       lazy: the sending engine deliberately does not record which peers a flow reached, because releasing a
       world with no segment is a no-op and tracking it would be state kept only to avoid one. Only the zone
       knows what the other instances are, which is this record's whole reason to exist. */
    if (f[0] === 'world.gone') {
      if (f.length < 2 || !f[1]) throw new Error(`a world.gone notice carried no world name: ${record}`);
      for (const i of instances) if (i !== e && i.live) i.ready.push(`world-gone\t${f[1]}`);
      flush();
      return;
    }
    if (f[0] === 'world.parked') {
      if (f.length < 2 || !f[1]) throw new Error(`a world.parked notice carried no vector: ${record}`);
      parkedWorlds.push(f[1]);
      return;
    }
    throw new Error(`the host emitted a NOTICE under the operation \`${f[0]}\`, which this zone does not ` +
                    `route: ${record} — a notice this zone drops is a document nothing runs or a message ` +
                    'nothing delivers, and every read through it parks its flow for the rest of the session');
  }

  async function onLine(e, line) {
    if (line.startsWith('@RESULT ')) { e.result = line.slice('@RESULT '.length); return; }
    const f = line.split('\t');
    if (f[0] === 'fetch') {
      if (f.length !== 6)
        throw new Error('the host announced a fetch that is not ' +
                        '`fetch<TAB>METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>URL`: ' +
                        `${line} — the bill is ` +
                        'the pending line verbatim and this zone splits it where the engine joined it, so a ' +
                        'short record is the two grammars having parted');
      const [, method, destination, initiator, provenance, url] = f;
      const key = `${method}\t${url}`;
      if (e.answered.has(key)) return;
      track(e, key, workFetch(e, method, destination, initiator, provenance, url));
    } else if (f[0] === 'request') {
      const id = Number(f[1]), op = f.slice(2).join('\t');
      const key = `req:${f[1]}`;
      if (e.answered.has(key)) return;
      if (!Number.isInteger(id))
        throw new Error(`the host announced a request whose id is not a number: ${line} — the id is the ` +
                        'rendezvous inside that instance, so an unreadable one would answer whatever request ' +
                        'zero belongs to');
      e.answered.set(key, Promise.resolve());
      /* EVERY CROSS-AGENT OPERATION ROUTES THE SAME WAY — a member of a navigable's Window, and the four
         internal methods a lent object performs. They differ in what the peer resolves, not in who resolves
         it, which is why this is a PREFIX and not a list: an operation added to remote_object.c reaches its
         instance with nothing here to remember. */
      if (op.startsWith('windowproxy.get\t') || op.startsWith('object.')) {
        askOperation(e, id, op);
      } else if (op.startsWith('document.fetch\t')) {
        /* §7.4.5's load for a document THIS instance holds — answered here rather than routed, because
           routing a same-origin load to a peer would be answering it out of another agent.
           THE PROVENANCE IS `null` AND IT IS STATED, which is `navigate`'s own word for "the record does not
           state it": a `document.fetch` names an ADDRESS and nothing else, exactly as a `navigable.create`
           does, so the two callers of that function reach the same decision from the same standing — this
           one is the SAME-AGENT half of the act whose cross-agent half is provisioned above it, and it would
           be incoherent for the agent boundary to change what a person had authorized. It was OMITTED here,
           which is not the same mistake as passing the wrong word and is a worse one: `undefined` is neither
           `null` nor one of the three tokens, so the check written to stop an unknown provenance THREW on the
           first same-origin child navigable of any session — the one shape it was never meant to be about. */
        track(e, key, (async () => {
          const loaded = await navigate(op.slice('document.fetch\t'.length), e.docUrl, 'document.fetch',
                                        null);
          if (loaded.declined) { e.ready.push(decline(loaded.declined)); return; }
          /* THE ANSWER IS §7.4.5's `{url, headers}` PLUS THE DOCUMENT AS BYTES: a Document is parsed from a
             byte sequence, so the bytes travel BESIDE the record rather than through a decode this zone would
             have had to run first — which is what left HTML §8.1.4.2's classic-script decode nothing to
             honour. The header list crosses as the response's HTTP FIELD LINES, which is where §7.1.3's opener
             policy and that document's own CSP are read from. The trailing `0` is ECMA-262 6.2.4's NORMAL
             completion: this zone fetched bytes rather than relaying another instance's program, so it has
             nothing to have thrown with. */
          e.ready.push(['answer', String(id),
                        b64(JSON.stringify({ url: loaded.url, headers: fieldLines(loaded.headers) })),
                        '0', b64(loaded.bytes)].join('\t'));
        })());
      } else {
        /* A REQUEST THIS ZONE DOES NOT CARRY IS REFUSED IN WORDS AND NOT LEFT SILENT: leaving it unanswered
           parks the asking flow for the rest of the session while the frontier reports nothing, which from
           outside is indistinguishable from a page that is merely slow. */
        e.ready.push(decline(
          `request ${id} \`${op.slice(0, 120)}\` — an operation this zone does not carry. It routes ` +
          '`windowproxy.get`, the `object.*` internal methods and `document.fetch`, and an operation outside ' +
          'those is one the engine emits for and nothing performs'));
      }
    } else if (f[0] === 'notice') {
      await onNotice(e, line.slice('notice\t'.length));
    } else if (line === 'stalled') {
      e.stalled = true;
      /* EVERYTHING OUTSTANDING ACROSS EVERY INSTANCE IS SETTLED FIRST, not just this one's: a payment held
         back here is one the child will read as a refusal, and the work that produces it may have been
         started by a sibling's round. */
      await Promise.all(instances.flatMap((i) => [...i.answered.values()]));
      if (fatal) throw fatal;
      retryHeld();
      flush();
    } else {
      throw new Error(`the host wrote a record under the verb \`${f[0]}\`, which this zone does not carry: ` +
                      `${line}`);
    }
  }

  async function drive(e) {
    let buf = '';

    for await (const chunk of e.child.stdout) {
      buf += chunk;
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl);
        buf = buf.slice(nl + 1);
        await onLine(e, line);
      }
    }
    if (buf) await onLine(e, buf);
    e.live = false;
    ({ code: e.exit, signal: e.signal } = await e.closed);
    /* HOW THIS INSTANCE ENDED, IN ONE PHRASE, so every reader below states the same fact the same way. */
    e.ended = e.signal ? `on ${e.signal}` : `with code ${e.exit}`;
    /* AN INSTANCE LEAVING CAN BE WHAT UNBLOCKS THE REST — the stuck answer is GLOBAL, so it is re-asked here
       as well as at every stall. */
    retryHeld();
    flush();
  }

  const root = await provision({ docId, url: docUrl, origin: new URL(docUrl).origin,
                                 headers: seeded.meta.headers, bytes: seeded.bytes,
                                 facts: topLevelFacts(docUrl) });

  /* SHIFTED RATHER THAN `Promise.all`, because the list GROWS: a peer provisioned three rounds in appends its
     driver to this queue, and an `all` taken over the queue as it stood would return before that peer's
     channel had closed — this process would then exit with a live child on the other end of a pipe. */
  while (loops.length) await loops.shift();
  if (fatal) throw fatal;

  /* WHAT COULD NOT BE ROUTED FOR THE WHOLE SESSION. A held record is an asking flow parked on a question
     nothing will ever answer, so it is a failure and not a note — reported HERE because "no holder YET" and
     "no holder EVER" are the same observation until every instance has ended.
     AND ONCE THEY HAVE, THEY STOP BEING ONE OBSERVATION AND THIS ZONE OWNS THE DIFFERENCE. Two states end up
     here and they are different failures with different next diffs: a document NO INSTANCE WAS EVER
     PROVISIONED FOR (the create was declined, or a record named a document nothing announced — a routing or
     policy failure), and a document an instance HELD AND THEN LEFT (the holder ended while an asker was still
     parked on it — a LIFETIME failure, and the asker's read is one §7.2.1 would still answer). Reporting both
     as "no instance ever held" was measurable and measured: a three-instance chain whose middle process
     aborted printed it about a document that had been provisioned, run, and answered an earlier read from the
     same asker. That is the defect CLAUDE.md names at §@S — a rung whose ABSENCE and whose ZERO read alike —
     performed on the one report that says what this zone could not do. The routing table still holds the
     ended instance, so the answer is a lookup and never an inference. */
  if (held.length) {
    const everHeld = (h) => instances.filter((i) => i.docId === h.doc);
    const orphaned = held.filter((h) => everHeld(h).length);
    const unknown = held.filter((h) => !everHeld(h).length);
    if (orphaned.length)
      console.error(`[trusted] ${orphaned.length} record(s) named a document whose instance HELD IT AND THEN ` +
                    'LEFT, so their askers were parked on a question that had an answerer and no longer ' +
                    `does: ${orphaned.map((h) => `${h.what} (instance ${everHeld(h).map((i) => i.tag)
                      .join(', ')} ended ${everHeld(h).map((i) => i.ended).join(', ')})`).join(' ; ')}`);
    if (unknown.length)
      console.error(`[trusted] ${unknown.length} record(s) named a document NO instance was ever provisioned ` +
                    `for, so their askers were parked on a question nothing answered: ${
                      unknown.map((h) => h.what).join(' ; ')}`);
  }
  if (parkedWorlds.length)
    console.error(`[trusted] ${parkedWorlds.length} foreign world vector(s) went to a residue this zone holds ` +
                  'no index for — a `world name -> parked document` index that outlives a session is what ' +
                  `would close it: ${parkedWorlds.join(' ')}`);
  /* EVERY PEER'S FINDINGS ARE REAL AND THIS ZONE PRINTS ONE DOCUMENT, WHICH IS STATED RATHER THAN LEFT TO BE
     DISCOVERED. A cross-origin child is a document that was explored, so its `@RESULT` is a finding set like
     any other; MERGING several into one report is a grammar this zone does not have — two documents, two
     bundle ids, two frontiers — and inventing one here would be this zone deciding what a finding set means. */
  for (const i of instances)
    if (i !== root)
      console.error(`[trusted] peer instance [${i.tag}] at ${i.docUrl} ended ${i.ended} and ` +
                    (i.result === null ? 'produced no @RESULT'
                                       : `produced a result of ${i.result.length} bytes, which this zone does ` +
                                         'not merge into the seed\'s'));

  if (root.result === null) {
    console.error(`[trusted] the host produced no @RESULT (ended ${root.ended}) — an ABSENT result and a result ` +
                  'that found nothing are different facts and this is the first, so nothing here may be ' +
                  'reported as a page that was analysed and found clean.');
    process.exitCode = root.exit || 1;
    return;
  }
  console.log(root.result);
  /* `exitCode` AND NOT `exit()`: the result is a line on a pipe and `process.exit` does not wait for it to
     drain, so the one thing this process exists to produce is the one thing that would be truncated. */
  /* A CHILD KILLED BY A SIGNAL HAS NO EXIT CODE, AND `exitCode = null` IS NODE'S WORD FOR SUCCESS. So a
     session whose engine printed an `@RESULT` and then ABORTED — a DCHECK firing after the result line, which
     is exactly the ordering `@RESULT`-then-teardown makes possible — left this process exiting 0 while the
     assert that fired was on its own stderr. The signal is the fact, so it decides the status. */
  process.exitCode = root.signal ? 1 : root.exit;
}

await main();
