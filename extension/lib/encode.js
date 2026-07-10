// lib/encode.js — Request-body encoding for replay/export: build GraphQL, form->JSON, form->JSPB, and
// form->protobuf wire bodies from the popup Send panel's field model. Extracted from the offscreen-brain.js
// monolith (one problem per file); loaded before it, resolves coerceValue + the protobuf lib at call-time.
// The Send-panel / Form Builder / OpenAPI-round-trip encoding feature, just relocated.

/**
 * Encode GraphQL body from popup operations array.
 * Supports single and batched (array) format, preserves extensions.
 */
function encodeGraphQLBody(bodyMsg) {
  const ops = bodyMsg.operations || [];
  const encode = (op) => {
    // Reddit-style persisted-operation envelope: the server maps `operation`
    // to a stored query doc. No query text goes over the wire. We emit the
    // exact shape the server expects instead of forcing a spec-compliant
    // `{query}` envelope that reddit's backend would reject.
    let obj;
    if (op.operation && !op.query) {
      obj = { operation: op.operation };
      if (op.variables) {
        try { obj.variables = typeof op.variables === "string" ? JSON.parse(op.variables) : op.variables; }
        catch (_) { obj.variables = op.variables; }
      }
      if (op.extensions) {
        try { obj.extensions = typeof op.extensions === "string" ? JSON.parse(op.extensions) : op.extensions; }
        catch (_) { obj.extensions = op.extensions; }
      }
    } else {
      obj = { query: op.query || "" };
      if (op.variables) {
        try { obj.variables = typeof op.variables === "string" ? JSON.parse(op.variables) : op.variables; }
        catch (_) { obj.variables = op.variables; }
      }
      if (op.operationName) obj.operationName = op.operationName;
      if (op.extensions) {
        try { obj.extensions = typeof op.extensions === "string" ? JSON.parse(op.extensions) : op.extensions; }
        catch (_) { obj.extensions = op.extensions; }
      }
    }
    // Attach any extra top-level fields preserved from the captured
    // envelope (csrf_token, clientId, rid, ...). Existing standard keys
    // win if a collision happens.
    if (op.extra && typeof op.extra === "object") {
      for (const k in op.extra) {
        if (!(k in obj)) obj[k] = op.extra[k];
      }
    }
    return obj;
  };
  if (bodyMsg.batched) return JSON.stringify(ops.map(encode));
  return JSON.stringify(ops.length > 0 ? encode(ops[0]) : { query: "" });
}

function encodeFormToJson(rootFields) {
  // Iterative tree builder. Each work item populates a target object
  // (or array element) from a fields list. Nested message/repeated
  // fields enqueue empty sub-objects whose children arrays drive a
  // later iteration. Replaces self-recursion so deeply-nested form
  // structures encode without growing the JS call stack.
  const root = {};
  const queue = [{ fields: rootFields, target: root }];
  while (queue.length > 0) {
    const { fields, target } = queue.shift();
    for (const f of fields) {
      const isObj = f.type === "message" || f.type === "object";

      if (f.label === "repeated") {
        const list = [];
        target[f.name] = list;
        if (Array.isArray(f.value)) {
          for (const v of f.value) {
            if (v && typeof v === "object" && !Array.isArray(v) && Array.isArray(v.children)) {
              const sub = {};
              list.push(sub);
              queue.push({ fields: v.children, target: sub });
            } else {
              list.push(isObj ? v : coerceValue(v, f.type));
            }
          }
        } else if (Array.isArray(f.children)) {
          for (const item of f.children) {
            if (Array.isArray(item.children)) {
              const sub = {};
              list.push(sub);
              queue.push({ fields: item.children, target: sub });
            } else {
              list.push(coerceValue(item.value, f.type));
            }
          }
        }
        continue;
      }

      if (isObj) {
        // Message/object: prefer children tree; fall back to raw value
        // when the caller has a parsed object but no tree (e.g. replay
        // auto-fill from captured JSON). Always surface the field even
        // when empty so servers see `{variables: {}}` rather than
        // dropping it.
        if (Array.isArray(f.children) && f.children.length) {
          const sub = {};
          target[f.name] = sub;
          queue.push({ fields: f.children, target: sub });
        } else if (f.value && typeof f.value === "object" && !Array.isArray(f.value)) {
          target[f.name] = f.value;
        } else {
          target[f.name] = {};
        }
        continue;
      }

      if (f.value == null && !f.children?.length) continue;
      target[f.name] = coerceValue(f.value, f.type);
    }
  }
  return root;
}

