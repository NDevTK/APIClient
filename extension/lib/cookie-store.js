// lib/cookie-store.js — a reply's Set-Cookie, stored the way the identity's own request would store it: RFC
// 6265bis (draft-ietf-httpbis-rfc6265bis-22) §5.6 "The Set-Cookie Header Field" parses each field value and
// §5.7 "Storage Model" decides whether the jar receives the cookie, with CHIPS
// (draft-cutler-httpbis-partitioned-cookies-01) §2.3 "Using Set-Cookie with Partitioned" choosing its
// partition. A kept cookie is written by the service worker's chrome.cookies.set; an ignored one is never
// written. Loaded after lib/site.js, lib/referrer-policy.js and lib/request-identity.js.

/* §5.5 "Cookie Lifetime Limits": 400 days. */
var COOKIE_AGE_LIMIT_SECONDS = 400 * 24 * 60 * 60;
var _COOKIE_CTL = /[\x00-\x08\x0A-\x1F\x7F]/;
var _COOKIE_MONTHS = ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"];

function _cookieOctets(s) { return new TextEncoder().encode(s).length; }
function _cookieTrimWsp(s) { return s.replace(/^[ \t]+|[ \t]+$/g, ""); }

/* §5.1.1 "Dates": the time in ms since the epoch, or null where the cookie-date fails to parse. */
function cookieDateParse(input) {
  var tokens = input.split(/[\x09\x20-\x2F\x3B-\x40\x5B-\x60\x7B-\x7E]+/).filter(function (t) { return t !== ""; });
  var time = null, day = null, month = null, year = null, m;
  for (var i = 0; i < tokens.length; i++) {
    var t = tokens[i];
    if (time === null && (m = /^(\d{1,2}):(\d{1,2}):(\d{1,2})(?:\D[\s\S]*)?$/.exec(t))) {
      time = [+m[1], +m[2], +m[3]];
      continue;
    }
    if (day === null && (m = /^(\d{1,2})(?:\D[\s\S]*)?$/.exec(t))) { day = +m[1]; continue; }
    if (month === null && _COOKIE_MONTHS.indexOf(t.slice(0, 3).toLowerCase()) >= 0) {
      month = _COOKIE_MONTHS.indexOf(t.slice(0, 3).toLowerCase());
      continue;
    }
    if (year === null && (m = /^(\d{2,4})(?:\D[\s\S]*)?$/.exec(t))) { year = +m[1]; continue; }
  }
  if (year !== null && year >= 70 && year <= 99) year += 1900;
  if (year !== null && year >= 0 && year <= 69) year += 2000;
  if (time === null || day === null || month === null || year === null || day < 1 || day > 31 || year < 1601 ||
      time[0] > 23 || time[1] > 59 || time[2] > 59) return null;
  var ms = Date.UTC(year, month, day, time[0], time[1], time[2]);
  // Step 6: a day the month does not have (31 April) is no date; Date.UTC would roll it into the next month.
  return new Date(ms).getUTCDate() === day ? ms : null;
}

/* §5.1.4 "Paths and Path-Match": the default-path of a request URL's path. */
function cookieDefaultPath(uriPath) {
  if (uriPath === "" || uriPath[0] !== "/") return "/";
  var last = uriPath.lastIndexOf("/");
  return last === 0 ? "/" : uriPath.slice(0, last);
}
function cookiePathMatches(requestPath, cookiePath) {
  if (requestPath === cookiePath) return true;
  if (requestPath.indexOf(cookiePath) !== 0) return false;
  return cookiePath[cookiePath.length - 1] === "/" || requestPath[cookiePath.length] === "/";
}

/* §5.1.3 "Domain Matching", over canonicalized (lowercase, A-label) inputs. */
function cookieDomainMatches(string, domain) {
  if (string === domain) return true;
  return string.length > domain.length && string.slice(string.length - domain.length) === domain &&
         string[string.length - domain.length - 1] === "." && _hostIsDomain(string);
}

/* §5.6's two algorithms over one set-cookie-string received from `url`, at `now` (ms): {name, value, attrs:
   [{name, value}]} with attribute names in the case §5.6.1–§5.6.7 spell them and each attribute's processed
   value, or {ignored: rule} where the string is ignored entirely. CHIPS §2.3 adds Partitioned. */
