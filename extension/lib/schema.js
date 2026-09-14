// lib/schema.js — Schema inference (VDD). Builds JSON-Schema-ish shapes from observed JSON and protobuf
// (JSPB) trees and merges/drifts them into a document's schema map. Extracted from the offscreen-brain.js
// monolith (one problem per file). Loaded BEFORE offscreen-brain.js in ast-worker.html; these functions stay
// global and resolve their callers at call-time, so load order is safe. KEEPS the protobuf/discovery schema-
// learning feature -- just relocated out of the 8000-line brain.

/* A MAP KEYED BY A NAME A STRANGER CHOSE HAS NO PROTOTYPE, AND THIS IS THE ONE PLACE THAT SAYS SO.
   lib/req2proto.js states the identical rule twice and answers it with a `Map` — "A MAP AND NOT AN OBJECT
   LITERAL, BECAUSE THE KEY IS A STRANGER'S TEXT" — because its tables are LOOKUPS. A learned schema's
   `properties` cannot be a Map: it is JSON that rides IndexedDB and structured clone. So the same rule needs
   the other primitive, and `Object.create(null)` is it — a plain object on both transports, with no
   `Object.prototype` underneath to answer for names no producer ever wrote.
   WHAT A LITERAL COSTS IS A LEARNED FIELD SILENTLY LOST, NEVER A CRASH — and the reach was measured against
   THIS FILE'S OWN CODE rather than modelled, because a model of the mechanism proves the mechanism and says
   nothing about which paths reach it. Three reaches, each run against the previous revision and against this
   one, each carrying `userId` and `results` as controls that behaved identically on both sides:
     • a schema this file BUILT, merged with a second observation of the same body — 1 of 10 lost, and it is
       `__proto__`. The other seven survive here only because the BUILD ran first and shadowed them with own
       properties, which is why this reach looks almost clean and is not.
     • a schema out of a DISCOVERY DOCUMENT (a third party's `schemas` object, which is what `doc.schemas`
       holds for a published API) merged against a newly observed body — 8 OF 10 LOST. Nothing shadowed
       anything, so `existing.properties["toString"]` answered a function, mergeSchemaInto's
       `existing.properties[key] ? key : …` read that as "already known", took the match arm, and the `!old`
       arm that ADDS a field was never reached: no write, no drift entry, nothing logged.
     • the protobuf tree's repeated-field count — 3 of 4 wrongly typed, a repeated field recorded as a single
       `string` instead of an `array`, which is a wrong TYPE on the surface this tool exists to produce.
   `__proto__` is worse again on the BUILD side, where `properties["__proto__"] = x` invokes
   Object.prototype's SETTER: the field is not stored at all AND the map's own prototype becomes the value,
   after which `for … in` over `properties` enumerates `type` as though the server had sent a field so named.
   AND JSON.parse DISAGREES WITH AN OBJECT LITERAL ABOUT EXACTLY ONE OF THE EIGHT, which is why the store's
   round trip cannot stand in for this check: `JSON.parse('{"__proto__":…}')` yields an OWN `__proto__`, so a
   schema restored from IndexedDB can hold a field a freshly-minted one cannot. One server, two paths, two
   answers about what it sent.
   THE READ SIDE NEEDS ITS OWN SPELLING, because this file does not mint every map it reads: `doc.schemas` is
   a Google discovery document's own object, or a store record JSON.parse rebuilt, and neither came from here.
   NEITHER SIDE MAY ASSERT. A property map's values are a stranger's — a discovery document states them — and
   CLAUDE.md is explicit that asserting on bytes a stranger stated hands that stranger an abort switch for the
   trusted zone. So the answer to a name nothing wrote is the REFUSAL these two give (it is not held, so it is
   absent, so the field is ADDED as the observation it is) and never a crash and never a default. */
function _strangerKeyedMap() { return Object.create(null); }
function _strangerKeyHeld(map, name) { return Object.prototype.hasOwnProperty.call(map, name); }
/* AND THE WRITE NEEDS A SPELLING OF ITS OWN, WHICH THE READ FIX ALONE DOES NOT GIVE — measured against the
   real file rather than assumed. Asking for an OWN property fixes seven of the eight names outright: the
   match arm stops firing, the `!old` arm runs, and the field is added. `__proto__` survives that repair,
   because `literal["__proto__"] = v` is not a store at all — it is Object.prototype's SETTER, so the plain
   write puts the field NOWHERE on a map this file did not mint, which is every map that came out of a
   discovery document or a JSON.parse of a store record. `defineProperty` states an own data property whatever
   the target's prototype is, and it round-trips: JSON.stringify and structuredClone both carry it, and the
   map's own prototype is left alone. A map minted above needs none of this — with no accessor to reach, a
   plain assignment already stores all eight — so this is for the maps that arrive from elsewhere. */
function _strangerKeySet(map, name, value) {
  Object.defineProperty(map, name, { value: value, writable: true, enumerable: true, configurable: true });
}

