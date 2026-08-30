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
 * EVERYTHING ELSE IS REFUSED, WITH THE REASON. A park made by RUNNING CODE — a page `fetch()`, an injected
 * `<script src>`, a dynamic `import()` — is DERIVED or FORCED, and this engine runs code on forced arms; the
 * register cannot yet tell those two apart, and the per-origin widening that would let a person say "fire them
 * at this host" does not exist. Its absence is a CRASH and not a default: the refusal travels to the child,
 * which prints it at the stall. That this zone is stricter than `bridge.js` — which fires every pending line
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
 * WHAT IT DOES NOT DO. It provisions no peer instance: a `navigable.create` notice still aborts in the child,
 * because ROUTING ("which instance holds which document") is the trusted zone's one other fact and is a diff of
 * its own. `engine/route.mjs` is where that mechanism is written today, against wasm instances. */
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
  'endpoint. The pending register states HTML §4.12.1\'s parser-inserted flag (solver/engine.h) and nothing ' +
  'finer, so a page fetch(), an injected <script src> and a dynamic import() are one class here and this zone ' +
  'fires none of them. TWO THINGS WOULD LIFT IT, in order: a flow that RECORDS whether its path took a forced ' +
  'arm, which is what separates DERIVED from FORCED; and the per-origin configuration CLAUDE.md requires ' +
  'before a forced request may be fired at all, which does not exist and whose absence is this crash rather ' +
  'than a default';