function cookieParse(input, url, now) {
  if (_COOKIE_CTL.test(input)) return { ignored: "a CTL character (RFC 6265bis §5.6 step 1)" };
  var semi = input.indexOf(";");
  var pair = semi >= 0 ? input.slice(0, semi) : input, rest = semi >= 0 ? input.slice(semi) : "";
  var eq = pair.indexOf("=");
  var name = _cookieTrimWsp(eq >= 0 ? pair.slice(0, eq) : "");
  var value = _cookieTrimWsp(eq >= 0 ? pair.slice(eq + 1) : pair);
  if (_cookieOctets(name) + _cookieOctets(value) > 4096)
    return { ignored: "a name and value over 4096 octets (RFC 6265bis §5.6 step 5)" };
  var attrs = [];
  while (rest !== "") {
    rest = rest.slice(1);
    var next = rest.indexOf(";");
    var av = next >= 0 ? rest.slice(0, next) : rest;
    rest = next >= 0 ? rest.slice(next) : "";
    var aeq = av.indexOf("=");
    var an = _cookieTrimWsp(aeq >= 0 ? av.slice(0, aeq) : av).toLowerCase();
    var aval = _cookieTrimWsp(aeq >= 0 ? av.slice(aeq + 1) : "");
    if (_cookieOctets(aval) > 1024) continue;
    if (an === "expires") {
      var expiry = cookieDateParse(aval);
      if (expiry === null) continue;
      attrs.push({ name: "Expires", value: Math.min(expiry, now + COOKIE_AGE_LIMIT_SECONDS * 1000) });
    } else if (an === "max-age") {
      if (!/^-?\d+$/.test(aval)) continue;
      var delta = Math.min(Number(aval), COOKIE_AGE_LIMIT_SECONDS);
      attrs.push({ name: "Max-Age", value: delta <= 0 ? -Infinity : now + delta * 1000 });
    } else if (an === "domain") {
      attrs.push({ name: "Domain", value: (aval[0] === "." ? aval.slice(1) : aval).toLowerCase() });
    } else if (an === "path") {
      attrs.push({ name: "Path",
                   value: aval === "" || aval[0] !== "/" ? cookieDefaultPath(new URL(url).pathname) : aval });
    } else if (an === "secure" || an === "httponly" || an === "partitioned") {
      attrs.push({ name: an === "secure" ? "Secure" : an === "httponly" ? "HttpOnly" : "Partitioned", value: "" });
    } else if (an === "samesite") {
      var e = aval.toLowerCase();
      attrs.push({ name: "SameSite",
                   value: e === "none" ? "None" : e === "strict" ? "Strict" : e === "lax" ? "Lax" : "Default" });
    }
  }
  return { name: name, value: value, attrs: attrs };
}
function _cookieLast(attrs, name) {
  for (var i = attrs.length - 1; i >= 0; i--) if (attrs[i].name === name) return attrs[i];
  return null;
}

var _COOKIE_API_SAME_SITE = { Strict: "strict", Lax: "lax", None: "no_restriction", Default: "unspecified" };

/* §5.7 "Storage Model" for a cookie received from `url` by a request that speaks with `facts`
   (identityFor's answer): {details} for chrome.cookies.set, or {ignored: rule}. Step 1's ignore is this
   extension's third-party-cookie setting, which spares a Partitioned cookie (measured in Chrome 148). The
   request URL is secure as Chrome counts it, a potentially trustworthy URL (measured: Chrome 148 sets a
   Secure cookie for http://127.0.0.1 and http://localhost). Step 16 reads the unpartitioned jar, where every
   cookie a non-secure one could overlay lives, since a partitioned cookie is secure. */