function generateSchemaFromPbTree(rootTree, rootName, schemas) {
  // Iterative worklist replaces self-recursion. Each entry is either a
  // "build" (create a fresh schema for tree, attach via slot) or "merge"
  // (build a fresh schema for tree, then fold its properties into an
  // existing schemas[key]). Handles arbitrarily nested protobuf trees
  // without growing the JS call stack.
  let result = null;
  const queue = [{ kind: "build", tree: rootTree, name: rootName, slot: { kind: "result" } }];
  function setSlot(slot, value) {
    if (slot.kind === "result") result = value;
    else if (slot.kind === "schemas") schemas[slot.key] = value;
    /* A GUARD AND NOT A GAP — the kinds are minted in this same function (the `result` seed above and the
       `schemas` job buildShell queues), so no input decides which arm is taken and this one is unreachable by
       construction. Without it an unknown kind attached the built schema to NOTHING and returned in silence,
       which is a field that vanishes out of the result with nothing anywhere to say a branch went missing. */
    else DFAIL("a pb-tree schema slot names kind `" + slot.kind + "`, which this worklist does not mint — it " +
               "mints `result` and `schemas` and nothing else, so a third is a slot built by something else " +
               "and the schema it carries would attach to nothing and disappear out of the returned tree");
  }
  function buildShell(tree, name, queueOut) {
    // First pass: count field occurrences to detect repeated fields
    /* THE COUNT MAP IS STRANGER-KEYED AND THE PROPERTY MAP BELOW IS NOT, which is why only one of them
       changes. `node.field` is a protobuf field NUMBER on the wire path and a raw JSON KEY on
       lib/protobuf.js's object-mode path, which builds `{ field: k, … }` straight out of `Object.entries` of
       a parsed body — so `fieldCounts["constructor"]` answered a FUNCTION, `(fn || 0) + 1` made the count a
       STRING, and `> 1` is then false for every occurrence: a repeated field by one of those eight names is
       not detected as repeated. `properties` is keyed by `field${node.field}`, which begins with `field` and
       therefore cannot collide with any name Object.prototype carries, so it stays a literal — a site that
       needs nothing, named here rather than swept with the one that did. */
    const fieldCounts = _strangerKeyedMap();
    for (const node of tree) {
      fieldCounts[node.field] = (fieldCounts[node.field] || 0) + 1;
    }
    const properties = {};
    const seen = new Set();
    for (const node of tree) {
      const fieldKey = `field${node.field}`;
      if (seen.has(node.field)) {
        // Repeated message field — additional occurrence merges into the
        // existing nested schema. Queue a "merge" job; don't recurse.
        if (node.message) {
          const nestedName = `${name}Field${node.field}`;
          if (schemas[nestedName]) {
            queueOut.push({ kind: "merge", tree: node.message,
              name: nestedName, mergeKey: nestedName });
          }
        }
        continue;
      }
      seen.add(node.field);

      const isRepeated = fieldCounts[node.field] > 1 || !!node.isRepeatedScalar || !!node.packed;
      let wireType;
      if (node.isJspb) {
        const val = node.value;
        if (typeof val === "boolean") wireType = "bool";
        else if (typeof val === "number") wireType = Number.isInteger(val) ? "int64" : "double";
        else if (typeof val === "string") wireType = "string";
        else if (node.isRepeatedScalar && Array.isArray(val) && val.length > 0) {
          const sample = val.find((v) => v != null);
          if (typeof sample === "boolean") wireType = "bool";
          else if (typeof sample === "number") wireType = Number.isInteger(sample) ? "int64" : "double";
          else wireType = "string";
        } else wireType = "string";
      } else if (node.packed) {
        wireType = "int64";
      } else {
        if (node.wire === 0) wireType = "int64";
        else if (node.wire === 5) wireType = "float";
        else if (node.wire === 1) wireType = "double";
        else if (node.string !== undefined) wireType = "string";
        else if (node.hex) wireType = "bytes";
        else wireType = "string";
      }
      const prop = {
        id: node.field,
        number: node.field,
        type: wireType,
        description: "Discovered via response capture",
      };
      if (isRepeated) {
        prop.type = "array";
        prop.items = { type: wireType };
      }
      if (node.message) {
        const nestedName = `${name}Field${node.field}`;
        if (isRepeated) {
          prop.items = { $ref: nestedName };
        } else {
          prop.type = "message";
          prop.$ref = nestedName;
        }
        // Queue nested build, attaching to schemas[nestedName].
        queueOut.push({ kind: "build", tree: node.message, name: nestedName,
          slot: { kind: "schemas", key: nestedName } });
      } else if (node.string !== undefined) {
        if (!isRepeated) prop.type = "string";
      }
      properties[fieldKey] = prop;
    }
    return { id: name, type: "object", properties };
  }
  while (queue.length > 0) {
    const job = queue.shift();
    const built = buildShell(job.tree, job.name, queue);
    if (job.kind === "build") {
      setSlot(job.slot, built);
    } else if (job.kind === "merge") {
      const existing = schemas[job.mergeKey];
      if (existing) {
        if (!existing.properties) existing.properties = _strangerKeyedMap();
        /* `|| {}` STOOD OVER A FIELD ITS ONLY PRODUCER ALWAYS WRITES. `built` is `buildShell`'s answer and
           that function has exactly ONE return, `{ id, type, properties }`, with `properties` bound at its
           top — so the default could not fire, and what it was covering is the day buildShell stops stating
           the name: this loop would then merge an empty set of fields and report a schema that gained
           nothing, which reads exactly like a second observation that carried no new field.
           The keys here are `field${n}` and nothing else, so the read below cannot reach a name
           Object.prototype carries and is left as a plain index — the mint is prototype-free for uniformity
           with every other property map in this file, not because this loop needs it. */
        DCHECK(!!built.properties && typeof built.properties === "object" && !Array.isArray(built.properties),
               "buildShell answered a schema with no properties map for `" + job.name + "` — it states one on " +
               "its single return, so an absent one is that function changed shape and this merge would fold " +
               "in nothing while reporting that it ran");
        for (const [k, v] of Object.entries(built.properties)) {
          if (!existing.properties[k]) existing.properties[k] = v;
        }
      }
    }
  }
  return result;
}

