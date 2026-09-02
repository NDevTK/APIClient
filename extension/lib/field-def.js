/* lib/field-def.js — THE FieldDef record: ONE description of "a form field", shared by every producer that
   has one to describe and by the Send panel that renders it.

   WHY THIS FILE EXISTS. Four producers describe the same thing in four vocabularies — a resolved URL
   parameter, a discovery/OpenAPI schema property, a protobuf probe's learned field, and a synthesized field
   read off a captured JSON body (the GraphQL variables tree is the last of these plus an alias). The form
   builder consumed all four, so wherever one producer was silent about a name another producer writes, the
   builder filled the gap: `fieldDef.type || "string"`, `fieldDef.parentSchema || "params"`,
   `fieldDef.children?.length`. That is CLAUDE.md §Architecture's defaulted-field defect exactly, and its
   failure here is the one §@S names: a shape that carries PROVENANCE and drops the DOMAIN renders an
   unconstrained field and a gate-constrained one with identical bytes, so the panel's silence about the gate
   is read as the positive statement "anything goes".

   THE RULE THIS FILE ENFORCES. A producer states the facts it observed; every other name takes its DECLARED
   ABSENT VALUE, which is a POSITIVE STATEMENT and never a hole a `||` filled — `_range: null` MEANS "no
   ordering statistic was observed", `children: null` MEANS "this field is not a message", `displayName: null`
   MEANS "render the wire name". The consumer reads each as itself.

   WHAT MAKES THAT DIFFERENT FROM THE `||` IT REPLACES, which is the whole point: the `||` sat at the consumer,
   where producer-silence and producer-BROKENNESS are the same bytes. Here the constructor knows every legal
   name, so it REJECTS one it does not know — a producer that renamed `_range` to `_ranges`, or that stopped
   writing a field the panel renders, crashes AT THE PRODUCER instead of rendering "unconstrained" forever.
   Silence is only ever deliberate because the alternative does not compile past this function.

   `undefined` IS NOT A VALUE HERE. This record crosses chrome.runtime.sendMessage from the offscreen zone to
   the popup, and that serialization DROPS an `undefined` property — so a field whose absent value were
   `undefined` would arrive ABSENT on one side of the boundary and present on the other, which is the same
   contract disagreement in a new place. Every absent value below is `null`, `false` or a fact.

   TRUST — ASSERT VS REFUSE. The record's SHAPE is decided by our own literals in the trusted zone, so a
   malformed record is OUR logic broken and a DCHECK is correct (CLAUDE.md §Offensive programming). Its VALUES
   are frequently third-party: a Google Discovery document fetched from the target's server, an OpenAPI file
   the researcher was handed, a protobuf field list learned from a server's own error reply, a captured request
   body the page composed. A DCHECK on those bytes would be the trusted zone crashing on attacker input, so
   each reader of an untrusted document REFUSES a value in a form this record does not carry — `fdDocString`,
   `fdDocKey`, `fdDocList`, `fdDocRecord` below — and refusing yields the declared absent value, which is the true
   statement about it ("that document makes no claim this panel can render"). By the time `makeFieldDef` sees a
   value it came through one of those, so its asserts are about us. */

/* A THIRD-PARTY DOCUMENT'S TEXT, or nothing. `{"name": {}}` in an imported spec is not a name — it is a
   document making no name claim, and `null` says so. This is a REFUSAL and not a coercion: nothing is
   stringified into a plausible label. */
function fdDocString(v) {
  return typeof v === "string" ? v : null;
}

/* A THIRD-PARTY DOCUMENT'S IDENTIFIER — text or a number, or nothing. A protobuf field number arrives as
   either `7` or `"7"` depending on which document wrote it (openapi-import takes `x-field-numbers` off a
   file the researcher was handed, and a Google discovery property's `id` is text), so this record carries
   both spellings and refuses everything else. A BOOLEAN is refused with the objects: `true` is not a field
   number under any reading, and admitting it here would hand `makeFieldDef` a value its own assert rejects —
   a refusal that manufactures the crash it exists to prevent. */
