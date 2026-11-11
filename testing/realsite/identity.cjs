/* A request's identity for the real-site diff, shared by the Chromium capture and the engine reader so the two
   sides are keyed alike: origin, path and the sorted query keys. Query values are left out of the identity
   because a cache-buster or a session token differs per load; a value is kept beside it only when it is short
   and holds no long token-like run. */
'use strict';

function redactValue(v) {
  return v.length > 24 || /[A-Za-z0-9_\-]{16,}/.test(v) ? `<${v.length} chars>` : v;
}

/* {identity, origin, path, query: [[key, value-or-redaction]]} of an http(s) or ws(s) URL; any other scheme keeps
   the scheme alone, and a string that does not parse is named as such. */
function describeUrl(raw) {
  let u;
  try { u = new URL(raw); } catch { return { identity: '<unparsable>', origin: null, path: null, query: [] }; }
  if (!['http:', 'https:', 'ws:', 'wss:'].includes(u.protocol))
    return { identity: u.protocol, origin: null, path: null, query: [] };
  const keys = [...new Set(u.searchParams.keys())].sort();
  return {
    identity: u.origin + u.pathname + (keys.length ? '?' + keys.join('&') : ''),
    origin: u.origin, path: u.pathname,
    query: [...u.searchParams].map(([k, v]) => [k, redactValue(v)]),
  };
}

module.exports = { describeUrl, redactValue };
