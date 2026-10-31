// lib/structured-fields.js — RFC 9651 Structured Field Values for HTTP, §4.2 "Parsing Structured Fields", for
// the "item" field type: what HTML §7.1.3.1 "The headers" asks of `Cross-Origin-Opener-Policy`. The engine's
// own parser is core/fetch/structured_fields.c; the trusted zone reads the headers of its own sends here.

/* The parse of `value`, a field's combined value (RFC 9110 §5.2's comma-joined lines, as a Headers object
   answers it), as an item: {bare: {type, value}, params: Map key → bare item}, or null where §4.2 fails
   parsing, which §4.2 has the caller treat as the field being absent. `type` is "integer", "decimal",
   "string", "token", "byte-sequence", "boolean", "date" or "display-string". */
function sfParseItem(value) {
  if (typeof value !== "string" || !/^[\x00-\x7f]*$/.test(value)) return null;
  const st = { s: value, i: 0 };
  try {
    while (st.s[st.i] === " ") st.i++;
    const bare = _sfBareItem(st);
    const params = _sfParams(st);
    while (st.s[st.i] === " ") st.i++;
    if (st.i !== st.s.length) return null;
    return { bare, params };
  } catch (e) {
    if (e !== _SF_FAIL) throw e;
    return null;
  }
}

/* §4.2's "fail parsing", thrown within this file and caught at its one entry. */
const _SF_FAIL = Object.freeze({ sfFail: true });
function _sfFail() { throw _SF_FAIL; }
const _sfDigit = (c) => c !== undefined && c >= "0" && c <= "9";
const _sfAlpha = (c) => c !== undefined && /[A-Za-z]/.test(c);
const _sfLcalpha = (c) => c !== undefined && c >= "a" && c <= "z";
/* RFC 9110 §5.6.2 Tokens' tchar. */
const _sfTchar = (c) => c !== undefined && /[!#$%&'*+\-.^_`|~0-9A-Za-z]/.test(c);

/* §4.2.3.1 Parsing a Bare Item. */
function _sfBareItem(st) {
  const c = st.s[st.i];
  if (c === "-" || _sfDigit(c)) return _sfNumber(st);
  if (c === "\"") return { type: "string", value: _sfString(st) };
  if (_sfAlpha(c) || c === "*") return { type: "token", value: _sfToken(st) };
  if (c === ":") return { type: "byte-sequence", value: _sfBytes(st) };
  if (c === "?") return { type: "boolean", value: _sfBoolean(st) };
  if (c === "@") {
    st.i++;
    const d = _sfNumber(st);
    if (d.type !== "integer") _sfFail();
    return { type: "date", value: d.value };
  }
  if (c === "%") return { type: "display-string", value: _sfDisplay(st) };
  return _sfFail();
}

/* §4.2.3.2 Parsing Parameters, with §4.2.3.3 Parsing a Key; a later duplicate key overwrites. */
function _sfParams(st) {
  const params = new Map();
  while (st.i < st.s.length && st.s[st.i] === ";") {
    st.i++;
    while (st.s[st.i] === " ") st.i++;
    if (!(_sfLcalpha(st.s[st.i]) || st.s[st.i] === "*")) _sfFail();
    let key = "";
    while (st.i < st.s.length && (_sfLcalpha(st.s[st.i]) || _sfDigit(st.s[st.i]) || "_-.*".includes(st.s[st.i])))
      key += st.s[st.i++];
    let v = { type: "boolean", value: true };
    if (st.s[st.i] === "=") { st.i++; v = _sfBareItem(st); }
    params.set(key, v);
  }
  return params;
}

/* §4.2.4 Parsing an Integer or Decimal. */
function _sfNumber(st) {
  let type = "integer", sign = 1, num = "";
  if (st.s[st.i] === "-") { st.i++; sign = -1; }
  if (st.i >= st.s.length || !_sfDigit(st.s[st.i])) _sfFail();
  while (st.i < st.s.length) {
    const c = st.s[st.i++];
    if (_sfDigit(c)) num += c;
    else if (type === "integer" && c === ".") {
      if (num.length > 12) _sfFail();
      num += c;
      type = "decimal";
    } else { st.i--; break; }
    if (type === "integer" && num.length > 15) _sfFail();
    if (type === "decimal" && num.length > 16) _sfFail();
  }
  if (type === "decimal" && (num.endsWith(".") || num.length - num.indexOf(".") - 1 > 3)) _sfFail();
  return { type, value: sign * Number(num) };
}

/* §4.2.5 Parsing a String. */
function _sfString(st) {
  let out = "";
  st.i++;
  while (st.i < st.s.length) {
    const c = st.s[st.i++];
    if (c === "\\") {
      if (st.i >= st.s.length) _sfFail();
      const n = st.s[st.i++];
      if (n !== "\"" && n !== "\\") _sfFail();
      out += n;
    } else if (c === "\"") return out;
    else if (c < " " || c > "~") _sfFail();
    else out += c;
  }
  return _sfFail();
}

/* §4.2.6 Parsing a Token: tchar, ":" and "/" after an ALPHA or "*". */
function _sfToken(st) {
  let out = "";
  while (st.i < st.s.length && (_sfTchar(st.s[st.i]) || st.s[st.i] === ":" || st.s[st.i] === "/"))
    out += st.s[st.i++];
  return out;
}

/* §4.2.7 Parsing a Byte Sequence: the base64 content's bytes, padding synthesized where absent (the §4.2.7
   SHOULD NOT fail on a missing `=` or on non-zero pad bits, which atob accepts too). */
function _sfBytes(st) {
  st.i++;
  const end = st.s.indexOf(":", st.i);
  if (end < 0) _sfFail();
  const b64 = st.s.slice(st.i, end);
  st.i = end + 1;
  if (!/^[A-Za-z0-9+/=]*$/.test(b64)) _sfFail();
  let bin;
  try { bin = atob(b64.replace(/=+$/, "").padEnd(Math.ceil(b64.replace(/=+$/, "").length / 4) * 4, "=")); }
  catch (e) { return _sfFail(); }
  return Uint8Array.from(bin, (ch) => ch.charCodeAt(0));
}

/* §4.2.8 Parsing a Boolean. */
function _sfBoolean(st) {
  st.i++;
  const c = st.s[st.i++];
  if (c === "1") return true;
  if (c === "0") return false;
  return _sfFail();
}

/* §4.2.10 Parsing a Display String: percent-encoded lowercase hex octets, decoded as UTF-8. */
function _sfDisplay(st) {
  if (st.s[st.i + 1] !== "\"") _sfFail();
  st.i += 2;
  const bytes = [];
  while (st.i < st.s.length) {
    const c = st.s[st.i++];
    if (c < " " || c > "~") _sfFail();
    if (c === "%") {
      const h = st.s.slice(st.i, st.i + 2);
      if (!/^[0-9a-f]{2}$/.test(h)) _sfFail();
      st.i += 2;
      bytes.push(parseInt(h, 16));
    } else if (c === "\"") {
      try { return new TextDecoder("utf-8", { fatal: true }).decode(new Uint8Array(bytes)); }
      catch (e) { return _sfFail(); }
    } else bytes.push(c.charCodeAt(0));
  }
  return _sfFail();
}

if (typeof self !== "undefined") self.sfParseItem = sfParseItem;
