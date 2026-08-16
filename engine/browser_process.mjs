/* THE BROWSER PROCESS'S DRIVER —  node engine/browser_process.mjs  (after node engine/build.mjs).
 *
 * IT IS IN THE REPOSITORY FOR THE REASON engine/route.mjs IS, one process over. SECURITY.md: a host that cannot
 * provision a second instance has not tested the transport, and every cross-instance mechanism is then a design
 * that has never run. route.mjs provisions a second RENDERER; this provisions the BROWSER PROCESS — the trusted
 * instance beside them — and plays the offscreen, which is the only zone that may hold its Module handle.
 *
 * WHAT IT ASKS. One operation: WHATWG MIME Sniffing §7, the computed MIME type of a response. That algorithm
 * was written into `engine/host/browser/core/mime/mime_sniff.c` and called from `solver/reply_decode.c` —
 * inside the RENDERER, which is the process a real browser never lets compute it, because CORB gates on the
 * result and a renderer that classifies a body for itself can mine one it would have been handed as an opaque
 * empty response. The implementation was correct; it was HOUSED wrongly, and it now lives in this process and
 * links into this artifact alone.
 *
 * THE CASES ARE §7'S OWN BRANCHES, not a sample of them: a resource with no Content-Type at all, the
 * `unknown/unknown` essence step 2 names, §7 step 1's XML-or-HTML shortcut, step 3's no-sniff flag, step 4's
 * Apache bug, step 6's image re-match, and the supplied type §7 leaves alone that this engine's one former
 * caller keyed on. Each states what the standard answers and why, so a disagreement names the step.
 *
 * AND IT DRIVES THE AUTHORIZATION BOUNDARY, which is the half a driver usually leaves as a claim. `mime.sniff`
 * is declared zone-only beside the operation, so a renderer asking for it must abort — a CHECK, fatal in dev
 * and in release, because a renderer being told what it is not entitled to is the one thing this process
 * exists to stop. A boundary nothing has ever crossed is a boundary nobody has measured, so this file
 * re-executes ITSELF with `--as-renderer`, in a child process, and requires that child to die: the parent
 * asserts the non-zero exit AND the `@E` line naming the operation. Two spawns for the same reason
 * `build.mjs`'s cold round trip is two — an abort cannot be observed from inside the process it kills. */
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const AS_RENDERER = process.argv.includes('--as-renderer');

const factory = await import(join(ENGINE, '..', 'extension', 'lib', 'qjs', 'browser_process.mjs'));
const boot = factory.default ?? factory;

/* THE OFFSCREEN'S ONE INSTANCE. It is created here and never again: this process is keyed on nothing, holds no
   per-document state, and a second one would be a second answer to a question that has one. */
const M = await boot();

const cs = (s) => {
  if (s === null) return 0;   /* the ZONE speaking for itself, and Fetch's "get a header" returning null */
  const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n);
  M.stringToUTF8(s, p, n);
  return p;
};

/* THE BYTES TRAVEL BESIDE THE RECORD, never inside it — §5.2's resource header is a BYTE SEQUENCE, and every
   way of putting one in text is an encode or a decode performed by a zone with no business doing either. A
   fixture written as source text is ENCODED here, which is a fixture's own business and never a decode. */
const bytes = (b) => {
  const u8 = typeof b === 'string' ? new Uint8Array([...b].map((c) => c.charCodeAt(0))) : b;
  const p = M._malloc(u8.length + 1);
  M.HEAPU8.set(u8, p);
  return [p, u8.length];
};

/* ONE REQUEST, AS THE ZONE MAKES IT. `requester` is NULL for the zone itself and a renderer's document id when
   the zone is RELAYING that renderer's request — stated here and never read out of the record, exactly as
   `qjs_route`'s sender origin is stated by the zone and never minted by an engine. */
function perform(requester, record, body) {
  const [p, n] = bytes(body);
  try {
    return String(M.ccall('bp_perform', 'string', ['number', 'number', 'number', 'number'],
                          [cs(requester), cs(record), p, n]) ?? '');
  } finally { M._free(p); }
}

/* THE RECORD FOR §7. Two fields is "this response carries no Content-Type header" (Fetch's "get a header"
   returning null); three is a header whose VALUE is the third field, empty value included. The value is last
   because it is the remainder — a header value may contain HTAB. */
const sniff = (contentType, noSniff, body) =>
  perform(null, 'mime.sniff\t' + (noSniff ? '1' : '0') + (contentType === null ? '' : '\t' + contentType), body);

/* ── the renderer's half: a child process, whose job is to die ─────────────────────────────────────────── */
if (AS_RENDERER) {
  /* `d1` is a renderer instance's document id — the name route.mjs's root document carries. The zone would be
     relaying this instance's request; the operation is declared zone-only, so the CHECK fires and this process
     never reaches the line below. */
  const answer = perform('d1', 'mime.sniff\t0\ttext/html', '<!doctype html>');
  console.error('[browser-process] the zone-only boundary DID NOT HOLD: a renderer was answered ' + answer);
  process.exit(0);   /* a zero exit is what the parent reads as the boundary having failed */
}

/* ── §7, case by case ──────────────────────────────────────────────────────────────────────────────────── */

/* §6.1's own byte patterns, built from the byte values the table writes rather than from source-text escapes.
   PNG's signature carries 0x1A, which is also a §3 BINARY DATA BYTE — which is what makes it the right body for
   §7.2's half of the Apache-bug case below, and why the two cases share it. */
const octets = (...b) => String.fromCharCode(...b);
const GIF = 'GIF89a' + octets(0x20, 0x20);
const PNG = octets(0x89) + 'PNG' + octets(0x0D, 0x0A, 0x1A, 0x0A);