function generateSchemaFromJson(rootJson, rootName, schemas, rootIsIndexed = false) {
  // Iterative worklist replaces self-recursion. Each entry pairs an input
  // (json, name, isIndexed) with a destination "slot" — where the
  // generated schema gets attached. The slot can be:
  //   - { kind: "result" }                → set the function's return value
  //   - { kind: "schemas", key: NAME }    → schemas[NAME] = schemaObj
  //   - { kind: "items", parent: SCHEMA } → SCHEMA.items = schemaObj
  // This keeps the JS stack at depth 1 even for deeply-nested JSON.
  // THE FIELD IS `kind` AND THIS LIST SPELLED IT `type` FOR ALL THREE. Nothing reads a comment, so it stayed
  // wrong: a reader building a fourth slot off this list would have written `{ type: … }` and setSlot would
  // have matched none of its arms — which, before the arm below existed, was a schema attached to nothing.
  let result = null;
  const queue = [{ json: rootJson, name: rootName, isIndexed: rootIsIndexed, slot: { kind: "result" } }];
  function setSlot(slot, value) {
    /* A FOURTH ARM STOOD HERE AND COULD NOT RUN — `else if (slot.kind === "prop") slot.parent[slot.key] = value`
       over a `{kind:"prop"}` this worklist mints NOWHERE: every `slot:` below is `result`, `schemas` or
       `items`, and the string "prop" occurred exactly once in the whole extension, at that arm. It is DELETED
       rather than left for somebody to wire up, because WHAT IT WOULD HAVE DONE is the reason not to: a prop
       slot's `key` is a field name, which on this path is a name a stranger chose, and `slot.parent[slot.key]
       = value` is that name used as a computed key on an object literal — the exact write `_strangerKeyedMap`
       above exists to stop, on a `parent` this function does not own. Attaching a nested schema is already
       what the `schemas` arm does, by $ref, and a dead path is untested code rather than working code.
       AND THE ELSE IS A GUARD AND NOT A GAP: the three kinds are minted in this same function, so no input
       decides which arm runs. Without it an unknown kind attached the schema to NOTHING and returned in
       silence — a branch of the learned shape gone, with no error and no empty value to notice. */
    if (slot.kind === "result") result = value;
    else if (slot.kind === "schemas") schemas[slot.key] = value;
    else if (slot.kind === "items") slot.parent.items = value;
    else DFAIL("a JSON schema slot names kind `" + slot.kind + "`, which this worklist does not mint — it " +
               "mints `result`, `schemas` and `items`, all queued in this same function, so a fourth is a " +
               "slot built by something else and the schema it carries would attach to nothing and " +
               "disappear out of the tree this call returns");
  }
  while (queue.length > 0) {
    const { json, name, isIndexed, slot } = queue.shift();

    if (Array.isArray(json)) {
      if (isIndexed) {
        const properties = {};
        const obj = { id: name, type: "object", properties };
        setSlot(slot, obj);
        for (let idx = 0; idx < json.length; idx++) {
          const val = json[idx];
          const fieldNum = idx + 1;
          const fieldKey = `field${fieldNum}`;
          const nestedName = `${name}_f${fieldNum}`;
          if (val === null || val === undefined) {
            properties[fieldKey] = {
              id: fieldNum,
              number: fieldNum,
              type: "string",
              description: "Learned (null)",
            };
          } else if (Array.isArray(val)) {
            const allPrim =
              val.length > 0 &&
              val.every(
                (v) => v === null || v === undefined ||
                  typeof v === "string" || typeof v === "number" || typeof v === "boolean",
              );
            if (allPrim) {
              const itemType = inferRepeatedItemType(val);
              properties[fieldKey] = {
                id: fieldNum, number: fieldNum, type: itemType, label: "repeated",
              };
            } else {
              properties[fieldKey] = { id: fieldNum, number: fieldNum, $ref: nestedName };
              queue.push({ json: val, name: nestedName, isIndexed: true,
                slot: { kind: "schemas", key: nestedName } });
            }
          } else if (typeof val === "object") {
            properties[fieldKey] = { id: fieldNum, number: fieldNum, $ref: nestedName };
            queue.push({ json: val, name: nestedName, isIndexed: false,
              slot: { kind: "schemas", key: nestedName } });
          } else {
            properties[fieldKey] = {
              id: fieldNum, number: fieldNum, type: inferJsonType(val),
            };
          }
        }
        continue;
      }
      // Non-indexed array → { type: "array", items: <schemaForFirstElement> }
      const arr = { type: "array", items: { type: "string" } };
      setSlot(slot, arr);
      if (json.length > 0) {
        queue.push({ json: json[0], name: name + "Item", isIndexed: false,
          slot: { kind: "items", parent: arr } });
      }
      continue;
    }

    if (typeof json === "object" && json !== null) {
      // KEYED BY `for (const key in json)` — the server's own field names, raw. `nestedName` beside it is
      // `safeKey`-stripped to alphanumerics and so is this file's own composition; the KEY is not.
      const properties = _strangerKeyedMap();
      const obj = { id: name, type: "object", properties };
      setSlot(slot, obj);
      for (const key in json) {
        const val = json[key];
        const safeKey = key.replace(/[^a-zA-Z0-9]/g, "");
        if (Array.isArray(val)) {
          const arr = { type: "array", items: { type: "string" } };
          properties[key] = arr;
          if (val.length > 0) {
            queue.push({ json: val[0], name: name + safeKey + "Item", isIndexed: false,
              slot: { kind: "items", parent: arr } });
          }
        } else if (typeof val === "object" && val !== null) {
          const nestedName = name + safeKey.charAt(0).toUpperCase() + safeKey.slice(1);
          properties[key] = { $ref: nestedName };
          queue.push({ json: val, name: nestedName, isIndexed: false,
            slot: { kind: "schemas", key: nestedName } });
        } else {
          properties[key] = { type: inferJsonType(val) };
        }
      }
      continue;
    }

    // Primitive
    setSlot(slot, { type: inferJsonType(json) });
  }
  return result;
}