function fdDocKey(v) {
  const t = typeof v;
  return (t === "string" || t === "number") ? v : null;
}

/* A THIRD-PARTY DOCUMENT'S LIST, or nothing — an `enum` that is not an array states no membership. */
function fdDocList(v) {
  return Array.isArray(v) ? v : null;
}

/* A THIRD-PARTY DOCUMENT'S RECORD, or nothing — `{"widget": null}` is a document that NAMED a property and
   described nothing, and `[1,2]` where a record belongs describes nothing either. It exists as one function
   because the alternative was the same `typeof x === "object" && !Array.isArray(x)` written at every reader,
   and one of them was always going to be forgotten: the property walker read `.$ref` straight off a null
   property and threw a TypeError out of the trusted zone on bytes a server chose — the crash-on-attacker-
   input these refusals exist to prevent, arriving through the one reader that did not have the guard. */
function fdDocRecord(v) {
  return (v && typeof v === "object" && !Array.isArray(v)) ? v : null;
}

/* WHERE A PARAMETER GOES — ONE VOCABULARY, because `location` is ONE field with THREE producers that each
   knew a different list, and nothing sat between them to notice. `endpoint.c` mints query/path/body and
   asserts exactly those at the mint AND at the emit. `openapi-import.js` writes an imported spec's `in`
   VERBATIM, which is OpenAPI Specification 3.1.1 §Parameter Object's four — "query", "header", "path" or
   "cookie", both `name` and `in` REQUIRED — or Swagger 2.0 §Parameter Object's five, "query", "header",
   "path", "formData" or "body". `openapi-export.js` then asserted a five-name list of its own.

   THAT LIST WAS WRONG IN BOTH DIRECTIONS AT ONCE, which is what a second copy of a vocabulary buys. It
   OMITTED `header` and `cookie`, so an ordinary spec-valid file — one `X-Api-Key` header parameter — was
   imported without complaint and then ABORTED THE TRUSTED ZONE on the way back out, a DCHECK firing on
   bytes the researcher was handed, which is the one thing CLAUDE.md §Offensive programming says an assert
   must never do. And it ADMITTED `form`, which no producer anywhere writes — a read with no writer sitting
   inside the very assert that was supposed to make this field falsifiable.

   So the list lives HERE, next to the record whose `location` it types, and the export asserts against it
   rather than restating it. A producer that learns a sixth spelling adds it once. */
const PARAM_LOCATIONS = Object.freeze([
  "query",    // OAS 3.1.1 + Swagger 2.0 + endpoint.c
  "path",     // OAS 3.1.1 + Swagger 2.0 + endpoint.c
  "header",   // OAS 3.1.1 + Swagger 2.0
  "cookie",   // OAS 3.1.1
  "body",     // Swagger 2.0 + endpoint.c (a request-body field)
  "formData", // Swagger 2.0
]);

/* A THIRD-PARTY DOCUMENT'S PARAMETER LOCATION, or nothing. A REFUSAL and not a default: `in: "sideband"` is
   a document naming a place this panel cannot put a value, and `in` absent is not a Parameter Object at all
   (both specs above make it REQUIRED) — neither is a query parameter, and answering both with "query" is
   the §@S wrong report, a fabricated placement rendered exactly like a declared one. The caller decides
   what a refusal means for it; what it must not do is invent a location the document never stated. */
function fdDocLocation(v) {
  return (typeof v === "string" && PARAM_LOCATIONS.indexOf(v) >= 0) ? v : null;
}

