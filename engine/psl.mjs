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

/* The public suffix, with the matching engine/host/browser/core/url/public_suffix.c carries, which is
   Chromium's (registry_controlled_domain.cc's GetRegistryInTrimmedHost over LookupSuffixInReversedSet): the
   longest entry of the list that is the host or follows a dot, a wildcard rule `*.X` and an exception rule
   `!X` stored as the entry X with a tag. A wildcard entry that is the whole host is itself the public suffix;
   otherwise it takes the label before it. An exception entry loses its leftmost label. No entry: the last
   label (the PSL's "*"). Returns null for a host that is not a domain. */
function publicSuffixIn(t, host) {
  if (!hostIsDomain(host)) return null;
  const dot = host.endsWith(".") ? "." : "";
  const core = dot ? host.slice(0, -1) : host;
  const starts = [0];
  for (let i = core.indexOf("."); i >= 0; i = core.indexOf(".", i + 1)) starts.push(i + 1);
  for (let k = 0; k < starts.length; k++) {
    const s = core.slice(starts[k]);
    const normal = has(t.N, s), wildcard = has(t.W, s), exception = has(t.E, s);
    if (!normal && !wildcard && !exception) continue;
    if (wildcard) return (k === 0 ? core : core.slice(starts[k - 1])) + dot;
    if (exception) {
      if (k + 1 >= starts.length)
        throw new Error("an exception rule matched a domain's last label — every exception carries the label " +
                        "its wildcard consumed");
      return core.slice(starts[k + 1]) + dot;
    }
    return s + dot;
  }
  return core.slice(starts[starts.length - 1]) + dot;
}

/* The registrable domain: null when the public suffix is null or is the host itself, otherwise the public
   suffix and the label before it. */
function registrableDomainIn(t, host) {
  const ps = publicSuffixIn(t, host);
  if (ps === null || ps === host) return null;
  const head = host.slice(0, host.length - ps.length - 1);
  return head.slice(head.lastIndexOf(".") + 1) + "." + ps;
}

/* Both answers over another list's three sorted rule arrays ({N, W, E}, engine/psl_list.mjs's parse), with
   the same matching: what testing/psl_agreement.mjs runs over the browser's own list. */
export function suffixerOver(t) {
  return { publicSuffixOf: (h) => publicSuffixIn(t, h), registrableDomainOf: (h) => registrableDomainIn(t, h) };
}
const ENGINE_LIST = { N: PSL_NORMAL, W: PSL_WILDCARD, E: PSL_EXCEPTION };
export const { publicSuffixOf, registrableDomainOf } = suffixerOver(ENGINE_LIST);

/* `site.registrableDomains`'s answer for `hosts`: each host's registrable domain or null, and the list it was
   answered from, which the zone keys its memo by. */
export function siteRegistrableDomains(hosts) {
  if (!Array.isArray(hosts) || hosts.some((h) => typeof h !== "string"))
    throw new Error("site.registrableDomains takes a list of hosts");
  const domains = {};
  for (const h of hosts) domains[h] = registrableDomainOf(h);
  return { version: "engine PSL " + PSL_VERSION, domains };
}