/**
 * Infer a protobuf-style type from a JS value.
 * More precise than raw `typeof` — distinguishes int vs float, bool, etc.
 */
function inferJsonType(val) {
  if (val === null || val === undefined) return "string";
  if (typeof val === "boolean") return "bool";
  if (typeof val === "number") {
    return Number.isInteger(val) ? "int64" : "double";
  }
  if (typeof val === "string") return "string";
  return "string";
}

/** Infer the best scalar type for a repeated field from sample values. */
function inferRepeatedItemType(arr) {
  for (const v of arr) {
    if (v === null || v === undefined) continue;
    return inferJsonType(v);
  }
  return "string";
}

/**
 * Merge new schema properties into an existing schema, preserving custom renames
 * and enriching with new fields. Existing fields keep customName/name if set;
 * new fields or missing type info gets filled in from the new observation.
 */
function mergeSchemaInto(doc, rootSchemaName, rootNewSchema) {
  // Iterative: merge doc.schemas[schemaName] ← newSchema, queueing
  // nested ($ref) merges instead of recursing. visited-set on the
  // merge target prevents cycles when a schema references itself or
  // forms a $ref loop. Replaces the previous self-recursive form whose
  // depth was bounded by JS-stack — adversarially-deep nested $refs
  // in a learned schema would crash the merge before this conversion.
  const visited = new Set();
  const queue = [{ schemaName: rootSchemaName, newSchema: rootNewSchema }];
  while (queue.length > 0) {
    const { schemaName, newSchema } = queue.shift();
    if (visited.has(schemaName)) continue;
    visited.add(schemaName);
    /* `doc.schemas` IS NOT A MAP THIS FILE MINTS — it is a Google discovery document's own `schemas` object,
       or a store record JSON.parse rebuilt — so the prototype-free mint above cannot reach it and the READ is
       where the rule is applied. Held-AND-truthy keeps the old falsy test EXACTLY for a name a producer wrote
       (a stored `null` under a real key still takes the assign arm); what it stops is a `$ref` or a schema
       name that is one of Object.prototype's eight, where `doc.schemas["constructor"]` answered the Object
       CONSTRUCTOR and this fell through to merge INTO it — `existing.properties = {}` and `existing._drift =
       []` then being written onto the global `Object` itself. */
    if (!_strangerKeyHeld(doc.schemas, schemaName) || !doc.schemas[schemaName]) {
      _strangerKeySet(doc.schemas, schemaName, newSchema);
      continue;
    }
    const existing = doc.schemas[schemaName];
    if (!existing.properties) existing.properties = _strangerKeyedMap();
    if (!existing._drift) existing._drift = [];
    const newProps = newSchema.properties || {};

    /* A FIELD-NUMBER MAP IS KEYED BY A FIELD NUMBER OR IT IS NOT KEYED AT ALL, and `p.number ?? p.id` was
       putting NAMES in it. The two spellings are real and both have writers — this file's own indexed mint
       states `id` and `number` as the SAME integer, while lib/discovery.js writes a Google Discovery
       property's `id`, which is a SCHEMA IDENTIFIER, and lib/popup-handlers.js's rename writes
       `id: <property key>` beside `number: parseInt(<key>) || null`. So the `??` was not a default over an
       absence at all: it was an alternation over two spellings, and it accepted whatever the second spelling
       held. A `null` number therefore fell through to a NAME, `numToKey["userName"] = "userName"`, and the
       branch below — which renames a stored property onto a new key when the two agree on a field number —
       fired on two properties agreeing on a schema NAME instead. That is a persisted schema silently
       renamed, from a match this map was never supposed to be able to make.
       THE LAW IS ASKED, NOT RESTATED. lib/encode.js's `pbWireTag` is the one statement of what a wire tag is
       (Protobuf Language Guide (proto 3), "Assigning Field Numbers": "You must give each field in your
       message definition a number between 1 and 536,870,911"), and it REFUSES everything else — so the
       alternation survives, each of its two limbs states a number or nothing, and a property that names no
       tag simply has no entry here. Matching then falls back to key equality, which is the true statement
       about a property whose number nobody stated. */
    const numToKey = {};
    for (const [k, p] of Object.entries(existing.properties)) {
      const n = pbWireTag(p.number) ?? pbWireTag(p.id);
      if (n !== null) numToKey[n] = k;
    }

    for (const [key, newProp] of Object.entries(newProps)) {
      const fieldNum = pbWireTag(newProp.number) ?? pbWireTag(newProp.id);
      /* THE LINE THE EIGHT NAMES WERE LOST ON, and the only repair it needs is asking for an OWN property.
         `key` is a field name a server chose and `existing.properties` may be a map this file did not mint,
         so `existing.properties["toString"]` answered a function from the prototype, `matchKey` became that
         name, `old` became the function, and the `!old` arm below — the one that ADDS a newly-observed field
         — was never taken. Nothing was written and nothing was recorded: the field simply was not in the
         learned surface, this session or any later one.
         `numToKey` beside it is NOT asked the same question and does not need to be: `pbWireTag` refuses
         anything that is not an integer in the Protobuf Language Guide's field-number range, so every key in
         it is a number and no number spells a name Object.prototype carries. */
      const matchKey = (_strangerKeyHeld(existing.properties, key) && existing.properties[key]) ? key
        : (fieldNum !== null && numToKey[fieldNum]) ? numToKey[fieldNum]
        : null;
      const old = matchKey ? existing.properties[matchKey] : null;

      if (!old) {
        _strangerKeySet(existing.properties, key, newProp);
        if (fieldNum !== null) numToKey[fieldNum] = key;
        existing._drift.push({ type: "field_added", field: key, fieldType: newProp.type, timestamp: Date.now() });
      } else {
        if (matchKey !== key && !old.customName && !/^field\d+$/.test(key)) {
          _strangerKeySet(existing.properties, key, old);
          delete existing.properties[matchKey];
          /* THE RENAME IS ONLY REACHED THROUGH A REAL TAG. `matchKey !== key` requires the match to have come
             from `numToKey`, which now holds only refused wire tags, so `fieldNum` here is an integer by
             construction — the old code wrote `numToKey[undefined]` on the one path where it was not. */
          numToKey[fieldNum] = key;
        }
        if (old.customName) {
          // Keep the user's rename
        } else if (newProp.name && !old.name) {
          old.name = newProp.name;
        }
        if (newProp.type && newProp.type !== old.type) {
          if (old.type === "string" && newProp.type !== "string") {
            existing._drift.push({ type: "type_changed", field: key || matchKey, from: old.type, to: newProp.type, timestamp: Date.now() });
            old.type = newProp.type;
          } else if (
            (old.type === "int64" || old.type === "int32") &&
            (newProp.type === "double" || newProp.type === "float")
          ) {
            existing._drift.push({ type: "type_changed", field: key || matchKey, from: old.type, to: newProp.type, timestamp: Date.now() });
            old.type = newProp.type;
          }
        }
        if (old.type === "array" && newProp.items) {
          if (!old.items) {
            old.items = newProp.items;
          } else {
            if (old.items.type === "string" && newProp.items.type && newProp.items.type !== "string") {
              old.items.type = newProp.items.type;
            }
            if (newProp.items.$ref && !old.items.$ref) {
              old.items.$ref = newProp.items.$ref;
            }
          }
        }
        if (newProp.id != null && old.id == null) old.id = newProp.id;
        if (newProp.number != null && old.number == null)
          old.number = newProp.number;
        if (newProp.$ref && !old.$ref) {
          old.$ref = newProp.$ref;
          old.type = "message";
        }
        if (newProp.children && !old.children) old.children = newProp.children;
        if (newProp.description && !old.description) old.description = newProp.description;
        // Queue nested $ref merge instead of recursing. OWN-PROPERTY, for the reason the top of this loop
        // gives: a `$ref` is a name out of a discovery document, so an inherited one made this push a job
        // whose `newSchema` was the Object constructor and whose merge then wrote onto it.
        if (newProp.$ref && _strangerKeyHeld(doc.schemas, newProp.$ref) && doc.schemas[newProp.$ref]) {
          queue.push({ schemaName: newProp.$ref, newSchema: doc.schemas[newProp.$ref] });
        }
      }
    }
    if (existing._drift.length > 50) existing._drift = existing._drift.slice(-50);
  }
}

