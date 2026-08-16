/* THE BROWSER PROCESS'S DRIVER —  node engine/bproc.mjs  (after node engine/build.mjs).
 *
 * WHY IT IS IN THE REPOSITORY. `engine/build.mjs` links `extension/lib/bproc/bproc.mjs` and nothing else in
 * this tree would ever load it: its shipped caller is a Worker inside a Chrome extension, so without this the
 * program would be BUILT and never RUN, which §Testing calls the excluded test one layer down. It is the same
 * role `engine/route.mjs` plays for the renderer ABI — a harness, not a pinned behaviour.
 *
 * ITS ORACLE IS THE STANDARD, WHICH IS WHY EACH ROW CARRIES THE SENTENCE THAT DECIDES IT. A test262 file is
 * self-validating because it states the spec's own answer; these rows do the same, naming the MIME Sniffing §
 * or the Chromium rule that produces each verdict. That is what makes this different from an expected-output
 * file: when the algorithm improves, a row changes only if the STANDARD's answer changed, and a row whose
 * comment cannot be traced to a sentence is a row that should not exist.
 *
 * AND EVERY ROW IS A MISLABEL, because that is the only case any of this exists for. A body whose declared type
 * and true type agree is decided by the header alone and proves nothing about §7 or about a byte ever having
 * been read.
 */
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const factory = await import(join(ENGINE, '..', 'extension', 'lib', 'bproc', 'bproc.mjs'));
const M = await (factory.default ?? factory)();

const enc = new TextEncoder();
function corb(contentType, noSniff, sameOrigin, bodyText) {
  const b = typeof bodyText === 'string' ? enc.encode(bodyText) : bodyText;
  const p = M._malloc(b.length + 1);
  if (!p) throw new Error('OOM placing the resource header');
  M.HEAPU8.set(b, p);
  try {
    const s = M.ccall('bp_corb_check', 'string',
                      ['string', 'number', 'number', 'number', 'number'],
                      [contentType, noSniff ? 1 : 0, sameOrigin ? 1 : 0, p, b.length]);
    const r = JSON.parse(s);
    if (typeof r.allow !== 'boolean' || typeof r.computed !== 'string' || typeof r.reason !== 'string')
      throw new Error('the browser process answered something other than a CORB verdict: ' + s);
    return r;
  } finally { M._free(p); }
}

const HTML = '<!doctype html><html><body><h1>login</h1></body></html>';
const JS   = '(function(){window.__chunk=1;})();\n';
const JSON_BODY = '{"user":{"id":42},"token":"abc"}';
const PNG  = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13]);

const ROWS = [
  { name: 'JS served as text/plain',
    why: '§7 step 4 — "text/plain" is one of §5.1\'s four check-for-apache-bug values, so the BYTES decide; ' +
         '§7.2 finds no binary data byte and answers text/plain, which is neither protected nor sniffable',
    ct: 'text/plain', nosniff: false, same: false, body: JS,
    want: { allow: true, computed: 'text/plain', reason: 'allowed' } },

  { name: 'HTML served as application/javascript',
    why: '§7 reaches step 8 and answers the SUPPLIED type — sniffing never downgrades a scriptable type, the ' +
         'mirror of the step-1 rule that keeps it from upgrading into one — so the mislabel is caught by ' +
         'CORB\'s own confirmation sniff, which is §7.1\'s scriptable table over the bytes',
    ct: 'application/javascript', nosniff: false, same: false, body: HTML,
    want: { allow: false, computed: 'application/javascript', reason: 'sniffed-html' } },

  { name: 'JSON served as application/javascript',
    why: '§7 has no JSON row at all, so this one is Chromium SniffForJSON\'s brace / string / colon machine — ' +
         'the classic JSON-hijack shape, and a syntax error as a JS statement, which is why it is evidence',
    ct: 'application/javascript', nosniff: false, same: false, body: JSON_BODY,
    want: { allow: false, computed: 'application/javascript', reason: 'sniffed-json' } },

  { name: 'HTML served honestly as text/html, cross-origin',
    why: '§7 step 1 returns an HTML supplied type before any byte is examined; text/html is CORB-protected',
    ct: 'text/html', nosniff: false, same: false, body: HTML,
    want: { allow: false, computed: 'text/html', reason: 'protected-type' } },

  { name: 'the same body SAME-ORIGIN',
    why: 'CORB protects across origins and nowhere else — the page\'s own data is its to read. It is still ' +
         'refused to a CODE loader, which is a load that could not have executed anyway',
    ct: 'text/html', nosniff: false, same: true, body: HTML,
    want: { allow: false, computed: 'text/html', reason: 'same-origin' } },

  { name: 'JS served honestly, same-origin',
    why: 'the other arm of the same rule: a JavaScript type is not protected, so the page\'s own chunk loads',
    ct: 'text/javascript', nosniff: false, same: true, body: JS,
    want: { allow: true, computed: 'text/javascript', reason: 'same-origin' } },

  { name: 'JS served as text/plain under nosniff',
    why: '§7 step 3 hands back the supplied type before step 4\'s apache-bug branch can look at bytes, and ' +
         'the server has said the label is final — so a cross-origin non-JS type is refused',
    ct: 'text/plain', nosniff: true, same: false, body: JS,
    want: { allow: false, computed: 'text/plain', reason: 'nosniff-not-js' } },

  { name: 'a PNG served as text/plain',
    why: '§7 step 4 again, and §7.2 finds 0x1A in the PNG signature — a binary data byte — so the computed ' +
         'type is application/octet-stream. Not markup and not JSON, so CORB has nothing to block on',
    ct: 'text/plain', nosniff: false, same: false, body: PNG,
    want: { allow: true, computed: 'application/octet-stream', reason: 'allowed' } },

  { name: 'a body with NO Content-Type at all',
    why: '§5.1: a null supplied type IS "undefined", which sends §7 to step 2 and §7.1\'s scriptable table — ' +
         'the one path on which sniffing may name text/html, and it names it here',
    ct: null, nosniff: false, same: false, body: HTML,
    want: { allow: false, computed: 'text/html', reason: 'protected-type' } },
];

let bad = 0;
for (const r of ROWS) {
  const got = corb(r.ct, r.nosniff, r.same, r.body);
  const ok = got.allow === r.want.allow && got.computed === r.want.computed && got.reason === r.want.reason;
  if (!ok) bad++;
  console.log((ok ? '  ok   ' : '  FAIL ') + r.name.padEnd(40) +
              (got.allow ? 'allow' : 'BLOCK') + '  ' + got.computed + '  ' + got.reason +
              (ok ? '' : '   want: ' + (r.want.allow ? 'allow' : 'BLOCK') + '  ' + r.want.computed +
                          '  ' + r.want.reason + '\n         ' + r.why));
}
console.log('[bproc] ' + (ROWS.length - bad) + '/' + ROWS.length + ' mislabelled-resource rows agree with the standard');
process.exit(bad ? 1 : 0);
