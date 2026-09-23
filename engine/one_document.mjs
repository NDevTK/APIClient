/* ONE DOCUMENT IN, ONE RESULT OUT — WITH NO NETWORK AT ALL.
 *
 * `node engine/one_document.mjs <binary> <html-file> <document-url> <paint-dir|->`, after
 * `node engine/build.mjs native`. A `-` for the last argument omits `--paint-dir`, which is the one
 * switch that decides whether this run RENDERS; see the measurement note at the bottom for why that
 * matters more than it sounds.
 *
 * WHY THIS EXISTS BESIDE `engine/trusted.mjs`, WHICH IS THE QUESTION ITS EXISTENCE HAS TO ANSWER.
 * `trusted.mjs` is the native host's TRUSTED ZONE: it fetches, it applies `extension/lib/safe-fetch.js`'s
 * policy, it answers parked requests, it seats peers. Every one of those is a moving part, and every one of
 * them is BETWEEN a reader and the question "what does this engine do with these bytes". A run of a real
 * page that renders a near-empty picture is consistent with a parse that stopped, a layout that produced no
 * boxes, a painter that reached none of them, a subresource that never arrived, a policy that refused it, and
 * a budget that ran out before any of it — six states, one artifact, and no way to separate them from the
 * outside. This entry removes five of the six by removing the network: it composes `test_forced.c`'s `--abi`
 * document record by hand, hands it the bytes of a file, and lets the engine parse, lay out and paint them
 * with nothing else in the picture.
 *
 * IT IS A DIAGNOSTIC OF THE ENGINE AND IT IS NOT A MIRROR, WHICH IS A DISTINCTION THIS REPOSITORY PAYS FOR.
 * CLAUDE.md's network policy is real traffic and no mirrors, and the corpus of saved site bundles that used
 * to live under `testing/corpus/mirror/` was DELETED for exactly that reason — a saved copy of somebody's
 * site is a claim about that site that goes stale the day they deploy, and a gate keyed on one measures a
 * page that no longer exists. Nothing here re-creates that. This entry FETCHES NOTHING and SERVES NOTHING:
 * the file it is handed is an operand a person named on a command line, every subresource that document
 * references is simply absent, and what it therefore measures is the renderer over the bytes that arrived —
 * never a site, never a page, and never a substitute for a real-network run. A number taken here is a number
 * about parse, layout and paint. It is not a number about a website, and a report that quotes one as though
 * it were has said something this entry cannot support.
 *
 * THE RECORD'S FACTS ARE `engine/top_level_facts.mjs`'s — HTML §7.5.1 "Shared document creation
 * infrastructure"'s eleven remaining facts for a document that is its own top-level traversable, read from
 * the ONE statement of them rather than copied. THE PARAGRAPH THIS REPLACES IS REWRITTEN AND NOT DELETED,
 * because its reasoning is what the next reader re-derives and what it argued FOR is the thing that was
 * wrong. It said they were copied "deliberately rather than imported, and the reason is the direction of the
 * dependency: `trusted.mjs` holds them as a closure-local inside a zone that also owns fetching, so importing
 * them would mean loading the network client to render a file." Every clause of that is TRUE and the
 * conclusion does not follow: what it establishes is that the facts must not come from THE ZONE, never that
 * they must be copied. The module is pure data — it reads `content.mojom.Renderer.Init`'s parameter names
 * and nothing else, opens no socket and never touches the built wasm glue — so this entry imports it and
 * still fetches nothing. The residual that named this copy is discharged at the bottom of this file.
 *
 * WHAT THIS CANNOT SEE, STATED HERE BECAUSE AN INSTRUMENT TRUSTED PAST ITS EVIDENCE IS WORSE THAN NONE:
 * anything a subresource would have changed. A document whose appearance comes from its stylesheets renders
 * here as its own UA defaults, and a document whose DOM its scripts build renders here as its server-sent
 * tree. That is not a defect of this entry, it is its POINT — the renderer is the subject and the network is
 * the thing being held still — but it means a picture from here and a picture from `trusted.mjs` are
 * pictures of two different documents, and a diff between them is not a finding about either.
 */
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync } from 'node:fs';
import { topLevelFactFields } from './top_level_facts.mjs';

