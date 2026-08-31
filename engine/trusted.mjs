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
 * which `safe-fetch.js` scopes to a request that is credentialed AND NOT `observed`, is correctly inert here
 * for the same reason it is armed there: the harm it prevents needs the session, and this process has none.
 *
 * WHAT THIS ZONE WILL FIRE, AND WHAT IT REFUSES — AND IT IS NO LONGER THIS ZONE THAT DECIDES.
 * CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE: every outbound request states whether it is OBSERVED, DERIVED
 * or FORCED, and a reply to a FORCED request is evidence about what a server says to a request no client
 * makes. Every request this process makes now STATES that word to the chokepoint, and `safe-fetch.js`'s
 * `_firingRefusal` answers — one function, one per-origin widening table, read by this host and by the
 * offscreen alike, because it is the same file loaded in both. What this zone contributes is the two facts
 * only it can state:
 *   · THE SEED IS `observed`. This zone originated it, from an address a person typed. Nothing derived it.
 *   · `--explore <origin>` IS A PERSON'S SENTENCE and it writes into that shared table, so one widening
 *     covers every act at that host — a navigation, a park, an XHR — instead of the navigations alone that a
 *     Set held in this file could ever have covered.
 * WHAT THAT MEANS IN PRACTICE: `observed` and `derived` are FIRED, `forced` waits on a widening. The previous
 * state of this file declined every DERIVED park, and its own decline text named this diff as the fix ("firing
 * it is the ACTIVE DISCOVERY CLAUDE.md §Attacker-sources calls REQUIRED. What is missing is the authorisation
 * … Saying it about fetches is the next diff here"). A refusal still travels with its reason: `workFetch`
 * declines in the CHOKEPOINT's words and the child prints them at the stall, because the party that refused is
 * the party that knows why. And the divergence this paragraph used to record — that this zone was stricter
 * than `bridge.js`, which fired every pending line — is gone by CONSTRUCTION rather than by agreement: there
 * is one policy and neither host holds a copy of it.
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
 * AND THE ROUTING ABOVE IS EXERCISED BY DEFAULT NOW, WHICH IT WAS NOT. Provisioning a peer means LOADING a
 * document at an address the PAGE'S OWN CODE chose, and this paragraph used to say this zone could not
 * establish that address's provenance — so, CLAUDE.md §Attacker-sources being explicit that a navigation whose
 * provenance is not established CRASHES at the decision, it refused every one of them and the whole
 * cross-instance surface below was a design that had never run. The three provisioning records now STATE what
 * the navigation is evidence of (`navigable.create` field 15, `navigable.swap` field 4, and the load job's own
 * `document.fetch`), composed in the engine from the one fact only the engine can see: whether the flow that
 * built the address had stood on an arm its own concrete example contradicts. An OBSERVED or DERIVED address
 * is navigated; a FORCED one waits on the PER-ORIGIN WIDENING (`--explore <origin>`), which is a person saying
 * "at this host, navigate what the bundle reaches past a forced gate" and is the zone's to own; and a record
 * that states nothing now THROWS rather than declining, which is the crash that bullet asks for.
 * WHAT REMAINS OPEN IS THE CREDENTIAL HALF AND THE REPLY'S GRADE, NOT THE FIRING DECISION. This process has
 * no cookie jar, so nothing here can act as a person; the offscreen can, and `bridge.js` still leaves
 * `msg.credentialed` unwritten for a learned GET. And a FORCED reply, once a widening lets one be fetched,
 * must be carried as FORCED by the engine and never merged into the observed pool (§@H) — that carrying is
 * the subproblem AFTER this one and it is named at the widening rather than assumed away.
 * A CONSTANT NAMED `UNSTATED_PROVENANCE` ONCE STOOD AT A CROSS-REFERENCE HERE, dead and unreferenced,
 * describing a zone that could not tell a page `fetch()` from a forced arm's. `PROVENANCE_DECLINE`,
 * `NAVIGATION_WIDENING` and `FORCED_NAVIGATION` have now followed it out for the mirror-image reason: not
 * that they described an absence that had been filled, but that they described a DECISION this file no longer
 * makes. A rule that outlives the place it belongs reads as authoritative from the wrong zone, which is the
 * stale-`DFAIL` failure with a network policy on it. */
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
  /* AND THE FIRING POLICY BESIDE IT, ASSERTED AT THE SAME DOOR AND FOR A SHARPER REASON. The chokepoint's
     absence is a process with no network; the POLICY's absence is a process that reaches the network with the
     one decision CLAUDE.md puts at that chokepoint silently missing — `--explore` would widen nothing while
     reading as though it had, and `navigate`/`workFetch` would take an `undefined` refusal as permission.
     That is the defaulted-read defect standing on a security decision, so it is a load-time abort rather than
     a TypeError three calls into a request. */
  /* `safeFetchMethodRefusal` IS ON THIS LIST FOR THE SAME REASON AND WITH THE SAME FAILURE SHAPE. It is the
     METHOD half of whether an act may be spent (RFC 9110 §9.2.1 "Safe Methods"), which the chokepoint enforces
     by ABSENCE — so nothing downstream can OBSERVE it, and both hosts wrote their own copy and answered
     differently. A zone that obtained the chokepoint without it would take an `undefined` refusal as permission
     and fire every method it was parked on. */
  for (const n of ['safeFetchWiden', 'safeFetchFiringRefusal', 'safeFetchWidenedOrigins',
                   'safeFetchMethodRefusal'])
    if (typeof sandbox[n] !== 'function')
      throw new Error(`extension/lib/safe-fetch.js did not install \`${n}\` — the firing decision and its ` +
                      'per-origin widening are that file\'s, read by this host and by the offscreen from the ' +
                      'same table, and a zone that obtained the chokepoint without them would fire every ' +
                      'grade while `--explore` authorized nothing');
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
  /* AND THE GRADE OF THE REFUSAL, WHICH IS THE ONE THING ABOUT A STATUS-0 RECORD A HOST MAY NOT WORK OUT FOR
     ITSELF. `status === 0` says a request produced no reply; it does not say whether a BROWSER would have
     refused it too, and that is the whole of the decision. `safe-fetch.js` applied the rule, so it states the
     answer on the record — `network` where Fetch §5.6 "Fetch methods"' network error is the FAITHFUL outcome
     (a `file:` scheme is Fetch §4.3 "Scheme fetch"'s own "Return a network error"; a §4.10 "CORS check"
     failure is §4.4 "HTTP fetch"'s), `decline` where only this tool refuses and there is no fact to relay. */
  if (r.refusal !== null &&
      (typeof r.refusal !== 'object' || (r.refusal.kind !== 'network' && r.refusal.kind !== 'decline') ||
       r.refusal.reason !== r.statusText))
    throw new Error(`safeFetch answered ${where} with a refusal it did not grade — the record carries ` +
                    '`refusal: null` for a reply and `{kind, reason}` for a refusal, with `kind` one of ' +
                    '`network` (a real browser refuses this same request, so §5.6\'s network error is the ' +
                    'faithful answer) and `decline` (only this tool refuses, so the flow parks). A host that ' +
                    'cannot read the grade re-derives it, which is how a second copy of a policy gets written');
  if ((r.status === 0) !== (r.refusal !== null))
    throw new Error(`safeFetch answered ${where} with a status and a refusal that disagree — ` +
                    'status 0 is the one status no HTTP response has and every refusal answers with it, so a ' +
                    'record carrying one without the other is a decline arriving as a reply');
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

/* …AND THE SAME WORDS ADDRESSED TO THE PARK THEY REFUSE, WHICH IS A DIFFERENT RECORD BECAUSE IT IS A DIFFERENT
   ACT. The one above owes nothing: it explains a NOTICE this zone would not provision, or states that nothing
   more is coming for an instance. This one ANSWERS one of the requests `qjs_pending` listed, and it is keyed on
   the same `(method, url)` pair a `provide` is because it answers the same question — so the engine can act on
   it: the record carries the refusal, the flow parked on it keeps its snapshot, and the engine forks the arm
   that runs the page's failure path (solver/engine.h's engine_decline).
   THE PAIR IS THE JOIN'S OWN, WHICH IS WHY IT IS `url` AND NEVER `abs`. The engine parked on the address it
   wrote on the pending line; `abs` is this zone resolving that address against the document in order to apply
   a policy to it, and a refusal that named the resolved form would refuse a request no register holds.
   THE REASON IS BASE64 AND THE PAIR IS NOT, exactly as `provide` writes them: a method is a token and a URL has
   had every tab and newline removed by URL Standard §4.4 "URL parsing", while a reason is a whole English
   sentence this zone composed and the channel is line-oriented. */
function declineRequest(method, url, reason) { return `decline-request\t${method}\t${url}\t${b64(reason)}`; }

/* THE FIRING DECISION IS NOT HERE ANY MORE, AND ITS ABSENCE IS THE DIFF RATHER THAN A DELETION.
 * Three constants stood in this space: `PROVENANCE_DECLINE`, a per-class refusal for every DERIVED and FORCED
 * park; `NAVIGATION_WIDENING`, a Set this file held; and `FORCED_NAVIGATION`, the sentence it refused a
 * navigation with. Every one of them was the RIGHT POLICY IN THE WRONG PLACE, and the proof is what the other
 * host was doing meanwhile: `bridge.js` fired every DERIVED and FORCED park uncredentialed while this file
 * declined all of them, and each zone's text explained itself perfectly. One question, two answers, neither of
 * them the policy — and the ONE thing that could not happen while it stayed that way is the thing this seam is
 * for: an origin a person widens is widened for the whole tool, not for whichever host they widened it in.
 * IT LIVES AT THE CHOKEPOINT NOW, WHICH IS WHERE CLAUDE.md PUTS IT: "the engine holds no network policy by
 * construction, so `safeFetch` decides, from the provenance the request declares beside its method and
 * credential state". This file loads `extension/lib/safe-fetch.js` VERBATIM into a realm of its own rather
 * than restating its rules, so the policy arrives here the same way the SOP, the CORS gate, the PNA guard and
 * the destructive-path deny list already do — `_firingRefusal` and its widening table, read by both hosts
 * because it is one table. `--explore <origin>` still exists and is still a person's sentence; it now writes
 * into that table (`ZONE.safeFetchWiden`) rather than into a Set this process alone could see.
 * AND IT NO LONGER COVERS NAVIGATIONS ALONE. The old comment gave a reason for that narrowness — "a
 * running-code FETCH has a second question this flag cannot answer (which CORB class the body is)" — and the
 * reason had already stopped being true: Fetch §2.2.5 "Requests"' DESTINATION rides every pending line and
 * `_destinationOf` answers the CORB question from it, for a park this zone has never seen. A rule kept alive
 * by a reason that has stopped being true is the stale-comment failure with a network policy attached, so it
 * goes out with the reason.
 * WHAT A REFUSAL STILL OWES THIS CHANNEL IS ITS WORDS, and it still gets them — `workFetch` declines with the
 * chokepoint's own `statusText`, which is the party that refused saying why, one zone further in than before.
 * THE ONE COMBINATION THAT IS NEVER A SETTING REMAINS UNREACHABLE FROM THIS PROCESS BY CONSTRUCTION —
 * credentialed AND state-mutating AND forced. This process has NO COOKIE JAR (see this file's header: Node's
 * `fetch` has no cookie store, so `credentials:"include"` would attach nothing and name a person who is not
 * present), and `safe-fetch.js` is GET-only BY ABSENCE. Both other conjuncts are false at every setting of
 * the widening, including its widest. */


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
      console.error('[trusted] `--explore` was given no origin. The flag is a person authorizing this tool to ' +
                    'fire what a bundle only reaches past a FORCED gate at a host; one that names none ' +
                    'authorizes nothing, and accepting it would leave this zone reading as though a ' +
                    'permission had been granted.');
      process.exit(2);
    }
    /* AND IT IS WRITTEN INTO THE CHOKEPOINT'S OWN TABLE, WHICH IS THE WHOLE OF WHAT THE FLAG NOW DOES. It used
       to add to a Set this file held, so a widening said something about NAVIGATIONS made by this process and
       nothing about anything else — while the same word arriving on a pending line was refused by a second
       rule beside it. `ZONE.safeFetchWiden` is `safe-fetch.js`'s `_EXPLORED`, the table `_firingRefusal`
       reads, so one sentence from a person now covers every request this tool makes at that host: a
       navigation, a park, an XHR. The chokepoint validates the value (a tuple origin, never a URL and never
       an opaque `null`) and aborts on anything else, which is why nothing is checked here. */
    ZONE.safeFetchWiden(new URL(v).origin);
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
     would name a person who is not present.
     AND `observed` IS STATED RATHER THAN LEFT OFF, which is the same sentence the first line of this comment
     already makes: a person typed this address. The chokepoint refuses a request whose grade it was not told
     (`_provenanceOf` is a CHECK), so there is no arm here that could take a default — but the word is also
     what the deny list is scoped by, and a seed is exactly the population that scoping is for. */
  const seed = await ZONE.safeFetch(target, { pageUrl: target, destination: 'document',
                                              provenance: 'observed', credentialed: false });
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

  /* HTML §7.5.1 "Shared document creation infrastructure"'s eleven remaining facts, for a document that is its
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
       · HTML §3.1.3 "Ancestor origins"' list is `none` by the same sentence read one algorithm along.
       · HTML §7.1.5 "Sandboxing"'s creation sandboxing flag set is EMPTY by that sentence read one algorithm
         further: the section fills a top-level browsing context's set from its POPUP sandboxing flag set,
         which is empty when the context is created and which only §7.3.1.7 "Navigable target names"'s rules
         for choosing a navigable ever populate — and nothing chose this one. `none` is that grammar's word
         for the empty set, stated rather than left blank because a navigable either carries flags or carries
         none and both are facts a host states. */
  const topLevelFacts = (url) =>
    [url, b64(''), '', 'unsafe-none', '', 'unsafe-none', '', 'u', 'null', 'none', 'none'];

  /* …AND THE SAME ELEVEN FOR A PEER, TAKEN OFF THE CREATE NOTICE VERBATIM. Not one of them is derivable in the
     instance that will host the child — they are items of the CREATOR's §7.1.7 container plus three separate
     statements about the navigable's place in the creator's frame tree — which is the whole reason the notice
     carries them. The POLICY is the record's REMAINDER because a raw CSP header field value may contain HTAB
     (RFC 9110 §5.5 "Field Values"), and it crosses this channel base64'd for exactly that reason; every other
     field is an origin serialization, a §7.1.4 token, remote_object.c's tag-and-base64 identity, §4.1's
     feature tokens or a SPACE-joined origin list, none of which can contain a tab and each of which the
     engine asserts as much at the writer. §7.1.5's flag set is the one whose members are joined by COMMA
     rather than by SPACE, because that section's flag names CONTAIN spaces.
     THE POLICY IS THE REMAINDER FROM FIELD 16 AND THAT INDEX MOVES WITH THE RECORD — a slice one short does
     not fail, it silently joins the field before the policy onto the FRONT of it, which is a creator's CSP
     arriving at the peer with an extra unparseable directive and every `@S` verdict decided against it. It
     moved when the navigation's PROVENANCE was inserted at field 15, and the provenance is deliberately NOT
     in this list: these are the facts the CHILD instance is built from, and what a navigation is evidence of
     is a fact about the LOAD this zone performs rather than about the Document it produces. */
  const createFacts = (f) =>
    [f[5], b64(f.slice(16).join('\t')), f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14]];

  /* PROVISION AN INSTANCE — one PROCESS per ORIGIN-KEYED AGENT CLUSTER, which is SECURITY.md's key and not
     "per document": a SAME-ORIGIN child is a second REALM in the creator's own heap and never reaches this
     zone at all (navigable.c's `child_in_this_agent` emits no notice for one), so everything that arrives
     here is a cluster this process does not host.
     THE SHAPE IS `wpt_runner.c`'s, WHICH IS WHY THERE IS NO SECOND ONE: that host already provisions a child
     with the child's NAME, ADDRESS, ORIGIN and POLICY over a pipe and stamps the child's origin from the
     parent. What differs is only which process is trusted — there the runner owns the network, here the
     parent does — and the record on the wire is this channel's `document` line rather than an argv. */
  /* …AND `referenced` IS THE ONE FACT OF THAT PROVISIONING THAT IS NOT ABOUT THE DOCUMENT. A document a peer
     holds a WindowProxy for may not run its timelines out: its last flow reports host-owed instead of
     finishing, so there is still somewhere for a `windowproxy.get` or a delivery to arrive
     (solver/engine.h's engine_set_referenced, reached through `qjs_set_referenced`). Without it a peer this
     zone provisioned ran its scripts, drained, closed — and the first cross-origin read its creator made
     arrived at a document with no timeline to answer in. The engine crashes there, at the far end, which is
     the right place for the crash and the wrong place for the fix: an instance exists because SOME OTHER
     AGENT created its navigable, and the party that knows that is the one holding the routing table.
     THE QUESTION IS "DOES ANOTHER INSTANCE HOLD A WINDOWPROXY FOR *THIS* DOCUMENT", AND IT IS ASKED PER ARM
     BECAUSE THE ANSWER IS NOT THE SAME FOR ALL FOUR. "The engine minted the name" is the tempting shortcut and
     it is WRONG for one of them: §7.1.3.2's swap mints a name too, and its step 10 note ("browsingContext will
     not be used by the new Document that we are about to create") DISCARDS the browsing context the creator's
     handle names — core/frame/browsing_context_group.c performs that discard and says in as many words that a
     read through the handle afterwards answers about the OLD document. So nothing holds a proxy for the
     swapped-TO document, and holding its frontier open would be waiting for a question that cannot be asked.
     BOTH ERRORS ARE SILENT AND THEY ARE NOT SYMMETRIC. Understating it drains a peer that was about to be
     read, and the crash lands in the ASKING instance's engine, one process away from the zone that decided
     it. Overstating it keeps a frontier alive until the session ends, which costs a process and truncates
     nothing. That is not a licence to guess high: it is why the rule is stated per arm with its reason, so
     the next arm has to answer the question rather than inherit an answer. */
  async function provision({ docId, url, origin, headers, bytes, facts, referenced }) {
    if (referenced !== 0 && referenced !== 1)
      throw new Error(`an instance was provisioned for ${url} without stating whether a peer holds a ` +
                      'reference into its document — the flag decides whether its timelines may finish, so ' +
                      'an unstated one is a peer that either drains before it is asked anything or waits ' +
                      'for a question nobody can ask, and neither is visible from here');
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
    e.say(['document', url, docId, b64(fieldLines(headers)), b64(bytes), ...facts,
           String(referenced)].join('\t'));
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
     THE DECISION IS READ OFF THE RECORD AND IS NO LONGER A BLANKET REFUSAL. Every record that reaches this
     function states what the navigation is evidence of — the create notice, §7.1.3.2's swap, the load job's
     own `document.fetch` and a route declaration — so an OBSERVED or DERIVED address is navigated freely
     (§Attacker-sources: that IS the capability, reaching what a bundle NAMES and no link exposes) and only a
     FORCED one waits on the per-origin widening.
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
       `null` WAS A FOURTH ANSWER HERE — "the record does not state it" — AND IT IS GONE WITH THE RECORDS THAT
       COULD NOT STATE IT. It was an honest description of a gap and it had exactly the failure mode a
       tolerated absence has: `navigable.create`, `navigable.swap` and `document.fetch` all passed it, so the
       arm below refused every child navigable and every peer this zone was ever told about, at every origin
       nobody had widened, which is every origin by default — and the refusal read as a policy working rather
       than as a capability missing. All three records carry the word now (core/frame/navigable.c and
       core/frame/browsing_context_group.c).
       BOTH THE ENUMERATION AND THE WIDENING ARE THE CHOKEPOINT'S NOW, AND THE CRASH IS STRICTLY STRONGER FOR
       IT. This function held a `throw` over the vocabulary and an arm over the widening; both are gone into
       `safe-fetch.js`, where `_provenanceOf` is a `CHECK` — fatal in dev AND release, because the arm an
       unstated grade falls to is the one that spends the network — and `_firingRefusal` reads the one
       widening table both hosts share. So the crash §Attacker-sources asks for still happens, at the line
       that opens the socket, for BOTH hosts, and this file no longer has a copy of it to drift.
       WHAT IS ASKED HERE IS NOT THE POLICY BUT THE SHAPE OF THIS CHANNEL'S ANSWER, and the two are different
       questions. A refusal the NETWORK made — a blocked scheme, a private target, a refused read — leaves a
       navigable that EXISTS and shows an error page: `replyRecord` answers null and the empty byte sequence
       below is HTML's own Document for it. A refusal this ZONE made is not that; nothing is provisioned at
       all, and the creating engine is told so with the reason, because `abi_report_declines` is the only
       thing that will ever print why a peer does not exist. `safeFetchFiringRefusal` is `_firingRefusal`
       itself, asked by a caller that needs the GRADE in order to say which — never a second copy of the rule,
       and never a string match on the `statusText` the same refusal would arrive in one call later. */
    const refusal = ZONE.safeFetchFiringRefusal(provenance, abs);
    if (refusal)
      return { declined: `${what} ${abs} — a DOCUMENT LOAD whose address stands on a ${refusal.toUpperCase()} ` +
                         'arm at an origin nobody has widened for exploration. CLAUDE.md §Attacker-sources ' +
                         'makes exactly this the per-origin widening ("default conservative, widened ' +
                         'deliberately per origin, never inferred from a site looking like a test"), so this ' +
                         'is the policy\'s ANSWER rather than a capability that is missing. Pass `--explore ' +
                         '<origin>` to widen it, and note what that obliges: §@H makes the reply to a forced ' +
                         'request evidence about what a server says to a request no client makes, so its ' +
                         'values are carried as FORCED and never merged into the observed pool. Until then ' +
                         'the address is DERIVED IN FULL and REPORTED, which §Attacker-sources says is not a ' +
                         'gap in the report but IS the report' };
    /* Fetch §2.2.5's `document` DESTINATION — this is HTML's navigate algorithm's own fetch, which is that
       section's own `document` row. Not script-like, so no CORB: an HTML parser is what reads these bytes. */
    const rec = replyRecord(await ZONE.safeFetch(abs, { pageUrl: fromDocUrl, destination: 'document',
                                                        provenance, credentialed: false }),
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
    /* THE PROVENANCE'S OWN VOCABULARY IS NOT RE-CHECKED HERE, and its absence is the unification rather than a
       check dropped. `_provenanceOf` is a `CHECK` in `safe-fetch.js` — fatal in dev AND release, one door, one
       message, both hosts — and this file loads that file verbatim. A second enumeration in a host is the
       thing that goes stale when a fourth token is added; the initiator's stays because the CHOKEPOINT reads
       no such field and this zone does (the implication below).
       THIS ZONE FIRES DERIVED PARKS NOW, WHICH IS THE CAPABILITY THIS DIFF ADDS AND NOT A RELAXATION. The arm
       that stood here declined every park that was not `observed`, and its own text said what was wrong with
       it: "a DERIVED park — the page's own code computed this address from real inputs, so it is a fact about
       the app and firing it is the ACTIVE DISCOVERY CLAUDE.md §Attacker-sources calls REQUIRED. What is
       missing is the authorisation … Saying it about fetches is the next diff here." This is that diff. The
       authorisation is `_firingRefusal` and it answers for every act at once: `observed` and `derived` fire,
       `forced` waits on the per-origin widening. The refusal arrives as the chokepoint's own reply record and
       is relayed below, in its words rather than in a copy of them held here. */
    /* AND `observed` IMPLIES PARSER-INSERTED, because that flag is one of the two facts it is composed from.
       Asserted rather than assumed: the two are stated independently on the line, so a provenance that
       reached `observed` by some other route is the composition having drifted from the flag it is made of.
       IT IS GUARDED BY THE GRADE RATHER THAN REACHED ONLY THROUGH IT. The implication is `observed ⇒ parser`,
       and it used to sit after an early return that made every other grade unreachable — so it read as an
       unconditional statement about every park this zone fired, which stopped being true the moment a DERIVED
       one did. A `script`-initiated DERIVED park is the ordinary case and asserts nothing. */
    if (provenance === 'observed' && initiator !== 'parser')
      throw new Error(`${method} ${abs} is OBSERVED and was not parser-inserted — the two are composed from ` +
                      'one flag (solver/pending.h), so this zone is being told two things about one park ' +
                      'that cannot both be true');
    const methodRefusal = ZONE.safeFetchMethodRefusal(method);
    if (methodRefusal) {
      /* §Attacker-sources: a state-mutating request is NEVER fired to learn. `safe-fetch.js` enforces this by
         ABSENCE (it hardcodes `method:"GET"` and reads neither `opts.method` nor `opts.body`), so issuing one
         here would fetch a POST's address as a GET and hand the bytes back under the POST's key — the reply
         would match the request it names and still be a response the server never gave for it. */
      /* THE GRADE IS THE CHOKEPOINT'S AND THE SENTENCE IS THIS HOST'S. `if (method !== 'GET')` stood here and
         `bridge.js` held the same test — the same question in two hosts, answered differently: this one
         declined and that one returned §5.6's network error for a request nobody sent. `safeFetchMethodRefusal`
         is that answer given once, in the same refusal vocabulary the reply record carries. */
      /* ADDRESSED TO THE PARK, because a flow IS waiting on this one. It used to be the unaddressed record,
         which said the right sentence to a reader and nothing at all to the engine — so the wait was spent in
         the only way a wait can be spent badly: not at all. The flow keeps its park AND gets the arm that runs
         its failure path, which is the whole of what §Solver-half's both-arms means for an outcome nobody
         observed. */
      e.ready.push(declineRequest(method, url,
                          `${method} ${abs} — ${methodRefusal.reason}. The chokepoint is GET-only by ` +
                           'ABSENCE (SECURITY.md §Network), so this address can only be DERIVED and reported, ' +
                           'never issued; answering it with a GET\'s body would be a wrong answer rather than ' +
                           'a missing one'));
      return;
    }
    /* THE CORB CLASS IS THE REQUEST'S OWN DESTINATION, PASSED THROUGH — never a keyword decided here. Fetch
       §2.2.5 "Requests" gives every request a destination and the engine states it at each park; the
       chokepoint asks §2.2.5's SCRIPT-LIKE predicate of it, so a body that becomes executable code must be
       JS-typed or same-origin and everything else is data. This line used to hardcode `as: 'script'` on the
       strength of the INITIATOR — sound only while this zone fired nothing but parser-inserted parks, and wrong
       the moment it fires anything else, which is what the arm above now does. Reading it off the field costs
       nothing and cannot go stale. */
    /* AND THE PROVENANCE BESIDE IT, WHICH IS THE FIRING DECISION'S ONE INPUT. This zone states it and does not
       test it: `_firingRefusal` refuses a FORCED park at an unwidened origin before a socket is opened, and
       the refusal comes back in the reply record's `statusText` like every other refusal that function makes. */
    const raw = await ZONE.safeFetch(abs, { pageUrl: e.docUrl, destination, provenance, credentialed: false });
    /* A REFUSAL THIS ZONE'S OWN POLICY MADE IS A DECLINE AND NOT A NETWORK ERROR, and the difference is what
       the flow does next. A `provide` of `null` is Fetch §5.6's network error: the page's request RESUMES down
       its failure path having been told the server could not be reached, which for a request nobody sent is a
       LIE about the world and teaches the flow something false. A decline settles nothing — the flow stays
       PARKED, exactly as §@S requires of a search not yet solved, and it fires the day the origin is widened.
       IT IS DISCRIMINATED BY ASKING THE POLICY, NEVER BY MATCHING THE `statusText`. The reason string is for a
       READER; a branch that parsed it would be a second copy of the rule written in a format nothing checks.
       IT IS READ OFF THE RECORD NOW RATHER THAN RE-ASKED, AND THAT CLOSED A HOLE THIS PARAGRAPH DID NOT KNOW
       IT HAD. The grade came from asking `_firingRefusal` a second time, which answers about the PROVENANCE
       and about nothing else — so a `blocked-destructive:` refusal, which is equally a decline (no browser
       refuses a GET to a logout path; this tool refuses to send one with the person's session), came back
       `null` from that question and was relayed as a NETWORK ERROR. Re-deriving a grade can only ever answer
       for the rule the re-derivation happens to know about; the chokepoint applied the rule, so the chokepoint
       states which one fired. Every `network` status-0 (a blocked scheme, a private target, CORB, a refused
       credentialed read) stays a network error, which is what a real browser makes of the same request.
       THE SENTENCE IS STILL THIS HOST'S, and picking WHICH sentence is a policy question rather than a
       `statusText` match: `safeFetchFiringRefusal` says whether a widening is what this park is waiting on, so
       the `--explore` paragraph is written only where `--explore` is the answer. */
    if (raw.refusal && raw.refusal.kind === 'decline') {
      const refusal = ZONE.safeFetchFiringRefusal(provenance, abs);
      if (!refusal) {
        e.ready.push(declineRequest(method, url,
                            `${method} ${abs} — ${raw.refusal.reason}. The chokepoint DECLINED to make this ` +
                             'request: no browser refuses it, so there is nothing to hand the flow back and a ' +
                             'network error would tell it the server was unreachable. The flow stays PARKED, ' +
                             'and the address is DERIVED IN FULL and REPORTED, which §Attacker-sources says ' +
                             'is not a gap in the report but IS the report'));
        return;
      }
      e.ready.push(declineRequest(method, url,
                            `${method} ${abs} — ${raw.refusal.reason}. A ${refusal.toUpperCase()} park at an ` +
                           'origin nobody has widened for exploration: pass `--explore <origin>` to widen it, ' +
                           'and note what that obliges — §@H makes the reply to a forced request evidence ' +
                           'about what a server says to a request no client makes, so its values are carried ' +
                           'as FORCED and never merged into the observed pool. Until then the request is ' +
                           'DERIVED IN FULL and REPORTED, which §Attacker-sources says is not a gap in the ' +
                           'report but IS the report'));
      return;
    }
    const rec = replyRecord(raw, `the ${provenance} ${destination || 'data'} load ${abs}`);
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
      /* SEVENTEEN FIELDS, and the count MOVES when the record grows: the field added last is exactly the one
         an unmoved count would let arrive as `undefined`, and every one of them below is read. */
      if (f.length < 17)
        throw new Error(`a navigable.create notice was short of its fields: ${record} — navigable.c writes the ` +
                        'child, the creator, the address, the origin, §8.1.3.1\'s top-level creation URL, CSP ' +
                        '§2.2\'s self-origin, the four items of §7.1.4\'s embedder policy, §7.3.1.3\'s parent ' +
                        'navigable and its container\'s Permissions Policy §9.5 answer, HTML §3.1.3\'s ' +
                        'ancestor origins, HTML §7.1.5\'s creation sandboxing flag set, what the NAVIGATION ' +
                        'is evidence of, and the policy');
      /* A DOCUMENT THIS ZONE ALREADY HOLDS AN INSTANCE FOR IS NOT PROVISIONED TWICE, and that is not a guard
         against a duplicate notice — it is what a REPLAYED document does, re-creating its child navigable and
         re-announcing it under the same name. A second instance would give one document two heaps and two
         object graphs, which is the state SECURITY.md's one-instance-per-cluster rule exists to prevent and
         which nothing downstream could tell from the routing. */
      if (holderOf(f[1])) return;
      /* FIELD 15 IS WHAT THE NAVIGATION IS EVIDENCE OF, AND IT IS WHY THIS ARM STOPPED BEING A BLANK REFUSAL.
         `null` here meant "the record does not state it", and §Attacker-sources makes an unestablished
         provenance a crash at the decision — so every cross-origin child navigable any page ever created was
         declined at every unwidened origin, which is every origin by default. The create notice now carries
         the word, so a child a page's own code named on a path that stood on no contradicted arm is `derived`
         and is navigated freely, which is the capability §What-the-tool-produces exists for; a FORCED one is
         still the per-origin widening this zone reads as `--explore <origin>`. */
      const loaded = await navigate(f[3], e.docUrl, `navigable.create ${f[1]}`, f[15]);
      if (loaded.declined) { e.ready.push(decline(loaded.declined)); return; }
      /* THE CHILD'S PRINCIPAL IS THE ORIGIN OF THE URL THIS ZONE FETCHED, derived here and never read off the
         notice even though the notice carries one: SECURITY.md draws the line at this exact record — a NAME
         may be minted by the untrusted side because it is only a name, while the ORIGIN is what every
         bundle's cross-origin check is written against. */
      /* REFERENCED, AND THE CREATE NOTICE IS ITSELF THE PROOF: the creating engine minted `f[1]` because its
         page already holds a WindowProxy for this navigable, which is the only reason a delivery has to be
         routed here at all. Its `w.length`, its `w.closed` and every post it makes arrive at this document,
         so this document's timelines may not run out before they do. */
      await provision({ docId: f[1], url: loaded.url, origin: new URL(loaded.url).origin,
                        headers: loaded.headers, bytes: loaded.bytes, facts: createFacts(f),
                        referenced: 1 });
      return;
    }
    /* `navigable.swap <new document> <url> <origin> <provenance>` — HTML §7.1.3.2 "Browsing context group switches due to
       opener policy": a navigation whose response's opener policy does not match its navigable's active
       document's builds that Document in a NEW top-level browsing context in a NEW browsing context group. It
       is the same provisioning act and a different record because §7.3.2.3 creates that context "with null,
       null, and group" — a NULL CREATOR, so there is no policy container to clone, no parent, no container
       element and no ancestor list, and the navigable's own address is its environments' top-level creation
       URL. A new GROUP is a new instance for the same reason a cross-origin child is: SECURITY.md keys one on
       `(browsing context group, origin)` and a swap changes the first half. */
    if (f[0] === 'navigable.swap') {
      if (f.length < 5 || !f[1] || !f[2] || !f[4])
        throw new Error(`a navigable.swap notice was short of its fields: ${record} — the fourth is what the ` +
                        'navigation that caused the swap is EVIDENCE OF, taken off that navigation\'s own ' +
                        'load job (core/frame/browsing_context_group.c), and the load below is decided from it');
      if (holderOf(f[1])) return;
      /* AND IT IS THE SAME NAVIGATION'S WORD, which is what keeps a swap from being a door an address this
         zone declined at `document.fetch` could be fetched through one notice later. */
      const loaded = await navigate(f[2], e.docUrl, `navigable.swap ${f[1]}`, f[4]);
      if (loaded.declined) { e.ready.push(decline(loaded.declined)); return; }
      /* NOT REFERENCED, WHICH IS THE ONE ARM WHERE "AN ENGINE NAMED IT" IS THE WRONG READING. §7.1.3.2 step
         10's note says the old browsing context "will not be used by the new Document that we are about to
         create", and core/frame/browsing_context_group.c discards it right after emitting this notice — so the
         page that navigated still holds its handle, and that handle answers about the document it had, not
         about this one. Nothing anywhere holds a proxy for the swapped-TO document, so keeping its timelines
         open would park its last flow for a question no instance can ask. */
      await provision({ docId: f[1], url: loaded.url, origin: new URL(loaded.url).origin,
                        headers: loaded.headers, bytes: loaded.bytes, facts: topLevelFacts(loaded.url),
                        referenced: 0 });
      return;
    }
    /* `document.seed <address> <provenance>` — AN ADDRESS THE APPLICATION DECLARED IS A PAGE OF ITSELF, from
       HTML §7.4.4 "Non-fragment synchronous \"navigations\""' URL and history update steps (which is where
       every client-side router's `history.pushState` ends up). It is the one document this zone provisions
       that nothing NAVIGATED to: the bundle merely NAMED the route, which is precisely the surface
       §What-the-tool-produces exists for and the one forced execution alone could never reach, because the
       code walks nowhere.
       IT IS UNDER THE SAME WIDENING AS EVERY OTHER NAVIGATION, AND THE SENTENCE HERE USED TO SAY OTHERWISE.
       It read "it is the ONE record that states its own provenance … the create says nothing about who named
       the address", which was true when this arm was written and is contradicted by this file's own header and
       by the call three blocks up: `navigable.create` states it at field 15, `navigable.swap` at field 4, and
       all three go through the one `navigate`, where a `forced` address waits on `--explore <origin>` and a
       `derived` one is loaded. A record's provenance is a fact about the ACT, so no record is privileged by
       carrying one — and a comment claiming a privilege the code does not grant is worse than one claiming a
       gap, because a reader acts on a permission. What IS this record's own is which words it can carry:
       `derived` or `forced` and never `observed` (solver/route_seed.h — no load of anything produces a
       declaration, so §4.12.1's parser-inserted conjunct is unreachable for it).
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
      /* AND NOT REFERENCED, WHICH IS THE SAME SENTENCE THE NAME ABOVE IS MINTED BY: nothing holds a proxy for
         a route the bundle merely DECLARED — this zone named the document because no page ever held one. */
      await provision({ docId: `seed${++seedSerial}`, url: loaded.url, origin: new URL(loaded.url).origin,
                        headers: loaded.headers, bytes: loaded.bytes, facts: topLevelFacts(loaded.url),
                        referenced: 0 });
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
        /* `document.fetch<TAB><provenance><TAB><url>` — §7.4.5's load for a document THIS instance holds,
           answered here rather than routed, because routing a same-origin load to a peer would be answering
           it out of another agent.
           THE RECORD STATES ITS PROVENANCE NOW AND THIS ARM USED TO PASS `null` FOR IT — `navigate`'s own word
           for "the record does not state it", which §Attacker-sources makes a crash at the decision, so every
           same-origin child navigable of every session was declined at every unwidened origin. The record
           names the same word its cross-agent twin above carries, taken off the same load job, so the two
           callers reach one decision from one fact: the agent boundary decides WHERE a document is built and
           never WHETHER this zone may spend the network on it.
           SPLIT AT THE FIRST TAB AFTER THE VERB, ADDRESS LAST — a URL cannot contain a tab (URL Standard §4.4
           "URL parsing" strips every ASCII tab from its input) and the vocabulary in front of it is three
           words of ASCII lowercase letters, so this grammar has exactly one place it comes apart. */
        track(e, key, (async () => {
          const args = op.slice('document.fetch\t'.length);
          const t = args.indexOf('\t');
          if (t <= 0 || t >= args.length - 1)
            throw new Error(`a document.fetch request is not \`document.fetch<TAB><provenance><TAB><url>\`: ` +
                            `${op} — core/frame/navigable.c writes both fields non-empty on every path, so a ` +
                            'record with one tab is this zone and that job no longer sharing a grammar and ' +
                            'the address read out of it would be a provenance token');
          const loaded = await navigate(args.slice(t + 1), e.docUrl, 'document.fetch', args.slice(0, t));
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

  /* THE SEED IS NOT REFERENCED. A person named this address and this zone fetched it; no agent created its
     navigable and no page holds a WindowProxy for it, so its frontier is entitled to DRAIN and its session to
     close — which is what produces the `@RESULT` this process exists to print. Stating it rather than leaving
     it off is the point of the flag being required: a root held open by mistake never finishes. */
  const root = await provision({ docId, url: docUrl, origin: new URL(docUrl).origin,
                                 headers: seeded.meta.headers, bytes: seeded.bytes,
                                 facts: topLevelFacts(docUrl), referenced: 0 });

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