async function main() {
  const target = process.argv[2];
  const bin = process.argv[3] || join(ENGINE, 'host', 'out', 'qjs-native-none');

  if (!target) {
    console.error('usage: node engine/trusted.mjs <url> [path-to-native-binary]');
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
  const seed = await ZONE.safeFetch(target, { pageUrl: target, credentialed: false });
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

  const child = spawn(bin, ['--abi'], { stdio: ['pipe', 'pipe', 'inherit'] });
  const say = (rec) => child.stdin.write(rec + '\n');

  child.stdout.setEncoding('utf8');   /* a StringDecoder, so a multi-byte character split across two chunk
                                         boundaries is not two replacement characters — the @RESULT line is
                                         the engine's own JSON and carries whatever a page put in it */
  say(['document', docUrl, docId, b64(fieldLines(seeded.meta.headers)), b64(seeded.bytes)].join('\t'));

  /* ONE ANSWER PER REQUEST, EVER — keyed on what the request IS. A pending entry stays on the register until
     it is filled and the child re-states the whole bill whenever it changes, so without this a re-announced
     request would be fetched again and `qjs_provide` would be called twice for one park, which is
     engine_provide's answered-twice abort. It is not a bound: nothing is dropped, and a request this zone
     never answers keeps being reported until the frontier stalls on it. */
  const answered = new Map();
  const ready = [];
  let result = null;
  /* A FAILED PIECE OF WORK IS RAISED AT THE STALL AND NOT SWALLOWED THERE. The work is started when the bill
     is announced and awaited a round or more later, so a rejection would otherwise sit unhandled for exactly
     as long as the engine keeps running — which is the window in which Node decides an unhandled rejection is
     a process-level event. Held, then thrown where it can be read. */
  let fatal = null;
  const track = (key, p) => answered.set(key, p.catch((e) => { fatal = fatal || e; }));

  const workFetch = async (method, initiator, url) => {
    const abs = new URL(url, docUrl).href;
    /* THE PRODUCER'S VOCABULARY, CHECKED BEFORE IT IS ACTED ON. solver/engine.h declares exactly two tokens,
       and a third would be routed by whichever arm of the test below happened to be written as the else —
       which is the defaulted-field defect landing on a firing decision. */
    if (initiator !== 'parser' && initiator !== 'script')
      throw new Error(`the pending line states the initiator \`${initiator}\`, which is neither token ` +
                      'solver/engine.h declares — this zone\'s firing decision reads that field, so an ' +
                      'unknown value must stop it rather than fall to a default');
    if (initiator !== 'parser') { ready.push(decline(`${method} ${abs} — ${UNSTATED_PROVENANCE}`)); return; }
    if (method !== 'GET') {
      /* §Attacker-sources: a state-mutating request is NEVER fired to learn. `safe-fetch.js` enforces this by
         ABSENCE (it hardcodes `method:"GET"` and reads neither `opts.method` nor `opts.body`), so issuing one
         here would fetch a POST's address as a GET and hand the bytes back under the POST's key — the reply
         would match the request it names and still be a response the server never gave for it. */
      ready.push(decline(`${method} ${abs} — a non-GET park. The chokepoint is GET-only by ABSENCE ` +
                         '(SECURITY.md §Network), so this address can only be DERIVED and reported, never ' +
                         'issued; answering it with a GET\'s body would be a wrong answer rather than a ' +
                         'missing one'));
      return;
    }
    /* A BODY THAT BECOMES EXECUTABLE CODE IS FETCHED `as:"script"`, which is the CORB class: a cross-origin
       HTML/JSON body must never be read as code. It is decided HERE from the initiator the engine stated,
       which is a parser-inserted `script` element by definition of that token. */
    const rec = replyRecord(await ZONE.safeFetch(abs, { pageUrl: docUrl, as: 'script', credentialed: false }),
                            `the parser-inserted script ${abs}`);
    ready.push(['provide', method, url, b64(rec ? JSON.stringify(rec.meta) : 'null'),
                rec ? b64(rec.bytes) : ABSENT].join('\t'));
  };

  const lines = (async function* () {
    let buf = '';
    for await (const chunk of child.stdout) {
      buf += chunk;
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) { yield buf.slice(0, nl); buf = buf.slice(nl + 1); }
    }
    if (buf) yield buf;
  })();

  for await (const line of lines) {
    if (line.startsWith('@RESULT ')) { result = line.slice('@RESULT '.length); continue; }
    const f = line.split('\t');
    if (f[0] === 'fetch') {
      if (f.length !== 4)
        throw new Error(`the host announced a fetch that is not \`fetch<TAB>METHOD<TAB>INITIATOR<TAB>URL\`: ` +
                        `${line} — the bill is the pending line verbatim and this zone splits it where the ` +
                        'engine joined it, so a short record is the two grammars having parted');
      const [, method, initiator, url] = f;
      const key = `${method}\t${url}`;
      if (answered.has(key)) continue;
      track(key, workFetch(method, initiator, url));
    } else if (f[0] === 'request') {
      const id = f[1], op = f.slice(2).join('\t');
      const key = `req:${id}`;
      if (answered.has(key)) continue;
      /* A CROSS-AGENT OPERATION IS REFUSED AND NOT ANSWERED, and the two ways of refusing are not the same:
         leaving it silent would park the asking flow for the rest of the session while the frontier reported
         nothing, which from outside is indistinguishable from a page that is merely slow. `document.fetch` is
         §7.4.5's load for a document this zone cannot seat, because seating it needs a SECOND instance and a
         table saying which instance holds which document — and that address is one the page's own code chose,
         so it carries the same unstated provenance as every other running-code park. */
      answered.set(key, Promise.resolve());
      ready.push(decline(
        `request ${id} \`${op.slice(0, 120)}\` — a cross-agent operation. Answering it needs a PEER INSTANCE ` +
        'and the routing table that says which instance holds which document, which is the trusted zone\'s ' +
        'other fact and is not built for this host; `engine/route.mjs` is where that mechanism is written ' +
        'today, against wasm instances. A document.fetch additionally carries the same unstated provenance as ' +
        'every running-code park, since the page chose the address'));
    } else if (line === 'stalled') {
      /* THE ONLY MOMENT THIS ZONE WRITES. Everything outstanding is settled first, because a payment held
         back here is one the child will read as a refusal — and the child is entitled to read a round that
         pays nothing as exactly that. */
      await Promise.all([...answered.values()]);
      if (fatal) throw fatal;
      const round = ready.splice(0, ready.length);
      for (const rec of round) say(rec);
      say('go');
    } else {
      throw new Error(`the host wrote a record under the verb \`${f[0]}\`, which this zone does not carry: ` +
                      `${line}`);
    }
  }

  const code = await new Promise((res) => child.on('close', res));
  if (result === null) {
    console.error(`[trusted] the host produced no @RESULT (exit ${code}) — an ABSENT result and a result that ` +
                  'found nothing are different facts and this is the first, so nothing here may be reported ' +
                  'as a page that was analysed and found clean.');
    process.exitCode = code || 1;
    return;
  }
  console.log(result);
  /* `exitCode` AND NOT `exit()`: the result is a line on a pipe and `process.exit` does not wait for it to
     drain, so the one thing this process exists to produce is the one thing that would be truncated. */
  process.exitCode = code;
}

await main();
