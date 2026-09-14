/* lib/stranger-keyed.js — A MAP KEYED BY A NAME A STRANGER CHOSE, WHICH IS A DIFFERENT THING FROM A MAP.
 *
 * An object literal is not an empty map. It already answers for `constructor`, `toString`, `valueOf`,
 * `hasOwnProperty`, `isPrototypeOf`, `propertyIsEnumerable`, `toLocaleString` and `__proto__`, so
 * `map[name]` for a name NO PRODUCER WROTE returns a FUNCTION or an OBJECT rather than `undefined` — and
 * every `if (!map[name])` get-or-create in this zone reads that as "already present". The name is not
 * exotic: this zone keys its maps on a URL PATH SEGMENT, a GraphQL `operationName`, a JSON response's own
 * field name, a header name, an OpenAPI tag. Each of those is a string a page or a server chose.
 *
 * lib/req2proto.js states this rule twice with two measured incidents behind it — "A MAP AND NOT AN OBJECT
 * LITERAL, BECAUSE THE KEY IS A STRANGER'S TEXT" — and answers it with a `Map`, which is right for a LOOKUP
 * TABLE and wrong for everything here: a learned schema and a learned method list are JSON that ride
 * IndexedDB and `chrome.runtime.sendMessage`'s structured clone, and a Map survives neither. So the same rule
 * needed a second primitive, and until this file it did not have one. THAT is why the same defect was written
 * independently in four files: there was nowhere to route to.
 *
 * WHAT IT COSTS, MEASURED AGAINST THIS ZONE'S OWN CODE RATHER THAN MODELLED, each with an ordinary name as a
 * control that behaved correctly on both sides of the repair:
 *   • lib/learn.js's method registration. `calculateMethodMetadata` answers `"constructor"` for the address
 *     `https://api.example.com/constructor`, and `"constructor"` again for ANY address when a GraphQL
 *     `operationName` hint says so — which is the hint's whole purpose, "each gets its own method entry keyed
 *     by operationName". The get-or-create then reads the prototype, skips the create, and hands the rest of
 *     the function `m = Object`. Two outcomes, both live: for the seven names whose value is a FUNCTION the
 *     DCHECK one line down FIRES, so a page's own URL path aborts the offscreen brain — a page-held abort
 *     switch on the trusted zone, which CLAUDE.md §Offensive-programming forbids by name and which this
 *     project has already paid for once. For `__proto__` the value is `Object.prototype`, the DCHECK PASSES
 *     because that is an object, and the next line writes `m._astInferred = true` — GLOBAL PROTOTYPE
 *     POLLUTION of the trusted realm, from a page's address. Either way the endpoint is never registered.
 *   • lib/schema.js's learned `properties`. A discovery document's schema merged against a newly observed
 *     body lost 8 OF 10 fields — no write, no drift entry, nothing logged.
 *   • lib/openapi-import.js's resource buckets, keyed by a lowercased OpenAPI TAG: `doc.resources["constructor"]`
 *     is truthy, the create is skipped, and `.methods` of the `Object` constructor is `undefined` — a
 *     TypeError that discards the whole import.
 *
 * NOTHING HERE ASSERTS, AND THAT IS THE DESIGN. The names are a stranger's, and CLAUDE.md is explicit that
 * asserting on bytes a stranger stated hands that stranger an abort switch for the trusted zone — which is
 * precisely the bug above. The answer to a name nothing wrote is the REFUSAL these three give: it is not
 * held, so it is absent, so a get-or-create CREATES it and the observation is learned as the observation it
 * is. A crash and a default are both wrong here, in opposite directions.
 *
 * WHICH OF THE THREE A SITE NEEDS IS DECIDED BY WHO MINTED THE MAP, and both cases are real in this zone:
 *   strangerKeyedMap()          the map is OURS. Mint it with no prototype and every plain `map[name]` read
 *                               and write afterwards is already correct — including for `__proto__`, which on
 *                               a null-prototype object is an ordinary own data property. This is the ROOT
 *                               fix and it is the one to reach for.
 *   strangerKeyHeld(map, name)  the map is NOT ours — a discovery document's own object, or a store record
 *                               `JSON.parse` rebuilt — so it has `Object.prototype` under it whatever we do
 *                               and the READ is where the rule has to be applied.
 *   strangerKeySet(map, name, v)  the same map, on the WRITE side, and it is not redundant with the read:
 *                               asking for an own property fixes seven of the eight names outright, and
 *                               `__proto__` survives that repair because `literal["__proto__"] = v` is not a
 *                               store at all — it is `Object.prototype`'s SETTER, so the plain write puts the
 *                               value NOWHERE. `defineProperty` states an own data property whatever the
 *                               target's prototype is, and it round-trips: `JSON.stringify` and
 *                               `structuredClone` both carry it and the map's own prototype is left alone.
 *
 * AND `JSON.parse` DISAGREES WITH AN OBJECT LITERAL ABOUT EXACTLY ONE OF THE EIGHT, which is why a store
 * round trip cannot stand in for any of this: `JSON.parse('{"__proto__":…}')` yields an OWN `__proto__`, so a
 * record restored from IndexedDB can carry a field a freshly-minted one cannot. One server, two paths, two
 * answers about what it sent.
 *
 * LOADED WHEREVER A FILE THAT KEYS ON A STRANGER'S TEXT IS LOADED, and named with its position for the reason
 * check.js is: ast-worker.html and popup.html both load it immediately after check.js. A realm that keys such
 * a map without this file does not key it worse — it throws ReferenceError at the mint, which is the loud
 * failure rather than the quiet one. */
function strangerKeyedMap() { return Object.create(null); }
function strangerKeyHeld(map, name) { return Object.prototype.hasOwnProperty.call(map, name); }
function strangerKeySet(map, name, value) {
  Object.defineProperty(map, name, { value: value, writable: true, enumerable: true, configurable: true });
}
