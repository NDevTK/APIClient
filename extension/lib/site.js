// lib/site.js — sites for the trusted zone, as the running browser computes them. A host's registrable domain
// is Chrome's net::registry_controlled_domains::GetDomainAndRegistry(host, INCLUDE_PRIVATE_REGISTRIES), which
// its cookie jar and its SchemefulSite (HTML §7.1.1.1 "Sites" obtain a site) are built on; the service worker
// answers it from chrome.publicSuffix (background.js's `site.registrableDomains`). Real Chrome is the oracle:
// where it departs from the PSL Format's algorithm, its own cookies and Sec-Fetch-Site depart with it.
//
// The offscreen document cannot call chrome.publicSuffix, so the answers are asked for in batches and memoized
// per host. Every async entry point that reaches a site question awaits `siteAsk` over the hosts it will ask
// about before any synchronous caller runs, and a synchronous read of a host nobody asked about is a defect of
// that entry point. The memo cannot go stale: Chrome's list changes only with the browser version, and every
// answer carries the version it is Chrome's for.

/* host -> its registrable domain, or null for a host with none (an IP literal, a public suffix itself, a
   single-label host). */
const _siteMemo = new Map();
/* The browser version every answer in the memo is from, set by the first answer. */
let _siteMemoVersion = null;

/* Fills the memo for `hosts` (hosts as the URL parser serializes them), in one round trip for those not yet
   asked. */
async function siteAsk(hosts) {
  const ask = [];
  for (const h of hosts) {
    CHECK(typeof h === "string", "siteAsk was given " + JSON.stringify(h) + ", which is not a host");
    if (!_siteMemo.has(h) && ask.indexOf(h) < 0) ask.push(h);
  }
  if (ask.length === 0) return;
  const r = await swRpc("site.registrableDomains", ask);
  CHECK(!!r && typeof r.version === "string" && !!r.domains && typeof r.domains === "object",
        "site.registrableDomains answered no {version, domains}");
  if (_siteMemoVersion === null) _siteMemoVersion = r.version;
  CHECK(r.version === _siteMemoVersion, "the site oracle answered for " + r.version + " after answering for " +
        _siteMemoVersion + " — a browser changes its list only with its version, so one zone sees one version");
  for (const h of ask) {
    const d = r.domains[h];
    CHECK(d === null || typeof d === "string", "site.registrableDomains answered nothing for " + JSON.stringify(h));
    _siteMemo.set(h, d);
  }
}

/* The hosts `urls` name, each a URL or a serialized origin, for `siteAsk`: an opaque origin ("null") and a URL
   whose origin is opaque name none, since no site question is asked of them. */
function siteHostsOf(urls) {
  const out = [];
  for (const u of urls) {
    if (u === "null") continue;
    DCHECK(URL.canParse(u) || URL.canParse(u + "/"), "siteHostsOf was given " + JSON.stringify(u) +
           ", which is neither a URL nor a serialized origin");
    const p = URL.canParse(u) ? new URL(u) : new URL(u + "/");
    if (p.origin !== "null") out.push(p.hostname);
  }
  return out;
}

/* A host as the URL parser serializes it (`URL.hostname`): an IPv6 address is bracketed and an IPv4 address
   is four decimal numbers. */
function _hostIsDomain(host) {
  return host !== "" && !host.startsWith("[") && !/^\d+\.\d+\.\d+\.\d+$/.test(host);
}

/* The browser's registrable domain for `host`, or null for none. */
function registrableDomainOf(host) {
  DCHECK(_siteMemo.has(host), "the site of " + JSON.stringify(host) + " was read before it was asked — the async " +
         "entry point that reached this read must `await siteAsk` over the hosts it asks about");
  return _siteMemo.get(host);
}

/* HTML §7.1.1.1 "obtain a site", serialized as Chrome's SchemefulSite: an opaque origin is null here, since
   nothing is same site with it but itself and the zone never speaks as one; otherwise scheme://(registrable
   domain or host). */
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