const [bin, htmlPath, url, outDir] = process.argv.slice(2);
if (!bin || !htmlPath || !url || !outDir) {
  throw new Error('usage: node engine/one_document.mjs <binary> <html-file> <document-url> <paint-dir|->\n' +
                  '  <paint-dir> receives one PAM per world this run leaves standing at a slice boundary;\n' +
                  '  `-` omits --paint-dir entirely, which is what makes a run measure parse and layout\n' +
                  '  WITHOUT the render. Every argument is REQUIRED: a default here would be this file\n' +
                  '  choosing a document, a name or an address on a caller\'s behalf, and the address in\n' +
                  '  particular is what every relative URL in the page resolves against.');
}

const b64 = (s) => Buffer.from(s).toString('base64');
const bytes = readFileSync(htmlPath);

/* THE ONE HEADER, AND IT IS A STATEMENT RATHER THAN A CONVENIENCE. `header_list_parse_field_lines` reads an
   empty block as "a response that carried no headers", which is a legitimate state — and a document served
   with no `content-type` is not the case anybody is diagnosing here. Stating `text/html` is what makes this
   run a run of the HTML road; a caller who wants the other road states it by editing this line, which is
   visible, rather than by discovering that the default sent them somewhere else. */
const headers = 'content-type: text/html; charset=utf-8\n';

/* HTML §7.5.1's eleven for a top-level traversable, from the ONE statement of them — see
   `engine/top_level_facts.mjs`, which is what this file's residual named as its next diff and which is
   now what both this entry and `trusted.mjs` read. */
const facts = topLevelFactFields(url);

/* THE DOCUMENT NAME IS THIS FILE'S OWN CONSTANT AND NOT THE ADDRESS. `abi_main` refuses an empty one and
   `abi_paint` composes the image's FILE NAME out of it, so a name derived from a URL would put a caller's
   address into a path. One instance, one name. */
const rec = ['document', url, 'one_document', b64(headers), bytes.toString('base64'), ...facts, '0'].join('\t');

const paint = outDir !== '-';
if (paint) mkdirSync(outDir, { recursive: true });

/* STDOUT AND STDERR ARE INHERITED, WHICH IS THE WHOLE OF THIS FILE'S OUTPUT CONTRACT. `--abi`'s stdout is a
   RECORD stream whose reader in `trusted.mjs` throws on a record it cannot route; this entry is not that
   reader and does not pretend to be one, so the records go to the caller unparsed and `@RESULT` is read by
   whoever asked. A parser here would be a second, weaker copy of the zone's. */
const child = spawn(bin, paint ? ['--abi', '--paint-dir', outDir] : ['--abi'],
                    { stdio: ['pipe', 'inherit', 'inherit'] });
child.stdin.write(rec + '\n');
child.stdin.end();

/* THE CHILD'S OWN TERMINATION, REPORTED RATHER THAN TRANSLATED. A run killed at `RLIMIT_CPU` and a run that
   finished are different facts and they look identical in a log whose last line is the engine's; an engine
   that ABORTED has already said why above this line and this adds nothing to it. The signal is printed
   because a caller bounding the run with `ulimit -t` needs to know which of the two it got, and `@RESULT`'s
   absence alone does not say. */
child.on('close', (code, signal) => {
  console.error(`[one_document] the engine ended with code=${code} signal=${signal}. An ABSENT @RESULT above ` +
                'and a @RESULT that found nothing are different facts; a signal here is the first.');
});

/* THE RESIDUAL THAT STOOD HERE IS DISCHARGED AND ITS ARGUMENT IS KEPT, because the argument is what a reader
   re-derives and re-implementing the copy is what they would re-derive. It said the eleven facts were a copy
   of `trusted.mjs`'s with nothing comparing the two, that the ARITY was covered by `abi_take` (which refuses
   an ABSENT field, so a record gone short stops) while a record gone WRONG parses and seats a document under
   a policy, an embedder policy or a secure-context answer the zone would not have given it — and it named
   its own next diff: "the eleven facts in one module both this file and `trusted.mjs` read … possible only
   because they are pure data and need none of the zone's fetching". That module is
   `engine/top_level_facts.mjs` and both files now read it, so there is one statement and no copy.
   WHAT THE DISCHARGE ADDED BEYOND ENDING THE COPY, AND IT IS THE HALF THE RESIDUAL COULD NOT SEE FROM HERE:
   the ARITY is no longer `abi_take`'s to catch one process away. The module walks the eleven NAMES off
   `content.mojom.Renderer.Init` itself and refuses BOTH directions of the skew at the composer — a declared
   parameter with no value (a driver older than the interface) and a value no parameter declares (one newer,
   or misspelled) — which is `abiOperands`' pair of checks, owed on this channel because the native record
   never goes through that function. */