async function cookieStorageModel(cookie, url, facts) {
  var u = new URL(url), host = u.hostname, attrs = cookie.attrs;
  if (_cookieLast(attrs, "Partitioned") === null) {
    var blocked = await thirdPartyCookiesBlocked(facts);
    if (blocked !== null) return { ignored: blocked + " (RFC 6265bis §5.7 step 1)" };
  }
  if (cookie.name === "" && cookie.value === "")
    return { ignored: "an empty name and value (RFC 6265bis §5.7 step 2)" };
  DCHECK(!_COOKIE_CTL.test(cookie.name + cookie.value) &&
         _cookieOctets(cookie.name) + _cookieOctets(cookie.value) <= 4096,
         "a parsed cookie carries a CTL character or more than 4096 octets — cookieParse ignores both before " +
         "it names a cookie, so §5.7 steps 3 and 4 hold by construction");
  var expiry = _cookieLast(attrs, "Max-Age") || _cookieLast(attrs, "Expires");
  var domainAttr = _cookieLast(attrs, "Domain");
  var domain = domainAttr === null ? "" : domainAttr.value;
  if (/[^\x01-\x7F]/.test(domain)) return { ignored: "a Domain outside CHAR (RFC 6265bis §5.7 step 8)" };
  /* RFC 6265bis §5.7 step 9, as Chrome decides it (net/cookies/cookie_util.cc's GetCookieDomainWithString): a
     Domain with no registrable domain of its own (a public suffix, a wildcard rule's parent included, or a
     single-label or IP host) sets a host cookie when it is the request's host and nothing otherwise. */
  await siteAsk(domain === "" ? [host] : [host, domain]);
  if (domain !== "" && registrableDomainOf(domain) === null) {
    if (domain !== host) return { ignored: "Domain=" + domain + " is a public suffix (RFC 6265bis §5.7 step 9)" };
    domain = "";
  }
  if (domain !== "" && !cookieDomainMatches(host, domain))
    return { ignored: host + " does not domain-match Domain=" + domain + " (RFC 6265bis §5.7 step 10)" };
  var hostOnly = domain === "";
  var pathAttr = _cookieLast(attrs, "Path");
  var path = pathAttr === null ? cookieDefaultPath(u.pathname) : pathAttr.value;
  var secure = _cookieLast(attrs, "Secure") !== null, secureUrl = urlPotentiallyTrustworthy(u);
  if (secure && !secureUrl) return { ignored: "Secure from a URL that is not secure (RFC 6265bis §5.7 step 13)" };
  var httpOnly = _cookieLast(attrs, "HttpOnly") !== null;
  if (!secure && !secureUrl) {
    var cookieDomain = hostOnly ? host : domain;
    var same = await swRpc("cookies.getAll", { name: cookie.name });
    for (var i = 0; i < same.length; i++) {
      var d = same[i].domain[0] === "." ? same[i].domain.slice(1) : same[i].domain;
      if (same[i].secure && (cookieDomainMatches(d, cookieDomain) || cookieDomainMatches(cookieDomain, d)) &&
          cookiePathMatches(path, same[i].path))
        return { ignored: "it would overlay the secure cookie " + JSON.stringify(cookie.name) + " of " +
                          same[i].domain + same[i].path + " (RFC 6265bis §5.7 step 16)" };
    }
  }
  var sameSiteAttr = _cookieLast(attrs, "SameSite");
  var sameSite = sameSiteAttr === null ? "Default" : sameSiteAttr.value;
  if (sameSite !== "None" && !facts.sameSite && !facts.topLevelNavigation)
    return { ignored: "SameSite " + sameSite + " from a cross-site request that is not a top-level navigation " +
                      "(RFC 6265bis §5.7 step 18)" };
  if (sameSite === "None" && !secure) return { ignored: "SameSite=None without Secure (RFC 6265bis §5.7 step 19)" };
  var lname = cookie.name.toLowerCase(), lvalue = cookie.value.toLowerCase();
  if (lname.indexOf("__secure-") === 0 && !secure)
    return { ignored: "a __Secure- name without Secure (RFC 6265bis §5.7 step 20)" };
  if (lname.indexOf("__host-") === 0 && !(secure && hostOnly && pathAttr !== null && path === "/"))
    return { ignored: "a __Host- name without Secure, host-only and Path=/ (RFC 6265bis §5.7 step 21)" };
  if (cookie.name === "" && (lvalue.indexOf("__secure-") === 0 || lvalue.indexOf("__host-") === 0))
    return { ignored: "an empty name whose value spells a prefix (RFC 6265bis §5.7 step 22)" };
  var partitioned = _cookieLast(attrs, "Partitioned") !== null;
  if (partitioned && !secure) return { ignored: "Partitioned without Secure (CHIPS §2.3)" };
  var details = { url: url, name: cookie.name, value: cookie.value, path: path, secure: secure, httpOnly: httpOnly,
                  sameSite: _COOKIE_API_SAME_SITE[sameSite] };
  if (!hostOnly) details.domain = domain;
  // §5.7 step 6's persistent-flag; an expiry before the epoch is chrome.cookies' earliest, which evicts.
  if (expiry !== null) details.expirationDate = Math.max(expiry.value, 0) / 1000;
  if (partitioned) details.partitionKey = { topLevelSite: facts.partitionKey.topLevelSite,
                                            hasCrossSiteAncestor: facts.partitionKey.hasCrossSiteAncestor };
  return { details: details };
}

/* Fetch §4.7 "HTTP-network fetch"'s "parse and store response `Set-Cookie` headers" for a reply to `url`
   whose request included credentials and spoke with `facts`: each field value in order, as `fields` lists
   them (a string, or null for one webRequest reported as bytes, which chrome.cookies.set cannot take).
   Answers [{name, stored} | {name, ignored}], where the browser's own refusal of a write is an ignore too. */
async function cookieStoreSetCookies(fields, url, facts) {
  CHECK(Array.isArray(fields) && !!facts && typeof facts.sameSite === "boolean" &&
        typeof facts.topLevelNavigation === "boolean" && !!facts.partitionKey &&
        typeof facts.partitionKey.topLevelSite === "string",
        "a reply's cookies were stored without its Set-Cookie list or identityFor's facts — the jar, the " +
        "partition and the SameSite step are all the request's");
  var out = [];
  for (var i = 0; i < fields.length; i++) {
    if (fields[i] === null) {
      out.push({ name: null, ignored: "webRequest reported the field as bytes, and chrome.cookies.set takes text" });
      continue;
    }
    var cookie = cookieParse(fields[i], url, Date.now());
    if (cookie.ignored) { out.push({ name: null, ignored: cookie.ignored }); continue; }
    var decided = await cookieStorageModel(cookie, url, facts);
    if (decided.ignored) { out.push({ name: cookie.name, ignored: decided.ignored }); continue; }
    var wrote = await swRpc("cookies.set", decided.details);
    out.push(wrote.refused === null ? { name: cookie.name, stored: true }
      : { name: cookie.name, ignored: "the browser's cookie store refused it: " + wrote.refused });
  }
  return out;
}