/**
 * Encode form fields as a JSPB array (indexed by field number).
 */
function encodeFormToJspb(rootFields) {
  // Iterative: each work item builds one JSPB array from a fields list.
  // Nested messages enqueue a fresh array that subsequent iterations
  // populate. Replaces self-recursion so deeply-nested message trees
  // (or pathological repeated-message arrays) encode without growing
  // the JS call stack.
  function buildOne(fields) {
    let mx = 0;
    for (const f of fields) {
      if (f.number > mx) mx = f.number;
    }
    return mx === 0 ? [] : new Array(mx).fill(null);
  }
  const root = buildOne(rootFields);
  const queue = [{ fields: rootFields, target: root }];
  while (queue.length > 0) {
    const { fields, target } = queue.shift();
    for (const f of fields) {
      if (!f.number) continue;
      const targetIdx = f.number - 1;
      if (f.type === "message" && f.label !== "repeated") {
        const sub = buildOne(f.children || []);
        target[targetIdx] = sub;
        queue.push({ fields: f.children || [], target: sub });
      } else if (f.label === "repeated" && f.type === "message" && Array.isArray(f.value)) {
        const repeated = [];
        target[targetIdx] = repeated;
        for (const item of f.value) {
          if (item && item.children) {
            const sub = buildOne(item.children);
            repeated.push(sub);
            queue.push({ fields: item.children, target: sub });
          } else if (Array.isArray(item)) {
            repeated.push(item);
          } else {
            repeated.push(item);
          }
        }
      } else if (f.label === "repeated" && Array.isArray(f.value)) {
        target[targetIdx] = f.value.map((v) => coerceValue(v, f.type));
      } else {
        target[targetIdx] = coerceValue(f.value, f.type);
      }
    }
  }
  return root;
}

/**
 * Encode form fields as binary protobuf.
 *
 * Iterative driver — replaces the prior encodeFormToProtobuf ↔
 * encodeSinglePbField mutual recursion that descended through nested
 * message types. Each stack frame encodes one fields array. When a
 * field is a nested message with children, the driver pushes a sub-
 * frame for the children and stashes the parent's pending field
 * number; the sub-frame's encoded bytes are wrapped via
 * pbEncodeLenField and appended to the parent's parts when the
 * sub-frame pops. encodeSinglePbField stays a pure scalar leaf
 * encoder — its message branch is gone.
 */
function encodeFormToProtobuf(fields) {
  const PACKABLE = new Set([
    "int32", "int64", "uint32", "uint64", "sint32", "sint64",
    "bool", "enum", "fixed32", "fixed64", "sfixed32", "sfixed64",
    "float", "double",
  ]);
  const stack = [{ fields: fields, parts: [], i: 0, pendingNum: null }];
  let lastBytes = null;
  while (stack.length > 0) {
    const top = stack[stack.length - 1];
    if (lastBytes !== null) {
      // A child frame just finished; wrap its bytes as a length-
      // delimited field on this frame's pending field number.
      top.parts.push(pbEncodeLenField(top.pendingNum, lastBytes));
      top.pendingNum = null;
      lastBytes = null;
    }
    let pushedSubFrame = false;
    while (top.i < top.fields.length) {
      const f = top.fields[top.i];
      if (!f.number) { top.i++; continue; }
      if (f.value == null && !(f.children && f.children.length)) { top.i++; continue; }
      if (f.label === "repeated" && Array.isArray(f.value)) {
        if (PACKABLE.has(f.type)) {
          const innerParts = [];
          for (const v of f.value) {
            innerParts.push(encodeSinglePbFieldRaw(f.type, v));
          }
          const packed = concatBytes.apply(null, innerParts.length ? innerParts : [new Uint8Array(0)]);
          top.parts.push(pbEncodeLenField(f.number, packed));
        } else {
          // Non-packable types (string, bytes, message): individual
          // tag+value pairs. The original passed children=null here,
          // so message-typed repeated fields fell through to the
          // string-coerce default; preserving that behavior.
          for (const v of f.value) {
            top.parts.push(encodeSinglePbField(f.number, f.type, v));
          }
        }
        top.i++;
        continue;
      }
      if (f.type === "message" && f.children && f.children.length) {
        // Push sub-frame for nested message; parent waits at its
        // pending field number until the child returns its bytes.
        top.pendingNum = f.number;
        top.i++;
        stack.push({ fields: f.children, parts: [], i: 0, pendingNum: null });
        pushedSubFrame = true;
        break;
      }
      top.parts.push(encodeSinglePbField(f.number, f.type, f.value));
      top.i++;
    }
    if (pushedSubFrame) continue;
    const bytes = concatBytes.apply(null, top.parts.length ? top.parts : [new Uint8Array(0)]);
    stack.pop();
    lastBytes = bytes;
  }
  return lastBytes;
}

