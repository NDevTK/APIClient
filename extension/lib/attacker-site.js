// lib/attacker-site.js — the attacker's site the person verifying configures: a domain whose every subdomain
// resolves to a server they run, holding a certificate for each name, which serves a document a live verify
// can evaluate a PoC in and open the victim from. The zone states it to each engine instance (main.c's
// qjs_set_attacker_site), whose composed message origins are under it, and checks it against what a live
// verify needs once, at configuration (safe-fetch.js's safeFetchAttackerSiteCheck), recording the outcome.

/* The refusal of `input` as an attacker site, or null for a site. A site is a domain of at least two labels,
   each letter-digit-hyphen and lowercase (RFC 1035 §2.3.1 Preferred name syntax, as RFC 1123 §2.1 relaxes its
   first character), at most 63 characters a label and 253 the name (RFC 1035 §2.3.4 Size limits), and read by
   the URL parser as that same domain: `https://<input>/` must serialize back unchanged, so userinfo, a port, a
   path, a query, a fragment, an uppercase letter or a trailing dot is refused rather than repaired, and a name
   ending in a number (URL §3.5 Host parsing's ends in a number checker) parses as an IPv4 address. The engine's
   concolic_host_domain asks the same and CHECKs what it is handed. */
function attackerSiteRefusal(input) {
  if (typeof input !== "string" || input === "") return "empty";
  const spelled = "https://" + input + "/";
  if (!URL.canParse(spelled)) return "not-a-host";
  const u = new URL(spelled);
  if (u.href !== spelled || u.hostname !== input) return "not-its-own-host:" + u.hostname;
  if (input.length > 253) return "longer-than-253";
  const labels = input.split(".");
  if (labels.length < 2) return "one-label";
  for (const l of labels)
    if (!/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(l)) return "label-not-ldh:" + l;
  if (/^[0-9]+$/.test(labels[labels.length - 1])) return "ends-in-a-number";
  return null;
}

/* The requirements a live verify puts on the site, in the order the check meets them on one send. */
const ATTACKER_SITE_REQUIREMENTS = Object.freeze(["wildcard-dns", "certificate", "serves", "csp", "coop"]);

/* The configured site and its check, {site, failing: [{requirement, why}]}, `failing` empty when every
   requirement was met; or null when none is configured. */
let _attackerSite = null;
function attackerSiteRecordIs(r) {
  return r !== null && typeof r === "object" && Object.keys(r).join(",") === "site,failing" &&
         attackerSiteRefusal(r.site) === null && Array.isArray(r.failing) &&
         r.failing.every((f) => f !== null && typeof f === "object" && Object.keys(f).join(",") === "requirement,why" &&
                                ATTACKER_SITE_REQUIREMENTS.indexOf(f.requirement) >= 0 && typeof f.why === "string");
}
function attackerSiteStated(record) {
  CHECK(record === null || attackerSiteRecordIs(record),
        "an attacker site was stated as " + JSON.stringify(record) + ", which is neither null nor a checked site");
  _attackerSite = record;
}
function attackerSiteNow() { return _attackerSite; }

/* Whose server `url` (a parsed URL) reaches: "attacker-site" when its host is the configured site or a name
   under it, compared on the URL parser's host and never on a spelling, and "another-party" otherwise, including
   whenever no site is configured. */
function attackerSiteReaches(url) {
  if (_attackerSite === null) return "another-party";
  const h = url.hostname, site = _attackerSite.site;
  return h === site || h.endsWith("." + site) ? "attacker-site" : "another-party";
}

/* The address the check sends to: a fresh name two labels under the site, so a resolution and a certificate for
   a name the person never configured shows what a composed origin needs: it joins every constant's labels under
   the site, at any depth, which wildcard DNS resolves and only a certificate issued per name covers (a
   certificate's `*` matches one label). */
function attackerSiteProbeUrl(site) {
  CHECK(attackerSiteRefusal(site) === null, "the attacker site check was asked of " + JSON.stringify(site));
  const b = new Uint8Array(8);
  crypto.getRandomValues(b);
  return "https://apiclient-check-" + Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("") +
         ".apiclient-depth." + site + "/";
}

/* The failing requirements of one check send's outcome: {netError, error} for a send that rejected, where
   `netError` is the browser's own reason (wire.js) or null where it stated none, or {status, headers} for an
   answer, `headers` lowercased names to combined values. A requirement the send never reached is failing as
   unchecked, and one whose evidence the browser did not state is failing as unknown. */
