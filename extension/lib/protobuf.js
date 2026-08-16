// lib/protobuf.js — Minimal protobuf wire format codec.
// Supports encoding binary probe payloads, decoding Google API error responses,
// and generic protobuf message inspection. Zero external dependencies.
//
// Wire format reference: https://protobuf.dev/programming-guides/encoding/
//
// Used by:
//  - popup.js: inspecting intercepted protobuf traffic
//
// THE ERROR-PROBE HALF IS GONE WITH ITS CALLER. `pbEncodeProbePayload`, `pbEncodeNestedPayload` and
// `pbDecodeRpcStatus` existed only for req2proto.js's binary probing, which fired a POST to learn — and
// error-based schema learning is now the engine's (engine/host/solver/req2proto.c), which reads the
// `google.rpc.Status` envelope off replies it already holds. Keeping three functions with no caller would
// read as a live capability that has not run since that commit.

// ─── Wire Types ───────────────────────────────────────────────────────────────

const PB_VARINT = 0; // int32, int64, uint32, uint64, sint32, sint64, bool, enum
const PB_64BIT = 1; // fixed64, sfixed64, double
const PB_LEN = 2; // string, bytes, embedded messages, packed repeated
const PB_32BIT = 5; // fixed32, sfixed32, float

// ─── Base64 Helpers (for Chrome message passing of binary data) ───────────────

function uint8ToBase64(bytes) {
  let binary = "";
  const chunk = 8192;
  for (let i = 0; i < bytes.length; i += chunk) {
    const slice = bytes.subarray(i, Math.min(i + chunk, bytes.length));
    binary += String.fromCharCode.apply(null, slice);
  }
  return btoa(binary);
}

