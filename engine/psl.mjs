/* The native host's site oracle: registrable domains from the engine's own Public Suffix List
 * (engine/public_suffix_table.mjs, which engine/pslgen.mjs writes beside the engine's C table), answered in the
 * shape the extension's service worker answers `site.registrableDomains` from chrome.publicSuffix. A process host
 * has no browser to ask, so engine/trusted.mjs and the testing probes that load the zone in node serve its site
 * questions from here. testing/psl_agreement.mjs measures this answer against the browser's.
 */
import { PSL_VERSION, PSL_NORMAL, PSL_WILDCARD, PSL_EXCEPTION } from "./public_suffix_table.mjs";

function has(rules, s) {
  let lo = 0, hi = rules.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (rules[mid] === s) return true;
    if (rules[mid] < s) lo = mid + 1;
    else hi = mid;
  }
  return false;
}

/* A host as the URL parser serializes it: an IPv6 address is bracketed and an IPv4 address is four decimal
   numbers, and neither has a public suffix. */
function hostIsDomain(host) {
  return host !== "" && !host.startsWith("[") && !/^\d+\.\d+\.\d+\.\d+$/.test(host);
}

/* The PSL Format's §Algorithm: an exception rule prevails, then the matching rule with the most labels, then
   "*". Returns null for a host that is not a domain. */
export function publicSuffixOf(host) {
  if (!hostIsDomain(host)) return null;
  const dot = host.endsWith(".") ? "." : "";
  const core = dot ? host.slice(0, -1) : host;
  const starts = [0];
  for (let i = core.indexOf("."); i >= 0; i = core.indexOf(".", i + 1)) starts.push(i + 1);
  for (let k = 0; k < starts.length; k++)
    if (has(PSL_EXCEPTION, core.slice(starts[k]))) {
      if (k + 1 >= starts.length)
        throw new Error("an exception rule matched a domain's last label — every exception carries the label " +
                        "its wildcard consumed");
      return core.slice(starts[k + 1]) + dot;
    }
  for (let k = 0; k < starts.length; k++) {
    if (has(PSL_NORMAL, core.slice(starts[k]))) return core.slice(starts[k]) + dot;
    if (k + 1 < starts.length && has(PSL_WILDCARD, core.slice(starts[k + 1]))) return core.slice(starts[k]) + dot;
  }
  return core.slice(starts[starts.length - 1]) + dot;
}

/* The registrable domain: null when the public suffix is null or is the host itself, otherwise the public
   suffix and the label before it. */
export function registrableDomainOf(host) {
  const ps = publicSuffixOf(host);
  if (ps === null || ps === host) return null;
  const head = host.slice(0, host.length - ps.length - 1);
  return head.slice(head.lastIndexOf(".") + 1) + "." + ps;
}

/* `site.registrableDomains`'s answer for `hosts`: each host's registrable domain or null, and the list it was
   answered from, which the zone keys its memo by. */
export function siteRegistrableDomains(hosts) {
  if (!Array.isArray(hosts) || hosts.some((h) => typeof h !== "string"))
    throw new Error("site.registrableDomains takes a list of hosts");
  const domains = {};
  for (const h of hosts) domains[h] = registrableDomainOf(h);
  return { version: "engine PSL " + PSL_VERSION, domains };
}
