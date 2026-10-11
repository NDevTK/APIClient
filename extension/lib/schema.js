// lib/schema.js — Schema inference (VDD). Builds JSON-Schema-ish shapes from observed JSON and protobuf
// (JSPB) trees and merges/drifts them into a document's schema map. Extracted from the offscreen-brain.js
// monolith (one problem per file). Loaded BEFORE offscreen-brain.js in ast-worker.html; these functions stay
// global and resolve their callers at call-time, so load order is safe. KEEPS the protobuf/discovery schema-
// learning feature -- just relocated out of the 8000-line brain.

/* THE MAPS BELOW ARE KEYED BY A SERVER'S OWN FIELD NAMES, so they are minted and read through
   lib/stranger-keyed.js, which states the rule once and carries the measurements — including this file's:
   a discovery document's schema merged against a newly observed body lost 8 OF 10 fields, with no write, no
   drift entry and nothing logged. The three primitives were declared here first and moved out the moment
   lib/learn.js turned out to need the same ones on the method map, because two right answers to one question
   is the shape that drifts. NOTHING HERE ASSERTS on a name: a property map's values are a stranger's, and an
   assert on those hands that stranger an abort switch for this zone. */

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
    const fieldCounts = strangerKeyedMap();
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
        if (!existing.properties) existing.properties = strangerKeyedMap();
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
       = value` is that name used as a computed key on an object literal — the exact write `strangerKeyedMap`
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
      const properties = strangerKeyedMap();
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
    if (!strangerKeyHeld(doc.schemas, schemaName) || !doc.schemas[schemaName]) {
      strangerKeySet(doc.schemas, schemaName, newSchema);
      continue;
    }
    const existing = doc.schemas[schemaName];
    if (!existing.properties) existing.properties = strangerKeyedMap();
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
      const matchKey = (strangerKeyHeld(existing.properties, key) && existing.properties[key]) ? key
        : (fieldNum !== null && numToKey[fieldNum]) ? numToKey[fieldNum]
        : null;
      const old = matchKey ? existing.properties[matchKey] : null;

      if (!old) {
        strangerKeySet(existing.properties, key, newProp);
        if (fieldNum !== null) numToKey[fieldNum] = key;
        existing._drift.push({ type: "field_added", field: key, fieldType: newProp.type, timestamp: Date.now() });
      } else {
        if (matchKey !== key && !old.customName && !/^field\d+$/.test(key)) {
          strangerKeySet(existing.properties, key, old);
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
        if (newProp.$ref && strangerKeyHeld(doc.schemas, newProp.$ref) && doc.schemas[newProp.$ref]) {
          queue.push({ schemaName: newProp.$ref, newSchema: doc.schemas[newProp.$ref] });
        }
      }
    }
    if (existing._drift.length > 50) existing._drift = existing._drift.slice(-50);
  }
}

// ─── Who acted: the grade an operator send carries ──────────────────────────

/* WHO ACTED. An operator send (lib/send.js `operatorSendAsFrame`, safe-fetch.js `safeFetchOperator`) speaks as
 * a frame the person picked, spends that frame's cookies, and is exempt from the destructive-path deny list,
 * because a person composed it at a surface that showed it. Nothing in the request distinguishes a person's
 * act from one a response handler composed, so the grade travels with the call, stated by the site that knows
 * (the popup command handler, the Send panel, the response handler), and the operator tier also CHECKs that
 * an extension document is the sender (`senderIsExtensionDocument`). `PAGE_CONTEXT_TOOL_INITIATED` exists because `lib/discovery-probe.js`'s
 * sweep serves both grades and routes a tool's act to `safeFetch`. CHECK and not DCHECK: the grade scopes an
 * exemption, so proceeding in release with an unstated one is the state to stop. */
const PAGE_CONTEXT_USER_INITIATED = "user-initiated";
const PAGE_CONTEXT_TOOL_INITIATED = "tool-initiated";
const _PAGE_CONTEXT_GRADES = [PAGE_CONTEXT_USER_INITIATED, PAGE_CONTEXT_TOOL_INITIATED];

/* `who` is the caller's own name, so an abort names the caller rather than this shared helper. */
function pageContextRequireUserInitiated(initiator, who) {
  CHECK(initiator === PAGE_CONTEXT_USER_INITIATED,
        "an operator send was reached by " + who + " with the initiator grade " + JSON.stringify(initiator) +
        " — that tier speaks as the page with the person's cookies and no deny list, which is scoped to acts a " +
        "person initiated; an act this tool decided on goes to safeFetch (lib/safe-fetch.js)");
}

/* A routing site asserts the grade before it dispatches on it, so a third spelling cannot take whichever arm
   the site wrote as its else. */
function pageContextStatedGrade(initiator, who) {
  CHECK(_PAGE_CONTEXT_GRADES.indexOf(initiator) >= 0,
        who + " named the initiator grade " + JSON.stringify(initiator) + ", which is neither of the two this " +
        "extension states (" + _PAGE_CONTEXT_GRADES.join(", ") + ") — the grade decides which transport the " +
        "request leaves by");
  return initiator;
}

/* The request facts of a request no observation describes: every member absent, so the identity's
   defaults apply (Fetch §5.4 "Request class"), which is what the page's own `fetch(url)` would send. */
const PAGE_CONTEXT_COMPOSED_INIT = Object.freeze({ credentials: null, mode: null, referrer: null,
                                                   referrerPolicy: null });
/* The Send panel's stated record for an operator send no person stated one for: nothing, so the frame's own
   Origin, Referer and jar are used (lib/safe-fetch.js safeFetchStatedShape). */
const PAGE_CONTEXT_STATED_NONE = Object.freeze({ origin: null, referer: null, jar: null });
