// lib/site.js — sites for the trusted zone, over the Public Suffix List lib/public-suffix-table.js carries:
// URL §3.2 "Hosts (domains)" public suffix and registrable domain, and HTML §7.1.1.1 "Sites" obtain a site
// and same site. The lookup is engine/host/browser/core/url/public_suffix.c's, over the same arrays.

function _pslHas(rules, s) {
  let lo = 0, hi = rules.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (rules[mid] === s) return true;
    if (rules[mid] < s) lo = mid + 1;
    else hi = mid;
  }
  return false;
}

/* A host as the URL parser serializes it (`URL.hostname`): an IPv6 address is bracketed and an IPv4 address
   is four decimal numbers, and URL §3.2 gives neither a public suffix. */
function _hostIsDomain(host) {
  return host !== "" && !host.startsWith("[") && !/^\d+\.\d+\.\d+\.\d+$/.test(host);
}

/* URL §3.2 "public suffix", by the PSL algorithm: an exception rule prevails, then the matching rule with
   the most labels, then "*". Returns null for a host that is not a domain. */
function publicSuffixOf(host) {
  if (!_hostIsDomain(host)) return null;
  const dot = host.endsWith(".") ? "." : "";
  const core = dot ? host.slice(0, -1) : host;
  const starts = [0];
  for (let i = core.indexOf("."); i >= 0; i = core.indexOf(".", i + 1)) starts.push(i + 1);
  for (let k = 0; k < starts.length; k++)
    if (_pslHas(PSL_EXCEPTION, core.slice(starts[k]))) {
      DCHECK(k + 1 < starts.length, "an exception rule matched a domain's last label — §Format makes every " +
                                    "exception carry the label its wildcard consumed");
      return core.slice(starts[k + 1]) + dot;
    }
  for (let k = 0; k < starts.length; k++) {
    if (_pslHas(PSL_NORMAL, core.slice(starts[k]))) return core.slice(starts[k]) + dot;
    if (k + 1 < starts.length && _pslHas(PSL_WILDCARD, core.slice(starts[k + 1]))) return core.slice(starts[k]) + dot;
  }
  return core.slice(starts[starts.length - 1]) + dot;
}

/* URL §3.2 "obtain the registrable domain": null when the public suffix is null or is the host itself,
   otherwise the public suffix and the label before it. */
function registrableDomainOf(host) {
  const ps = publicSuffixOf(host);
  if (ps === null || ps === host) return null;
  const head = host.slice(0, host.length - ps.length - 1);
  return head.slice(head.lastIndexOf(".") + 1) + "." + ps;
}

/* HTML §7.1.1.1 "obtain a site", serialized: an opaque origin is null here, since nothing is same site with
   it but itself and the zone never speaks as one; otherwise scheme://(registrable domain or host). */
function siteOf(url) {
  DCHECK(URL.canParse(url), "siteOf was given " + JSON.stringify(url) + ", which is not a URL");
  const u = new URL(url);
  if (u.origin === "null") return null;
  const rd = registrableDomainOf(u.hostname);
  return u.protocol + "//" + (rd === null ? u.hostname : rd);
}

/* HTML §7.1.1.1 "same site" for two tuple origins' URLs: the same scheme and the same site host. */
function sameSite(urlA, urlB) {
  const a = siteOf(urlA), b = siteOf(urlB);
  return a !== null && a === b;
}