/* CAN THIS RECORD STATE `v` AS THE FIELD'S EXAMPLE — asked by every mint of the example pair, because
   `_exampleValue: null` is this record's spelling of "NOTHING was computed" and a producer that writes `null`
   as a VALUE has therefore written the absence, together with a `_exampleValueSource` naming where that
   absence came from. That pair is not merely odd: the Send panel's prefill badge is keyed on the SOURCE, so
   the record renders "prefill: enum" beside whatever the captured request happened to put in the box — a
   value this tool observed, published under the claim that this tool computed it, which is the one thing §@H
   forbids a report from saying.
   A DECLARED LIST MAY LEGITIMATELY HOLD `null`, WHICH IS WHY THIS IS A REFUSAL AND NOT A DCHECK — the same
   line `fdDocString` and `fdDocList` stand on. JSON Schema Validation 2020-12 §6.1.2 enum says of the array's
   members: "Elements in the array might be of any type, including null", so an imported spec's
   `enum: [null, "weekly"]` is a document making a claim this record has no way to carry, not a producer of
   ours gone silent. The true answer for it is the one every refusal here gives: this record states no example.
   `undefined` is refused beside it for the reason the header gives — it is not a value here at all, because
   chrome.runtime.sendMessage DROPS an `undefined` property and the pair would arrive split across the zones. */
function fdCanStateExample(v) {
  return v !== null && v !== undefined;
}

/* THE EXAMPLE PAIR READ AS THE ONE FACT IT IS — the attribution a document made for a value THAT DOCUMENT
   ALSO STATED, or nothing. Every projection below reads `_exampleValue` and `_exampleValueSource` off the
   same third-party record, and a source is not a fact of its own: it says where a value came from, so a
   source over an absent value describes the origin of nothing. `fdDocString` alone cannot see that, because
   it is handed one half and asked about its TYPE — and the halves are read one line apart, which is exactly
   the shape a producer's silence hides in.
   IT IS A REFUSAL RATHER THAN AN ASSERT FOR THE REASON THE HEADER GIVES: these bytes are a Google discovery
   document a target's server served or an OpenAPI file a researcher was handed, and such a document can name
   `_exampleValueSource` — every name in this record is a name a JSON object can carry — while naming no
   value. Crashing the trusted zone on that is the one thing an assert must never do. What it yields instead
   is the record's declared absence, which is the true statement about a document that attributed nothing. */
function fdDocExampleSource(value, source) {
  return fdCanStateExample(value) ? fdDocString(source) : null;
}

/* THE RECORD. Every name the Send panel reads is here, with the value that MEANS "the producer observed
   nothing of this kind". `name`, `type`, `label` and `required` have no absent value: a field with no wire
   key, no type, no cardinality or no requiredness is not a field, and a producer that cannot state one has
   nothing to render — so they are stated, never defaulted, and `makeFieldDef` asserts each. */