// Encode a single scalar protobuf field (tag + value). Pure leaf —
// message-typed fields are now handled by encodeFormToProtobuf's
// driver, so the message branch is no longer here. The 4-arg signature
// is kept so existing call sites compile, but the children param is
// unused.
function encodeSinglePbField(num, type, value /*, children */) {
  switch (type) {
    case "string":
      return pbEncodeLenField(num, String(value));
    case "bytes":
      return pbEncodeLenField(num, base64ToUint8(String(value)));
    case "bool":
      return pbEncodeVarintField(num, value ? 1 : 0);
    case "enum":
    case "int32":
    case "int64":
    case "uint32":
    case "uint64":
      return pbEncodeVarintField(num, Number(value) || 0);
    case "sint32":
    case "sint64": {
      // Arithmetic ZigZag to avoid 32-bit truncation from bitwise ops
      const n = Number(value) || 0;
      const zigzag = n >= 0 ? n * 2 : (-n) * 2 - 1;
      return pbEncodeVarintField(num, zigzag);
    }
    case "float":
    case "fixed32":
    case "sfixed32": {
      const buf = new Uint8Array(4);
      if (type === "float")
        new DataView(buf.buffer).setFloat32(0, Number(value) || 0, true);
      else new DataView(buf.buffer).setUint32(0, Number(value) || 0, true);
      return concatBytes(pbTag(num, PB_32BIT), buf);
    }
    case "double": {
      const buf = new Uint8Array(8);
      new DataView(buf.buffer).setFloat64(0, Number(value) || 0, true);
      return concatBytes(pbTag(num, PB_64BIT), buf);
    }
    case "fixed64":
    case "sfixed64": {
      // 64-bit integer encoding (not float64)
      const buf = new Uint8Array(8);
      const n = Number(value) || 0;
      const dv = new DataView(buf.buffer);
      dv.setUint32(0, n >>> 0, true);
      dv.setUint32(4, Math.floor(n / 0x100000000) >>> 0, true);
      return concatBytes(pbTag(num, PB_64BIT), buf);
    }
    default:
      return pbEncodeLenField(num, String(value));
  }
}

/**
 * Encode a single protobuf scalar value WITHOUT the field tag.
 * Used for packed repeated encoding where values are concatenated inside
 * a single length-delimited field.
 */
function encodeSinglePbFieldRaw(type, value) {
  switch (type) {
    case "bool":
      return pbWriteVarint(value ? 1 : 0);
    case "enum":
    case "int32":
    case "int64":
    case "uint32":
    case "uint64":
      return pbWriteVarint(Number(value) || 0);
    case "sint32":
    case "sint64": {
      const n = Number(value) || 0;
      return pbWriteVarint(n >= 0 ? n * 2 : (-n) * 2 - 1);
    }
    case "float":
    case "fixed32":
    case "sfixed32": {
      const buf = new Uint8Array(4);
      if (type === "float")
        new DataView(buf.buffer).setFloat32(0, Number(value) || 0, true);
      else new DataView(buf.buffer).setUint32(0, Number(value) || 0, true);
      return buf;
    }
    case "double": {
      const buf = new Uint8Array(8);
      new DataView(buf.buffer).setFloat64(0, Number(value) || 0, true);
      return buf;
    }
    case "fixed64":
    case "sfixed64": {
      const buf = new Uint8Array(8);
      const n = Number(value) || 0;
      const dv = new DataView(buf.buffer);
      dv.setUint32(0, n >>> 0, true);
      dv.setUint32(4, Math.floor(n / 0x100000000) >>> 0, true);
      return buf;
    }
    default:
      return pbWriteVarint(Number(value) || 0);
  }
}