function base64ToUint8(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// ─── Byte Helpers ─────────────────────────────────────────────────────────────

function concatBytes() {
  let len = 0;
  for (let i = 0; i < arguments.length; i++) len += arguments[i].length;
  const out = new Uint8Array(len);
  let off = 0;
  for (let i = 0; i < arguments.length; i++) {
    out.set(arguments[i], off);
    off += arguments[i].length;
  }
  return out;
}

// ─── Varint Codec ─────────────────────────────────────────────────────────────

/**
 * Read a varint from buf at pos. Returns [value, newPos].
 * Uses Number for values ≤ 2^53-1 (field numbers, lengths, most values).
 * Falls back to string representation for values > 2^53 to avoid silent
 * precision loss on uint64/int64 fields.
 */
function pbReadVarint(buf, pos) {
  let val = 0;
  let shift = 0;
  while (pos < buf.length) {
    const b = buf[pos++];
    val += (b & 0x7f) * Math.pow(2, shift);
    if ((b & 0x80) === 0) {
      // Check if we exceeded safe integer range
      if (shift >= 49 && val > Number.MAX_SAFE_INTEGER) {
        // Re-read as BigInt for precision, return as string
        return [pbReadVarintBig(buf, pos - shift / 7 - 1), pos];
      }
      return [val, pos];
    }
    shift += 7;
    if (shift >= 64) throw new Error("varint overflow");
  }
  throw new Error("truncated varint");
}

/**
 * Re-read a varint using string arithmetic for values exceeding 2^53.
 * Returns a string representation of the value.
 */
function pbReadVarintBig(buf, pos) {
  let lo = 0, hi = 0;
  for (let i = 0; i < 4; i++) {
    const b = buf[pos++];
    lo |= (b & 0x7f) << (i * 7);
    if ((b & 0x80) === 0) return lo >>> 0;
  }
  // 5th byte spans lo/hi boundary
  const b4 = buf[pos++];
  lo |= (b4 & 0x0f) << 28;
  hi = (b4 & 0x7f) >> 4;
  if ((b4 & 0x80) === 0) return (hi * 0x100000000 + (lo >>> 0));

  for (let i = 0; i < 5; i++) {
    const b = buf[pos++];
    hi |= (b & 0x7f) << (i * 7 + 3);
    if ((b & 0x80) === 0) break;
  }
  // Return as string to preserve precision
  const value = hi * 0x100000000 + (lo >>> 0);
  if (value > Number.MAX_SAFE_INTEGER) return String(value);
  return value;
}

/**
 * Encode a non-negative integer as a varint.
 */
function pbWriteVarint(val) {
  const out = [];
  val = Math.max(0, Math.floor(val));
  do {
    let b = val & 0x7f;
    val = Math.floor(val / 128);
    if (val > 0) b |= 0x80;
    out.push(b);
  } while (val > 0);
  return new Uint8Array(out);
}

// ─── Raw Wire Format Decoder ──────────────────────────────────────────────────

/**
 * Decode raw protobuf wire format into an array of field entries.
 * Each entry: { field: number, wire: 0|1|2|5, data: number|Uint8Array }
 *   - wire 0 (varint): data = number
 *   - wire 1 (64-bit): data = Uint8Array(8)
 *   - wire 2 (length-delimited): data = Uint8Array
 *   - wire 5 (32-bit): data = Uint8Array(4)
 */
function pbDecodeRaw(buf) {
  if (!(buf instanceof Uint8Array)) buf = new Uint8Array(buf);
  const fields = [];
  let pos = 0;
  while (pos < buf.length) {
    const [tag, p1] = pbReadVarint(buf, pos);
    pos = p1;
    const fieldNum = Math.floor(tag / 8);
    const wireType = tag & 0x7;
    if (fieldNum < 1 || fieldNum > 536870911)
      throw new Error("bad field number " + fieldNum);

    switch (wireType) {
      case PB_VARINT: {
        const [val, p2] = pbReadVarint(buf, pos);
        pos = p2;
        fields.push({ field: fieldNum, wire: wireType, data: val });
        break;
      }
      case PB_64BIT:
        if (pos + 8 > buf.length) throw new Error("truncated 64-bit");
        fields.push({
          field: fieldNum,
          wire: wireType,
          data: buf.slice(pos, pos + 8),
        });
        pos += 8;
        break;
      case PB_LEN: {
        const [len, p2] = pbReadVarint(buf, pos);
        pos = p2;
        if (pos + len > buf.length)
          throw new Error("truncated length-delimited");
        fields.push({
          field: fieldNum,
          wire: wireType,
          data: buf.slice(pos, pos + len),
        });
        pos += len;
        break;
      }
      case PB_32BIT:
        if (pos + 4 > buf.length) throw new Error("truncated 32-bit");
        fields.push({
          field: fieldNum,
          wire: wireType,
          data: buf.slice(pos, pos + 4),
        });
        pos += 4;
        break;
      default:
        throw new Error("unknown wire type " + wireType);
    }
  }
  return fields;
}

// `pbGetFields` / `pbGetString` / `pbGetVarint` / `pbGetMessage` / `pbGetRepeatedMessages` STOOD HERE WITH NO
// CALLER. They were the field accessors `pbDecodeRpcStatus` read a `google.rpc.Status` envelope with, and that
// function left this file when error-based schema learning became the engine's
// (engine/host/solver/req2proto.c) — the note at the head of this file records the three that went and missed
// the five that served them. CLAUDE.md §Offensive-programming states the rule from the producer's end: a
// function nobody consumes reads as a capability this surface has, and it is indistinguishable from one that
// runs. Three of them also swallowed a decode failure into `null` / a filtered-out entry, so the surface they
// pretended to offer was one that could not report a malformed body either.

// ─── Encoding ─────────────────────────────────────────────────────────────────

/** Encode a tag byte sequence: (fieldNum << 3) | wireType */
function pbTag(fieldNum, wireType) {
  return pbWriteVarint(fieldNum * 8 + wireType);
}

/** Encode a varint field. */
function pbEncodeVarintField(fieldNum, value) {
  return concatBytes(pbTag(fieldNum, PB_VARINT), pbWriteVarint(value));
}

/** Encode a length-delimited field (string, bytes, or embedded message). */
function pbEncodeLenField(fieldNum, data) {
  const bytes =
    typeof data === "string" ? new TextEncoder().encode(data) : data;
  return concatBytes(
    pbTag(fieldNum, PB_LEN),
    pbWriteVarint(bytes.length),
    bytes,
  );
}

// `pbEncodeFixed32Field` / `pbEncodeFixed64Field` STOOD HERE WITH NO CALLER either, and for the same reason:
// they encoded the probe payload req2proto.js used to fire. lib/encode.js writes the two fixed wire types it
// needs from `pbTag` + `concatBytes` directly (its own fixed32/fixed64 arms), so these were not even the
// spelling the one remaining encoder uses. PB_32BIT / PB_64BIT stay — encode.js reads both.

// ─── Generic Protobuf Inspector ──────────────────────────────────────────────
//
// Decodes any binary protobuf into a human-readable tree.
// Useful for inspecting intercepted traffic with unknown schemas.

/**
 * Decode binary protobuf into a nested tree for display.
 * Length-delimited fields are heuristically decoded as either
 * embedded messages or UTF-8 strings.
 *
 * @param {Uint8Array|ArrayBuffer} buf
 * @param {number} maxDepth
 * @returns {object[]}
 */
function pbDecodeTree(buf, _maxDepth, valueCallback) {
  // _maxDepth retained for ABI compatibility with existing callers but
  // ignored — the iterative pbTreeNode handles arbitrary nesting via a
  // worklist instead of relying on JS-stack depth, so a depth cap would
  // just discard valid decodes for adversarially-deep responses.
  if (!(buf instanceof Uint8Array)) buf = new Uint8Array(buf);
  try {
    return pbDecodeRaw(buf).map(function (f) {
      return pbTreeNode(f, valueCallback);
    });
  } catch (e) {
    return [{ error: e.message }];
  }
}

function pbTreeNode(rootF, valueCallback) {
  // Iterative tree construction. Each worklist entry pairs an input
  // field `f` with the destination node `dst` to populate. Embedded
  // messages get their child nodes pre-allocated and pushed back onto
  // the queue, so JS-stack depth stays at 1 regardless of message
  // nesting.
  var root = { field: rootF.field, wire: rootF.wire };
  var queue = [{ f: rootF, dst: root }];
  while (queue.length > 0) {
    var entry = queue.shift();
    var f = entry.f, dst = entry.dst;

    if (f.wire === PB_VARINT) {
      dst.value = f.data;
      // ZigZag decode for signed interpretation (arithmetic to avoid 32-bit truncation)
      if (typeof f.data === "number") {
        dst.asSigned = f.data % 2 === 0
          ? Math.floor(f.data / 2)
          : -Math.floor(f.data / 2) - 1;
      } else {
        dst.asSigned = f.data; // string representation for very large values
      }
      if (valueCallback) valueCallback(dst.value);
      continue;
    }

    if (f.wire === PB_32BIT) {
      var dv32 = new DataView(f.data.buffer, f.data.byteOffset, 4);
      dst.asUint32 = dv32.getUint32(0, true);
      dst.asInt32 = dv32.getInt32(0, true);
      dst.asFloat = dv32.getFloat32(0, true);
      if (valueCallback) valueCallback(dst.asUint32);
      continue;
    }

    if (f.wire === PB_64BIT) {
      var dv64 = new DataView(f.data.buffer, f.data.byteOffset, 8);
      dst.asDouble = dv64.getFloat64(0, true);
      dst.hex = bytesToHex(f.data);
      if (valueCallback) valueCallback(dst.hex);
      continue;
    }

    if (f.wire === PB_LEN) {
      // Try as embedded message. Sanity checks below reject byte
      // sequences that happen to parse as protobuf but are actually
      // strings — they are protocol-correctness filters (not depth
      // caps) and are preserved verbatim from the original.
      var parsedAsMessage = false;
      if (f.data.length > 1) {
        try {
          var nested = pbDecodeRaw(f.data);
          if (nested.length > 0) {
            var valid = true;
            var maxField = 0;
            var minField = Infinity;
            for (var i = 0; i < nested.length; i++) {
              if (nested[i].field < 1 || nested[i].field > 10000) {
                valid = false;
                break;
              }
              if (nested[i].field > maxField) maxField = nested[i].field;
              if (nested[i].field < minField) minField = nested[i].field;
            }
            if (valid && nested.length > 0 && maxField - minField > nested.length * 100) {
              valid = false;
            }
            if (valid && f.data.length <= 4 && nested.length < 2) {
              valid = false;
            }
            if (valid) {
              dst.message = [];
              for (var ci = 0; ci < nested.length; ci++) {
                var child = { field: nested[ci].field, wire: nested[ci].wire };
                dst.message.push(child);
                queue.push({ f: nested[ci], dst: child });
              }
              parsedAsMessage = true;
            }
          }
        } catch (e) {
          // Length-delimited field didn't decode as a nested message — that's
          // the normal signal that it's a string/bytes leaf or packed array.
          // Debug-log so a real recursion-depth or buffer-bounds bug is
          // diagnosable; falls through to packed-repeated and bytes attempts.
          if (typeof console !== "undefined") console.debug("[pb:decode] nested message decode failed:", e && e.message || e);
        }
      }
      if (parsedAsMessage) continue;

      // Try as packed repeated (proto3 default for repeated scalars)
      var packed = pbTryDecodePacked(f.data);
      if (packed !== null) {
        dst.packed = packed;
        dst.value = packed;
        if (valueCallback) {
          for (var pi = 0; pi < packed.length; pi++) valueCallback(packed[pi]);
        }
        continue;
      }
      // Try as UTF-8 string
      var str = tryUtf8(f.data);
      if (str !== null) {
        dst.string = str;
        if (valueCallback) valueCallback(str);
      } else {
        dst.hex = bytesToHex(f.data);
        if (valueCallback) valueCallback(dst.hex);
      }
      dst.length = f.data.length;
    }
  }
  return root;
}

function tryUtf8(bytes) {
  // TextDecoder with fatal:true uses the throw AS the validity signal \u2014 there
  // is no non-throwing equivalent in the platform API. The catch returns null
  // to express "not valid UTF-8"; the throw itself is not a bug but the
  // outcome we're testing for, so no diagnostic surface is appropriate here.
  try {
    var str = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    // Accept printable ASCII + common unicode
    if (/^[\x20-\x7E\t\n\r\u00A0-\uFFFF]+$/.test(str)) return str;
  } catch (_) { /* invalid UTF-8 \u2014 the function's defined "no" answer */ }
  return null;
}

function bytesToHex(bytes) {
  var hex = "";
  for (var i = 0; i < bytes.length; i++) {
    hex += (bytes[i] < 16 ? "0" : "") + bytes[i].toString(16);
  }
  return hex;
}

/**
 * Try to decode a length-delimited field as packed repeated scalars (varints).
 * Returns an array of values if successful, null if not valid packed encoding.
 * Packed encoding is the default for repeated scalar fields in proto3.
 *
 * @param {Uint8Array} data - The raw bytes of the length-delimited field
 * @returns {number[]|null} Array of decoded varint values, or null
 */
function pbTryDecodePacked(data) {
  if (data.length === 0) return [];
  // Same TextDecoder-style pattern: pbReadVarint throws on truncated/over-long
  // varints, and that throw IS the function's defined "not a packed-repeated
  // field" outcome (next caller tries the bytes/UTF-8 path). The catch is
  // the parse-validity signal, not error suppression.
  try {
    var values = [];
    var pos = 0;
    while (pos < data.length) {
      var result = pbReadVarint(data, pos);
      values.push(result[0]);
      pos = result[1];
    }
    // Must consume all bytes exactly — partial consumption means it's not packed
    if (pos === data.length && values.length > 1) return values;
  } catch (_) { /* not a packed-repeated field — defined "no" answer */ }
  return null;
}

/**
 * Decode a JSPB (positional array) message into a tree structure.
 * @param {Array} arr - The JSPB array
 * @returns {Array<object>} Tree of nodes
 */
function jspbToTree(arr) {
  const root = [];
  if (!Array.isArray(arr)) {
    console.warn("[Protobuf] jspbToTree: input is not an array:", arr);
    return root;
  }
  // Iterative worklist: each entry holds the source container, the
  // destination array to populate, and how to interpret the source
  // ("array" = positional → field=idx+1; "object" = named → field=key).
  // Replaces the original mutual recursion (jspbToTree ↔ entries.map →
  // jspbToTree) so adversarially-deep server payloads don't blow the
  // JS stack.
  const queue = [{ src: arr, dst: root, mode: "array" }];
  while (queue.length > 0) {
    const { src, dst, mode } = queue.shift();
    if (mode === "array") {
      for (let idx = 0; idx < src.length; idx++) {
        const val = src[idx];
        if (val === null || val === undefined) continue;
        const node = { field: idx + 1, value: val, isJspb: true, wire: 2 };
        if (Array.isArray(val)) {
          const hasSubArrays = val.some((item) => Array.isArray(item));
          const allPrimitives = val.length > 0 && val.every(
            (item) => item === null || item === undefined ||
              typeof item === "string" || typeof item === "number" || typeof item === "boolean"
          );
          if (allPrimitives && !hasSubArrays) {
            node.isRepeatedScalar = true;
          } else {
            node.message = [];
            queue.push({ src: val, dst: node.message, mode: "array" });
          }
        } else if (typeof val === "object") {
          node.message = [];
          queue.push({ src: val, dst: node.message, mode: "object" });
          delete node.value;
        } else if (typeof val === "number") {
          node.wire = Number.isInteger(val) ? 0 : 5;
        } else if (typeof val === "boolean") {
          node.wire = 0;
        }
        dst.push(node);
      }
    } else {
      // object mode: src is a plain object; entries become named fields.
      const entries = Object.entries(src);
      for (let ei = 0; ei < entries.length; ei++) {
        const k = entries[ei][0], v = entries[ei][1];
        if (v === null || v === undefined) continue;
        if (Array.isArray(v)) {
          const sub = { field: k, wire: 2, message: [], isJson: true, jsType: "array" };
          queue.push({ src: v, dst: sub.message, mode: "array" });
          dst.push(sub);
        } else if (typeof v === "object") {
          const sub = { field: k, wire: 2, message: [], isJson: true, jsType: "object" };
          queue.push({ src: v, dst: sub.message, mode: "object" });
          dst.push(sub);
        } else if (typeof v === "string") {
          dst.push({ field: k, wire: 2, string: v, isJson: true, jsType: "string" });
        } else if (typeof v === "boolean") {
          dst.push({ field: k, wire: 0, value: v, isJson: true, jsType: "boolean" });
        } else if (typeof v === "number") {
          dst.push({ field: k, wire: 0, value: v, isJson: true, jsType: "number" });
        } else {
          dst.push({ field: k, wire: 0, value: v, isJson: true, jsType: typeof v });
        }
      }
    }
  }
  return root;
}