const FIELD_DEF_ABSENT = Object.freeze({
  // ── identity ──
  displayName: null,        // an alias to render INSTEAD of `name` (the GraphQL variables tree writes one);
                            // null = render the wire name itself.
  customName: false,        // the reviewer renamed this field in a previous session.
  isNameMarker: false,      // `name` is a MARKER this panel MINTED to make a structural fact visible, and
                            // addresses NOTHING — no wire key, no document declaration, no observation off a
                            // wire. `false` = `name` is a wire key, which is what every other producer states
                            // by silence for the same reason `customName: false` says the reviewer has not
                            // renamed it. NO READER OF A THIRD-PARTY DOCUMENT WRITES IT, deliberately: a
                            // document that named `isNameMarker` would be claiming its own property is not a
                            // key, and the way to disbelieve a document is not to read it. That is what makes
                            // this one of the record's OWN facts, asserted below rather than refused.
  parentSchema: null,       // the schema a rename persists under; null = this field has no rename target.
  number: null,             // protobuf/JSPB field number; null = this field is not numbered.
  isNumberGuessed: false,   // `number` is lib/discovery.js's POSITIONAL index, not one any document declared.
  location: null,           // "path" | "query" | "body" … for a URL parameter; null = not a URL parameter.
  // ── type ──
  format: null,             // a declared format narrower than `type`; null = none declared.
  messageType: null,        // the message schema this field's children came from; null = not a message.
  children: null,           // a message's own fields; null = NOT a message. `[]` = a message with no fields,
                            // which is a different statement and must stay distinguishable from it.
  enum: null,               // the membership a document declared; null = none declared.
  enumDescriptions: null,   // per-member text, index-aligned with `enum`; null = none.
  description: null,        // human text; null = none.
  // ── domain and example (CLAUDE.md §@S: a shape states PROVENANCE and DOMAIN, and dropping either is a
  //    wrong report rather than a partial one — so every one of these has a stated absence) ──
  _range: null,             // {min,max} the OBSERVED values spanned — a statistic; null = none observed.
  _bounds: null,            // the interval the bundle's own ordering gates REQUIRE (JSON Schema Validation
                            // 2020-12 §6.2's keys, carried unrenamed from endpoint.c); null = none proved.
  _excludedValues: null,    // values the forced execution PROVED this field is not; null = nothing proved,
                            // `[]` = a claim that was disproved. Two different facts, both preserved.
  _predicates: null,        // the METHOD-CALL gates the bundle's own code ran on this field and the ARM each
                            // answered — [{method,arguments,holds}], the engine's record carried unrenamed
                            // from endpoint.c. It is the third of the three ways a gate narrows a domain (an
                            // equality determines a value, an ordering an interval, a call neither), and the
                            // one §@H names in its headline example. null = nothing proved, `[]` = a claim a
                            // later path disproved — the same two facts `_excludedValues` keeps apart.
  _looselyEquals: null,     // the LOOSE EQUALITIES (`==`) the bundle's own code HELD of this field —
                            // [{value,type}], the engine's record carried unrenamed from endpoint.c. It is
                            // the fourth way a gate narrows a domain and the one arm of an equality that had
                            // nowhere to go: ECMAScript §7.2.14 IsStrictlyEqual step 1 makes a `===` that
                            // held a DETERMINATION, so it arrives as a value in `_astValidValues`, while
                            // §7.2.13 IsLooselyEqual coerces and its holding arm determines none — the engine
                            // records the predicate instead. `type` is half the fact, not a label on it:
                            // §7.1.19 ToString flattens `undefined`, `null`, `0` and `false` onto text a
                            // String operand can also spell. null = nothing proved, `[]` = a claim a later
                            // path disproved — the same two facts `_excludedValues` keeps apart.
  _astValidValues: null,    // values the bundle was observed setting this field to ON A PATH THAT STOOD ON NO
                            // FORCED ARM — the pool a consumer may OFFER from; null = none observed.
  _astForcedValues: null,   // values EVERY sighting of which stood on a forced arm — a real observation this
                            // run made, and one no real client's request carries, so it is a pool of its own
                            // rather than an entry in the one above (lib/endpoint-record.js's
                            // `provenanceOffersExample` states why the line is a FIELD NAME and not a grade
                            // each reader must remember to consult). null = none; `[]` never reaches here.
  _detectedEnum: false,     // the `enum` above was inferred from observations, not declared.
  _defaultValue: null,      // an observed default; null = none.
  _defaultConfidence: null, // how often that default held; null = not measured.
  _requiredConfidence: null,// how often the field was present; null = not measured.
  _exampleValue: null,      // the ONE value to prefill; null = nothing to prefill (never invented — §@H).
  _exampleValueSource: null,// where that value came from; null = there is no value to attribute.
});

const _FIELD_DEF_STATED = ["name", "type", "label", "required"];
const _FIELD_DEF_LABELS = ["required", "optional", "repeated"];

function _fdNullOr(v, t) { return v === null || typeof v === t; }
function _fdNullOrList(v) { return v === null || Array.isArray(v); }

/* THE ONE ORIGIN. Every FieldDef the Send panel renders is built here, including one that has just crossed
   chrome.runtime.sendMessage (the crossing is exactly where a record can arrive short of a name, so the
   re-construction on arrival is the boundary check rather than a redundant one). `where` names the producer,
   because an assertion that cannot say WHICH of the producers went silent sends the reader to read all of
   them. */