// ─── Page-Context Fetch Bridge ───────────────────────────────────────────────
// Routes fetch requests through the content script so they execute with the
// page's cookie jar and Origin. The content script shares the page's cookies,
// so the browser attaches them automatically. Targets a specific frameId when
// the request originated from an iframe (e.g. proxy.html).
//
// If the original tab/frame is unreachable, a minimized background window is
// opened to the initiator origin so the content script loads and carries the
// right cookies + Origin.

/* WHAT THE RELAY ANSWERS, ASSERTED WHERE THE RECORD CROSSES BACK INTO THIS ZONE — ONCE, rather than at each
   of the consumers that read it. `content.js handlePageFetch` has exactly TWO answers and this is the line
   that says so:
     • a NETWORK-ERROR arm, `{ error }` — its `fetch()` rejected, which Fetch §5.6 Fetch methods defines as
       the TypeError a network error becomes. That is an OUTCOME the caller is entitled to, not a broken
       contract, and it carries no other field.
     • a SUCCESS record, which that function returns from two `return`s (the binary arm and the text arm) and
       writes `ok`/`status`/`statusText`/`headers`/`body` on BOTH. `bodyEncoding` is written on the BINARY arm
       ALONE, so its ABSENCE IS THE POSITIVE STATEMENT "these bytes are text" — the one field here a consumer
       may legitimately not find, and the reason this asserts `undefined`-or-`"base64"` rather than presence.
   IT IS ASSERTED HERE BECAUSE THIS IS THE ORIGIN. Every consumer of this record — lib/send.js's manual
   replay, lib/req2proto.js's error probe — reads the SAME two answers, so a contract checked at each of them
   is the hand-copied list CLAUDE.md warns about: one copy goes short and the field it stopped naming becomes
   a default in that consumer alone. Downstream of this line every field named here is a field that EXISTS,
   which is what lets `resp.body || ""` and `resp.headers?.[k]` be deleted rather than kept "just in case" —
   and each of those was a place where a relay that stopped writing a field would have rendered an empty
   response body, or a header list with nothing in it, as the server's own answer.
   THE ERROR ARM IS CHECKED TOO, and it is not symmetry for its own sake: `{ error: undefined }` reads as a
   SUCCESS record here and then as a body-less response downstream, so an error whose message went missing is
   the one shape that would pass through this whole edge saying nothing. */