const CASES = [
  /* §7 step 2 → §7.1: no supplied type at all, so the answer comes from §6.1's image table. */
  [null, false, GIF, 'image/gif',
   '§7 step 2 with an undefined supplied type runs §7.1, whose image table matches GIF89a'],
  /* §7 step 2 again — `unknown/unknown` is one of the three essences the step names, so a server that states
     it is stating nothing and the bytes decide. */
  ['unknown/unknown', false, PNG, 'image/png',
   "§7 step 2 treats `unknown/unknown` as no statement at all, so §7.1's image table answers"],
  /* §7 step 1: an HTML supplied type is returned BEFORE any byte is looked at — the step that keeps sniffing
     from ever upgrading a resource INTO a scriptable type it was not declared as. The parameter rides along,
     because §4.5 serializes the record and the record has one. */
  ['text/html;charset=utf-8', false, GIF, 'text/html;charset=utf-8',
   '§7 step 1 returns an HTML supplied type unchanged, bytes unread, parameters intact'],
  /* §7 step 3: the no-sniff flag stops the algorithm at the supplied type. The same bytes as the case above
     it, so the flag is the only thing that differs and the answer changes because of it. */
  ['text/plain;charset=utf-8', true, GIF, 'text/plain;charset=utf-8',
   "§7 step 3 returns the supplied type when the no-sniff flag is set — this is `X-Content-Type-Options: " +
   'nosniff`, and the identical body sniffs to image/gif without it'],
  /* §7 step 4 → §7.2: `text/plain` EXACTLY is §5.1's check-for-apache-bug, and a body with a binary data byte
     is then §7.2's `application/octet-stream`. */
  ['text/plain', false, PNG, 'application/octet-stream',
   "§7 step 4's Apache-bug value sends a binary body to §7.2, which answers application/octet-stream"],
  /* And the same value with a body §7.2 finds no binary data byte in. */
  ['text/plain', false, 'plain enough', 'text/plain',
   '§7.2 answers text/plain for a body with no binary data byte'],
  /* §7 step 6: an IMAGE supplied type is re-matched against §6.1's table, and a server that mislabelled a PNG
     as a GIF is corrected by its bytes. */
  ['image/gif', false, PNG, 'image/png',
   "§7 step 6 re-matches an image supplied type against §6.1's patterns, so mislabelled bytes win"],
  /* THE CASE THIS ENGINE'S FORMER CALLER KEYED ON. `solver/reply_decode.c` reads a React Flight stream, and
     `text/x-component` is touched by no rule in §7 — which is the measurement its own comment cites for saying
     the sniff decided zero of the endpoints in the @H surface. It has to stay true, and now something asks. */
  ['text/x-component', false, '0:{"a":1}\n', 'text/x-component',
   '§7 reaches step 8 for a supplied type no rule touches, so a Flight stream is what its server said it is'],
  /* A HEADER PRESENT WITH AN EMPTY VALUE IS NOT AN ABSENT HEADER, even though §5.1 reaches the same undefined
     supplied type for both. The record can say each, which is the point; if it could not, the fact would be
     lost at the boundary rather than at the step that stops caring about it. */
  ['', false, GIF, 'image/gif',
   "an empty Content-Type value parses as no MIME type, which is §5.1's undefined supplied type"],
  /* A RESPONSE WITH NO BODY AT ALL is §5.2's "the end of the resource is reached" and not an error. */
  [null, false, '', 'text/plain',
   '§7.1 over an empty resource header finds no binary data byte and answers text/plain'],
];

let failed = 0;
for (const [ct, noSniff, body, want, why] of CASES) {
  const got = sniff(ct, noSniff, body);
  const shown = ct === null ? '(no Content-Type header)' : JSON.stringify(ct);
  if (got === want) {
    console.log(`  ok   ${shown}${noSniff ? ' +nosniff' : ''} -> ${got}`);
  } else {
    failed++;
    console.error(`  FAIL ${shown}${noSniff ? ' +nosniff' : ''} -> ${got}   expected ${want}\n       ${why}`);
  }
}

/* ── the boundary, measured ────────────────────────────────────────────────────────────────────────────── */

const child = spawnSync(process.execPath, [fileURLToPath(import.meta.url), '--as-renderer'],
                        { encoding: 'utf8' });
const said = (child.stderr || '') + (child.stdout || '');
/* THE ABORT AND WHAT IT SAID, both. A non-zero exit on its own is satisfied by any crash — a missing artifact,
   a bad ccall — so the `@E` line is what says the abort was THIS boundary and not an accident on the way to it. */
const aborted = child.status !== 0;
const named = said.includes('@E {');        /* check.h's always-fatal tag, emitted before the abort */
const zoneOnly = said.includes('zone-only');
if (aborted && named && zoneOnly) {
  console.log('  ok   a renderer asking a zone-only operation aborted the browser process');
} else {
  failed++;
  console.error('  FAIL a renderer asked `mime.sniff` and the browser process ' +
                (aborted ? 'aborted without naming the boundary' : 'ANSWERED IT') +
                ` (exit ${child.status})\n${said.split('\n').slice(-6).join('\n')}`);
}

if (failed) {
  console.error(`[browser-process] FAILED: ${failed} case(s) — each names the §7 step it disagrees with, and a ` +
                'disagreement is the standard being read wrong here rather than a case being wrong there');
  process.exit(1);
}
console.log(`[browser-process] OK — one browser-process instance, ${CASES.length} §7 answers, ` +
            'and a renderer refused');