function makeFieldDef(parts, where) {
  DCHECK(!!parts && typeof parts === "object" && !Array.isArray(parts),
         "a FieldDef was constructed from something that is not a record of stated facts (" + where + ") — " +
         "every producer passes an object literal naming what it observed, so anything else is that producer " +
         "broken and the Send panel would render a field nothing described");

  /* A NAME THIS RECORD DOES NOT CARRY IS A PRODUCER TALKING TO NOBODY. It is the read-with-no-writer defect
     seen from the writing side, and it is the reason this constructor exists rather than an `Object.assign`:
     a producer that renames a field, or writes one the panel has never rendered, is caught HERE instead of
     rendering the declared absence of the name it meant to write, for ever, with nothing to say so. */
  for (const k of Object.keys(parts)) {
    DCHECK(Object.prototype.hasOwnProperty.call(FIELD_DEF_ABSENT, k) || _FIELD_DEF_STATED.indexOf(k) >= 0,
           "a FieldDef producer stated `" + k + "`, which this record does not carry (" + where + ") — either " +
           "the name is a typo for one it does carry (in which case the fact it states is being dropped and " +
           "the panel renders that field's declared absence instead) or the record must learn the name; a " +
           "third option, writing it anyway, is a producer emitting into a reader that does not exist");
  }

  const fd = {};
  for (const k of Object.keys(FIELD_DEF_ABSENT)) {
    fd[k] = Object.prototype.hasOwnProperty.call(parts, k) ? parts[k] : FIELD_DEF_ABSENT[k];
  }
  for (const k of _FIELD_DEF_STATED) {
    DCHECK(Object.prototype.hasOwnProperty.call(parts, k),
           "a FieldDef producer did not state `" + k + "` (" + where + ") — a field with no wire key, no " +
           "type, no cardinality or no requiredness is not a field, so these four have no absent value to " +
           "fall back on and this one would render as whichever producer's habit the consumer copied");
    fd[k] = parts[k];
  }

  /* THE VALUES ARE OURS BY THE TIME THEY ARRIVE. Each reader of a third-party document refuses through
     fdDocString/fdDocKey/fdDocList above, so a value in the wrong form here is one of OUR producers
     bypassing that boundary — a DCHECK, not a refusal. */
  DCHECK(typeof fd.name === "string" && typeof fd.type === "string",
         "a FieldDef's `name`/`type` is not text (" + where + ") — a document that named something other " +
         "than a string is refused at the reader that read it (fdDocString), so a non-string arriving here " +
         "is that refusal bypassed and the panel would render an object as a field's identity");
  DCHECK(_FIELD_DEF_LABELS.indexOf(fd.label) >= 0,
         "a FieldDef's `label` is `" + fd.label + "`, which is not one of required/optional/repeated (" +
         where + ") — the builder branches on `repeated` to decide whether this field is a list at all, so a " +
         "fourth spelling silently renders an array as a single input");
  DCHECK(typeof fd.required === "boolean" && typeof fd.customName === "boolean" &&
         typeof fd.isNumberGuessed === "boolean" && typeof fd._detectedEnum === "boolean" &&
         typeof fd.isNameMarker === "boolean",
         "a FieldDef's required/customName/isNumberGuessed/_detectedEnum/isNameMarker is not a boolean (" +
         where + ") — each is a claim the panel renders or withholds a control on, and a non-boolean renders " +
         "whichever way truthiness happens to fall");
  /* A MARKER ADDRESSES NOTHING, AND THE THREE NAMES THAT WOULD PUT IT ON A WIRE STATE THEIR ABSENCE BESIDE
     IT — which is what keeps `isNameMarker` from becoming a SECOND bit that can disagree with the ones this
     record already carries (CLAUDE.md §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS: one FACT, and each name a
     question asked of it, never two bits free to contradict each other). `number` is a WIRE TAG, and a
     marker carrying one fills an encoder's slot under a key no document declared; `children: []` MEANS "a
     message with no fields", which goes out as `{}` under that same key, while `null` is the marker's own
     word for "there is nothing here to descend into"; and `customName: true` says a REVIEWER named this
     field, which can only have happened through the rename this fact exists to withhold — so a marker
     carrying one is the suppression already having failed, arriving as a record rather than as a click. */
  DCHECK(!fd.isNameMarker ||
         (fd.number === null && fd.children === null && fd.customName === false),
         "a FieldDef states `isNameMarker: true` beside a wire tag, a children list or a reviewer's rename (" +
         where + ") — a marker is a name this panel minted and no document declared, so it addresses nothing: " +
         "a `number` would fill an encoder's slot under it, `children: []` would put `{}` on the wire under " +
         "it, and `customName` would say a reviewer renamed a position that is not a field");
  DCHECK(_fdNullOr(fd.displayName, "string") && _fdNullOr(fd.parentSchema, "string") &&
         _fdNullOr(fd.location, "string") && _fdNullOr(fd.format, "string") &&
         _fdNullOr(fd.messageType, "string") && _fdNullOr(fd.description, "string") &&
         _fdNullOr(fd._exampleValueSource, "string"),
         "a FieldDef carries text-or-nothing in a third form (" + where + ") — `null` is this record's one " +
         "spelling of \"the producer observed none\", and a second spelling is a consumer having to guess " +
         "which of them it is looking at");
  DCHECK(fd.number === null || typeof fd.number === "string" || typeof fd.number === "number",
         "a FieldDef's `number` is neither a scalar nor null (" + where + ") — openapi-import takes it " +
         "verbatim from an imported spec's `x-field-numbers`, which fdDocKey refuses when it is not one, " +
         "so anything else here is that refusal bypassed");
  DCHECK(_fdNullOrList(fd.children) && _fdNullOrList(fd.enum) && _fdNullOrList(fd.enumDescriptions) &&
         _fdNullOrList(fd._excludedValues) && _fdNullOrList(fd._astValidValues) &&
         _fdNullOrList(fd._astForcedValues) && _fdNullOrList(fd._predicates) &&
         _fdNullOrList(fd._looselyEquals),
         "a FieldDef carries a list-or-nothing in a third form (" + where + ") — `children: null` means NOT " +
         "a message and `children: []` means a message with no fields, and a consumer that cannot tell them " +
         "apart renders one as the other");
  /* THE TWO VALUE POOLS ARE DISJOINT, AND THAT DISJOINTNESS *IS* THE FOLD. A value one sighting computed on
     a forced arm and another computed without one is `derived` by lib/endpoint-record.js's
     `mostObservedProvenance` — the same rule the method's own grade folds by, one level down — and this
     record spells that fold as WHICH POOL the value sits in. So a value in both pools is not a duplicate to
     tidy: it is the fold having failed to happen, and it renders the same bytes twice under two
     contradictory claims, one of which the panel labels as a request no client makes. */
  DCHECK(fd._astValidValues === null || fd._astForcedValues === null ||
         !fd._astForcedValues.some((v) => fd._astValidValues.indexOf(v) >= 0),
         "a FieldDef carries the same learned value in both the offerable and the forced pool (" + where +
         ") — membership of one pool or the other IS this record's spelling of the per-value grade fold, so " +
         "a value in both is a producer that appended where it had to promote, and the panel would offer the " +
         "value as one the app computes and label it as one no client sends");
  DCHECK(_fdNullOr(fd._defaultConfidence, "number") && _fdNullOr(fd._requiredConfidence, "number"),
         "a FieldDef's confidence is neither a number nor null (" + where + ") — the panel renders it as a " +
         "percentage, and a non-number renders as NaN%");
  DCHECK((fd._range === null || (typeof fd._range === "object" && !Array.isArray(fd._range))) &&
         (fd._bounds === null || (typeof fd._bounds === "object" && !Array.isArray(fd._bounds))),
         "a FieldDef's `_range`/`_bounds` is neither a record nor null (" + where + ") — both are DOMAINS " +
         "the panel renders as constraints beside the input, and a scalar there would render as a value the " +
         "code computed, which is the one thing §@H forbids a domain from becoming");
  /* THE EXAMPLE PAIR IS ONE FACT AND ONE HALF OF IT CANNOT ARRIVE ALONE. `_exampleValueSource` says WHERE
     the example came from, so a source standing over `_exampleValue: null` states the origin of a value the
     record has, on the line above it, said does not exist. It is not a harmless inconsistency: the Send
     panel's prefill badge reads the SOURCE and the box's contents come from the CAPTURED request, so the two
     halves render together as "this tool computed the value you are looking at" over a value that came off
     the wire. Every mint of the pair — lib/stats.js's `pickExampleValue`, lib/send.js's learned-path-segment
     attach — asks `fdCanStateExample` before it writes either half, so a split pair HERE is one of those
     mints having been written in two statements rather than one, which is ours and crashes.
     THE REVERSE IS LEGAL AND IS DELIBERATELY NOT ASSERTED: `fdDocString` refuses an attribution a document
     spelled as something other than text, which leaves a real example with no source — a value the panel
     prefills and does not badge, which is the true statement about it. */
  DCHECK(fd._exampleValueSource === null || fdCanStateExample(fd._exampleValue),
         "a FieldDef carries `_exampleValueSource: " + JSON.stringify(fd._exampleValueSource) + "` over an " +
         "`_exampleValue` of " + JSON.stringify(fd._exampleValue) + " (" + where + ") — `null` is this " +
         "record's one spelling of \"nothing was computed for this field\", so a source beside it attributes " +
         "an example that does not exist, and the Send panel's prefill badge is keyed on the source alone: " +
         "it would label the CAPTURED request's own value as one this tool derived");
  return fd;
}