function _checkPageFetchReply(reply, url) {
  DCHECK(!!reply && typeof reply === "object",
         "the page-context relay answered a PAGE_FETCH with no record at all — content.js returns one from " +
         "every arm of handlePageFetch and swRpc passes it back verbatim, so an absent one is that relay or " +
         "the service worker's __rpc envelope broken, for " + url);
  if ("error" in reply) {
    DCHECK(typeof reply.error === "string" && reply.error !== "",
           "the page-context relay answered a PAGE_FETCH with an `error` that names nothing — the reason IS " +
           "the whole of what this arm carries, and an empty one is reported to the reviewer as a request " +
           "that failed for no stated cause, for " + url);
    return;
  }
  DCHECK(typeof reply.ok === "boolean" && typeof reply.status === "number" &&
         typeof reply.statusText === "string" && !!reply.headers && typeof reply.headers === "object" &&
         typeof reply.body === "string",
         "the page-context relay answered a PAGE_FETCH with an incomplete response record — handlePageFetch " +
         "writes ok/status/statusText/headers/body on BOTH of its success returns, and a consumer that finds " +
         "one missing renders the gap as the server's own answer, for " + url);
  DCHECK(reply.bodyEncoding === undefined || reply.bodyEncoding === "base64",
         "the page-context relay named a body encoding this edge does not speak — content.js writes " +
         "`bodyEncoding: \"base64\"` on its binary arm and NOTHING on its text arm, so absence means text " +
         "and any third spelling is a body every consumer would decode as the wrong one, for " + url);
}

/**
 * Send a PAGE_FETCH message to a tab's content script.
 */
async function _sendPageFetch(tabId, url, opts, documentId) {
  // documentId-ONLY routing. A credentialed page-context read must hit the EXACT
  // document (its own origin/credentials). NO frameId fallback — a frameId is
  // reused across navigations and could resolve to a DIFFERENT origin; and with
  // no target option tabs.sendMessage would broadcast to every frame in the tab.
  // No documentId → refuse rather than risk a wrong-origin / broadcast read.
  if (!documentId) return { error: "blocked: no documentId for page-context fetch" };
  /* THE METHOD IS THE CALLER'S, NEVER THIS LINE'S. `pageContextFetch` DCHECKs it is a non-empty string before
     this literal is built and content.js DCHECKs it again where the message lands, so a `|| "GET"` here was a
     THIRD copy of a default whose only reachable effect is to run a different request than the caller named —
     the very substitution both of those asserts exist to prevent. `headers` is the same: all three entries
     write one (possibly empty), so a hole here would be a header list a caller composed and this line quietly
     replaced with none. */
  DCHECK(!!opts.headers && typeof opts.headers === "object",
         "a page-context fetch reached the relay with no headers object — every entry writes one (empty when " +
         "there is nothing to send), and an absent one is a header list its caller composed and this " +
         "message would carry none of");
  /* NULL IS THE STATEMENT, AND NOW EVERY CALLER MAKES IT — which is what turned these two from defaults into
     assertions. content.js branches on `msg.body != null` and on `msg.bodyEncoding === "base64"`, so `null`
     means "no request body" and "the body is text"; those are POSITIVE answers a bodyless GET is entitled to
     give, and they are exactly what `pageContextGet` now writes rather than leaving out. The `?? null` that
     stood here spoke them on the caller's behalf, and that is the whole defect: a producer that STOPPED
     writing `body` — a rename, a dropped branch in `lib/send.js`'s encoder — was indistinguishable from a GET,
     so a POST the popup composed would have been relayed as a request carrying nothing and its answer
     rendered as the server's reply to the body the user typed. There is no third spelling on this edge:
     `undefined` is now a caller that named neither. */
  DCHECK(opts.body === null || typeof opts.body === "string",
         "a page-context fetch reached the relay with no body statement — lib/send.js and lib/req2proto.js " +
         "each write a string or null, and pageContextGet writes null because a GET has none, so an absent " +
         "one is a caller whose body went missing and whose request would be relayed carrying nothing");
  DCHECK(opts.bodyEncoding === null || opts.bodyEncoding === "base64",
         "a page-context fetch named a body encoding this edge does not speak — content.js decodes base64 " +
         "and treats null as text, so any third spelling (or an absent one) is a body the page would send " +
         "raw where its caller had encoded it, or decode where its caller had not");
  const reply = await swRpc(
    "tabs.sendMessage",
    tabId,
    {
      type: "PAGE_FETCH",
      url,
      method: opts.method,
      headers: opts.headers,
      body: opts.body,
      bodyEncoding: opts.bodyEncoding,
    },
    { documentId },
  );
  _checkPageFetchReply(reply, url);
  return reply;
}

/* WHO ACTED — THE ONE FACT THIS RELAY CANNOT SEE, SO IT IS TOLD, AND THE TELLING IS ASSERTED.
 *
 * The destructive-path deny list is scoped out of this transport (see `pageContextFetch` below), and the whole
 * of that argument is that a HUMAN composed the request at a surface that showed it to them. That makes the
 * grade of the ACT the load-bearing fact of this file — and it was, for a while, a fact about WHICH FUNCTIONS
 * HAPPENED TO CALL WHICH. Three entries reached this relay with nobody at a surface: the automatic discovery
 * sweep, the automatic error probe, and the service-info probe fired the instant a response body arrived. Every
 * one of them was credentialed by construction, two of them POSTed a body the app never produced, and the
 * exemption above covered all three because the relay had no way to ask.
 *
 * IT IS NOT INFERRED HERE AND IT CANNOT BE. Nothing this function can see distinguishes the two grades: the
 * URL, the verb, the headers and the body are identical in shape whether an operator typed them or a response
 * handler composed them, and a relay that guessed would be answering the question its own exemption rests on
 * with a heuristic. So the grade TRAVELS WITH THE CALL, stated by the site that knows — the popup command
 * handler, the Send panel, the response handler — and is carried, never re-derived, down every frame between
 * that site and this one.
 *
 * TWO VALUES, AND ONLY ONE OF THEM MAY BE HERE. `PAGE_CONTEXT_TOOL_INITIATED` exists because a routing site
 * genuinely holds it (`lib/discovery-probe.js`'s sweep serves both grades and picks its transport from this
 * value), not because this relay has an arm for it: an act this tool decided on goes to `safeFetch`, which is
 * the chokepoint SECURITY.md names and the one that asks the provenance, credential and deny-list questions.
 * A caller that cannot state its grade is a caller that must not proceed — an absent value takes the same arm
 * as `tool`, which is the arm that aborts, so forgetting to state one is not a way to be exempted.
 *
 * CHECK AND NOT DCHECK: this is the authorization grade the exemption is scoped by, so proceeding in RELEASE
 * with an unstated one is exactly the state that was shipping. */
