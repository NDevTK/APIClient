/* The trusted zone's chokepoint scripts, loaded verbatim into a vm context the way the offscreen document
   (extension/ast-worker.html) loads them. Every process host of the zone loads this one list, so a dependency
   safe-fetch.js gains reaches each host in the same diff that adds it to the shipped document. */

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runInContext } from 'node:vm';
import { siteRegistrableDomains } from './psl.mjs';

export const EXT_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', 'extension');

/* In ast-worker.html's order; the assert below holds the two together. */
export const ZONE_SCRIPTS = Object.freeze([
  'check.js', 'lib/site.js', 'lib/referrer-policy.js', 'lib/request-identity.js', 'lib/cookie-store.js',
  'speak-as-rule.js', 'lib/speak-as.js', 'lib/navigable-notice.js', 'lib/document-record.js', 'lib/http-cache.js',
  'lib/safe-fetch.js', 'lib/boot-span.js', 'lib/novelty-set.js', 'lib/census-rows.js',
]);

const SHIPPED = [...readFileSync(join(EXT_DIR, 'ast-worker.html'), 'utf8')
  .matchAll(/<script src="([^"]+)"><\/script>/g)].map((m) => m[1]);

/* Throws unless `files` occur in the shipped document, in its order. */
function assertShippedOrder(files) {
  let at = -1;
  for (const f of files) {
    const i = SHIPPED.indexOf(f, at + 1);
    if (i < 0)
      throw new Error(`engine/zone.mjs loads \`${f}\` ${SHIPPED.includes(f) ? 'out of' : 'absent from'} ` +
                      'ast-worker.html\'s script order, so a process zone would run a program the extension does not');
    at = i;
  }
}
assertShippedOrder(ZONE_SCRIPTS);

/* lib/site.js's site question answered as a process with no browser answers it: from the engine's own list
   (engine/psl.mjs). Any other service-worker call is refused. */
export async function processSiteRpc(api, hosts) {
  if (api !== 'site.registrableDomains') throw new Error('this process zone serves no rpc ' + JSON.stringify(api));
  return siteRegistrableDomains(hosts);
}

/* Runs ZONE_SCRIPTS, then `extra` (each also in shipped order after them), in `sandbox`, a context created by
   the caller with the host objects it injects. `sandbox.swRpc` is required: lib/site.js asks it every site. */
export function loadZone(sandbox, extra = []) {
  if (typeof sandbox.swRpc !== 'function')
    throw new Error('a process zone needs a `swRpc` that answers lib/site.js\'s site question (processSiteRpc, ' +
                    'or a driver\'s own table)');
  const files = [...ZONE_SCRIPTS, ...extra];
  assertShippedOrder(files);
  /* The classic scripts share one global scope, so a later script's function or var silently replaces an
     earlier script's function of the same name in the shipped document too; a host refuses to load that. */
  const declaredBy = new Map();
  for (const f of files) {
    const before = new Map(Object.getOwnPropertyNames(sandbox).map((k) => [k, sandbox[k]]));
    runInContext(readFileSync(join(EXT_DIR, f), 'utf8'), sandbox, { filename: join(EXT_DIR, f) });
    for (const k of Object.getOwnPropertyNames(sandbox)) {
      if (before.has(k) && before.get(k) === sandbox[k]) continue;
      if (declaredBy.has(k) && typeof before.get(k) === 'function')
        throw new Error(`\`${f}\` replaces the global function \`${k}\` that \`${declaredBy.get(k)}\` declares; ` +
                        'every caller of the first now reaches the second, so rename one');
      declaredBy.set(k, f);
    }
  }
  return sandbox;
}