/* THE FIELDS A COLLECTED FIELD STATES, or `null` when it states no message at all — ONE reading of
   `children`, because FOUR encoders read that name and read it FOUR different ways, and only two of the four
   were reading the record.
     WHAT THE RECORD SAYS is above, at `FIELD_DEF_ABSENT`: `children: null` MEANS "this field is not a
   message" and `[]` MEANS "a message with no fields", "which is a different statement and must stay
   distinguishable from it". Two facts, and the encoders disagreed about whether they are two:
   lib/encode.js's protobuf encoder asked `f.children && f.children.length` and read the `null` as itself;
   its JSPB encoder wrote `f.children || []`, which SUBSTITUTES the second statement for the first — the one
   thing the record forbids, performed on the way to the wire.
     AND THE SUBSTITUTION WAS NOT THE `||`. It was ALSO written as a POSITIVE TEST, twice, and that is the
   half a default-detector cannot reach: lib/encode.js's `encodeFormToJson` and lib/popup-gql.js's copy of it
   each asked `Array.isArray(f.children) && f.children.length` and gave the `null` to an `else` that wrote
   `{}` — the same collapse as `|| []`, spelled so that nothing scanning for a default could see it. An
   earlier form of this paragraph credited both of those with reading the `null` as itself, on the strength of
   the ONE branch of each that did (the scalar `!f.children?.length` guard); their MESSAGE branch did not, and
   a census taken one branch at a time is what let the fourth encoder be miscounted as the only offender.
     THAT SUBSTITUTION HAS TWO NAMED PRODUCERS, AND AN EARLIER FORM OF THIS PARAGRAPH KNEW ONE. The first is
   lib/discovery.js's `_circularRefSentinel`, which states `type: "message"` with `children` unstated —
   deliberately, and its own comment says why: "`children: null` says there is no message to descend into". It
   is a TRUNCATION MARKER for a `$ref` that pointed back onto its own chain, and under either spelling it
   encoded as an EMPTY SUBMESSAGE — the JSPB slot its field number names, or `{}` under its literal `"..."`
   key in a JSON or GraphQL-variables body — a message the panel never described, composed out of the
   record's word for "nothing to describe", sent to a server as if it were a field the researcher filled.
     THE SECOND IS THIRD-PARTY AND IS THE ONE THAT CAN CARRY A WIRE TAG. lib/send.js's `_probeFieldsToDefs`
   takes a probe's field list — bytes a target's own error reply chose — and writes `type` verbatim beside
   `children: kids === null ? null : []`, so a probe that names a message field and lists no `children` states
   this very pair. It differs from the sentinel in exactly the property that decides reach: the sentinel is
   pushed with no `number` (its `_stepResolveSchema` branch returns before the positional-number loop), and
   BOTH protobuf encoders skip a field with no wire tag before they ever look at `children` — a probe field
   carries `fdDocKey(pf.number)`, so it is the one that would reach the JSPB slot and the protobuf sub-frame.
     AND THEY DIFFER IN ONE MORE PLACE, WHICH IS WHERE THE NAME ITSELF IS READ AS A KEY. A probe's field name
   is a name the TARGET's own error reply stated; the sentinel's `"..."` is one this extension MINTED, and no
   name above could say so — so the Send panel offered the marker a RENAME control, and a rename on a key no
   schema declares CREATES that property in the stored document (lib/popup-handlers.js RENAME_FIELD), which
   the discovery walk then renders and sends as a field. `isNameMarker` is that fact, stated by the one
   producer that mints a name; it is asserted here rather than refused because no reader of a third-party
   document writes it. Neither this pair nor a name-shaped test could have told the two apart.
     WHAT NEITHER OF THEM REACHES IS AN ENCODER, AND THAT IS NOW A DECISION RATHER THAN AN ACCIDENT. Every
   record the encoders walk comes from `_collectShallow`, which returns `children: null` only for a SCALAR or
   a REPEATED field — and every encoder branches on `repeated` before it branches on `message`, so no
   non-repeated message field carrying `null` reaches the collapsing branch. lib/popup-form.js used to close
   the second half of that by accident: `_buildFieldStep` rendered a `children: null` message as a BARE TEXT
   INPUT with no `.form-message-group`, and `_collectShallow`'s `!childContainer` then dropped the wrapper
   whole — so the panel offered the researcher a box, discarded every keystroke typed into it, and the marker
   never left the popup because a DOM query happened to miss. That is now stated at both ends: the builder
   renders the row with NO control on it and writes `data-message-fields` saying which of the two absences it
   is, and the collector reads that statement, collects nothing, and DCHECKs the OTHER question its one
   `return null` used to answer as well (a message group that failed to render). Nothing is sent for a
   position the panel could not describe, which is the same wire the four encoders now reach on their own.
     SO THE ENCODERS' READING OF `null` IS NOT DEAD CODE KEPT WARM — it is what a probe-produced numbered
   message field would hit the moment one arrives, and it is the reason fixing the panel could be done
   without activating a fabrication. The vocabulary is asserted here so that cannot be the day it is
   discovered.
     SO THE READ IS A NAMED OPERATION AND THE VOCABULARY IS ASSERTED HERE. `undefined` is not one of the two
   answers and never was: `makeFieldDef` writes every key, and lib/popup-form.js's `_collectShallow` — the
   producer of every record the four encoders actually walk — states `children` on all four of its returns.
   A third spelling is therefore a producer of OURS bypassing both, which is what `?.` and `||` were quietly
   surviving, and it crashes. */