const PAGE_CONTEXT_USER_INITIATED = "user-initiated";
const PAGE_CONTEXT_TOOL_INITIATED = "tool-initiated";
const _PAGE_CONTEXT_GRADES = [PAGE_CONTEXT_USER_INITIATED, PAGE_CONTEXT_TOOL_INITIATED];

/* THE SITE TRAVELS WITH THE QUESTION. This helper is called from the relay AND from the two relay-fn factories
   in offscreen-brain.js, so an abort raised at its own line would name this file for every caller in the
   extension — the shared-helper defect CLAUDE.md names. `who` is the caller's own name, stated at the call. */
function pageContextRequireUserInitiated(initiator, who) {
  CHECK(initiator === PAGE_CONTEXT_USER_INITIATED,
        "the page-context relay was reached by " + who + " with the initiator grade " +
        JSON.stringify(initiator) + " — this transport issues the request AS THE PAGE with the person's own " +
        "cookies and is exempt from the credentialed destructive-path deny list, and that exemption is scoped " +
        "to acts a HUMAN initiated at a surface that showed them the request. An act this tool decided on goes " +
        "to safeFetch (lib/safe-fetch.js), which asks the provenance, credential and deny-list questions this " +
        "edge cannot; an unstated grade is a caller that never answered the question at all");
}

/* A GRADE THAT IS NEITHER OF THE TWO IS A ROUTING SITE READING A VALUE NOBODY MINTED, and it takes whichever
   arm the site wrote as its else — which is why the routing sites assert the value before they dispatch on it
   rather than testing for `user` and calling everything else automatic. */
function pageContextStatedGrade(initiator, who) {
  CHECK(_PAGE_CONTEXT_GRADES.indexOf(initiator) >= 0,
        who + " named the initiator grade " + JSON.stringify(initiator) + ", which is neither of the two this " +
        "extension states (" + _PAGE_CONTEXT_GRADES.join(", ") + ") — the grade decides which TRANSPORT the " +
        "request leaves by, so a third spelling is a request routed by whichever arm the dispatch happened to " +
        "write last");
  return initiator;
}

/* THE PAGE-CONTEXT EDGE HAS THREE ENTRIES, AND WHICH VERB EACH ONE MAY NAME IS A PROPERTY OF ITS CALLER.
 *
 * THE SHAPE RULE IS DELETED, AND IT WAS THIS FILE'S. It said there were two entries and that the learning one
 * had NO METHOD PARAMETER AT ALL — "there is no place to express a POST" — and SECURITY.md carried the same
 * paragraph. Both are gone, because the rule was false about one of the two systems it constrained.
 * `lib/req2proto.js` learns by sending a DELIBERATELY MALFORMED body to a Google API and reading the
 * `google.rpc.Status` rejection, which describes the request the service wanted: the POST is the MECHANISM,
 * and an endpoint that answers 4xx to a malformed body has not been mutated. A rule that makes that
 * unexpressible does not prevent a state change, it prevents a measurement.
 *
 * WHAT IS TRUE AND STAYS: the far end cannot hold any rule. `content.js handlePageFetch` takes `msg.method`
 * verbatim and content.js is the UNTRUSTED zone, so the trusted sender is the only place a verb is decided.
 * That is why each entry is NAMED FOR ITS OPERATION rather than for a method — the operation is what a reader
 * can check against, and every one of the three states its verb at the call site rather than inheriting one.
 *   pageContextGet   LEARNING a published document. GET, with no parameter to say otherwise.
 *   pageContextSend  the popup's MANUAL REPLAY. Any method — the human chose it in the Send panel.
 *   pageContextFetch the ERROR PROBE (lib/req2proto.js). It names POST because the probe IS a POST.
 *
 * AND EVERY ONE OF THE THREE NAMES ITS INITIATOR GRADE AT THE CALL SITE, for the same reason it names its verb
 * there: the far end holds no policy, so the trusted sender is the only place either fact is decided. The verb
 * says WHAT is being asked; the grade says WHOSE ACT it is, which is what the deny-list exemption below is
 * scoped by. Both are stated rather than inherited, and both are asserted before the message is built. */

/* LEARNING. A GET of a published URL as the page, credentialed by the page's own jar. */
async function pageContextGet(tabId, url, headers, documentId, initiator) {
  /* A GET HAS NO BODY, AND THIS ENTRY SAYS SO RATHER THAN LEAVING THE RELAY TO INFER IT. This was the one
     producer of the three that omitted `body`/`bodyEncoding`, and that omission is what forced `_sendPageFetch`
     to spell a `?? null` — a default that then covered every OTHER caller too, so a body one of them stopped
     writing arrived looking exactly like this one. Stating the absence here is what lets the relay assert it
     for all three. */
  return pageContextFetch(
    tabId,
    url,
    { method: "GET", headers: headers || {}, body: null, bodyEncoding: null },
    documentId,
    initiator,
  );
}