function attackerSiteRequirements(outcome) {
  const failing = [];
  const fail = (requirement, why) => failing.push({ requirement, why });
  const unchecked = (from, why) =>
    ATTACKER_SITE_REQUIREMENTS.slice(ATTACKER_SITE_REQUIREMENTS.indexOf(from)).forEach((r) => fail(r, why));
  if ("error" in outcome) {
    const ne = outcome.netError;
    if (ne === null) {
      unchecked("wildcard-dns", "unknown: the send failed (" + outcome.error + ") and the browser stated no reason");
    } else if (ne === "net::ERR_NAME_NOT_RESOLVED") {
      fail("wildcard-dns", ne);
      unchecked("certificate", "unchecked: the name did not resolve");
    } else if (/^net::ERR_(CERT_|SSL_)/.test(ne)) {
      fail("certificate", ne + " for a name two labels under the site: a composed origin needs a certificate " +
                          "for each name, which a wildcard certificate does not give");
      unchecked("serves", "unchecked: the connection was refused at TLS");
    } else {
      fail("serves", ne);
      unchecked("csp", "unchecked: no response");
    }
    return failing;
  }
  if (outcome.status >= 300 && outcome.status < 400) {
    fail("serves", "answered " + outcome.status + ", a redirect, so the document is another address's");
    unchecked("csp", "unchecked: the response is a redirect");
    return failing;
  }
  const evalRefusal = _attackerSiteEvalRefusal(outcome.headers["content-security-policy"]);
  if (evalRefusal !== null) fail("csp", evalRefusal);
  const coop = _attackerSiteOpenerPolicy(outcome.headers["cross-origin-opener-policy"]);
  if (coop === "same-origin") fail("coop", "Cross-Origin-Opener-Policy is same-origin, which opens the victim " +
                                           "in another browsing context group with no opener to post from");
  return failing;
}

/* Whether the answer's enforced policies let the attacker document evaluate a string: CSP3 §4.4.1
   EnsureCSPDoesNotBlockStringCompilation reads each policy's `script-src`, else its `default-src`, and blocks
   unless the list holds `'unsafe-eval'`. The policies are §2.2.2 "Parse response's Content Security Policies"
   over the header's comma-separated members, each parsed by §2.2.1 "Parse a serialized CSP" (directive names
   lowercased, a repeated name ignored). That algorithm first runs Trusted Types' compliant-string check, which
   a plain string fails under an enforced `require-trusted-types-for 'script'` unless a default policy the
   page's own script creates accepts it, so such a policy is a refusal too. Answers the refusal or null.
   Named residual. Not covered: a policy the document states in a `<meta http-equiv>` (HTML §4.2.5.3), which
   this header read cannot see. Next diff builds: the check reading it in the opened attacker document itself
   when a live verify arms it. Absence shows as: a site checked as meeting `csp` whose Run button reports an
   EvalError naming a policy. */
function _attackerSiteEvalRefusal(value) {
  if (value === undefined) return null;
  for (const serialized of value.split(",")) {
    const directives = new Map();
    for (const raw of serialized.split(";")) {
      const token = raw.replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, "");
      if (token === "" || !/^[\x00-\x7f]*$/.test(token)) continue;
      const parts = token.split(/[\t\n\f\r ]+/);
      const name = parts[0].toLowerCase();
      if (!directives.has(name)) directives.set(name, parts.slice(1));
    }
    const list = directives.has("script-src") ? directives.get("script-src") : directives.get("default-src");
    if (list !== undefined && !list.some((x) => x.toLowerCase() === "'unsafe-eval'"))
      return "an enforced policy's " + (directives.has("script-src") ? "script-src" : "default-src") +
             " lacks 'unsafe-eval': " + serialized.trim();
    const tt = directives.get("require-trusted-types-for");
    if (tt !== undefined && tt.indexOf("'script'") >= 0)
      return "an enforced policy requires Trusted Types for scripts: " + serialized.trim();
  }
  return null;
}

/* HTML §7.1.3.1 "The headers"' obtain an opener policy, over the header's value: the parsed item's token when
   it names a policy, and "unsafe-none" for an absent, unparsable or unrecognised field. A secure context is
   given, since the site is `https:`; `same-origin-plus-COEP` is a `same-origin` token, and answers so here. */
function _attackerSiteOpenerPolicy(value) {
  if (value === undefined) return "unsafe-none";
  const item = sfParseItem(value);
  if (item === null || item.bare.type !== "token") return "unsafe-none";
  return ["same-origin", "same-origin-allow-popups", "noopener-allow-popups"].indexOf(item.bare.value) >= 0
    ? item.bare.value : "unsafe-none";
}

if (typeof self !== "undefined") {
  self.attackerSiteRefusal = attackerSiteRefusal;
  self.attackerSiteStated = attackerSiteStated;
  self.attackerSiteNow = attackerSiteNow;
  self.attackerSiteRecordIs = attackerSiteRecordIs;
}