function fdChildren(f, where) {
  DCHECK(!!f && typeof f === "object" && !Array.isArray(f),
         "a field's children were asked of something that is not a field record (" + where + ") — the " +
         "encoders walk lib/popup-form.js `_collectShallow`'s own results, so anything else is that walk " +
         "handing an encoder a value it collected from somewhere it does not describe");
  DCHECK(f.children === null || Array.isArray(f.children),
         "a field states `children` as neither a list nor null (" + where + ") — `null` MEANS this field is " +
         "not a message and `[]` MEANS a message with no fields, and those are two facts this record keeps " +
         "apart; a third spelling is a producer that stopped stating the name, which every `|| []` and " +
         "`?.length` on this seam used to answer with the wrong one of the two");
  return f.children;
}

/* DOES THIS FIELD CARRY A MESSAGE TREE TO DESCEND INTO — the question the JSON, JSPB, protobuf and GraphQL
   encoders each ask before walking. It folds the two absences the record keeps apart (`null` = not a
   message, `[]` = a message with no fields) onto ONE answer, which is correct HERE and only here: a walk
   over no fields and a walk over a field that is not a message do the same nothing. The fold is stated
   rather than fallen into, which is the whole difference from `!f.children?.length` — that spelling folded
   a THIRD case in with them, an absent name, and had no way to say it had. */
function fdHasChildren(f, where) {
  const kids = fdChildren(f, where);
  return kids !== null && kids.length > 0;
}