/* THE POPUP'S MANUAL REPLAY — the user named the method, the URL and the body in the Send panel. */
async function pageContextSend(tabId, url, opts, documentId, initiator) {
  DCHECK(!!opts && typeof opts.method === "string" && opts.method !== "",
         "a manual page-context send named no method — this entry exists because the USER chose one, and an " +
         "absent one would silently become a GET of a URL the user meant to POST to");
  return pageContextFetch(tabId, url, opts, documentId, initiator);
}

/**
 * Fetch through a content script in the EXACT document (credentialed,
 * session-aware). Routed by documentId ONLY — no frameId fallback, because a
 * frame is reused across navigations and could be a different origin.
 * @param {string} documentId — the target document (stable across the page's life)
 */
async function pageContextFetch(tabId, url, opts, documentId, initiator) {
  /* WHOSE ACT THIS IS, ASKED FIRST AND ASKED HERE. It is asked before the URL is even parsed because it is not
     a property of the request — every other check below is about the bytes, and this one is about whether this
     transport may carry them at all. */
  pageContextRequireUserInitiated(initiator, "a caller of pageContextFetch naming " + url);
  DCHECK(!!opts && typeof opts.method === "string" && opts.method !== "",
         "a page-context fetch reached the relay with no method — every entry above states one at its call " +
         "site, so an absent method is a caller that named no operation and whose verb nobody decided");
  // Validate URL
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
      return { error: "blocked: invalid protocol" };
    }
  } catch (_) {
    return { error: "blocked: invalid URL" };
  }

  /* THE DESTRUCTIVE-PATH DENY LIST MUST NEVER APPLY HERE, AND THAT IS A RULE ABOUT WHO ACTED RATHER THAN
     ABOUT WHICH TRANSPORT CARRIED IT. The list is a floor under requests THIS TOOL COMPOSED AND FIRED ON ITS
     OWN — CLAUDE.md's own statement of why it exists is "the accidental logout is exactly the class it exists
     to stop: a GET that ends the person's session mid-analysis is a CSRF this tool committed against its own
     user". Every clause of that sentence is about an act nobody asked for. It is sound precisely because it is
     ALLOWED TO BE WRONG: a wrong deny costs one unfired request that forced execution still derives and
     reports in full, so refusing on a guess is cheap.
     THAT ARITHMETIC INVERTS THE MOMENT A HUMAN COMPOSED THE REQUEST. A deny list standing there is not a floor
     under the tool's autonomy, it is the tool VETOING ITS OPERATOR on a substring match — and the cost is no
     longer one unfired request the report still carries, it is a person told their own explicit act was
     "blocked" by a pattern they never saw and cannot argue with. SECURITY.md's egress taxonomy already draws
     exactly this line: a path answering the operator "is authorized by a human at a surface that shows them
     the bytes". Authorization by a human at that surface is the strongest grade this project has; a token list
     is the weakest, and the weak one does not overrule the strong one.
     AND THE EXEMPTION IS SCOPED BY A FACT THIS FILE NOW REQUIRES RATHER THAN BY ONE IT USED TO ASSERT. This
     paragraph said every entry here was "operator-typed … or an operator-initiated probe", and that sentence
     was FALSE OF THREE CALLERS the whole time it stood: `lib/response-decode.js` fired an automatic discovery
     sweep, an automatic req2proto error probe and an automatic service-info probe, all three from
     `handleResponseBody` — the instant a response body arrived, with nobody at any surface. The claim was true
     of the entries this file DECLARES and false of the code that reached them, which is the exact shape of a
     claim that cannot be checked where it is written: the relay could see the request and never the act.
     So the fact is now CARRIED and ASSERTED (`pageContextRequireUserInitiated`, at the head of this function),
     and the automatic callers were moved off this transport rather than exempted by it. That is what makes the
     absence of the list here a scope statement instead of a hole in one.
     SO THE ABSENCE HERE IS A DECISION, NOT A GAP. A gate was added at this exact site and removed; if a future
     reader finds the relay "unprotected" and reaches for `safeFetchDestructiveRefusal`, that reader has found
     the thing that was deliberately taken out. What legitimately guards this relay is the same thing that
     guards any operator action: the operator seeing what they are sending, plus the grade above, which is what
     makes "the operator is seeing this" a checkable claim rather than a description of the call graph. The
     `safeFetch` chokepoint keeps its own list for its own autonomous requests, which is where a floor under
     autonomy belongs — and it is where the requests that used to be exempted here now go. */

  // Try the original tab's target frame
  let _pageFetchErr = null;
  if (tabId != null) {
    try {
      return await _sendPageFetch(tabId, url, opts, documentId);
    } catch (e) {
      /* Page-context fetch failed (tab closed, content script not injected,
         frame removed, etc.). Capture the underlying reason so the caller
         sees more than "content script unreachable". An invariant abort travels ON through it first
         (extension/check.js): this catch's whole job is to turn a throw into `relay_failed`, which is the
         answer every caller reads as "the page could not be reached", and a DCHECK arriving here would be
         reported as exactly that — a broken contract in this zone presented as an unreachable tab. */
      RETHROW_FATAL(e);
      _pageFetchErr = e && e.message || String(e);
      console.debug("[brain] page-context fetch relay failed:", _pageFetchErr, "tabId=" + tabId + " url=" + url);
    }
  }

  return {
    error: "relay_failed: content script unreachable on tab " + tabId + (_pageFetchErr ? " (" + _pageFetchErr + ")" : ""),
  };
}
